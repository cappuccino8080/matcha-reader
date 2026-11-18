#requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Init', 'Update')]
    [string]$Phase,
    [string]$DeviceUrl,
    [string]$ArchivePath = (Join-Path $env:USERPROFILE 'Downloads/Hojicha_EPUB_RealDevice_Tests.zip'),
    [switch]$Plan
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$DESTINATION = '/__Test'
$MAX_ENTRY_BYTES = 1048576
$REQUEST_TIMEOUT_SECONDS = 120

# ====================================================================
# 機能・概要: ZIP内の指定エントリを展開せず読み込み、サイズを制限する。
# 引数: Archive - 開いたZIP、Name - エントリ名
# 戻値: エントリのバイト列
function Read-ArchiveEntry {
    param([System.IO.Compression.ZipArchive]$Archive, [string]$Name)
    $entries = @($Archive.Entries | Where-Object { $_.FullName -ceq $Name })
    if ($entries.Count -ne 1 -or $entries[0].Length -gt $MAX_ENTRY_BYTES) {
        throw "ZIP entry missing, duplicated or too large: $Name"
    }
    $stream = $entries[0].Open()
    $buffer = [System.IO.MemoryStream]::new()
    try {
        $stream.CopyTo($buffer)
        return ,$buffer.ToArray()
    } finally {
        $stream.Dispose()
        $buffer.Dispose()
    }
}

# ====================================================================
# 機能・概要: 検証用SHA-256を算出する。
# 引数: Bytes - 対象データ
# 戻値: 小文字の16進数ハッシュ
function Get-BytesHash {
    param([byte[]]$Bytes)
    $hash = [System.Security.Cryptography.SHA256]::Create()
    try {
        return [Convert]::ToHexString($hash.ComputeHash($Bytes)).ToLowerInvariant()
    } finally {
        $hash.Dispose()
    }
}

# ====================================================================
# 機能・概要: HTTP応答を読み取り、失敗時は本文を含めて中断する。自動再送しない。
# 引数: Client - HTTPクライアント、Method - HTTPメソッド、Route - URL末尾、Content - 送信本文
# 戻値: 応答本文のバイト列
function Invoke-DeviceRequest {
    param(
        [System.Net.Http.HttpClient]$Client,
        [string]$Method,
        [string]$Route,
        [System.Net.Http.HttpContent]$Content
    )
    $request = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::new($Method), $Route)
    $request.Content = $Content
    try {
        $response = $Client.SendAsync($request).GetAwaiter().GetResult()
        try {
            $bytes = $response.Content.ReadAsByteArrayAsync().GetAwaiter().GetResult()
            if (-not $response.IsSuccessStatusCode) {
                throw "HTTP $([int]$response.StatusCode) $Route : $([Text.Encoding]::UTF8.GetString($bytes))"
            }
            return ,$bytes
        } finally {
            $response.Dispose()
        }
    } finally {
        $request.Dispose()
    }
}

# ====================================================================
# 機能・概要: 実機のフォルダ一覧を取得する。
# 引数: Client - HTTPクライアント、Path - SD上のフォルダ
# 戻値: 一覧の各要素
function Get-DeviceFiles {
    param([System.Net.Http.HttpClient]$Client, [string]$Path)
    $bytes = Invoke-DeviceRequest $Client 'GET' ("/api/files?path=" + [Uri]::EscapeDataString($Path))
    return ([Text.Encoding]::UTF8.GetString($bytes) | ConvertFrom-Json)
}

