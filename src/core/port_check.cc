#include "config.h"

#include "core/port_check.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <ctime>
#include <memory>
#include <regex>
#include <sstream>
#include <utility>

#include <curl/curl.h>
#include <torrent/common.h>
#include <torrent/exceptions.h>
#include <torrent/runtime/network_manager.h>
#include <torrent/utils/log.h>

#include "globals.h"
#include "rpc/parse_commands.h"

namespace core {

namespace {

std::once_flag g_curl_init_flag;

struct CurlHandle {
  CurlHandle() = default;

  CURL* handle{curl_easy_init()};

  ~CurlHandle() {
    if (handle != nullptr)
      curl_easy_cleanup(handle);
  }

  CurlHandle(const CurlHandle&) = delete;
  CurlHandle& operator=(const CurlHandle&) = delete;
};

struct CurlHeaders {
  CurlHeaders() = default;

  curl_slist* headers{};

  ~CurlHeaders() {
    if (headers != nullptr)
      curl_slist_free_all(headers);
  }

  void append(const char* value) {
    headers = curl_slist_append(headers, value);
  }
};

size_t
receive_write(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

bool
is_wildcard_ip(const std::string& value) {
  return value.empty() || value == "0.0.0.0" || value == "::" || value == "::0";
}

std::string
trim(std::string value) {
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), [](unsigned char c) {
                return !std::isspace(c);
              }));
  value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char c) {
                return !std::isspace(c);
              }).base(),
              value.end());
  return value;
}

bool
perform_request(
    CURL* handle,
    const std::string& url,
    const std::string& body,
    long timeout_seconds,
    bool use_cookie_jar,
    const char* content_type,
    std::string* response,
    std::string* error) {
  response->clear();
  curl_easy_reset(handle);

  curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &receive_write);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, response);
  curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(handle, CURLOPT_TIMEOUT, timeout_seconds);
  curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, timeout_seconds);
  curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(handle, CURLOPT_USERAGENT, "rTorrent port-check");

  if (use_cookie_jar)
    curl_easy_setopt(handle, CURLOPT_COOKIEFILE, "");

  CurlHeaders headers;

  if (!body.empty()) {
    curl_easy_setopt(handle, CURLOPT_POST, 1L);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, body.size());

    if (content_type != nullptr) {
      headers.append(content_type);
      curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers.headers);
    }
  }

  auto code = curl_easy_perform(handle);

  if (code != CURLE_OK) {
    *error = curl_easy_strerror(code);
    return false;
  }

  long status = 0;
  curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);

  if (status != 200) {
    *error = "HTTP " + std::to_string(status);
    return false;
  }

  return true;
}

std::string
url_encode(CURL* handle, const std::string& value) {
  char* escaped = curl_easy_escape(handle, value.c_str(), value.size());

  if (escaped == nullptr)
    throw torrent::internal_error("PortCheck failed to URL encode request data.");

  std::string result = escaped;
  curl_free(escaped);
  return result;
}

std::string
extract_regex(const std::string& value, const std::regex& pattern) {
  std::smatch match;

  if (!std::regex_search(value, match, pattern) || match.size() < 2)
    return std::string();

  return trim(match[1].str());
}

std::string
resolve_public_ip(
    CURL* handle,
    core::PortCheck::Provider provider,
    long timeout_seconds,
    std::string* error,
    std::string* landing_page) {
  std::string page_url;
  std::regex  ip_pattern;

  if (provider == core::PortCheck::Provider::yougetsignal) {
    page_url = "https://www.yougetsignal.com/tools/open-ports/";
    ip_pattern = std::regex(
        R"(<p style=\"font-size: 1\.4em;\">([^<]+))",
        std::regex::icase);
  } else {
    page_url = "https://portchecker.co/";
    ip_pattern = std::regex(R"(data-ip=\"([^\"]+))", std::regex::icase);
  }

  if (!perform_request(
          handle,
          page_url,
          std::string(),
          timeout_seconds,
          true,
          nullptr,
          landing_page,
          error))
    return std::string();

  return extract_regex(*landing_page, ip_pattern);
}

