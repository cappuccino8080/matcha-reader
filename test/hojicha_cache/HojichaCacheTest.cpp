#include <HalStorage.h>
#include <HojichaCacheRefresh.h>

#include <cassert>
#include <functional>
#include <iostream>

using hojicha::UpdateState;
using hojicha::UploadCheck;

const std::string BOOK = "/Books/Novel.epub";
const std::string PART = BOOK + ".part";
const std::string CACHE = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(BOOK));
const std::string MARKER =
    std::string(hojicha::UPDATE_DIRECTORY) + "/" + std::to_string(std::hash<std::string>{}(BOOK)) + ".pending";

void seed() {
  Storage.reset();
  Storage.files[BOOK] = {false, "old EPUB"};
  Storage.files[PART] = {false, "new EPUB"};
  Storage.files[CACHE] = {true, {}};
  for (const char* directory : {"sections", "vsections", "html"}) {
    Storage.files[CACHE + '/' + directory] = {true, {}};
    Storage.files[CACHE + '/' + directory + "/0.bin"] = {false, "old content"};
  }
  for (const char* name : {"book.bin", "img_0_0.jpg", "img_0_0.pxc", "img_v1_2.PNG", "img_v1_2.pxc", "progress.bin",
                           "readerprefs.bin", "css_rules.cache", "language.bin", "pace.bin", "ruby.bin", "wlscan.bin",
                           "cover.bmp", "thumb_100.bmp", "img_notes.txt", "img_01_2.jpg"}) {
    Storage.files[CACHE + '/' + name] = {false, name};
  }
  Storage.files["/.crosspoint/epub_other/progress.bin"] = {false, "other book"};
  Storage.files["/.crosspoint/bookmarks/novel.json"] = {false, "bookmarks"};
  Storage.files["/.crosspoint/clippings/novel.bin"] = {false, "clippings"};
}

void assertPreserved() {
  for (const char* name : {"progress.bin", "readerprefs.bin", "css_rules.cache", "language.bin", "pace.bin", "ruby.bin",
                           "wlscan.bin", "cover.bmp", "thumb_100.bmp", "img_notes.txt", "img_01_2.jpg"}) {
    assert(Storage.files.at(CACHE + '/' + name).bytes == name);
  }
  assert(Storage.files.at("/.crosspoint/epub_other/progress.bin").bytes == "other book");
  assert(Storage.files.at("/.crosspoint/bookmarks/novel.json").bytes == "bookmarks");
  assert(Storage.files.at("/.crosspoint/clippings/novel.bin").bytes == "clippings");
}

void assertDeleted() {
  for (const char* name :
       {"book.bin", "sections", "vsections", "html", "img_0_0.jpg", "img_0_0.pxc", "img_v1_2.PNG", "img_v1_2.pxc"})
    assert(!Storage.files.count(CACHE + '/' + name));
}

void checkNames() {
  for (const char* name : {"img_0_0.jpg", "img_1_234.jpeg", "img_v2_0.pxc", "img_v0_99.GIF", "img_0_1.bmp"}) {
    assert(hojicha::isContentImageName(name));
  }
  for (const char* name : {"img_notes.png", "img_0_.jpg", "img_-1_2.png", "img_00_1.jpg", "img_v_1_2.png",
                           "img_1_2.jpg.bak", "img_1_2.txt", "img_1_2/3.png", "cover.bmp"}) {
    assert(!hojicha::isContentImageName(name));
  }
  for (const char* path : {"/Books/Novel.epub", "/Books/Japanese space/book.epub", "/Novel.EPUB"}) {
    assert(hojicha::isSafeContentBookPath(path));
  }
  for (const char* path : {"Books/x.epub", "/Books/../x.epub", "/Books//x.epub", "/Books/./x.epub", "/Books\\x.epub",
                           "/Books /x.epub", "/BOO~1/x.epub", "/Books/?/x.epub"}) {
    assert(!hojicha::isSafeContentBookPath(path));
  }
}

