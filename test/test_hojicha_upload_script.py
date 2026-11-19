"""実機に接続せず、PowerShell転送スクリプトのHTTP契約を検証する。"""

from email.parser import BytesParser
from email.policy import default
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Thread
from urllib.parse import parse_qs, urlsplit
import hashlib
import json
import shutil
import subprocess
import struct
import tempfile
import unittest
import zipfile


ROOT = Path(__file__).resolve().parents[1]


class UploadScriptTests(unittest.TestCase):
    # ====================================================================
    def test_cache_snapshots(self):
        """機能・概要: 三冊だけの保存、book.bin削除後の対応表再利用、差分CSVを確認する。"""
        titles = ["検証A・末尾に第6章と第7章を追加", "検証B・第3章微修正と末尾追加", "検証C・第3章の前に閑話を挿入"]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sd = root / "sd"
            books = sd / "__Test"
            books.mkdir(parents=True)
            for index, case in enumerate("ABC"):
                (books / f"Hojicha_Test_{case}.epub").write_bytes(b"epub")
                cache = sd / ".crosspoint" / f"epub_{index}"
                cache.mkdir(parents=True)
                title = titles[index].encode()
                (cache / "book.bin").write_bytes(struct.pack("<BIHHI", 11, 0, 5, 5, len(title)) + title)
                (cache / "progress.bin").write_bytes(b"saved position")
                (cache / "readerprefs.bin").write_bytes(b"settings")
                (cache / "css_rules.cache").write_bytes(b"styles")
            other = sd / ".crosspoint" / "epub_999"
            other.mkdir()
            (other / "progress.bin").write_bytes(b"other book")
            backup = root / "backup"

            # ====================================================================
            def run(stage):
                """機能・概要: 模擬SDフォルダに対してキャッシュ保存を実行する。"""
                return subprocess.run([
                    shutil.which("pwsh") or "pwsh", "-NoProfile", "-File",
                    str(ROOT / "scripts/Save-HojichaTestCache.ps1"), "-Stage", stage,
                    "-SdRoot", str(sd), "-BackupRoot", str(backup)
                ], capture_output=True, timeout=30)

            result = run("Before")
            self.assertEqual(result.returncode, 0, (result.stdout + result.stderr).decode("utf-8", errors="replace"))
            self.assertFalse((backup / "Before/epub_999").exists())
            self.assertNotEqual(run("Before").returncode, 0)
            for index in range(3):
                (sd / f".crosspoint/epub_{index}/book.bin").unlink()
            result = run("After")
            self.assertEqual(result.returncode, 0, (result.stdout + result.stderr).decode("utf-8", errors="replace"))
            report = (backup / "After/comparison.csv").read_text(encoding="utf-8-sig")
            self.assertEqual(report.count('"Removed"'), 3)
            self.assertEqual(report.count('"Unchanged"'), 9)
            self.assertEqual((other / "progress.bin").read_bytes(), b"other book")

    # ====================================================================
    def test_transfer_phases_and_failures(self):
        """機能・概要: 初期配置・保持更新・誤上書き拒否・失敗後の中断を模擬HTTPで確認する。"""
        state = {"folder": False, "books": {}, "uploads": [], "fail": "", "corrupt": False}

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"
            # ====================================================================
            def log_message(self, *_args):
                """機能・概要: テスト用HTTPの通常アクセスログを抑止する。"""

            # ====================================================================
            def respond(self, code, body):
                """機能・概要: 指定したHTTPコードと本文を返す。"""
                self.send_response(code)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            # ====================================================================
            def do_GET(self):
                """機能・概要: フォルダ一覧と読戻しの応答を生成する。"""
                route = urlsplit(self.path)
                path = parse_qs(route.query)["path"][0]
                if route.path == "/download":
                    data = state["books"][Path(path).name]
                    self.respond(200, b"corrupt" if state["corrupt"] else data)
                    return
                if path == "/":
                    items = [{"name": "__Test", "isDirectory": True}] if state["folder"] else []
                else:
                    items = [{"name": name, "isDirectory": False} for name in state["books"]]
                self.respond(200, json.dumps(items).encode())

            # ====================================================================
            def do_POST(self):
                """機能・概要: フォルダ作成とmultipart転送を解析し、指定時は失敗させる。"""
                body = self.rfile.read(int(self.headers["Content-Length"]))
                route = urlsplit(self.path)
                if route.path == "/mkdir":
                    assert parse_qs(body.decode()) == {"name": ["__Test"], "path": ["/"]}
                    state["folder"] = True
                else:
                    mime = BytesParser(policy=default).parsebytes(
                        f'Content-Type: {self.headers["Content-Type"]}\r\n\r\n'.encode() + body)
                    part = list(mime.iter_parts())[0]
                    name = part.get_filename()
                    # 実機の簡易パーサーは先頭のContent-Dispositionと引用符付き2項目を前提とする。
                    headers = body.split(b'\r\n\r\n', 1)[0].split(b'\r\n')[1:]
                    expected_header = f'Content-Disposition: form-data; name="file"; filename="{name}"'.encode()
                    if not headers or headers[0] != expected_header:
                        self.respond(400, b"unsupported multipart headers for device parser")
                        return
                    assert part.get_param("name", header="content-disposition") == "file"
                    state["uploads"].append((name, parse_qs(route.query)))
                    if name == state["fail"]:
                        self.respond(500, b"injected upload failure")
                        return
                    state["books"][name] = part.get_payload(decode=True)
                self.respond(200, b"ok")

        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "test.zip"
            manifest = []
            with zipfile.ZipFile(archive, "w") as target:
                for case in "ABC":
                    name = f"Hojicha_Test_{case}.epub"
                    item = {"filename": name}
                    for phase in ("Init", "Update"):
                        data = f"test payload {phase} {case}".encode()
                        target.writestr(f"{phase}/{name}", data)
                        item[phase.lower()] = {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}
                    manifest.append(item)
                target.writestr("manifest.json", json.dumps(manifest))

            server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
            worker = Thread(target=server.serve_forever, daemon=True)
            worker.start()

            # ====================================================================
            def run(phase):
                """機能・概要: ローカル模擬サーバーに対して転送スクリプトを実行する。"""
                return subprocess.run([
                    shutil.which("pwsh") or "pwsh", "-NoProfile", "-File",
                    str(ROOT / "scripts/Send-HojichaTestBooks.ps1"), "-Phase", phase,
                    "-ArchivePath", str(archive), "-DeviceUrl", f"http://127.0.0.1:{server.server_port}"
                ], capture_output=True, timeout=30)

            try:
                self.assertNotEqual(run("Update").returncode, 0)
                self.assertEqual(state["uploads"], [])
                result = run("Init")
                self.assertEqual(result.returncode, 0, (result.stdout + result.stderr).decode("utf-8", errors="replace"))
                self.assertEqual(len(state["uploads"]), 3)
                self.assertTrue(all(query == {"path": ["/__Test"]} for _, query in state["uploads"]))
                self.assertNotEqual(run("Init").returncode, 0)
                self.assertEqual(len(state["uploads"]), 3)
                result = run("Update")
                self.assertEqual(result.returncode, 0, (result.stdout + result.stderr).decode("utf-8", errors="replace"))
                self.assertEqual(len(state["uploads"]), 6)
                expected = {"path": ["/__Test"], "overwrite": ["1"], "cache": ["content"]}
                self.assertTrue(all(query == expected for _, query in state["uploads"][3:]))
                state["uploads"].clear()
                state["fail"] = "Hojicha_Test_B.epub"
                self.assertNotEqual(run("Update").returncode, 0)
                self.assertEqual(len(state["uploads"]), 2)
                state["fail"] = ""
                state["uploads"].clear()
                state["corrupt"] = True
                self.assertNotEqual(run("Update").returncode, 0)
                self.assertEqual(len(state["uploads"]), 1)
            finally:
                server.shutdown()
                server.server_close()
                worker.join()


if __name__ == "__main__":
    unittest.main()
