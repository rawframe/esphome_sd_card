#include "sd_file_server.h"
#include <stdio.h>        // Added for fopen, fread
#include <sys/stat.h>     // Added for stat
#include "esphome/core/log.h"
#include "esphome/components/network/util.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace sd_file_server {

static const char *TAG = "sd_file_server";

SDFileServer::SDFileServer(web_server_base::WebServerBase *base) : base_(base) {}

void SDFileServer::setup() {
  this->base_->add_handler(this);
}

void SDFileServer::dump_config() {
  ESP_LOGCONFIG(TAG, "SD File Server:");
  ESP_LOGCONFIG(TAG, "  Address: %s:%u", network::get_use_address().c_str(), this->base_->get_port());
  ESP_LOGCONFIG(TAG, "  Url Prefix: %s", this->url_prefix_.c_str());
  ESP_LOGCONFIG(TAG, "  Root Path: %s", this->root_path_.c_str());
  ESP_LOGCONFIG(TAG, "  Deletation Enabled: %s", TRUEFALSE(this->deletion_enabled_));
  ESP_LOGCONFIG(TAG, "  Download Enabled : %s", TRUEFALSE(this->download_enabled_));
  ESP_LOGCONFIG(TAG, "  Upload Enabled   : %s", TRUEFALSE(this->upload_enabled_));
}

bool SDFileServer::canHandle(AsyncWebServerRequest *request) const {
  if (request->url().startsWith(this->build_prefix().c_str())) {
    ESP_LOGD(TAG, "Handling request: %s", request->url().c_str());
    return true;
  }
  return false;
}

void SDFileServer::handleRequest(AsyncWebServerRequest *request) {
  if (str_startswith(std::string(request->url().c_str()), this->build_prefix())) {
    if (request->method() == HTTP_GET) {
      this->handle_get(request);
      return;
    }
    if (request->method() == HTTP_DELETE) {
      this->handle_delete(request);
      return;
    }
  }
}

void SDFileServer::handleUpload(AsyncWebServerRequest *request, const String &filename,
                                size_t index, uint8_t *data, size_t len, bool final) {
  if (!this->upload_enabled_) {
    request->send(401, "application/json", "{ \"error\": \"file upload is disabled\" }");
    return;
  }

  std::string extracted = this->extract_path_from_url(std::string(request->url().c_str()));
  std::string path = this->build_absolute_path(extracted);

  if (index == 0 && !this->sd_mmc_card_->is_directory(path)) {
    auto response = request->beginResponse(401, "application/json",
                                           "{ \"error\": \"invalid upload folder\" }");
    response->addHeader("Connection", "close");
    request->send(response);
    return;
  }

  std::string file_name(filename.c_str());

  if (index == 0) {
    ESP_LOGD(TAG, "uploading file %s to %s", file_name.c_str(), path.c_str());
    this->sd_mmc_card_->write_file(Path::join(path, file_name).c_str(), data, len);
    return;
  }

  this->sd_mmc_card_->append_file(Path::join(path, file_name).c_str(), data, len);

  if (final) {
    auto response = request->beginResponse(201, "text/html", "upload success");
    response->addHeader("Connection", "close");
    request->send(response);
  }
}

void SDFileServer::handle_get(AsyncWebServerRequest *request) const {
  std::string extracted = this->extract_path_from_url(std::string(request->url().c_str()));
  std::string path = this->build_absolute_path(extracted);

  if (!this->sd_mmc_card_->is_directory(path)) {
    handle_download(request, path);
    return;
  }

  handle_index(request, path);
}

void SDFileServer::write_row(AsyncResponseStream *response,
                             sd_mmc_card::FileInfo const &info) const {
  std::string uri = "/" + Path::join(this->url_prefix_, Path::remove_root_path(info.path, this->root_path_));
  std::string file_name = Path::file_name(info.path);

  response->print("<tr><td>");

  if (info.is_directory) {
    response->print("<a href=\"");
    response->print(uri.c_str());
    response->print("\">");
    response->print(file_name.c_str());
    response->print("</a>");
  } else {
    response->print(file_name.c_str());
  }

  response->print("</td><td>");

  if (info.is_directory) {
    response->print("Folder");
  } else {
    response->print("<span class=\"file-type\">");
    response->print(Path::file_type(file_name).c_str());
    response->print("</span>");
  }

  response->print("</td><td>");

  if (!info.is_directory) {
    response->print(sd_mmc_card::format_size(info.size).c_str());
  }

  response->print("</td><td class=\"file-actions\">");

  if (!info.is_directory) {
    if (this->download_enabled_) {
      response->print("<button onClick=\"download_file('");
      response->print(uri.c_str());
      response->print("','");
      response->print(file_name.c_str());
      response->print("')\">Download</button>");
    }
    if (this->deletion_enabled_) {
      response->print("<button onClick=\"delete_file('");
      response->print(uri.c_str());
      response->print("')\">Delete</button>");
    }
  }

  response->print("</td></tr>");
}