void checkAdmission() {
  seed();
  assert(hojicha::checkContentUpload(BOOK, PART) == UploadCheck::StagingConflict);
  Storage.files.erase(PART);
  assert(hojicha::checkContentUpload(BOOK, PART) == UploadCheck::Ready);
  Storage.files.erase(BOOK);
  assert(hojicha::checkContentUpload(BOOK, PART) == UploadCheck::MissingBook);
  std::string failed;
  assert(hojicha::beginContentUpdate(BOOK, failed));
  assert(hojicha::checkContentUpload(BOOK, PART) == UploadCheck::Ready);
  Storage.files[PART] = {true, {}};
  assert(hojicha::checkContentUpload(BOOK, PART) == UploadCheck::StagingConflict);
  Storage.files[MARKER].bytes.back() ^= 1;
  assert(hojicha::updateState(BOOK) == UpdateState::Invalid);
  assert(hojicha::checkContentUpload(BOOK, PART) == UploadCheck::InvalidRecord);
  assert(hojicha::checkContentUpload("/Books/data.json", "/Books/data.json.part") == UploadCheck::InvalidPath);
}

void checkFatSpelling() {
  seed();
  Storage.caseInsensitive = true;
  Storage.files.erase(PART);
  for (const char* path : {"/books/Novel.epub", "/Books/novel.epub", "/Books/Novel.EPUB"}) {
    assert(Storage.exists(path));
    assert(hojicha::checkContentUpload(path, std::string(path) + ".part") == UploadCheck::InvalidPath);
  }
  assert(hojicha::checkContentUpload(BOOK, PART) == UploadCheck::Ready);
  Storage.files.erase(BOOK);
  std::string failed;
  const std::string alias = "/books/Novel.epub";
  assert(hojicha::beginContentUpdate(alias, failed));
  assert(hojicha::checkContentUpload(alias, alias + ".part") == UploadCheck::InvalidPath);
  assert(!hojicha::canRenamePath("/Books"));
  assert(!hojicha::canRenamePath("/books"));
  assert(!hojicha::canRenamePath(BOOK));
  assert(hojicha::canRenamePath("/Books/Other.epub"));
  assert(hojicha::canRenamePath("/Books2"));
  assertPreserved();
}

void checkSuccessAndRetry() {
  seed();
  std::string failed;
  assert(hojicha::installContentUpdate(BOOK, PART, failed));
  assert(Storage.files.at(BOOK).bytes == "new EPUB");
  assert(hojicha::updateState(BOOK) == UpdateState::Pending);
  assertDeleted();
  assertPreserved();
  Storage.failRemove = MARKER;
  assert(!hojicha::finishContentUpdate(BOOK, failed));
  assert(failed == MARKER);
  assert(hojicha::updateState(BOOK) == UpdateState::Pending);
  Storage.failRemove.clear();
  Storage.files[PART] = {false, "new EPUB"};
  assert(hojicha::installContentUpdate(BOOK, PART, failed));
  assert(hojicha::finishContentUpdate(BOOK, failed));
  assert(hojicha::updateState(BOOK) == UpdateState::None);
  assertPreserved();
}

