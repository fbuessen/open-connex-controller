#include "http_server.h"
#include "app.h"
#include "api.h"
#include "json_support.h"
#include "network.h"
#include "security.h"
#include "web_assets.h"
#include <WiFi.h>
#include <esp_http_server.h>
#include <esp_random.h>
#include <lwip/sockets.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>

namespace firmware {
namespace {
httpd_handle_t server = nullptr;
SemaphoreHandle_t sessionMutex = nullptr;
struct Session { int fd = -1; uint32_t deadline = 0; } sessions[2];
char nonce[33];
struct Deadline { uint32_t at; };

// HTTP only: documented transport hooks preserve the bundled HTTP parser while
// bounding the entire connection, not just gaps between successive bytes.
int transport(httpd_handle_t handle, int fd, void* buffer, size_t length, int flags, bool sending) {
  auto* deadline = static_cast<Deadline*>(httpd_sess_get_transport_ctx(handle, fd));
  const uint32_t now = millis();
  if (!deadline || connex::reached(now, deadline->at)) return HTTPD_SOCK_ERR_TIMEOUT;
  const uint32_t remaining = deadline->at - now;
  timeval timeout{static_cast<long>(remaining / 1000), static_cast<long>((remaining % 1000) * 1000)};
  if (setsockopt(fd, SOL_SOCKET, sending ? SO_SNDTIMEO : SO_RCVTIMEO, &timeout, sizeof timeout))
    return HTTPD_SOCK_ERR_FAIL;
  const int count = sending ? send(fd, buffer, length, flags) : recv(fd, buffer, length, flags);
  if (count < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT
                         ? HTTPD_SOCK_ERR_TIMEOUT : HTTPD_SOCK_ERR_FAIL;
  return count;
}
int receive(httpd_handle_t h, int fd, char* b, size_t n, int flags) { return transport(h, fd, b, n, flags, false); }
int transmit(httpd_handle_t h, int fd, const char* b, size_t n, int flags) { return transport(h, fd, const_cast<char*>(b), n, flags, true); }
esp_err_t openSession(httpd_handle_t h, int fd) {
  static uint32_t window = 0;
  static unsigned admitted = 0;
  const uint32_t now = millis();
  if (now - window >= 1000) { window = now; admitted = 0; }
  if (admitted >= 12) return ESP_FAIL;
  ++admitted;
  auto* deadline = static_cast<Deadline*>(malloc(sizeof(Deadline)));
  if (!deadline) return ESP_FAIL;
  deadline->at = millis() + 5000;
  xSemaphoreTake(sessionMutex, portMAX_DELAY);
  bool available = false;
  for (auto& s : sessions) if (s.fd < 0) { s.fd = fd; s.deadline = deadline->at; available = true; break; }
  xSemaphoreGive(sessionMutex);
  if (!available) { free(deadline); return ESP_FAIL; }
  httpd_sess_set_transport_ctx(h, fd, deadline, free);
  if (httpd_sess_set_recv_override(h, fd, receive) != ESP_OK ||
      httpd_sess_set_send_override(h, fd, transmit) != ESP_OK) return ESP_FAIL;
  return ESP_OK;
}
void closeSession(httpd_handle_t, int fd) {
  xSemaphoreTake(sessionMutex, portMAX_DELAY);
  for (auto& s : sessions) if (s.fd == fd) s.fd = -1;
  close(fd);
  xSemaphoreGive(sessionMutex);
}

// Only complete, validated password requests receive a compute allowance.
// Header/body intake and all other routes retain the five-second deadline.
bool allowPasswordWork(httpd_req_t* request) {
  const int fd = httpd_req_to_sockfd(request);
  auto* deadline = static_cast<Deadline*>(httpd_sess_get_transport_ctx(request->handle, fd));
  xSemaphoreTake(sessionMutex, portMAX_DELAY);
  const uint32_t now = millis(); bool allowed = false;
  for (auto& s : sessions) if (s.fd == fd && deadline && !connex::reached(now, s.deadline)) {
    deadline->at = s.deadline = now + 60000; allowed = true; break;
  }
  xSemaphoreGive(sessionMutex); return allowed;
}

const char* statusText(int status) {
  switch (status) {
    case 200: return "200 OK"; case 201: return "201 Created";
    case 202: return "202 Accepted"; case 204: return "204 No Content";
    case 302: return "302 Found"; case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 403: return "403 Forbidden"; case 404: return "404 Not Found";
    case 405: return "405 Method Not Allowed"; case 409: return "409 Conflict";
    case 413: return "413 Content Too Large"; case 415: return "415 Unsupported Media Type";
    case 422: return "422 Unprocessable Content"; case 429: return "429 Too Many Requests";
    case 503: return "503 Service Unavailable"; default: return "500 Internal Server Error";
  }
}
void headers(httpd_req_t* request, int status, const char* type) {
  httpd_resp_set_status(request, statusText(status));
  httpd_resp_set_type(request, type);
  httpd_resp_set_hdr(request, "Connection", "close");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(request, "Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
}
esp_err_t sendJson(httpd_req_t* request, int status, JsonDocument& value, const char* location = nullptr) {
  if (value.overflowed()) return ESP_FAIL;
  headers(request, status, "application/json");
  if (location && *location) httpd_resp_set_hdr(request, "Location", location);
  if (status == 202 || status == 204) { httpd_resp_send(request, nullptr, 0); return ESP_FAIL; }
  char body[4096];
  if (measureJson(value) >= sizeof body) return ESP_FAIL;
  const size_t length = serializeJson(value, body, sizeof body);
  httpd_resp_send(request, body, length);
  return ESP_FAIL; // One request per connection; close even on success.
}
esp_err_t error(httpd_req_t* request, int status, const char* message) {
  Json out; out.value["error"]["code"] = status == 401 ? "unauthorized" : status == 503 ? "unavailable" : "invalid_request";
  out.value["error"]["message"] = message;
  return sendJson(request, status, out.value);
}
esp_err_t asset(httpd_req_t* request, const char* body, const char* type) {
  headers(request, 200, type); httpd_resp_send(request, body, strlen(body)); return ESP_FAIL;
}
bool header(httpd_req_t* request, const char* key, char* value, size_t capacity) {
  const size_t length = httpd_req_get_hdr_value_len(request, key);
  if (!length || length >= capacity) return false;
  return httpd_req_get_hdr_value_str(request, key, value, capacity) == ESP_OK;
}
bool validHost(const char* host) {
  const String ip = WiFi.localIP().toString();
  return allowedHttpHost(host, ip.c_str());
}
struct AccessLock {
  bool locked = accessMutex && xSemaphoreTake(accessMutex, pdMS_TO_TICKS(100)) == pdTRUE;
  ~AccessLock() { if (locked) xSemaphoreGive(accessMutex); }
};
const char* stateName(connex::NetworkState state) {
  using connex::NetworkState;
  switch (state) {
    case NetworkState::Setup: return "setup"; case NetworkState::Trial: return "connecting";
    case NetworkState::Grace: return "saved"; case NetworkState::Configured: return "configured";
    case NetworkState::Editing: return "updating"; case NetworkState::Resetting: return "resetting";
    default: return "error";
  }
}

esp_err_t route(httpd_req_t* request) {
  const bool get = request->method == HTTP_GET;
  const bool post = request->method == HTTP_POST;
  const bool patch = request->method == HTTP_PATCH;
  const bool del = request->method == HTTP_DELETE;
  const char* path = request->uri;
  char host[64];
  if (!header(request, "Host", host, sizeof host) || !validHost(host)) {
    if (network.portal() && get && strncmp(path, "/api/", 5) && strncmp(path, "/ui/", 4)) {
      headers(request, 302, "text/plain"); httpd_resp_set_hdr(request, "Location", "http://192.168.4.1/");
      httpd_resp_send(request, nullptr, 0); return ESP_FAIL;
    }
    return error(request, 403, "Unexpected Host header");
  }
  if (!get) {
    char origin[96], expected[80];
    snprintf(expected, sizeof expected, "http://%s", host);
    if (httpd_req_get_hdr_value_len(request, "Origin") &&
        (!header(request, "Origin", origin, sizeof origin) || strcmp(origin, expected)))
      return error(request, 403, "Cross-origin changes are not permitted");
    char site[32];
    if (header(request, "Sec-Fetch-Site", site, sizeof site) && !strcmp(site, "cross-site"))
      return error(request, 403, "Cross-site changes are not permitted");
  }
  if (httpd_req_get_hdr_value_len(request, "Transfer-Encoding")) return error(request, 400, "Chunked requests are unsupported");
  if (request->content_len > 1024) return error(request, 413, "Request body exceeds 1024 bytes");
  if ((get || del) && request->content_len) return error(request, 400, "This request must not have a body");
  if (get && !strcmp(path, "/")) return asset(request, kIndex, "text/html; charset=utf-8");
  if (get && !strcmp(path, "/app.js")) return asset(request, kScript, "application/javascript");
  if (get && !strcmp(path, "/style.css")) return asset(request, kStyle, "text/css");
  if (get && !strcmp(path, "/favicon.svg")) return asset(request, kFavicon, "image/svg+xml");

  AccessLock security;
  if (!security.locked || !access.ready()) return error(request, 503, "Access storage unavailable; local recovery required");
  if (resetRequested && !get) return error(request, 409, "Setup reset is in progress");
  char cookies[512] = {}, session[65] = {}, authorization[96] = {};
  if (header(request, "Cookie", cookies, sizeof cookies)) connex::sessionCookie(cookies, session);
  const bool authenticated = access.session(session, millis());
  const bool publicApi = !strncmp(path, "/api/v1/", 8);
  const bool browserApi = !strncmp(path, "/ui/api/v1/", 11);
  const bool networkRoute = !strcmp(path, "/ui/network") || !strcmp(path, "/ui/networks") || !strcmp(path, "/ui/network/scan");
  const bool authEntry = !strcmp(path, "/ui/session") || !strcmp(path, "/ui/login") || !strcmp(path, "/ui/enroll");
  if (publicApi) {
    if (!header(request, "Authorization", authorization, sizeof authorization) || !access.bearer(authorization))
      return error(request, 401, "A valid bearer token is required");
  } else if (!authEntry && !(networkRoute && network.portal()) && !authenticated) {
    return error(request, 401, "Sign in to the controller");
  }
  if (!get && !publicApi) {
    char supplied[40] = {};
    if (!header(request, "X-Connex-Nonce", supplied, sizeof supplied) || strlen(supplied) != 32 || !connex::constantEqual(supplied, nonce, sizeof nonce))
      return error(request, 403, "Reload the page before submitting");
  }

  Json in, out;
  if (post || patch) {
    char type[64];
    if (!header(request, "Content-Type", type, sizeof type) || strncasecmp(type, "application/json", 16) ||
        (type[16] && type[16] != ';')) return error(request, 415, "Use application/json");
    char body[1025]; size_t used = 0;
    while (used < request->content_len) {
      const int count = httpd_req_recv(request, body + used, request->content_len - used);
      if (count <= 0) return ESP_FAIL;
      used += count;
    }
    body[used] = 0;
    const bool parsed = parseJson(in.value, body, used);
    memset(body, 0, sizeof body);
    if (!parsed) return error(request, 400, "Expected a bounded JSON object");
  }
  char setCookie[160] = {};
  if (get && !strcmp(path, "/ui/session")) {
    out.value["authenticated"] = authenticated; out.value["configured"] = access.configured();
    out.value["enrollment_allowed"] = access.enrollmentAllowed(network.portal());
    out.value["portal"] = network.portal(); out.value["nonce"] = nonce;
    return sendJson(request, 200, out.value);
  }
  if (post && (!strcmp(path, "/ui/login") || !strcmp(path, "/ui/enroll"))) {
    char password[129], issued[65];
    if (in.value.size() != 1 || !stringField(in.value["password"], password, sizeof password))
      return error(request, 422, "Supply a password of 10–128 bytes");
    if (hashBusy()) { connex::wipe(password, sizeof password); return error(request, 503, "Password worker is busy; try later"); }
    if (!allowPasswordWork(request)) { connex::wipe(password, sizeof password); return ESP_FAIL; }
    const int status = !strcmp(path, "/ui/login") ? access.login(password, millis(), issued)
        : access.enroll(password, network.portal(), millis(), issued);
    connex::wipe(password, sizeof password); in.value.clear();
    if (status != 200) return error(request, status, status == 429 ? "Too many attempts; wait a minute" : "Sign-in or password setup was not accepted");
    snprintf(setCookie, sizeof setCookie, "connex_session=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=%lu", issued,
             static_cast<unsigned long>(connex::kSessionSeconds));
    httpd_resp_set_hdr(request, "Set-Cookie", setCookie);
    out.value["authenticated"] = true; return sendJson(request, 200, out.value);
  }
  if (post && !strcmp(path, "/ui/logout")) {
    if (in.value.size()) return error(request, 422, "Expected an empty object");
    access.logout(session);
    httpd_resp_set_hdr(request, "Set-Cookie", "connex_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    return sendJson(request, 204, out.value);
  }
  if (post && !strcmp(path, "/ui/password")) {
    char current[129], replacement[129];
    if (in.value.size() != 2 || !stringField(in.value["current"], current, sizeof current) ||
        !stringField(in.value["password"], replacement, sizeof replacement)) return error(request, 422, "Supply current and new passwords");
    if (hashBusy()) { connex::wipe(current, sizeof current); connex::wipe(replacement, sizeof replacement); return error(request, 503, "Password worker is busy; try later"); }
    if (!allowPasswordWork(request)) { connex::wipe(current, sizeof current); connex::wipe(replacement, sizeof replacement); return ESP_FAIL; }
    const int status = access.password(current, replacement, millis());
    connex::wipe(current, sizeof current); connex::wipe(replacement, sizeof replacement); in.value.clear();
    if (status != 200) return error(request, status, "Password change was not accepted; check the current password or try later");
    httpd_resp_set_hdr(request, "Set-Cookie", "connex_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    return sendJson(request, 204, out.value);
  }
  if (!strcmp(path, "/ui/token")) {
    if (!get && !post && !del) return error(request, 405, "Method not allowed");
    if (post && in.value.size()) return error(request, 422, "Expected an empty object");
    const int status = post ? access.rotateToken() : del ? access.revokeToken() : 200;
    if (status != 200) return error(request, status, "Token update could not be verified; reload before retrying");
    out.value["token"] = access.token(); return sendJson(request, 200, out.value);
  }
  if (!strcmp(path, "/ui/icons")) {
    if (!get && !patch) return error(request, 405, "Method not allowed");
    UiPreferences preferences;
    if (storage.loadUi(preferences) == LoadResult::Error) return error(request, 503, "Icon storage unavailable");
    constexpr const char* icons[] = {"sofa", "bed", "desk", "kitchen", "bathroom", "hallway"};
    if (xSemaphoreTake(controlMutex, pdMS_TO_TICKS(100)) != pdTRUE) return error(request, 503, "Controller is busy");
    if (patch) {
      char id[32], icon[16]; uint64_t zoneId = 0; int selected = -1, slot = -1;
      const bool valid = in.value.size() == 2 && stringField(in.value["id"], id, sizeof id) &&
        connex::parseZoneId(id, zoneId) && stringField(in.value["icon"], icon, sizeof icon);
      if (valid) for (unsigned i = 0; i < 6; ++i) if (!strcmp(icon, icons[i])) selected = i;
      if (valid) for (size_t i = 0; i < connex::kZones; ++i)
        if (controller.snapshot().zones[i].active && controller.snapshot().zones[i].id == zoneId) slot = i;
      if (!valid || selected < 0 || slot < 0) { xSemaphoreGive(controlMutex); return error(request, 422, "Supply an existing zone and room icon"); }
      preferences.zones[slot] = {zoneId, static_cast<uint8_t>(selected)};
      if (storage.saveUi(preferences) != connex::SaveResult::Saved) {
        xSemaphoreGive(controlMutex); return error(request, 503, "Icon save could not be verified");
      }
    }
    auto list = out.value["icons"].to<JsonObject>();
    for (size_t i = 0; i < connex::kZones; ++i) {
      const auto& z = controller.snapshot().zones[i]; if (!z.active) continue;
      char id[32]; connex::formatId("zone_", z.id, id, sizeof id);
      list[id] = icons[preferences.zones[i].id == z.id ? preferences.zones[i].icon : 0];
    }
    xSemaphoreGive(controlMutex); return sendJson(request, 200, out.value);
  }
  if (publicApi || browserApi) {
    if (network.portal()) return error(request, 409, "Complete Wi-Fi setup before using heater controls");
    if (xSemaphoreTake(controlMutex, pdMS_TO_TICKS(100)) != pdTRUE) return error(request, 503, "Controller is busy");
    const ApiReply reply = api(controller, get ? "GET" : post ? "POST" : patch ? "PATCH" : del ? "DELETE" : "OTHER",
                               browserApi ? path + 3 : path, in.value.as<JsonVariantConst>(), out.value, millis(), networkHealthy.load());
    xSemaphoreGive(controlMutex);
    if (reply.uncertain) return ESP_FAIL;
    return sendJson(request, reply.status, out.value, reply.location);
  }
  if (get && !strcmp(path, "/ui/network")) {
    NetworkStatus status;
    if (!network.status(status)) return error(request, 503, "Network manager is busy");
    out.value["state"] = stateName(status.state); out.value["ssid"] = status.ssid;
    out.value["connected"] = status.connected; out.value["error"] = status.error;
    out.value["remaining_seconds"] = status.remaining; out.value["scanning"] = status.scanning;
    out.value["hostname"] = hostname; out.value["nonce"] = nonce;
    return sendJson(request, 200, out.value);
  }
  if (get && !strcmp(path, "/ui/networks")) {
    ScanEntry entries[12]; size_t count; bool scanning;
    if (!network.scanResults(entries, count, scanning)) return error(request, 503, "Network manager is busy");
    out.value["scanning"] = scanning;
    auto list = out.value["networks"].to<JsonArray>();
    for (size_t i = 0; i < count; ++i) {
      auto entry = list.add<JsonObject>(); entry["ssid"] = entries[i].ssid;
      entry["rssi"] = entries[i].rssi; entry["secured"] = entries[i].secured;
    }
    return sendJson(request, 200, out.value);
  }
  if (post && (!strcmp(path, "/ui/network") || !strcmp(path, "/ui/network/scan"))) {
    if (!strcmp(path, "/ui/network/scan")) {
      if (in.value.size()) return error(request, 422, "Scan request must be empty");
      const int status = network.requestScan();
      return status == 202 ? sendJson(request, status, out.value) : error(request, status, "Scanning is busy or unavailable");
    }
    WifiCredentials candidate;
    if (!access.configured()) return error(request, 409, "Choose a web password before connecting Wi-Fi");
    if (in.value.size() != 2 || !stringField(in.value["ssid"], candidate.ssid, sizeof candidate.ssid) ||
        !candidate.ssid[0] || !stringField(in.value["password"], candidate.password, sizeof candidate.password))
      return error(request, 422, "Supply an SSID (1–32 bytes) and password");
    const size_t length = strlen(candidate.password);
    if (length && length < 8) return error(request, 422, "Wi-Fi password must be empty or at least 8 bytes");
    if (length == 64) for (const char* p = candidate.password; *p; ++p)
      if (!isxdigit(static_cast<unsigned char>(*p))) return error(request, 422, "A 64-byte Wi-Fi key must be hexadecimal");
    uint32_t generation = 0;
    const int status = network.submit(candidate, generation);
    memset(candidate.password, 0, sizeof candidate.password);
    if (status != 202) return error(request, status, "A connection attempt, scan, or reset is already active");
    const auto response = sendJson(request, status, out.value);
    network.releaseSubmission(generation);
    return response;
  }
  if (get && network.portal()) {
    headers(request, 302, "text/plain"); httpd_resp_set_hdr(request, "Location", "http://192.168.4.1/");
    httpd_resp_send(request, nullptr, 0); return ESP_FAIL;
  }
  return error(request, 404, "Resource not found");
}
}
bool startHttp() {
  sessionMutex = xSemaphoreCreateMutex();
  if (!sessionMutex) return false;
  uint8_t random[16]; esp_fill_random(random, sizeof random);
  for (size_t i = 0; i < sizeof random; ++i) snprintf(nonce + 2 * i, 3, "%02x", random[i]);
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 16384; config.max_open_sockets = 2; config.max_uri_handlers = 1;
  config.core_id = 0; config.task_priority = 2;
  config.max_resp_headers = 8; config.max_req_hdr_len = 1024; config.max_uri_len = 128;
  config.backlog_conn = 2; config.lru_purge_enable = true;
  config.recv_wait_timeout = 1; config.send_wait_timeout = 1;
  config.open_fn = openSession; config.close_fn = closeSession;
  config.uri_match_fn = httpd_uri_match_wildcard;
  if (httpd_start(&server, &config) != ESP_OK) return false;
  httpd_uri_t handler{}; handler.uri = "/*"; handler.method = static_cast<httpd_method_t>(HTTP_ANY); handler.handler = route;
  if (httpd_register_uri_handler(server, &handler) != ESP_OK) { httpd_stop(server); server = nullptr; return false; }
  return true;
}
void pollHttpDeadlines() {
  if (!server || xSemaphoreTake(sessionMutex, 0) != pdTRUE) return;
  for (auto& s : sessions) if (s.fd >= 0 && connex::reached(millis(), s.deadline))
    shutdown(s.fd, SHUT_RDWR); // Wake idle/slow sessions; HTTP task remains the sole closer.
  xSemaphoreGive(sessionMutex);
}
}
