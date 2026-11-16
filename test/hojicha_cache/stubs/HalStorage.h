#pragma once

#include <strings.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

struct Node {
  bool directory = false;
  std::string bytes;
};

class HalFile;
class TestStorage {
 public:
  std::map<std::string, Node> files;
  std::string failOpen;
  std::string failRemove;
  std::string failEnumeration;
  std::string failExists;
  std::string failClose;
  bool failWrite = false;
  bool failMkdir = false;
  bool failRename = false;
  bool mounted = true;
  bool caseInsensitive = false;
  int failRemoveNumber = 0;
  int removes = 0;

  void reset() {
    *this = TestStorage{};
    files["/"] = {true, {}};
    files["/.crosspoint"] = {true, {}};
    files["/Books"] = {true, {}};
  }
  bool ready() const { return mounted; }
  std::string resolve(const char* path) const {
    for (const auto& [name, node] : files) {
      if (caseInsensitive ? strcasecmp(name.c_str(), path) == 0 : name == path) return name;
    }
    return {};
  }
  bool exists(const char* path) const { return path != failExists && !resolve(path).empty(); }
  bool ensureDirectoryExists(const char* path) {
    if (failMkdir) return false;
    files[path] = {true, {}};
    return true;
  }
  HalFile open(const char* path);
  bool openFileForRead(const char*, const char* path, HalFile& file);
  bool openFileForWrite(const char*, const char* path, HalFile& file);
  template <typename Text>
    requires requires(const Text& text) { text.c_str(); }
  bool openFileForWrite(const char* module, const Text& path, HalFile& file) {
    return openFileForWrite(module, path.c_str(), file);
  }
  bool remove(const char* path) {
    ++removes;
    if (path == failRemove || (failRemoveNumber && removes == failRemoveNumber)) return false;
    auto at = files.find(path);
    if (at == files.end() || at->second.directory) return false;
    files.erase(at);
    return true;
  }
  bool removeDir(const char* path) {
    if (path == failRemove || !files.count(path) || !files.at(path).directory) return false;
    const std::string prefix = std::string(path) + '/';
    std::vector<std::string> children;
    for (const auto& [name, node] : files) {
      if (name.starts_with(prefix) && !node.directory) children.push_back(name);
    }
    for (const auto& child : children) {
      if (!remove(child.c_str())) return false;
    }
    for (auto it = files.begin(); it != files.end();) {
      if (it->first == path || it->first.starts_with(prefix))
        it = files.erase(it);
      else
        ++it;
    }
    return true;
  }
  bool replaceFile(const char* from, const char* to) {
    if (exists(to) && !remove(to)) return false;
    if (failRename || !files.count(from)) return false;
    files[to] = files.at(from);
    files.erase(from);
    return true;
  }
};

inline TestStorage Storage;

class HalFile {
  std::string path_;
  size_t offset_ = 0;
  std::string lastEntry_;
  bool valid_ = false;

 public:
  HalFile() = default;
  explicit HalFile(std::string path) : path_(std::move(path)), valid_(true) {}
  HalFile(HalFile&&) = default;
  HalFile& operator=(HalFile&&) = default;
  explicit operator bool() const { return valid_; }
  bool isDirectory() const { return valid_ && Storage.files.at(path_).directory; }
  size_t size() const { return Storage.files.at(path_).bytes.size(); }
  int read(void* out, size_t count) {
    if (!valid_) return -1;
    const auto& bytes = Storage.files.at(path_).bytes;
    count = std::min(count, bytes.size() - offset_);
    memcpy(out, bytes.data() + offset_, count);
    offset_ += count;
    return static_cast<int>(count);
  }
  size_t write(const void* data, size_t count) {
    if (Storage.failWrite) return 0;
    Storage.files.at(path_).bytes.append(static_cast<const char*>(data), count);
    return count;
  }
  bool close() {
    const bool ok = valid_ && path_ != Storage.failClose;
    valid_ = false;
    return ok;
  }
  uint8_t getError() const { return path_ == Storage.failEnumeration ? 1 : 0; }
  size_t getName(char* out, size_t capacity) const {
    const auto name = path_.substr(path_.rfind('/') + 1);
    const size_t count = std::min(capacity - 1, name.size());
    memcpy(out, name.data(), count);
    out[count] = '\0';
    return count;
  }
  void rewindDirectory() { lastEntry_.clear(); }
  HalFile openNextFile() {
    if (getError()) return {};
    const std::string prefix = path_ == "/" ? "/" : path_ + '/';
    for (const auto& [name, node] : Storage.files) {
      if (name > lastEntry_ && name.starts_with(prefix) && name.size() > prefix.size() &&
          name.find('/', prefix.size()) == std::string::npos) {
        lastEntry_ = name;
        return HalFile(name);
      }
    }
    return {};
  }
};

inline HalFile TestStorage::open(const char* path) {
  const auto actual = resolve(path);
  return path != failOpen && !actual.empty() ? HalFile(actual) : HalFile{};
}
inline bool TestStorage::openFileForRead(const char*, const char* path, HalFile& file) {
  file = open(path);
  return static_cast<bool>(file);
}
inline bool TestStorage::openFileForWrite(const char*, const char* path, HalFile& file) {
  if (path == failOpen) return false;
  files[path] = {false, {}};
  file = HalFile(path);
  return true;
}
