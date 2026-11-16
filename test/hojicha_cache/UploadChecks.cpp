// Included after the real HTTP upload callback bodies by test_hojicha_cache.py.
const std::string BOOK = "/Books/Novel.epub";
const std::string CACHE = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(BOOK));

void seedUpload() {
  Storage.reset();
  Storage.files[BOOK] = {false, "old"};
  Storage.files[CACHE] = {true, {}};
  Storage.files[CACHE + "/book.bin"] = {false, "old metadata"};
  Storage.files[CACHE + "/progress.bin"] = {false, "saved position"};
  fullClears = 0;
  library::dirtyCalls = 0;
}

void sendUpload(CrossPointWebServer& web, WebServer& server) {
  server.data.status = UPLOAD_FILE_START;
  web.handleUpload(web.upload);
  server.data.status = UPLOAD_FILE_WRITE;
  server.data.buf = reinterpret_cast<const uint8_t*>("new content");
  server.data.currentSize = strlen("new content");
  web.handleUpload(web.upload);
  server.data.status = UPLOAD_FILE_END;
  web.handleUpload(web.upload);
  web.handleUploadPost(web.upload);
}

int main() {
  {
    seedUpload();
    WebServer server;
    CrossPointWebServer web{&server};
    sendUpload(web, server);
    assert(server.response == 200 && fullClears == 0);
    assert(Storage.files.count("/.crosspoint/library.dirty"));
    assert(Storage.files.at(BOOK).bytes == "new content");
    assert(Storage.files.at(CACHE + "/progress.bin").bytes == "saved position");
    assert(!Storage.files.count(CACHE + "/book.bin"));
    assert(hojicha::updateState(BOOK) == hojicha::UpdateState::None);
    // A POST with no multipart file must not repeat the previous success.
    web.handleUploadPost(web.upload);
    assert(server.response == 400);
  }
  for (int bad = 0; bad < 8; ++bad) {
    seedUpload();
    WebServer server;
    CrossPointWebServer web{&server};
    if (bad == 0) server.args["cache"] = "unknown";
    if (bad == 1) server.args["cache"] = "";
    if (bad == 2) Storage.files.erase(BOOK);
    if (bad == 3) server.data.filename = "data.json";
    if (bad == 4) server.args.erase("overwrite");
    if (bad == 5) server.args["path"] = "/Books/../Books";
    if (bad == 6) server.args["path"] = "/Books//sub";
    if (bad == 7) Storage.files[BOOK] = {true, {}};
    const auto original = Storage.files;
    sendUpload(web, server);
    assert(server.response == 400 && fullClears == 0);
    assert(Storage.files.size() == original.size());
    for (const auto& [name, node] : original) assert(Storage.files.at(name).bytes == node.bytes);
  }
  {
    seedUpload();
    WebServer server;
    CrossPointWebServer web{&server};
    server.args.erase("cache");
    sendUpload(web, server);
    assert(server.response == 200 && fullClears == 1);
    assert(!Storage.files.count(CACHE + "/progress.bin"));
    // Existing PUT completion keeps its full-clear behavior.
    seedUpload();
    WebDAVHandler dav;
    dav.handlePut(server);
    assert(server.response == 204 && fullClears == 1);
    assert(!Storage.files.count(CACHE + "/progress.bin"));
  }
  {
    seedUpload();
    WebServer server;
    CrossPointWebServer web{&server};
    Storage.failRename = true;
    sendUpload(web, server);
    assert(server.response == 500 && fullClears == 0);
    assert(!Storage.files.count(BOOK));
    assert(Storage.files.count(BOOK + ".part"));
    assert(Storage.files.count(CACHE + "/progress.bin"));
    Storage.failRename = false;
    sendUpload(web, server);
    assert(server.response == 200 && fullClears == 0);
    assert(Storage.files.at(BOOK).bytes == "new content");
  }
  {
    seedUpload();
    WebServer server;
    CrossPointWebServer web{&server};
    Storage.failRemove = CACHE + "/book.bin";
    sendUpload(web, server);
    assert(server.response == 500 && fullClears == 0);
    assert(hojicha::updateState(BOOK) == hojicha::UpdateState::Pending);
    assert(Storage.files.at(CACHE + "/progress.bin").bytes == "saved position");
  }
  {
    seedUpload();
    WebServer server;
    CrossPointWebServer web{&server};
    Storage.failClose = "/.crosspoint/library.dirty";
    sendUpload(web, server);
    assert(server.response == 500 && fullClears == 0);
    assert(hojicha::updateState(BOOK) == hojicha::UpdateState::Pending);
    Storage.failClose.clear();
    sendUpload(web, server);
    assert(server.response == 200);
    assert(Storage.files.at(CACHE + "/progress.bin").bytes == "saved position");
  }
  assert(protectedpaths::isSensitivePath("/.crosspoint/hojicha-updates/123.pending"));
  {
    seedUpload();
    std::string failed;
    assert(hojicha::beginContentUpdate(BOOK, failed));
    Storage.files.erase(BOOK);
    Storage.files[BOOK + ".part"] = {false, "new EPUB"};
    assert(!renamestate::renamePathWithState(BOOK, "/Books/Renamed.epub", "TEST"));
    assert(!renamestate::renamePathWithState("/Books", "/Renamed", "TEST"));
    assert(!renamestate::renamePathWithState("/books", "/Renamed", "TEST"));
    assert(bookwalk::walkCalls == 0);
    assert(renameMutations == 0);
    assert(Storage.files.at(CACHE + "/progress.bin").bytes == "saved position");
  }
  assert(protectedpaths::isSensitivePath("/.CROSSPOINT/HOJICHA-UPDATES/123.pending"));
  assert(protectedpaths::isSensitiveOrHoldsOne("/.crosspoint/hojicha-updates"));
  std::cout << "Production HTTP upload and WebDAV completion regression checks passed\n";
}
