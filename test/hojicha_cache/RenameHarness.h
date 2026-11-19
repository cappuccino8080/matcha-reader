#pragma once

namespace bookwalk {
inline int walkCalls = 0;
template <typename Callback>
void forEachBook(const std::string&, int, Callback callback) {
  ++walkCalls;
  callback(std::string("/Books/Novel.epub"));
}
}  // namespace bookwalk
namespace BookmarkUtil {
inline std::string getBookmarkPath(const std::string& path) { return path + ".bookmarks"; }
}  // namespace BookmarkUtil
namespace FsHelpers {
inline bool hasEpubExtension(std::string_view name) { return name.ends_with(".epub"); }
inline bool hasReflowableBookExtension(const std::string& name) { return name.ends_with(".epub"); }
}  // namespace FsHelpers
inline int renameMutations = 0;
struct ClippingStore {
  static bool moveBook(const std::string&, const std::string&) {
    ++renameMutations;
    return true;
  }
};
struct RecentBooksStub {
  void updatePath(const std::string&, const std::string&, const std::string&, const std::string&) {}
};
inline RecentBooksStub RECENT_BOOKS;
namespace renamestate {
inline std::string rebase(const std::string& book, const std::string& from, const std::string& to) {
  return to + book.substr(from.size());
}
inline std::string bookCachePath(const std::string& path) { return path + ".cache"; }
inline bool moveState(const std::string&, const std::string&, bool& moved, const char*) {
  ++renameMutations;
  moved = true;
  return true;
}
inline void moveStateBack(const std::string&, const std::string&, const char*) {}
bool renamePathWithState(const std::string&, const std::string&, const char*);
}  // namespace renamestate
