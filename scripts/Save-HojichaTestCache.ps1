#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Before', 'After')]
    [string]$Stage,
    [Parameter(Mandatory)]
    [string]$SdRoot,
    [string]$BackupRoot = (Join-Path $PSScriptRoot '../build/hojicha-device-test')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$TEST_TITLES = [ordered]@{
    'Hojicha_Test_A.epub' = '検証A・末尾に第6章と第7章を追加'
    'Hojicha_Test_B.epub' = '検証B・第3章微修正と末尾追加'
    'Hojicha_Test_C.epub' = '検証C・第3章の前に閑話を挿入'
}

# ====================================================================
# 機能・概要: リンク経由で別の場所を読み書きしないよう対象を確認する。
# 引数: Item - ファイルまたはフォルダ
function Assert-OrdinaryItem {
    param([System.IO.FileSystemInfo]$Item)
    if ($Item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
        throw "Links are not supported: $($Item.FullName)"
    }
}

# ====================================================================
# 機能・概要: 現行book.bin形式のタイトルを読み、対象キャッシュを識別する。
# 引数: Path - book.binパス
# 戻値: タイトル。未対応形式や不完全なヘッダーは空文字
function Get-CacheTitle {
    param([string]$Path)
    $BOOK_CACHE_VERSION = 11
    $TITLE_OFFSET = 9 # version(1), LUT offset(4), spine count(2), TOC count(2)
    $MAX_TITLE_BYTES = 4096
    $stream = [System.IO.File]::OpenRead($Path)
    $reader = [System.IO.BinaryReader]::new($stream)
    try {
        if ($stream.Length -lt ($TITLE_OFFSET + 4) -or $reader.ReadByte() -ne $BOOK_CACHE_VERSION) { return '' }
        $stream.Position = $TITLE_OFFSET
        $length = $reader.ReadUInt32()
        if ($length -gt $MAX_TITLE_BYTES -or $length -gt ($stream.Length - $stream.Position)) { return '' }
        return [Text.Encoding]::UTF8.GetString($reader.ReadBytes($length))
    } finally {
        $reader.Dispose()
    }
}