try {
    # 送信開始前に3冊とも検証し、ZIPの内容をコマンドとして実行しない。
    $archive = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $ArchivePath).Path)
    try {
        $manifest = [Text.Encoding]::UTF8.GetString((Read-ArchiveEntry $archive 'manifest.json')) | ConvertFrom-Json
        $books = foreach ($case in @('A', 'B', 'C')) {
            $name = "Hojicha_Test_$case.epub"
            $records = @($manifest | Where-Object { $_.filename -ceq $name })
            if ($records.Count -ne 1) { throw "Invalid manifest: $name" }
            $bytes = Read-ArchiveEntry $archive "$Phase/$name"
            $expected = $records[0].($Phase.ToLowerInvariant())
            $hash = Get-BytesHash $bytes
            if ($bytes.Length -ne $expected.size -or $hash -cne $expected.sha256) {
                throw "Archive integrity check failed: $Phase/$name"
            }
            [pscustomobject]@{ Name = $name; Bytes = $bytes; Hash = $hash }
        }
    } finally {
        $archive.Dispose()
    }

    $uploadRoute = '/upload?path=%2F__Test'
    if ($Phase -eq 'Update') { $uploadRoute += '&overwrite=1&cache=content' }
    Write-Output "Phase: $Phase; destination: $DESTINATION"
    foreach ($book in $books) { Write-Output "$($book.Name)  $($book.Bytes.Length) bytes  SHA256=$($book.Hash)" }
    Write-Output "POST $uploadRoute"
    if ($Plan) {
        Write-Output 'Plan only: no device connection or upload.'
        exit 0
    }
    $address = $null
    if (-not [Uri]::TryCreate($DeviceUrl, [UriKind]::Absolute, [ref]$address) -or
        $address.Scheme -notin @('http', 'https') -or $address.AbsolutePath -ne '/' -or
        $address.Query -or $address.Fragment -or $address.UserInfo) {
        throw 'Specify -DeviceUrl http://DEVICE_IP (no path, query or credentials).'
    }
    $handler = [System.Net.Http.HttpClientHandler]::new()
    $handler.AllowAutoRedirect = $false
    $client = [System.Net.Http.HttpClient]::new($handler)
    $client.BaseAddress = $address
    $client.Timeout = [TimeSpan]::FromSeconds($REQUEST_TIMEOUT_SECONDS)
    try {
        $root = @(Get-DeviceFiles $client '/')
        $folder = @($root | Where-Object { $_.name -ieq '__Test' })
        if ($folder.Count -gt 0 -and ($folder.Count -ne 1 -or
                $folder[0].name -cne '__Test' -or -not $folder[0].isDirectory)) {
            throw 'Destination must be the exact directory /__Test.'
        }
        if ($folder.Count -eq 0) {
            if ($Phase -eq 'Update') { throw 'Upload Init and read the books before Update: /__Test is missing.' }
            $fields = [System.Collections.Generic.Dictionary[string,string]]::new()
            $fields.Add('name', '__Test')
            $fields.Add('path', '/')
            $form = [System.Net.Http.FormUrlEncodedContent]::new($fields)
            $null = Invoke-DeviceRequest $client 'POST' '/mkdir' $form
        }
        $existing = @(Get-DeviceFiles $client $DESTINATION)
        foreach ($book in $books) {
            $matches = @($existing | Where-Object { $_.name -ieq $book.Name })
            if ($Phase -eq 'Init' -and $matches.Count -gt 0) {
                throw "Init refuses to overwrite existing book: $($book.Name)"
            }
            if ($Phase -eq 'Update' -and ($matches.Count -ne 1 -or
                    $matches[0].name -cne $book.Name -or $matches[0].isDirectory)) {
                throw "Update requires an existing book with exact spelling: $($book.Name)"
            }
            if (@($existing | Where-Object { $_.name -ieq "$($book.Name).part" }).Count -gt 0) {
                throw "Staging file exists; inspect recovery instructions first: $($book.Name).part"
            }
        }
        foreach ($book in $books) {
            $form = [System.Net.Http.MultipartFormDataContent]::new()
            $part = [System.Net.Http.ByteArrayContent]::new($book.Bytes)
            # 実機パーサーに合わせ、先頭を引用符付きContent-Dispositionにし、filename*を付けない。
            $disposition = [System.Net.Http.Headers.ContentDispositionHeaderValue]::new('form-data')
            $disposition.Name = '"file"'
            $disposition.FileName = '"' + $book.Name + '"'
            $part.Headers.ContentDisposition = $disposition
            $part.Headers.ContentType = [System.Net.Http.Headers.MediaTypeHeaderValue]::new('application/epub+zip')
            $form.Add($part)
            Write-Output "Uploading $($book.Name)..."
            $null = Invoke-DeviceRequest $client 'POST' $uploadRoute $form
            $downloadRoute = '/download?path=' + [Uri]::EscapeDataString("$DESTINATION/$($book.Name)")
            $received = Invoke-DeviceRequest $client 'GET' $downloadRoute
            if ((Get-BytesHash $received) -cne $book.Hash) { throw "Readback mismatch: $($book.Name)" }
            Write-Output "Verified: $($book.Name)"
        }
    } finally {
        $client.Dispose()
    }
    Write-Output "Completed $Phase. Leave File Transfer mode before opening the books."
    exit 0
} catch {
    Write-Error -ErrorRecord $_ -ErrorAction Continue
    if ($_.Exception.InnerException) { Write-Output $_.Exception.InnerException.ToString() }
    Write-Output 'Stopped. Earlier books may already have completed; no rollback or automatic retry was performed.'
    exit 1
}