void SDFileServer::handle_index(AsyncWebServerRequest *request, std::string const &path) const {
  AsyncResponseStream *response = request->beginResponseStream("text/html");

  response->print(F("<html lang=\"en\"><meta name=viewport content=\"width=device-width, initial-scale=1,user-scalable=no\"><title>SD Card Files</title>"));
  response->print(F("<style>body{font-family:'Segoe UI',sans-serif;margin:0;padding:2rem;background:#f5f5f7;}table{width:100%;border-collapse:collapse;}th,td{padding:12px;border-bottom:1px solid #e0e0e0;}button{cursor:pointer;padding:6px 12px;background:#0066cc;color:white;border:none;border-radius:4px;}</style>"));

  response->print(F("<h2>SD Card Files</h2><div class=\"breadcrumb\"><a href=\"/\">Home</a>"));

  std::string current_path = "/";
  std::string relative_path = Path::join(this->url_prefix_, Path::remove_root_path(path, this->root_path_));
  std::vector<std::string> parts = Path::split_path(relative_path);

  for (std::string const &part : parts) {
    if (!part.empty()) {
      current_path = Path::join(current_path, part);
      response->print(" > <a href=\"");
      response->print(current_path.c_str());
      response->print("\">");
      response->print(part.c_str());
      response->print("</a>");
    }
  }

  response->print("</div>");

  if (this->upload_enabled_) {
    response->print(F("<div class=\"upload-form\"><form method=\"POST\" enctype=\"multipart/form-data\"><input type=\"file\" name=\"file\"><input type=\"submit\" value=\"Upload\"></form></div>"));
  }

  auto entries = this->sd_mmc_card_->list_directory_file_info(path, 0);
  for (auto const &entry : entries) write_row(response, entry);

  response->print(F("<script>function delete_file(path){if(confirm('Delete?'))fetch(path,{method:\"DELETE\"}).then(()=>location.reload());}function download_file(path,filename){location.href=path;}</script>"));

  request->send(response);
}

// -------------------------------------------------------------------------
// REWRITTEN HANDLE_DOWNLOAD FOR ESP-IDF & SPEED
// -------------------------------------------------------------------------

void SDFileServer::handle_download(AsyncWebServerRequest *request, std::string const &path) const {
  if (!this->download_enabled_) {
    request->send(401, "application/json", "{ \"error\": \"file download is disabled\" }");
    return;
  }

  FILE *file = fopen(path.c_str(), "rb");
  if (!file) {
    ESP_LOGE(TAG, "Failed to open file for reading: %s", path.c_str());
    request->send(404, "text/plain", "File not found");
    return;
  }

  fseek(file, 0, SEEK_END);
  size_t file_size = ftell(file);
  fseek(file, 0, SEEK_SET);

  AsyncWebServerResponse *response = request->beginChunkedResponse(
      Path::mime_type(path).c_str(),
      [file](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
        size_t bytes_read = fread(buffer, 1, maxLen, file);
        if (bytes_read == 0) fclose(file);
        return bytes_read;
      });

  response->addHeader("Content-Length", String(file_size));
  request->send(response);
}

void SDFileServer::handle_delete(AsyncWebServerRequest *request) {
  if (!this->deletion_enabled_) {
    request->send(401, "application/json", "{ \"error\": \"file deletion is disabled\" }");
    return;
  }

  std::string extracted = this->extract_path_from_url(std::string(request->url().c_str()));
  std::string path = this->build_absolute_path(extracted);

  if (this->sd_mmc_card_->delete_file(path)) {
    request->send(200, "application/json", "{\"status\":\"deleted\"}");
    return;
  }

  request->send(400, "application/json", "{ \"error\": \"failed to delete file\" }");
}

