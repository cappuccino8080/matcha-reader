#include "HojichaCacheRefresh.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <strings.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>

namespace hojicha {
namespace {
constexpr size_t GENERATED_PATH_BYTES = 80;
constexpr size_t MARKER_HEADER_BYTES = 10;
constexpr uint32_t HASH_BASIS = 2166136261u;
constexpr uint32_t HASH_PRIME = 16777619u;

enum class Presence { Absent, Present, Error };

// exists() alone cannot distinguish a missing entry from an SD read failure.
// All callers pass bounded, generated paths (never an EPUB filename).
Presence probeGeneratedPath(const char* path) {
  if (Storage.exists(path)) return Presence::Present;
  const char* slash = strrchr(path, '/');
  if (!slash || strcmp(path, "/") == 0) return Presence::Error;
  char parent[GENERATED_PATH_BYTES];
  const size_t count = slash == path ? 1 : static_cast<size_t>(slash - path);
  if (count >= sizeof(parent)) return Presence::Error;
  memcpy(parent, path, count);
  parent[count] = '\0';
  HalFile directory = Storage.open(parent);
  if (!directory) {
    // A missing updates/cache directory is normal; an unreadable one is not.
    if (strcmp(parent, "/") == 0) return Presence::Error;
    return probeGeneratedPath(parent) == Presence::Absent ? Presence::Absent : Presence::Error;
  }
  if (!directory.isDirectory()) return Presence::Error;
  while (true) {
    HalFile entry = directory.openNextFile();
    if (!entry) return directory.getError() ? Presence::Error : Presence::Absent;
    char name[64];
    const size_t length = entry.getName(name, sizeof(name));
    if (length == 0) return Presence::Error;
    if (length < sizeof(name) - 1 && strcasecmp(name, slash + 1) == 0) return Presence::Present;
  }
}

void markerPath(std::string_view bookPath, char* out, size_t capacity) {
  // The standard requires string and string_view hashes of equal text to agree.
  snprintf(out, capacity, "%s/%zu.pending", UPDATE_DIRECTORY, std::hash<std::string_view>{}(bookPath));
}

uint32_t pathChecksum(std::string_view path) {
  uint32_t hash = HASH_BASIS;
  for (const unsigned char byte : path) hash = (hash ^ byte) * HASH_PRIME;
  return hash;
}

void markerHeader(std::string_view path, uint8_t (&header)[MARKER_HEADER_BYTES]) {
  memcpy(header, "HCU1", 4);
  header[4] = static_cast<uint8_t>(path.size());
  header[5] = static_cast<uint8_t>(path.size() >> 8);
  const uint32_t checksum = pathChecksum(path);
  for (size_t i = 0; i < sizeof(checksum); ++i) header[6 + i] = static_cast<uint8_t>(checksum >> (8 * i));
}

bool fail(const char* path, std::string& failedPath) {
  failedPath = path;
  LOG_ERR("HCU", "Content update failed: %s", path);
  return false;
}

bool removeItem(const char* path, bool directory, std::string& failedPath) {
  const Presence presence = probeGeneratedPath(path);
  if (presence == Presence::Absent) return true;
  if (presence == Presence::Error) return fail(path, failedPath);
  const bool removed = directory ? Storage.removeDir(path) : Storage.remove(path);
  if (!removed) return fail(path, failedPath);
  return true;
}

UploadCheck checkPathSpelling(const std::string& bookPath, bool recovering) {
  // FAT lookup is case-insensitive, but the existing cache hash is not. Check every
  // component without rewriting the user's path or allocating a buffer per component.
  struct Scratch {
    char prefix[MAX_BOOK_PATH_BYTES + 1];
    char actual[MAX_BOOK_PATH_BYTES + 1];
  };
  auto scratch = makeUniqueNoThrow<Scratch>();
  if (!scratch) {
    LOG_ERR("HCU", "OOM: %zu bytes for path spelling check", sizeof(Scratch));
    return UploadCheck::StorageError;
  }
  memcpy(scratch->prefix, bookPath.c_str(), bookPath.size() + 1);
  for (size_t start = 1; start < bookPath.size();) {
    const size_t slash = bookPath.find('/', start);
    const size_t end = slash == std::string::npos ? bookPath.size() : slash;
    const bool final = end == bookPath.size();
    scratch->prefix[end] = '\0';
    HalFile entry = Storage.open(scratch->prefix);
    if (!entry) {
      if (!final || Storage.exists(scratch->prefix)) return UploadCheck::StorageError;
      return recovering ? UploadCheck::Ready : UploadCheck::MissingBook;
    }
    if (entry.isDirectory() == final) return UploadCheck::InvalidPath;
    const size_t length = entry.getName(scratch->actual, sizeof(scratch->actual));
    if (length == 0 || length >= sizeof(scratch->actual) - 1) return UploadCheck::StorageError;
    if (std::string_view(scratch->actual, length) != std::string_view(bookPath).substr(start, end - start)) {
      return UploadCheck::InvalidPath;
    }
    if (final) return UploadCheck::Ready;
    scratch->prefix[end] = '/';
    start = end + 1;
  }
  return UploadCheck::InvalidPath;
}

bool decimalPart(std::string_view& name, char delimiter) {
  const size_t end = name.find(delimiter);
  // The generators use nonnegative int indices with no leading zeroes.
  constexpr size_t MAX_INDEX_DIGITS = 10;
  if (end == 0 || end == std::string_view::npos || end > MAX_INDEX_DIGITS) return false;
  if (end > 1 && name[0] == '0') return false;
  unsigned int value = 0;
  for (size_t i = 0; i < end; ++i) {
    if (name[i] < '0' || name[i] > '9') return false;
    const unsigned int digit = name[i] - '0';
    if (value > (static_cast<unsigned int>(std::numeric_limits<int>::max()) - digit) / 10) return false;
    value = value * 10 + digit;
  }
  name.remove_prefix(end + 1);
  return true;
}
}  // namespace

bool isSafeContentBookPath(std::string_view path) {
  if (path.empty() || path.size() > MAX_BOOK_PATH_BYTES || path.front() != '/' || path.back() == '/') return false;
  path.remove_prefix(1);
  while (!path.empty()) {
    const size_t slash = path.find('/');
    const auto part = path.substr(0, slash);
    if (part.empty() || part == "." || part == ".." || part.front() == ' ' || part.back() == ' ' ||
        part.back() == '.') {
      return false;
    }
    for (const unsigned char c : part) {
      if (c < 32 || c == 127 || c == '\\' || c == ':' || c == '~' || c == '*' || c == '?' || c == '"' || c == '<' ||
          c == '>' || c == '|')
        return false;
    }
    if (slash == std::string_view::npos) return true;
    path.remove_prefix(slash + 1);
  }
  return false;
}

bool isContentImageName(std::string_view name) {
  if (!name.starts_with("img_")) return false;
  name.remove_prefix(4);
  if (name.starts_with('v')) name.remove_prefix(1);
  if (!decimalPart(name, '_') || !decimalPart(name, '.')) return false;
  char extension[5] = {};
  if (name.empty() || name.size() >= sizeof(extension)) return false;
  for (size_t i = 0; i < name.size(); ++i) {
    const char c = name[i];
    extension[i] = c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
  }
  return strcmp(extension, "jpg") == 0 || strcmp(extension, "jpeg") == 0 || strcmp(extension, "png") == 0 ||
         strcmp(extension, "bmp") == 0 || strcmp(extension, "gif") == 0 || strcmp(extension, "pxc") == 0;
}

UpdateState updateState(const std::string& bookPath) {
  if (!Storage.ready()) return UpdateState::Unavailable;
  char path[GENERATED_PATH_BYTES];
  markerPath(bookPath, path, sizeof(path));
  const Presence presence = probeGeneratedPath(path);
  if (presence == Presence::Absent) return UpdateState::None;
  if (presence == Presence::Error) return UpdateState::Unavailable;
  HalFile file;
  if (!Storage.openFileForRead("HCU", path, file)) return UpdateState::Unavailable;
  if (!isSafeContentBookPath(bookPath) || file.isDirectory() || file.size() != MARKER_HEADER_BYTES + bookPath.size()) {
    return UpdateState::Invalid;
  }
  uint8_t expected[MARKER_HEADER_BYTES];
  markerHeader(bookPath, expected);
  uint8_t actual[MARKER_HEADER_BYTES];
  if (file.read(actual, sizeof(actual)) != sizeof(actual) || memcmp(actual, expected, sizeof(actual)) != 0) {
    return UpdateState::Invalid;
  }
  // Compare in small chunks: no path-sized stack or heap buffer.
  char chunk[32];
  for (size_t offset = 0; offset < bookPath.size();) {
    const size_t count = std::min(sizeof(chunk), bookPath.size() - offset);
    if (file.read(chunk, count) != static_cast<int>(count) || memcmp(chunk, bookPath.data() + offset, count) != 0) {
      return UpdateState::Invalid;
    }
    offset += count;
  }
  return UpdateState::Pending;
}

bool beginContentUpdate(const std::string& bookPath, std::string& failedPath) {
  char path[GENERATED_PATH_BYTES];
  markerPath(bookPath, path, sizeof(path));
  if (!isSafeContentBookPath(bookPath)) return fail(path, failedPath);
  switch (updateState(bookPath)) {
    case UpdateState::Pending:
      return true;
    case UpdateState::Invalid:
    case UpdateState::Unavailable:
      return fail(path, failedPath);
    case UpdateState::None:
      break;
  }
  if (!Storage.ensureDirectoryExists(UPDATE_DIRECTORY)) return fail(UPDATE_DIRECTORY, failedPath);
  HalFile file;
  if (!Storage.openFileForWrite("HCU", path, file)) return fail(path, failedPath);
  uint8_t header[MARKER_HEADER_BYTES];
  markerHeader(bookPath, header);
  const bool written = file.write(header, sizeof(header)) == sizeof(header) &&
                       file.write(bookPath.data(), bookPath.size()) == bookPath.size();
  const bool closed = file.close();
  if (!written || !closed || updateState(bookPath) != UpdateState::Pending) return fail(path, failedPath);
  return true;
}

UploadCheck checkContentUpload(const std::string& bookPath, const std::string& stagedPath) {
  if (!isSafeContentBookPath(bookPath) || bookPath.size() < 5 ||
      strcasecmp(bookPath.c_str() + bookPath.size() - 5, ".epub") != 0 || stagedPath != bookPath + ".part") {
    return UploadCheck::InvalidPath;
  }
  const UpdateState state = updateState(bookPath);
  if (state == UpdateState::Invalid) return UploadCheck::InvalidRecord;
  if (state == UpdateState::Unavailable) return UploadCheck::StorageError;
  const UploadCheck spelling = checkPathSpelling(bookPath, state == UpdateState::Pending);
  if (spelling != UploadCheck::Ready) return spelling;
  if (Storage.exists(stagedPath.c_str())) {
    if (state != UpdateState::Pending) return UploadCheck::StagingConflict;
    HalFile staged = Storage.open(stagedPath.c_str());
    if (!staged) return UploadCheck::StorageError;
    if (staged.isDirectory()) return UploadCheck::StagingConflict;
  }
  return UploadCheck::Ready;
}

bool installContentUpdate(const std::string& bookPath, const std::string& stagedPath, std::string& failedPath,
                          void (*service)()) {
  if (!beginContentUpdate(bookPath, failedPath)) return false;
  if (!Storage.replaceFile(stagedPath.c_str(), bookPath.c_str())) return fail(bookPath.c_str(), failedPath);
  return invalidateBookContentCache(bookPath, failedPath, service);
}

bool invalidateBookContentCache(const std::string& bookPath, std::string& failedPath, void (*service)()) {
  char cache[GENERATED_PATH_BYTES];
  snprintf(cache, sizeof(cache), "/.crosspoint/epub_%zu", std::hash<std::string>{}(bookPath));
  if (updateState(bookPath) != UpdateState::Pending) return fail(cache, failedPath);
  const Presence presence = probeGeneratedPath(cache);
  if (presence == Presence::Absent) return true;
  if (presence == Presence::Error) return fail(cache, failedPath);
  // Fixed generated names keep both the stack and path allocation independent of book size.
  char path[GENERATED_PATH_BYTES];
  static constexpr const char* ITEMS[] = {"book.bin", "sections", "vsections", "html"};
  for (size_t i = 0; i < std::size(ITEMS); ++i) {
    if (service) service();
    snprintf(path, sizeof(path), "%s/%s", cache, ITEMS[i]);
    if (!removeItem(path, i != 0, failedPath)) return false;
  }
  HalFile directory = Storage.open(cache);
  if (!directory || !directory.isDirectory()) return fail(cache, failedPath);
  // FAT removes directory slots in place; keep the cursor after each removed entry.
  while (true) {
    if (service) service();
    HalFile entry = directory.openNextFile();
    if (!entry) {
      if (directory.getError()) return fail(cache, failedPath);
      break;
    }
    char name[40];
    const size_t count = entry.getName(name, sizeof(name));
    const bool image = count > 0 && count < sizeof(name) - 1 && !entry.isDirectory() && isContentImageName(name);
    if (count == 0 || !entry.close()) return fail(cache, failedPath);
    if (!image) continue;
    snprintf(path, sizeof(path), "%s/%s", cache, name);
    if (!removeItem(path, false, failedPath)) return false;
  }
  return true;
}

bool finishContentUpdate(const std::string& bookPath, std::string& failedPath) {
  char path[GENERATED_PATH_BYTES];
  markerPath(bookPath, path, sizeof(path));
  if (updateState(bookPath) != UpdateState::Pending) return fail(path, failedPath);
  return removeItem(path, false, failedPath);
}

bool discardContentUpdate(const std::string& bookPath) {
  char path[GENERATED_PATH_BYTES];
  markerPath(bookPath, path, sizeof(path));
  const Presence presence = probeGeneratedPath(path);
  if (presence == Presence::Absent) return true;
  if (presence == Presence::Error || !Storage.remove(path)) {
    LOG_ERR("HCU", "Cannot remove completed update record: %s", path);
    return false;
  }
  return true;
}

bool canRenamePath(const std::string& path) {
  const Presence presence = probeGeneratedPath(UPDATE_DIRECTORY);
  if (presence == Presence::Absent) return true;
  if (presence == Presence::Error) return false;
  HalFile directory = Storage.open(UPDATE_DIRECTORY);
  if (!directory || !directory.isDirectory()) return false;
  // One bounded allocation for all records, independent of the number of pending books.
  auto recorded = makeUniqueNoThrow<char[]>(MAX_BOOK_PATH_BYTES + 1);
  if (!recorded) {
    LOG_ERR("HCU", "OOM: pending path scan");
    return false;
  }
  std::string_view root = path;
  while (root.ends_with('/')) root.remove_suffix(1);
  while (true) {
    HalFile record = directory.openNextFile();
    if (!record) return directory.getError() == 0;
    if (record.isDirectory() || record.size() <= MARKER_HEADER_BYTES ||
        record.size() > MARKER_HEADER_BYTES + MAX_BOOK_PATH_BYTES)
      return false;
    uint8_t header[MARKER_HEADER_BYTES];
    if (record.read(header, sizeof(header)) != sizeof(header)) return false;
    const size_t length = record.size() - MARKER_HEADER_BYTES;
    if (record.read(recorded.get(), length) != static_cast<int>(length)) return false;
    recorded[length] = '\0';
    const std::string_view book(recorded.get(), length);
    uint8_t expected[MARKER_HEADER_BYTES];
    markerHeader(book, expected);
    if (!isSafeContentBookPath(book) || memcmp(header, expected, sizeof(header)) != 0) return false;
    char expectedPath[GENERATED_PATH_BYTES];
    markerPath(book, expectedPath, sizeof(expectedPath));
    char name[40];
    const size_t nameLength = record.getName(name, sizeof(name));
    if (nameLength == 0 || nameLength >= sizeof(name) - 1 || strcmp(name, strrchr(expectedPath, '/') + 1) != 0) {
      return false;
    }
    // Reject case aliases too: FAT can rename the same parent through another spelling.
    if (book.size() >= root.size() && strncasecmp(book.data(), root.data(), root.size()) == 0 &&
        (book.size() == root.size() || book[root.size()] == '/'))
      return false;
  }
}
}  // namespace hojicha