core::PortCheck::WorkerResult
make_error_result(
    core::PortCheck::Provider provider,
    int64_t port,
    std::string ip,
    const std::string& error) {
  core::PortCheck::WorkerResult result;
  result.provider = provider;
  result.status = core::PortCheck::Status::error;
  result.checked_at = std::time(nullptr);
  result.last_error = error;
  result.ip = std::move(ip);
  result.port = port;
  return result;
}

} // namespace

PortCheck::PortCheck() {
  m_timeout.slot() = std::bind(&PortCheck::on_timeout, this);
}

PortCheck::~PortCheck() {
  stop();
}

std::string
PortCheck::provider_name() const {
  return provider_to_string(m_provider);
}

void
PortCheck::set_provider(const std::string& value) {
  auto provider = parse_provider(value);

  stop();
  m_provider = provider;
  reset_cached_state();
  m_stopRequested = false;

  if (m_provider != Provider::off)
    start();
}

void
PortCheck::set_interval_seconds(int64_t value) {
  if (value <= 0)
    throw torrent::input_error("Port check interval must be greater than zero.");

  m_interval = std::chrono::seconds(value);

  if (is_active())
    restart();
}

std::string
PortCheck::status_name() const {
  return status_to_string(m_status);
}

void
PortCheck::start() {
  if (m_provider == Provider::off)
    return;

  torrent::this_thread::scheduler()->erase(&m_timeout);
  schedule_after(std::chrono::seconds(1));
}

void
PortCheck::stop() {
  torrent::this_thread::scheduler()->erase(&m_timeout);
  torrent::main_thread::cancel_callback(this);

  m_stopRequested = true;

  if (m_worker.joinable())
    m_worker.join();

  m_inFlight = false;
}

void
PortCheck::restart() {
  stop();
  m_stopRequested = false;

  if (m_provider != Provider::off)
    start();
}

void
PortCheck::reset_cached_state() {
  m_status = Status::unknown;
  m_lastCheckAt = 0;
  m_lastError.clear();
  m_lastIp.clear();
  m_lastPort = 0;
}

void
PortCheck::schedule_after(std::chrono::seconds delay) {
  torrent::this_thread::scheduler()->wait_for_ceil_seconds(&m_timeout, delay);
}

void
PortCheck::on_timeout() {
  launch_check();
}

void
PortCheck::launch_check() {
  if (m_provider == Provider::off)
    return;

  if (m_inFlight) {
    schedule_after(m_interval);
    return;
  }

  if (m_worker.joinable())
    m_worker.join();

  WorkerInput input;
  input.provider = m_provider;
  input.port = torrent::runtime::listen_port();
  input.timeout_seconds = std::clamp<int64_t>(m_interval.count(), 5, 120);

  auto local_address = rpc::call_command_string("network.local_address");
  if (!is_wildcard_ip(local_address))
    input.explicit_ip = local_address;

  m_inFlight = true;

  m_worker = std::thread([this, input]() {
    auto result = perform_check(input);

    if (m_stopRequested)
      return;

    torrent::main_thread::callback(this, [this, result = std::move(result)]() mutable {
      finish_check(std::move(result));
    });
  });
}

void
PortCheck::finish_check(WorkerResult result) {
  m_inFlight = false;

  if (result.provider != m_provider || m_stopRequested)
    return;

  m_status = result.status;
  m_lastCheckAt = result.checked_at;
  m_lastError = std::move(result.last_error);
  m_lastIp = std::move(result.ip);
  m_lastPort = result.port;

  schedule_after(m_interval);
}

PortCheck::Provider
PortCheck::parse_provider(const std::string& value) {
  if (value == "off")
    return Provider::off;
  if (value == "yougetsignal")
    return Provider::yougetsignal;
  if (value == "portchecker")
    return Provider::portchecker;

  throw torrent::input_error(
      "Invalid port-check provider. Use 'off', 'yougetsignal', or 'portchecker'.");
}