void checkMarkerCreationFailures() {
  enum class Failure { HeaderWrite, PathWrite, Close, ReadOpen, HeaderRead, PathRead, ReadMismatch };
  for (const auto failure : {Failure::HeaderWrite, Failure::PathWrite, Failure::Close, Failure::ReadOpen,
                             Failure::HeaderRead, Failure::PathRead, Failure::ReadMismatch}) {
    for (const bool cleanupFails : {false, true}) {
      seed();
      const auto original = Storage.files;
      switch (failure) {
        case Failure::HeaderWrite:
          Storage.shortWriteNumber = 1;
          break;
        case Failure::PathWrite:
          Storage.shortWriteNumber = 2;
          break;
        case Failure::Close:
          Storage.failClose = MARKER;
          break;
        case Failure::ReadOpen:
          Storage.failReadOpen = MARKER;
          break;
        case Failure::HeaderRead:
          Storage.shortReadNumber = 1;
          break;
        case Failure::PathRead:
          Storage.shortReadNumber = 2;
          break;
        case Failure::ReadMismatch:
          Storage.corruptRead = true;
          break;
      }
      if (cleanupFails) Storage.failRemove = MARKER;
      std::string failed;
      assert(!hojicha::installContentUpdate(BOOK, PART, failed));
      assert(failed == MARKER);
      assert(Storage.removes == 1);
      assert(Storage.files.count(MARKER) == static_cast<size_t>(cleanupFails));
      for (const auto& [path, node] : original) {
        assert(Storage.files.at(path).directory == node.directory);
        assert(Storage.files.at(path).bytes == node.bytes);
      }
      Storage.shortWriteNumber = Storage.shortReadNumber = 0;
      Storage.failClose.clear();
      Storage.failReadOpen.clear();
      Storage.corruptRead = false;
      if (!cleanupFails) {
        assert(hojicha::updateState(BOOK) == UpdateState::None);
        assert(hojicha::installContentUpdate(BOOK, PART, failed));
        assert(hojicha::finishContentUpdate(BOOK, failed));
        assertDeleted();
        assertPreserved();
      }
    }
  }

  seed();
  std::string failed;
  assert(hojicha::beginContentUpdate(BOOK, failed));
  const std::string pending = Storage.files.at(MARKER).bytes;
  const int writes = Storage.writes;
  Storage.failWrite = true;
  Storage.failClose = MARKER;
  assert(hojicha::beginContentUpdate(BOOK, failed));
  assert(Storage.writes == writes && Storage.removes == 0);
  assert(Storage.files.at(MARKER).bytes == pending);
  Storage.failReadOpen = MARKER;
  assert(!hojicha::beginContentUpdate(BOOK, failed));
  assert(Storage.removes == 0 && Storage.files.at(MARKER).bytes == pending);
  Storage.failReadOpen.clear();
  Storage.files[MARKER].bytes = "damaged previous record";
  assert(!hojicha::beginContentUpdate(BOOK, failed));
  assert(Storage.removes == 0 && Storage.files.at(MARKER).bytes == "damaged previous record");
  assertPreserved();
}

void checkFailures() {
  std::string failed;
  for (int removeNumber = 1; removeNumber <= 9; ++removeNumber) {
    seed();
    Storage.failRemoveNumber = removeNumber;
    assert(!hojicha::installContentUpdate(BOOK, PART, failed));
    assert(hojicha::updateState(BOOK) == UpdateState::Pending);
    assertPreserved();
    Storage.failRemoveNumber = 0;
    Storage.files[PART] = {false, "new EPUB"};
    assert(hojicha::installContentUpdate(BOOK, PART, failed));
    assert(hojicha::finishContentUpdate(BOOK, failed));
    assertDeleted();
    assertPreserved();
  }
  seed();
  Storage.failRename = true;
  assert(!hojicha::installContentUpdate(BOOK, PART, failed));
  assert(!Storage.files.count(BOOK));
  assert(Storage.files.count(PART));
  assert(Storage.files.count(CACHE + "/book.bin"));
  assert(hojicha::checkContentUpload(BOOK, PART) == UploadCheck::Ready);
  Storage.failRename = false;
  assert(hojicha::installContentUpdate(BOOK, PART, failed));
  assert(hojicha::finishContentUpdate(BOOK, failed));
  assertPreserved();

  for (int failure = 0; failure < 3; ++failure) {
    seed();
    if (failure == 0) Storage.failWrite = true;
    if (failure == 1) Storage.failClose = MARKER;
    if (failure == 2) Storage.failMkdir = true;
    assert(!hojicha::installContentUpdate(BOOK, PART, failed));
    assert(Storage.files.at(BOOK).bytes == "old EPUB");
    assert(Storage.files.count(CACHE + "/book.bin"));
    assertPreserved();
  }
  seed();
  assert(hojicha::beginContentUpdate(BOOK, failed));
  Storage.failEnumeration = CACHE;
  assert(!hojicha::installContentUpdate(BOOK, PART, failed));
  assert(hojicha::updateState(BOOK) == UpdateState::Pending);
  Storage.failEnumeration.clear();
  Storage.failOpen = MARKER;
  assert(hojicha::updateState(BOOK) == UpdateState::Unavailable);
  Storage.failOpen.clear();
  Storage.failExists = MARKER;
  assert(hojicha::updateState(BOOK) == UpdateState::Pending);
  Storage.failEnumeration = hojicha::UPDATE_DIRECTORY;
  assert(hojicha::updateState(BOOK) == UpdateState::Unavailable);
}

int main() {
  checkNames();
  checkAdmission();
  checkFatSpelling();
  checkSuccessAndRetry();
  checkMarkerCreationFailures();
  checkFailures();
  std::cout << "Hojicha cache admission, preservation, interruption and retry checks passed\n";
}
