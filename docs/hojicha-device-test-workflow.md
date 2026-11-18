# Three-book device test workflow

Use PowerShell 7.2 or newer. These host-only helpers do not flash firmware or modify
Git. Install the intended test firmware separately before testing. Device results
are not implied by the automated host tests.

The upload source defaults to
`$env:USERPROFILE/Downloads/Hojicha_EPUB_RealDevice_Tests.zip`. Override it with
`-ArchivePath`. The script verifies all three selected EPUBs against the ZIP's
manifest before connecting. The destination is fixed at `/__Test`.

Run from the repository root. Replace the address and USB drive with the values
shown on your computer/device. Use a new PC backup directory for each test run:

```powershell
$Device = 'http://192.168.1.100'
$Sd = 'F:\'
$Backup = 'E:\Development\HojichaReader\build\device-test-run1'
```

1. Enter File Transfer mode on Wi-Fi. Send the initial books:

   ```powershell
   pwsh -NoProfile -File scripts/Send-HojichaTestBooks.ps1 -Phase Init -DeviceUrl $Device
   ```

   The helper creates `/__Test` if absent. It refuses to overwrite any of the three
   existing books or staging files, and uses ordinary POST without `overwrite`.
   Each successful upload is downloaded and verified by SHA-256.

2. Leave File Transfer and read **all three books into chapter 3**. Record the text,
   chapter, font and writing direction. Leave the reader so progress is saved.

3. Enter USB Drive mode, connect the SD to the PC, and save the pre-update caches:

   ```powershell
   pwsh -NoProfile -File scripts/Save-HojichaTestCache.ps1 -Stage Before -SdRoot $Sd -BackupRoot $Backup
   ```

   Only the three test-book cache directories are copied. The helper discovers
   them from their unique titles in version-11 `book.bin`, matching the current
   `BookMetadataCache` layout. It does not compute firmware cache hashes on the PC.
   Missing or duplicate title matches stop the operation. Do not keep another
   copy of these test titles elsewhere on the SD. Other books' metadata may be
   read for identification, but their caches and global settings are not copied.

4. Safely eject the USB volume, leave USB Drive mode, disconnect USB, and enter
   Wi-Fi File Transfer mode. Send the updates:

   ```powershell
   pwsh -NoProfile -File scripts/Send-HojichaTestBooks.ps1 -Phase Update -DeviceUrl $Device
   ```

   This uses `POST /upload?path=/__Test&overwrite=1&cache=content`, sequentially for
   A, B and C. All three original filenames must exist with the exact spelling.
   The helper never falls back to an ordinary overwrite or WebDAV PUT.

5. **Do not open the books yet.** Leave Wi-Fi File Transfer and reconnect in USB
   Drive mode. Save the post-update caches using the same PC backup directory:

   ```powershell
   pwsh -NoProfile -File scripts/Save-HojichaTestCache.ps1 -Stage After -SdRoot $Sd -BackupRoot $Backup
   ```

   The saved Before mapping identifies the same directories even though
   `book.bin` has been removed. Both snapshots have file hashes and copies;
   `After/comparison.csv` reports `Unchanged`, `Changed`, `Removed`, or `Added`.
   A snapshot is complete only when its `snapshot.json` exists. Existing snapshots
   are never overwritten. SD files are read only, and links are rejected.

6. Safely eject and disconnect USB, then open each book and record its restored
   position, settings, TOC and text. Close/reopen and reboot to check persistence.

Expected file comparisons before opening:

- Existing `progress.bin`, `readerprefs.bin`, and valid `css_rules.cache`: unchanged.
  A file that was never generated will not appear in the snapshot.
- `book.bin`, chapter HTML/layouts and generated chapter images: removed when present.
- Other files in those cache directories: preserved according to the existing
  [content-update contract](hojicha-content-update.md).

Expected book contents: A/B have chapters 1–7; B chapter 3 changes `青い丸印` to
`赤い三角印`; C inserts `閑話 雨宿り` before chapter 3 and adds chapter 6. The saved
chapter index is not adjusted for insertion: C can resume in a different chapter
even when the progress bytes are identical. Record the result, rather than treating
exact text-position preservation as guaranteed. These EPUBs contain no images,
so they do not exercise generated-image deletion.

## Failures and limitations

`-Plan` on the upload helper checks the ZIP and prints the planned route without
connecting to a device. A running device must already support `cache=content`;
these helpers cannot prove firmware compatibility from its version string.

On an HTTP error, timeout or download mismatch, the helper stops immediately with
a nonzero exit status. Earlier books can already be updated; no automatic retry,
rollback or deletion occurs. Inspect the device and the recovery guide first.
Leftover `.part` files are deliberately not removed. If Init partially succeeded,
another Init run refuses those existing books; resolve the test state manually.

Copies and hash comparisons show which bytes changed; they do not prove render
correctness, power-loss recovery or absence of SD corruption. The three cache
snapshots do not include global settings, bookmarks, clippings, or pending-update
records stored elsewhere. They are focused test evidence, not a full SD backup.