std::string
PortCheck::provider_to_string(Provider provider) {
  switch (provider) {
  case Provider::off:
    return "off";
  case Provider::yougetsignal:
    return "yougetsignal";
  case Provider::portchecker:
    return "portchecker";
  }

  return "off";
}

std::string
PortCheck::status_to_string(Status status) {
  switch (status) {
  case Status::unknown:
    return "unknown";
  case Status::open:
    return "open";
  case Status::closed:
    return "closed";
  case Status::error:
    return "error";
  }

  return "unknown";
}

PortCheck::WorkerResult
PortCheck::perform_check(const WorkerInput& input) {
  if (input.provider == Provider::off)
    return WorkerResult();

  std::call_once(g_curl_init_flag, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });

  if (input.port <= 0)
    return make_error_result(
        input.provider,
        input.port,
        std::string(),
        "listening port unavailable");

  CurlHandle handle;
  if (handle.handle == nullptr)
    return make_error_result(
        input.provider,
        input.port,
        std::string(),
        "could not initialize curl");

  std::string error;
  std::string page_body;
  std::string ip = input.explicit_ip;

  if (ip.empty()) {
    ip = resolve_public_ip(
        handle.handle,
        input.provider,
        input.timeout_seconds,
        &error,
        &page_body);

    if (ip.empty())
      return make_error_result(
          input.provider,
          input.port,
          std::string(),
          error.empty() ? "could not resolve public ip" : error);
  }

  if (input.provider == Provider::portchecker) {
    if (!perform_request(
            handle.handle,
            "https://portchecker.co/check-it",
            std::string(),
            input.timeout_seconds,
            true,
            nullptr,
            &page_body,
            &error))
      return make_error_result(input.provider, input.port, ip, error);
  }

  std::string request_body;
  std::string response;

  if (input.provider == Provider::yougetsignal) {
    request_body =
        "remoteAddress=" + url_encode(handle.handle, ip) +
        "&portNumber=" + std::to_string(input.port);

    if (!perform_request(
            handle.handle,
            "https://ports.yougetsignal.com/check-port.php",
            request_body,
            input.timeout_seconds,
            true,
            "Content-Type: application/x-www-form-urlencoded",
            &response,
            &error))
      return make_error_result(input.provider, input.port, ip, error);

    WorkerResult result;
    result.provider = input.provider;
    result.checked_at = std::time(nullptr);
    result.ip = ip;
    result.port = input.port;

    if (response.find("closed") != std::string::npos)
      result.status = Status::closed;
    else if (response.find("open") != std::string::npos)
      result.status = Status::open;
    else
      return make_error_result(input.provider, input.port, ip, "could not parse provider response");

    return result;
  }

  auto csrf = extract_regex(
      page_body,
      std::regex(R"(name=\"_csrf\" value=\"([^\"]+))", std::regex::icase));

  if (csrf.empty())
    return make_error_result(input.provider, input.port, ip, "could not parse csrf token");

  request_body =
      "target_ip=" + url_encode(handle.handle, ip) +
      "&port=" + std::to_string(input.port) +
      "&_csrf=" + url_encode(handle.handle, csrf);

  if (!perform_request(
          handle.handle,
          "https://portchecker.co/check-it",
          request_body,
          input.timeout_seconds,
          true,
          "Content-Type: application/x-www-form-urlencoded",
          &response,
          &error))
    return make_error_result(input.provider, input.port, ip, error);

  WorkerResult result;
  result.provider = input.provider;
  result.checked_at = std::time(nullptr);
  result.ip = ip;
  result.port = input.port;

  if (response.find(">closed<") != std::string::npos)
    result.status = Status::closed;
  else if (response.find(">open<") != std::string::npos)
    result.status = Status::open;
  else
    return make_error_result(input.provider, input.port, ip, "could not parse provider response");

  return result;
}

} // namespace core