try {
    $sd = (Get-Item -LiteralPath $SdRoot -Force)
    Assert-OrdinaryItem $sd
    $cacheRoot = Get-Item -LiteralPath (Join-Path $sd.FullName '.crosspoint') -Force
    Assert-OrdinaryItem $cacheRoot
    $testFolder = Get-Item -LiteralPath (Join-Path $sd.FullName '__Test') -Force
    Assert-OrdinaryItem $testFolder
    foreach ($name in $TEST_TITLES.Keys) {
        $book = Get-Item -LiteralPath (Join-Path $testFolder.FullName $name) -Force
        Assert-OrdinaryItem $book
        if ($book.PSIsContainer) { throw "Expected an EPUB file: $name" }
    }
    $backup = [IO.Path]::GetFullPath($BackupRoot)
    $sdPrefix = $sd.FullName.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if ($backup.Equals($sd.FullName, [StringComparison]::OrdinalIgnoreCase) -or
        $backup.StartsWith($sdPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'BackupRoot must be on the PC, outside the SD root.'
    }
    # 既存の親も確認し、PC上のリンクを通じたSDへの書込みを防ぐ。
    $ancestor = [IO.DirectoryInfo]::new($backup)
    while ($null -ne $ancestor) {
        if ($ancestor.Exists) { Assert-OrdinaryItem $ancestor }
        $ancestor = $ancestor.Parent
    }
    $destination = Join-Path $backup $Stage
    if (Test-Path -LiteralPath $destination) { throw "Snapshot already exists; choose another BackupRoot: $destination" }
    if ($Stage -eq 'Before') {
        $mapping = @()
        $candidates = @(Get-ChildItem -LiteralPath $cacheRoot.FullName -Directory -Force |
            Where-Object { $_.Name -cmatch '^epub_[0-9]+$' })
        foreach ($directory in $candidates) {
            Assert-OrdinaryItem $directory
            $metadata = Join-Path $directory.FullName 'book.bin'
            if (-not (Test-Path -LiteralPath $metadata -PathType Leaf)) { continue }
            Assert-OrdinaryItem (Get-Item -LiteralPath $metadata -Force)
            $title = Get-CacheTitle $metadata
            foreach ($name in $TEST_TITLES.Keys) {
                if ($title -ceq $TEST_TITLES[$name]) {
                    $mapping += [pscustomobject]@{ Book = $name; Cache = $directory.Name; Title = $title }
                }
            }
        }
    } else {
        $before = Get-Content -LiteralPath (Join-Path $backup 'Before/snapshot.json') -Raw | ConvertFrom-Json
        $mapping = @($before.Mapping)
    }
    if ($mapping.Count -ne 3) { throw 'Expected exactly three test caches. Read all three books first; duplicate titles are ambiguous.' }
    foreach ($name in $TEST_TITLES.Keys) {
        $matches = @($mapping | Where-Object { $_.Book -ceq $name -and $_.Title -ceq $TEST_TITLES[$name] })
        if ($matches.Count -ne 1 -or $matches[0].Cache -cnotmatch '^epub_[0-9]+$') {
            throw "Invalid or ambiguous cache mapping: $name"
        }
    }
    if (@($mapping.Cache | Select-Object -Unique).Count -ne 3) { throw 'Duplicate cache mapping.' }
    $null = [IO.Directory]::CreateDirectory($destination)
    $inventory = @()
    foreach ($item in $mapping) {
        $source = Get-Item -LiteralPath (Join-Path $cacheRoot.FullName $item.Cache) -Force
        Assert-OrdinaryItem $source
        if (-not $source.PSIsContainer) { throw "Missing cache directory: $($item.Cache)" }
        # 列挙してリンクを拒否してからコピーし、SD側は変更しない。
        $children = @(Get-ChildItem -LiteralPath $source.FullName -Recurse -Force)
        foreach ($child in $children) { Assert-OrdinaryItem $child }
        $target = Join-Path $destination $item.Cache
        $null = [IO.Directory]::CreateDirectory($target)
        foreach ($child in $children) {
            $relative = [IO.Path]::GetRelativePath($source.FullName, $child.FullName)
            $copy = Join-Path $target $relative
            if ($child.PSIsContainer) {
                $null = [IO.Directory]::CreateDirectory($copy)
                continue
            }
            $hash = (Get-FileHash -LiteralPath $child.FullName -Algorithm SHA256).Hash
            $null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($copy))
            [IO.File]::Copy($child.FullName, $copy, $false)
            if ((Get-FileHash -LiteralPath $copy -Algorithm SHA256).Hash -cne $hash -or
                (Get-FileHash -LiteralPath $child.FullName -Algorithm SHA256).Hash -cne $hash) {
                throw "Cache changed during copy: $($child.FullName)"
            }
            $inventory += [pscustomobject]@{
                Path = "$($item.Cache)/$($relative.Replace('\', '/'))"; Size = $child.Length; SHA256 = $hash
            }
        }
        Write-Output "Saved $($item.Book) -> $($item.Cache)"
    }
    if ($Stage -eq 'After') {
        $old = @{}
        foreach ($file in $before.Files) { $old[$file.Path] = $file.SHA256 }
        $current = @{}
        foreach ($file in $inventory) { $current[$file.Path] = $file.SHA256 }
        $comparison = foreach ($path in @(@($old.Keys) + @($current.Keys) | Sort-Object -Unique)) {
            $status = if (-not $current.ContainsKey($path)) { 'Removed' }
                elseif (-not $old.ContainsKey($path)) { 'Added' }
                elseif ($old[$path] -ceq $current[$path]) { 'Unchanged' }
                else { 'Changed' }
            [pscustomobject]@{ Path = $path; Status = $status }
        }
        $comparison | Export-Csv -LiteralPath (Join-Path $destination 'comparison.csv') -NoTypeInformation -Encoding utf8
        $comparison | Group-Object Status | ForEach-Object { Write-Output "$($_.Name): $($_.Count)" }
    }
    # このファイルが存在するときだけ保存完了と扱い、不完全なBeforeをAfterに使わない。
    [pscustomobject]@{ Stage = $Stage; SavedAt = [DateTimeOffset]::Now.ToString('o'); Mapping = $mapping; Files = $inventory } |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $destination 'snapshot.json') -Encoding utf8
    Write-Output "Snapshot complete: $destination"
    exit 0
} catch {
    Write-Error -ErrorRecord $_ -ErrorAction Continue
    Write-Output 'Snapshot incomplete. Source SD files were not modified; use a new BackupRoot for another attempt.'
    exit 1
}