std::string SDFileServer::build_prefix() const {
  if (this->url_prefix_.empty() || this->url_prefix_.at(0) != '/')
    return "/" + this->url_prefix_;
  return this->url_prefix_;
}

std::string SDFileServer::extract_path_from_url(std::string const &url) const {
  std::string prefix = this->build_prefix();
  return url.substr(prefix.size(), url.size() - prefix.size());
}

std::string SDFileServer::build_absolute_path(std::string relative_path) const {
  if (relative_path.empty()) return this->root_path_;
  return Path::join(this->root_path_, relative_path);
}

// ---------------- PATH HELPERS ----------------

std::string Path::file_name(std::string const &path) {
  size_t pos = path.rfind(Path::separator);
  if (pos != std::string::npos) return path.substr(pos + 1);
  return "";
}

bool Path::is_absolute(std::string const &path) {
  return path.size() && path[0] == separator;
}

bool Path::trailing_slash(std::string const &path) {
  return path.size() && path[path.length() - 1] == separator;
}

std::string Path::join(std::string const &first, std::string const &second) {
  std::string result = first;

  if (!trailing_slash(first) && !is_absolute(second)) {
    result.push_back(separator);
  }
  if (trailing_slash(first) && is_absolute(second)) {
    result.pop_back();
  }

  result.append(second);
  return result;
}

std::string Path::remove_root_path(std::string path, std::string const &root) {
  if (!str_startswith(path, root)) return path;
  if (path.size() == root.size() || path.size() < 2) return "/";
  return path.erase(0, root.size());
}

std::vector<std::string> Path::split_path(std::string path) {
  std::vector<std::string> parts;
  size_t pos = 0;

  while ((pos = path.find('/')) != std::string::npos) {
    std::string part = path.substr(0, pos);
    if (!part.empty()) parts.push_back(part);
    path.erase(0, pos + 1);
  }

  parts.push_back(path);
  return parts;
}

std::string Path::extension(std::string const &file) {
  size_t pos = file.find_last_of('.');
  if (pos == std::string::npos) return "";
  return file.substr(pos + 1);
}

std::string Path::file_type(std::string const &file) {
  static const std::map<std::string, std::string> file_types = {
      {"mp3", "Audio (MP3)"}, {"wav", "Audio (WAV)"}, {"png", "Image (PNG)"},
      {"jpg", "Image (JPG)"}, {"jpeg", "Image (JPEG)"}, {"bmp", "Image (BMP)"},
      {"txt", "Text (TXT)"}, {"log", "Text (LOG)"}, {"csv", "Text (CSV)"},
      {"html", "Web (HTML)"}, {"css", "Web (CSS)"}, {"js", "Web (JS)"},
      {"json", "Data (JSON)"}, {"xml", "Data (XML)"}, {"zip", "Archive (ZIP)"},
      {"gz", "Archive (GZ)"}, {"tar", "Archive (TAR)"}, {"mp4", "Video (MP4)"},
      {"avi", "Video (AVI)"}, {"webm", "Video (WEBM)"}};

  std::string ext = Path::extension(file);
  if (ext.empty()) return "File";

  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

  auto it = file_types.find(ext);
  if (it != file_types.end()) return it->second;

  return "File (" + ext + ")";
}

std::string Path::mime_type(std::string const &file) {
  static const std::map<std::string, std::string> file_types = {
      {"mp3", "audio/mpeg"}, {"wav", "audio/vnd.wav"}, {"png", "image/png"},
      {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"bmp", "image/bmp"},
      {"txt", "text/plain"}, {"log", "text/plain"}, {"csv", "text/csv"},
      {"html", "text/html"}, {"css", "text/css"}, {"js", "text/javascript"},
      {"json", "application/json"}, {"xml", "application/xml"},
      {"zip", "application/zip"}, {"gz", "application/gzip"},
      {"tar", "application/x-tar"}, {"mp4", "video/mp4"},
      {"avi", "video/x-msvideo"}, {"webm", "video/webm"}};

  std::string ext = Path::extension(file);
  if (!ext.empty()) {
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    auto it = file_types.find(ext);
    if (it != file_types.end()) return it->second;
  }

  return "application/octet-stream";
}

}  // namespace sd_file_server
}  // namespace esphome
