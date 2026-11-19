#pragma once

#include <string>
#include <string_view>

namespace hojicha {

inline constexpr char UPDATE_DIRECTORY[] = "/.crosspoint/hojicha-updates";
inline constexpr size_t MAX_BOOK_PATH_BYTES = 1023;

enum class UpdateState { None, Pending, Invalid, Unavailable };
enum class UploadCheck { Ready, InvalidPath, MissingBook, InvalidRecord, StagingConflict, StorageError };

// A pending record blocks cache reads until a complete retransmission succeeds.
UpdateState updateState(const std::string& bookPath);
UploadCheck checkContentUpload(const std::string& bookPath, const std::string& stagedPath);
bool installContentUpdate(const std::string& bookPath, const std::string& stagedPath, std::string& failedPath,
                          void (*service)() = nullptr);
bool beginContentUpdate(const std::string& bookPath, std::string& failedPath);
bool finishContentUpdate(const std::string& bookPath, std::string& failedPath);
bool invalidateBookContentCache(const std::string& bookPath, std::string& failedPath, void (*service)() = nullptr);
// A successful legacy full-cache clear also makes a pending record unnecessary.
bool discardContentUpdate(const std::string& bookPath);
// Checks records, not just existing EPUBs: replacement may have removed the formal file.
bool canRenamePath(const std::string& path);
bool isContentImageName(std::string_view name);
bool isSafeContentBookPath(std::string_view path);

}  // namespace hojicha
