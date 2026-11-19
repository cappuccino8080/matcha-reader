"""Hojichaの実際の更新処理とHTTPコールバックをホスト上で検証する。"""

from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
TEST_DIR = ROOT / "test" / "hojicha_cache"


class HojichaCacheTests(unittest.TestCase):
    # ====================================================================
    def compile_and_run(self, source: Path, output_dir: Path, css: bool = False) -> None:
        """機能・概要: 本番のC++更新処理をスタブと結合して実行する。

        引数: source - 検証コード、output_dir - 一時ビルド先、css - CSS検証用の依存関係を使う
        """
        includes = [TEST_DIR / "stubs", ROOT / "lib/HojichaCacheRefresh", ROOT / "lib/Memory", ROOT / "src"]
        sources = [source, ROOT / "lib/HojichaCacheRefresh/HojichaCacheRefresh.cpp"]
        if css:
            includes = [ROOT / "test/css_parser/stubs", ROOT / "lib/Epub/Epub/css",
                        ROOT / "lib/AsciiText", ROOT / "lib/Memory"]
            sources = [source, ROOT / "lib/Epub/Epub/css/CssParser.cpp"]
        executable = output_dir / ("check.exe" if os.name == "nt" else "check")
        compiler = shutil.which("cl") if os.name == "nt" else shutil.which("g++")
        if not compiler:
            self.fail("C++20 host compiler required (run Windows tests from a Visual Studio developer environment)")
        if os.name == "nt":
            command = [compiler, "/nologo", "/std:c++20", "/Zc:preprocessor", "/EHsc", "/W3", "/UNDEBUG"]
            if css:
                command.append("/Dstrncasecmp=_strnicmp")
            command += [f"/I{path}" for path in includes] + [str(path) for path in sources]
            command += [f"/Fe{executable}"]
        else:
            command = [compiler, "-std=c++20", "-Wall", "-Wextra", "-UNDEBUG"]
            command += [f"-I{path}" for path in includes] + [str(path) for path in sources]
            command += ["-o", str(executable)]
        subprocess.run(command, cwd=output_dir, check=True)
        subprocess.run([str(executable)], cwd=output_dir, check=True)

    # ====================================================================
    def test_cache_faults(self) -> None:
        """機能・概要: 選択削除・保持・再送とストレージ障害を検証する。"""
        with tempfile.TemporaryDirectory() as directory:
            self.compile_and_run(TEST_DIR / "HojichaCacheTest.cpp", Path(directory))

    # ====================================================================
    def test_css_validation(self) -> None:
        """機能・概要: 既存CSS検証の保持・破損・形式不一致・再生成を確認する。"""
        with tempfile.TemporaryDirectory() as directory:
            self.compile_and_run(TEST_DIR / "CssCacheCheck.cpp", Path(directory), css=True)

    # ====================================================================
    def test_upload_callbacks(self) -> None:
        """機能・概要: 実ソースから抽出した転送コールバックの分岐と副作用を検証する。"""
        web = (ROOT / "src/network/CrossPointWebServer.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/network/CrossPointWebServer.h").read_text(encoding="utf-8")
        dav = (ROOT / "src/network/WebDAVHandler.cpp").read_text(encoding="utf-8")
        library = (ROOT / "lib/LibraryIndex/LibraryBuilder.cpp").read_text(encoding="utf-8")
        persist = library[library.index("bool persistLibraryIndexDirty("):library.index("bool buildLibraryIndex(")]
        rename = (ROOT / "src/util/RenameWithState.cpp").read_text(encoding="utf-8")
        rename = rename[rename.index("bool renamePathWithState("):rename.rindex("}  // namespace renamestate")]
        state = header[header.index("  struct UploadState {"):header.index("  CrossPointWebServer();")]
        callbacks = web[web.index("static unsigned long uploadStartTime"):web.index("void CrossPointWebServer::handleCreateFolder")]
        put = dav[dav.index("void WebDAVHandler::handlePut("):dav.index("// ── DELETE")]
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "UploadTest.cpp"
            source.write_text(
                f'#include "{(TEST_DIR / "UploadHarness.h").as_posix()}"\n'
                "class CrossPointWebServer { public:\n" + state
                + "WebServer* server; bool running = true;\n"
                + "explicit CrossPointWebServer(WebServer* s) : server(s) {}\n"
                + "bool dropUploadIfCancelled() const { return false; }\n"
                + "void handleUpload(UploadState&) const; void handleUploadPost(UploadState&) const; };\n"
                + 'namespace library { constexpr char CACHE_DIR[] = "/.crosspoint"; '
                + 'constexpr char DIRTY_PATH[] = "/.crosspoint/library.dirty"; bool dirtyInMemory = false;\n'
                + persist + '}\n' + callbacks + put
                + f'\n#include "{(TEST_DIR / "RenameHarness.h").as_posix()}"\n'
                + 'namespace renamestate {\n' + rename + '}\n'
                + f'\n#include "{(TEST_DIR / "UploadChecks.cpp").as_posix()}"\n',
                encoding="utf-8",
            )
            self.compile_and_run(source, Path(directory))


if __name__ == "__main__":
    unittest.main()
