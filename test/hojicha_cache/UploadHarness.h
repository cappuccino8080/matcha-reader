#pragma once
#include <HalStorage.h>
#include <HojichaCacheRefresh.h>
#include <Memory.h>

#include <cassert>
#include <functional>
#include <iostream>

#include "network/ProtectedPaths.h"

#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define tr(key) #key

class String : public std::string {
 public:
  using std::string::string;
  String(const std::string& value) : std::string(value) {}
  bool isEmpty() const { return empty(); }
  bool endsWith(const char* suffix) const { return ends_with(suffix); }
};

enum UploadStatus { UPLOAD_FILE_START, UPLOAD_FILE_WRITE, UPLOAD_FILE_END, UPLOAD_FILE_ABORTED };
struct HTTPUpload {
  UploadStatus status = UPLOAD_FILE_START;
  String filename = "Novel.epub";
  const uint8_t* buf = nullptr;
  size_t currentSize = 0;
};
struct Client {
  bool connected() const { return true; }
};
struct WebServer {
  HTTPUpload data;
  std::map<std::string, String> args{{"path", "/Books"}, {"overwrite", "1"}, {"cache", "content"}};
  int response = 0;
  String body;
  HTTPUpload& upload() { return data; }
  bool hasArg(const char* key) { return args.count(key); }
  String arg(const char* key) { return args[key]; }
  Client client() { return {}; }
  void send(int status, const char* = "", String text = "") {
    response = status;
    body = text;
  }
};
inline unsigned long millis() { return 1; }
inline void delay(unsigned long) {}
inline void resetTaskWatchdogIfSubscribed() {}
inline String normalizeWebPath(const String& path) { return path; }
inline bool pathHasProtectedComponent(const String&) { return false; }

namespace FsHelpers {
inline bool isSafePathComponent(const String& name) {
  return !name.empty() && name != "." && name != ".." && name.find_first_of("/\\") == std::string::npos;
}
inline bool hasEpubExtension(const String& name) {
  return name.size() >= 5 && strcasecmp(name.c_str() + name.size() - 5, ".epub") == 0;
}
}  // namespace FsHelpers
namespace library {
inline int dirtyCalls = 0;
inline bool markLibraryIndexDirty() {
  ++dirtyCalls;
  return true;
}
bool persistLibraryIndexDirty(std::string& failedPath);
inline bool isLibraryBookFile(const char*) { return true; }
}  // namespace library
inline int fullClears = 0;
inline void clearBookCache(const char* path) {
  ++fullClears;
  Storage.removeDir(("/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(path))).c_str());
  hojicha::discardContentUpdate(path);
}

class WebDAVHandler {
 public:
  bool _putOk = true;
  bool _putExisted = true;
  String getRequestPath(WebServer&) const { return "/Books/Novel.epub"; }
  bool isProtectedPath(const String&) const { return false; }
  void handlePut(WebServer& s);
};
