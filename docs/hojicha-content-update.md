# Hojicha EPUB content updates

This implements chapter 9 of `02_HojichaReader_Firmware_Spec_v1.1.md`, with the
following user-approved changes to the initial design:

- Text and spine changes do not reject an update. Reading state is retained;
  the saved chapter index/text offset can point elsewhere after structural edits.
- Recovery uses full PC retransmission. The device blocks unfinished books;
  it does not retry invalidation on open, keep an old EPUB backup, or roll back.
- PC automation, Catalog, State and virtual libraries are outside this change.

## Usage

Leave the reader and enter File Transfer. With a tested Hojicha firmware installed:

```powershell
curl.exe --fail-with-body -F "file=@Novel.epub" "http://DEVICE/upload?path=/Books&overwrite=1&cache=content"
```

`path` is the destination directory; URL-encode spaces and Japanese text in the URL.
The multipart filename supplies the final EPUB name. Keep that full path unchanged.
Match the exact on-card spelling, including the case of every directory and filename.
FAT can open case aliases, but the existing reading-cache key distinguishes them;
this mode rejects aliases rather than updating the wrong cache.
The client declares that this is an update of the same work. There is no content or
spine comparison gate. Use the same CSS template; a template change is not detected.

The mode removes `book.bin`, `sections/`, `vsections/`, `html/`, and chapter images
named `img_<number>_<number>` or `img_v<number>_<number>` with jpg/jpeg/png/bmp/gif/pxc
extensions (image extension case is ignored). Numeric names follow the generators:
no signs or leading zeroes except the number zero itself. Unrelated `img_*` files
and directories are preserved.

It preserves `progress.bin`, reader preferences, language choice, bookmarks,
clippings and statistics. It also leaves covers, thumbnails, `ruby.bin`, `wlscan.bin`
and other unlisted files alone. Old cover art or harvested ruby readings can remain.
Valid `css_rules.cache` is preserved; malformed/outdated CSS is regenerated when
embedded styles are enabled. To change CSS intentionally, back up the affected book
cache, remove only its `css_rules.cache`, and reopen with embedded styles enabled;
the existing CSS rebuild also invalidates chapter layouts. Never remove the whole
book cache as a shortcut when reading state must survive.

## Interrupted updates

The fully written `.part` is closed before a pending record is written. The record
has a versioned header, path length, checksum and the exact full path. The checksum
detects corruption; it is not an authentication mechanism. The record directory is
reserved from general HTTP/plugin file operations (WebDAV already blocks hidden paths).

Only after the record is closed and read back does the device replace the EPUB.
It then removes the selected caches, persists a library-dirty notification, and
removes the record last. Missing cache files are harmless. If writing, closing or
verifying a new record fails, the device attempts to delete that new record and
aborts without touching the original EPUB or its caches. Successful cleanup leaves
the original book readable. Failed cleanup is logged and can leave the book blocked;
use the manual recovery steps below. A pre-existing record is never removed by this
cleanup. Failures after replacement retain the read barrier. There is no full-clear fallback.

After a connection loss or missing success response, retransmit the entire EPUB
with the same filename, directory, `overwrite=1` and `cache=content`. Do not resume
from a byte offset. A matching valid record permits this even if replacement removed
the original but failed to rename `.part`. A remaining `.part` is not automatically
installed: it is overwritten by the complete retry. A new book without a record is
still rejected. A successful retry preserves reading state and unblocks the book.

A book with a pending, unreadable or malformed record cannot be opened through the
reader or used as an EPUB metadata-cache source. Opening it shows a retransmission
message; a missing file may no longer appear in a fresh directory listing. Existing
library rows can retain old list metadata until reconciliation. Other books remain
usable when storage is healthy. No background recovery worker is added.
State-preserving rename is refused for a book with an unfinished update (including
a folder containing it), because moving its cache would bypass the path-bound record.
Complete retransmission at the original path before renaming it.
This also covers a missing formal EPUB and case aliases of its parent. If a pending
record is unreadable or malformed, state-preserving renames are held until that record
is repaired, because its affected path cannot be established safely.

If retry is refused because the record is damaged or `.part` is unrelated:

1. Stop transfers and back up the SD card, including the original EPUB, `.part`,
   the book cache and `/.crosspoint/hojicha-updates/`.
2. Use the logged full path to identify the exact book and cache. Do not infer the
   cache hash on a PC: its standard-library hash can differ from the firmware's.
3. With the device stopped/USB Drive exclusively owning the card, restore a known
   complete EPUB to that exact path and remove only its listed content caches.
4. Only after those steps succeed, remove that book's damaged pending record and
   stale `.part`. Keep `progress.bin`, settings and all other books untouched.
5. Safely eject, restart, and verify the contents and saved position.

An ordinary POST or PUT still performs its legacy full cache clear. This also
retires the pending record after the full clear succeeds, but loses per-cache reading
state as before. Do not use it as a position-preserving recovery command.

FAT replacement and SD writes are not guaranteed atomic under physical power loss.
This design detects unfinished application operations; filesystem/card corruption
still requires backup-based recovery. No hardware power-loss result is claimed.

## Acceptance tests

The owner accepted normal-update behavior on X4 Pro on 2026-11-18:

- A and B resumed at the original chapter-3 position; B displayed the updated sentence.
- C resumed in the inserted interlude, consistent with the unchanged spine index.
  A follow-up from old chapter page 3/3 resumed at interlude page 2/2 rather than an invalid page.
- Before/after snapshots of all three books preserved progress, preferences and CSS byte-for-byte.
  All three `book.bin` files and nine vertical-section files were removed; B's `pace.bin` was preserved.
  The saved files were independently hashed: 22 before and 10 after.

These results cover normal updates and the tested insertion/page-range case. Physical SD-error,
power-loss recovery and generated-image deletion were not tested on hardware. Automated fault
injection covers storage failures; it does not establish physical power-loss behavior.

For further testing, back up the card and use disposable books, with position sync disabled:

1. Record position text, per-book preferences, and hashes of preserved files. Update
   with chapter additions, text edits and inserted images. Before first open, verify
   `progress.bin` and preferences are byte-identical and only listed caches disappeared.
2. Open in horizontal and vertical modes. Check new TOC, text and images, close/reopen,
   and reboot. Position after chapter rearrangement is recorded, not presumed accurate.
3. Verify same-CSS reuse, old-format/truncated CSS regeneration, and explicit CSS changes.
4. Check ordinary POST and WebDAV PUT still clear the full book cache. Check another
   book, bookmarks, clippings and statistics remain unchanged for content updates.
5. Interrupt transfer, replacement and deletion on disposable data. Confirm no old
   chapter is displayed, state survives, and full retransmission completes recovery.
   Include a missing formal EPUB, malformed marker, failed marker removal, full card,
   unrelated `.part`, and a lost success response.
6. Test the incomplete-update popup with buttons/touch, remapping and all orientations.
   Measure internal RAM, PSRAM, largest free blocks and stack headroom over repeated
   transfers/opens. The new code streams records and reuses the 4 KB upload buffer;
   it does not retain an EPUB-sized buffer or allocate a new task. Admission uses a
   checked 2 KB temporary scratch allocation for exact on-card path spelling; it is
   released before allocating the upload buffer. Existing HAL file handles still
   allocate internally. Include an image-heavy book to measure directory traversal.
   Rename preflight scans pending records with one checked 1 KB temporary path buffer.

Automated fault-injection tests use the production cache/update implementation with
a storage stub. They do not substitute for X4 Pro SD, rendering or power-loss tests.
Hardware testing is required before a PR can be opened.
