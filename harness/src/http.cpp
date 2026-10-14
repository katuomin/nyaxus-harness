#include "nyaxus/http.hpp"
#include "nyaxus/log.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <future>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t_ = int;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
using socklen_t_ = socklen_t;
#endif

namespace nyaxus {
namespace http {

// ── Sockets ────────────────────────────────────────────────────────────────

SocketHandle invalid_socket() {
#if defined(_WIN32)
    return INVALID_SOCKET;
#else
    return -1;
#endif
}

bool socket_valid(SocketHandle handle) {
#if defined(_WIN32)
    return handle != INVALID_SOCKET;
#else
    return handle >= 0;
#endif
}

bool socket_init() {
#if defined(_WIN32)
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return true;
#endif
}

void socket_cleanup() {
#if defined(_WIN32)
    WSACleanup();
#endif
}

void socket_close(SocketHandle handle) {
    if (!socket_valid(handle)) return;
#if defined(_WIN32)
    closesocket(handle);
#else
    close(handle);
#endif
}

// True when the last socket_read failed because of the read timeout (no data
// yet) rather than a real error: the caller can keep waiting.
static bool socket_read_timed_out() {
#if defined(_WIN32)
    return WSAGetLastError() == WSAETIMEDOUT;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

static bool socket_set_read_timeout(SocketHandle handle, long long timeout_ms) {
    if (timeout_ms <= 0) return true;
#if defined(_WIN32)
    const DWORD timeout = static_cast<DWORD>(timeout_ms);
    return setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO,
                      reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0;
#else
    struct timeval tv;
    tv.tv_sec = static_cast<long>(timeout_ms / 1000);
    tv.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
    return setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

static void socket_set_reuseaddr(SocketHandle handle) {
    int one = 1;
#if defined(_WIN32)
    setsockopt(handle, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&one), sizeof(one));
#else
    setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
}

// ── sockets: resolve + connect (sync, with the exceptfds refusal fix) ───────
namespace {

}  // namespace

SocketHandle socket_connect(const std::string& host, int port, long long int timeout_ms) {
    // Sync dial (the structure that ran in production for months): resolve,
    // one non-blocking connect per resolved address with a select window.
    // The 2026-10-03 exceptfds fix matters here: a refused peer completes
    // via the exception set in ~2 s on Windows instead of waiting out the
    // whole timeout per address.
    const auto dial_started = std::chrono::steady_clock::now();
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    const std::string port_string = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_string.c_str(), &hints, &result) != 0 ||
        !result) {
        log::warn("socket_connect resolve failed host=" + host);
        return invalid_socket();
    }

    SocketHandle handle = invalid_socket();
    for (addrinfo* addr = result; addr; addr = addr->ai_next) {
        handle = static_cast<SocketHandle>(::socket(addr->ai_family,
                                                    addr->ai_socktype,
                                                    addr->ai_protocol));
        if (!socket_valid(handle)) continue;

#if defined(_WIN32)
        u_long mode = 1;
        ioctlsocket(handle, FIONBIO, &mode);
#else
        int flags = fcntl(handle, F_GETFL, 0);
        fcntl(handle, F_SETFL, flags | O_NONBLOCK);
#endif

        const int connect_result =
            ::connect(handle, addr->ai_addr, static_cast<int>(addr->ai_addrlen));
        if (connect_result == 0) break;

#if defined(_WIN32)
        const int err = WSAGetLastError();
        const bool in_progress = (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS);
#else
        const bool in_progress = (errno == EINPROGRESS || errno == EWOULDBLOCK);
#endif
        if (in_progress) {
            fd_set write_fds;
            fd_set except_fds;
            FD_ZERO(&write_fds);
            FD_SET(handle, &write_fds);
            FD_ZERO(&except_fds);
            FD_SET(handle, &except_fds);
            timeval tv;
            tv.tv_sec = static_cast<long>(timeout_ms / 1000);
            tv.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
            const int ready = select(static_cast<int>(handle) + 1, nullptr,
                                     &write_fds, &except_fds, &tv);
            if (ready > 0) {
                int so_error = 0;
                socklen_t_ len = sizeof(so_error);
#if defined(_WIN32)
                getsockopt(handle, SOL_SOCKET, SO_ERROR,
                           reinterpret_cast<char*>(&so_error), &len);
#else
                getsockopt(handle, SOL_SOCKET, SO_ERROR, &so_error, &len);
#endif
                if (so_error == 0) break;
            }
        }

        socket_close(handle);
        handle = invalid_socket();
    }

    freeaddrinfo(result);

    if (socket_valid(handle)) {
#if defined(_WIN32)
        u_long mode = 0;
        ioctlsocket(handle, FIONBIO, &mode);
#else
        int flags = fcntl(handle, F_GETFL, 0);
        fcntl(handle, F_SETFL, flags & ~O_NONBLOCK);
#endif
    }
    return handle;
}

bool socket_write_all(SocketHandle handle, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
#if defined(_WIN32)
        const int written =
            ::send(handle, data.data() + sent, static_cast<int>(data.size() - sent), 0);
#else
        const ssize_t written =
            ::send(handle, data.data() + sent, data.size() - sent, 0);
#endif
        if (written <= 0) return false;
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

int socket_read(SocketHandle handle, char* buffer, std::size_t size) {
#if defined(_WIN32)
    return ::recv(handle, buffer, static_cast<int>(size), 0);
#else
    return static_cast<int>(::recv(handle, buffer, size, 0));
#endif
}

// ── SseWriter ──────────────────────────────────────────────────────────────

SseWriter::SseWriter(SocketHandle handle) : handle_(handle) {}

bool SseWriter::write(const std::string& data, std::uint64_t id) {
    std::string frame;
    if (id != 0) frame += "id: " + std::to_string(id) + "\n";
    frame += "data: " + data + "\n\n";
    return socket_write_all(handle_, frame);
}

bool SseWriter::retry(std::uint32_t milliseconds) const {
    return socket_write_all(handle_, "retry: " + std::to_string(milliseconds) + "\n\n");
}

bool SseWriter::named(const std::string& event, const std::string& data) {
    return socket_write_all(handle_, "event: " + event + "\ndata: " + data + "\n\n");
}

bool SseWriter::close() const {
    return socket_write_all(handle_, "data: [DONE]\n\n");
}

bool SseWriter::client_gone() const {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(handle_, &fds);
    timeval tv{};
    const int ready = select(static_cast<int>(handle_) + 1, &fds, nullptr, nullptr, &tv);
    if (ready <= 0) return false;
    char byte;
    // 0 means the peer closed its write side; any data on an SSE stream
    // means the client is talking back, which also ends the stream.
    return socket_read(handle_, &byte, 1) <= 0;
}

// ── Server ─────────────────────────────────────────────────────────────────

Server::Server(int port) : port_(port) {}

void Server::get(const std::string& path, Handler handler) {
    get_routes_[path] = Route{std::move(handler), SseHandler{}, false};
}

void Server::post(const std::string& path, Handler handler) {
    post_routes_[path] = Route{std::move(handler), SseHandler{}, false};
}

void Server::put(const std::string& path, Handler handler) {
    put_routes_[path] = Route{std::move(handler), SseHandler{}, false};
}

void Server::sse(const std::string& path, SseHandler handler) {
    get_routes_[path] = Route{Handler{}, std::move(handler), true};
}

void Server::set_request_logger(RequestLogger logger) {
    std::lock_guard<std::mutex> lock(logger_mutex_);
    request_logger_ = std::move(logger);
}

std::vector<Server::RouteInfo> Server::route_inventory() const {
    std::vector<RouteInfo> out;
    for (const auto& [path, route] : get_routes_) {
        out.push_back({route.is_sse ? "SSE" : "GET", path});
    }
    for (const auto& [path, route] : post_routes_) {
        out.push_back({"POST", path});
    }
    for (const auto& [path, route] : put_routes_) {
        out.push_back({"PUT", path});
    }
    std::sort(out.begin(), out.end(), [](const RouteInfo& a, const RouteInfo& b) {
        if (a.method != b.method) return a.method < b.method;
        return a.path < b.path;
    });
    return out;
}

void Server::log_request(const std::string& method, const std::string& path,
                         int status, long long duration_ms) {
    RequestLogger logger;
    {
        std::lock_guard<std::mutex> lock(logger_mutex_);
        logger = request_logger_;
    }
    if (logger) logger(method, path, status, duration_ms);
}

bool Server::start(const std::string& host) {
    if (!socket_init()) return false;

    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* result = nullptr;
    const std::string port_string = std::to_string(port_);
    const std::string bind_host = host.empty() ? "0.0.0.0" : host;
    if (getaddrinfo(bind_host.c_str(), port_string.c_str(), &hints, &result) != 0 ||
        !result) {
        return false;
    }

    listener_ = invalid_socket();
    for (addrinfo* addr = result; addr; addr = addr->ai_next) {
        listener_ = static_cast<SocketHandle>(::socket(addr->ai_family, addr->ai_socktype,
                                                       addr->ai_protocol));
        if (!socket_valid(listener_)) continue;
        socket_set_reuseaddr(listener_);
        if (::bind(listener_, addr->ai_addr, static_cast<int>(addr->ai_addrlen)) == 0) {
            break;
        }
        socket_close(listener_);
        listener_ = invalid_socket();
    }
    freeaddrinfo(result);

    if (!socket_valid(listener_)) return false;
    if (::listen(listener_, 128) != 0) {
        socket_close(listener_);
        listener_ = invalid_socket();
        return false;
    }
    return true;
}

void Server::run() {
    if (!socket_valid(listener_)) return;
    while (true) {
        SocketHandle client = static_cast<SocketHandle>(::accept(listener_, nullptr, nullptr));
        if (!socket_valid(client)) break;
        socket_set_read_timeout(client, 30000);
        std::thread([this, client]() {
            // One wedged connection must not take the whole server down:
            // an escaping exception would fastfail the daemon process.
            try {
                handle_connection(client);
            } catch (const std::exception& e) {
                log::error(std::string("uncaught exception in http connection: ") + e.what());
            } catch (...) {
                log::error("uncaught unknown exception in http connection");
            }
            socket_close(client);
        }).detach();
    }
}

static std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> segments;
    std::size_t cursor = 0;
    while (cursor < path.size()) {
        const std::size_t slash = path.find('/', cursor);
        const std::string segment = path.substr(
            cursor, slash == std::string::npos ? std::string::npos : slash - cursor);
        if (!segment.empty()) segments.push_back(segment);
        if (slash == std::string::npos) break;
        cursor = slash + 1;
    }
    return segments;
}

// A registered route may contain `{name}` wildcards that match exactly one
// path segment. Static segments must match exactly.
static bool path_matches(const std::string& registered, const std::string& actual) {
    const std::vector<std::string> want = split_path(registered);
    const std::vector<std::string> got = split_path(actual);
    if (want.size() != got.size()) return false;
    for (std::size_t i = 0; i < want.size(); ++i) {
        const std::string& w = want[i];
        if (!w.empty() && w.front() == '{' && w.back() == '}') continue;
        if (w != got[i]) return false;
    }
    return true;
}

// Read and parse one HTTP/1.1 request from the socket. `carry` holds
// over-read bytes from a previous request on the same keep-alive
// connection; on success it holds any new over-read. Returns false on EOF,
// read timeout, or a malformed head.
static bool read_request(SocketHandle handle, std::string& carry, Request& out) {
    std::string buffer = std::move(carry);
    carry.clear();
    char chunk[8192];
    std::size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        const int count = socket_read(handle, chunk, sizeof(chunk));
        if (count <= 0) return false;
        buffer.append(chunk, static_cast<std::size_t>(count));
        header_end = buffer.find("\r\n\r\n");
        if (buffer.size() > (1u << 20)) return false;  // head too large
    }

    std::istringstream line_stream(buffer.substr(0, header_end));
    std::string method, path, version;
    line_stream >> method >> path >> version;
    if (method.empty() || path.empty() || version.empty()) return false;

    // Split the query string off before route matching; handlers that need it
    // read Request::query (e.g. the SSE resume cursor).
    std::string query;
    const std::size_t question = path.find('?');
    if (question != std::string::npos) {
        query = path.substr(question + 1);
        path.erase(question);
    }

    std::map<std::string, std::string> headers;
    std::size_t content_length = 0;
    std::size_t cursor = 0;
    while (cursor < header_end) {
        const std::size_t line_end = buffer.find("\r\n", cursor);
        if (line_end == std::string::npos || line_end > header_end) break;
        const std::string header_line = buffer.substr(cursor, line_end - cursor);
        cursor = line_end + 2;
        const std::size_t colon = header_line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = header_line.substr(0, colon);
        std::string value = header_line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.erase(value.begin());
        }
        for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        headers[key] = value;
        if (key == "content-length") {
            try {
                content_length = static_cast<std::size_t>(std::stoull(value));
            } catch (...) {
                content_length = 0;
            }
        }
    }

    std::string body = buffer.substr(header_end + 4);
    while (body.size() < content_length) {
        const int count = socket_read(handle, chunk, sizeof(chunk));
        if (count <= 0) return false;
        body.append(chunk, static_cast<std::size_t>(count));
    }
    if (body.size() > content_length) {
        carry = body.substr(content_length);
        body.resize(content_length);
    }

    out = Request{method, path, headers, std::move(body), std::move(query)};
    return true;
}

void Server::handle_connection(SocketHandle handle) {
    // HTTP/1.1 keep-alive: serve requests until the client closes or the
    // read times out. Browsers pool these sockets, so a server that closes
    // after every response leaves dead sockets in the pool and a failed
    // first attempt on EventSource costs a full reconnect interval.
    std::string carry;
    while (true) {
        Request request;
        if (!read_request(handle, carry, request)) return;

        // Any method outside GET/POST/PUT skips matching and falls into the
        // 405/404 handling below (the old catch-all dispatched every method
        // to the GET table, so e.g. PUT /api/v1/settings silently 200'd as a
        // read).
        auto* routes = &get_routes_;
        if (request.method == "POST") routes = &post_routes_;
        else if (request.method == "PUT") routes = &put_routes_;
        else if (request.method != "GET") routes = nullptr;

        const Route* matched = nullptr;
        if (routes) {
            for (const auto& [registered, route] : *routes) {
                if (path_matches(registered, request.path)) {
                    matched = &route;
                    break;
                }
            }
        }
        if (!matched) {
            // Method mismatch on a known path is a 405 with the allowed
            // methods; a path no table knows is a 404.
            std::string allow;
            auto add_allowed = [&request, &allow](
                                   const std::map<std::string, Route>& table,
                                   const char* name) {
                for (const auto& [registered, route] : table) {
                    if (path_matches(registered, request.path)) {
                        if (!allow.empty()) allow += ", ";
                        allow += name;
                        return;
                    }
                }
            };
            add_allowed(get_routes_, "GET");
            add_allowed(post_routes_, "POST");
            add_allowed(put_routes_, "PUT");
            const bool method_not_allowed = !allow.empty();
            const std::string not_found =
                method_not_allowed
                    ? "{\"error\":{\"message\":\"method not allowed\",\"type\":\"method_not_allowed\"}}"
                    : "{\"error\":{\"message\":\"not found\",\"type\":\"not_found\"}}";
            const std::string response =
                "HTTP/1.1 " + std::string(method_not_allowed ? "405 Method Not Allowed" : "404 Not Found") +
                "\r\nContent-Type: application/json" +
                (method_not_allowed ? ("\r\nAllow: " + allow) : std::string()) +
                "\r\nContent-Length: " + std::to_string(not_found.size()) +
                "\r\nConnection: keep-alive\r\n\r\n" + not_found;
            if (!socket_write_all(handle, response)) return;
            log_request(request.method, request.path, method_not_allowed ? 405 : 404, 0);
            continue;
        }

        const Route& route = *matched;

        if (route.is_sse) {
            std::string head =
                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: "
                "no-cache\r\nConnection: keep-alive\r\nX-Accel-Buffering: no\r\n\r\n";
            if (!socket_write_all(handle, head)) return;
            log_request(request.method, request.path, 200, -1);
            SseWriter writer(handle);
            try {
                route.sse_handler(writer, request);
            } catch (...) {
                // stream ended
            }
            return;  // an SSE stream owns the rest of the connection
        }

        const auto request_start = std::chrono::steady_clock::now();
        Response response;
        try {
            response = route.handler(request);
        } catch (const std::exception& error) {
            const std::string message =
                "{\"error\":{\"message\":\"" + std::string(error.what()) +
                "\",\"type\":\"internal_error\"}}";
            const std::string out = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: "
                                    "application/json\r\nContent-Length: " +
                                    std::to_string(message.size()) +
                                    "\r\nConnection: close\r\n\r\n" + message;
            socket_write_all(handle, out);
            log_request(request.method, request.path, 500,
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - request_start).count());
            return;
        }
        log_request(request.method, request.path, response.status,
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - request_start).count());

        std::string content_type = "application/json";
        if (response.headers.count("content-type")) content_type = response.headers["content-type"];
        const std::string body_out = response.body;
        std::string out = "HTTP/1.1 " + std::to_string(response.status) + " " + response.reason +
                          "\r\nContent-Type: " + content_type +
                          "\r\nContent-Length: " + std::to_string(body_out.size()) +
                          "\r\nConnection: keep-alive\r\n\r\n" + body_out;
        if (!socket_write_all(handle, out)) return;
    }
}

// ── Client ─────────────────────────────────────────────────────────────────

Url parse_url(const std::string& url) {
    Url parsed;
    std::string remaining = url;
    const std::size_t scheme = remaining.find("://");
    if (scheme != std::string::npos) {
        std::string name = remaining.substr(0, scheme);
        for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        parsed.tls = name == "https" || name == "wss";
        remaining = remaining.substr(scheme + 3);
    }
    std::size_t slash = remaining.find('/');
    std::string authority = remaining;
    if (slash != std::string::npos) {
        authority = remaining.substr(0, slash);
        parsed.path = remaining.substr(slash);
        if (parsed.path.empty()) parsed.path = "/";
    } else {
        parsed.path = "/";
    }
    const std::size_t colon = authority.find(':');
    if (colon != std::string::npos) {
        parsed.host = authority.substr(0, colon);
        try {
            parsed.port = std::stoi(authority.substr(colon + 1));
        } catch (...) {
            parsed.port = parsed.tls ? 443 : 80;
        }
    } else {
        parsed.host = authority;
        parsed.port = parsed.tls ? 443 : 80;
    }
    parsed.valid = !parsed.host.empty();
    return parsed;
}

bool post_stream(const std::string& url,
                 const std::string& body,
                 const std::map<std::string, std::string>& headers,
                 const std::function<bool(const std::string& payload)>& on_payload,
                 long long timeout_ms,
                 int* status_out,
                 std::string* error_out,
                 const std::function<bool()>& should_cancel,
                 bool* stalled_out) {
    if (stalled_out) *stalled_out = false;
    const Url parsed = parse_url(url);
    if (!parsed.valid) {
        if (error_out) *error_out = "invalid provider url";
        return false;
    }

    SocketHandle handle = socket_connect(parsed.host, parsed.port, timeout_ms);
    if (!socket_valid(handle)) return false;
    // Poll the socket in short intervals so should_cancel is honored while the
    // provider is silent (e.g. a long prompt pass before the first token):
    // one blocked read of the full timeout would swallow a cancel for minutes.
    // The total silence is still capped at timeout_ms below, so the original
    // "no data for the whole timeout" failure semantics are preserved.
    constexpr long long kReadPollMs = 2000;
    socket_set_read_timeout(handle, kReadPollMs);

    std::ostringstream request;
    request << "POST " << parsed.path << " HTTP/1.1\r\n";
    request << "Host: " << parsed.host << "\r\n";
    for (const auto& [key, value] : headers) {
        request << key << ": " << value << "\r\n";
    }
    request << "Content-Length: " << body.size() << "\r\n\r\n";
    request << body;

    const bool wrote = socket_write_all(handle, request.str());
    if (!wrote) {
        socket_close(handle);
        return false;
    }

    std::string buffer;
    bool headers_done = false;
    std::size_t body_start = 0;
    std::size_t expected_length = 0;
    bool chunked = false;
    int status = 0;
    bool done = false;
    bool cancelled = false;
    bool delivered_any = false;
    bool saw_done = false;
    bool stalled = false;
    long long silent_ms = 0;

    char chunk[8192];
    while (!done) {
        if (should_cancel && should_cancel()) {
            cancelled = true;
            break;
        }
        const int count = socket_read(handle, chunk, sizeof(chunk));
        if (count < 0) {
            if (socket_read_timed_out()) {
                silent_ms += kReadPollMs;
                if (silent_ms >= timeout_ms) {
                    stalled = true;  // the provider went quiet for the whole cap
                    break;
                }
                continue;  // no data yet; re-check should_cancel and keep polling
            }
            break;
        }
        if (count == 0) break;
        silent_ms = 0;
        buffer.append(chunk, static_cast<std::size_t>(count));

        if (!headers_done) {
            const std::size_t header_end = buffer.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                const std::string head = buffer.substr(0, header_end);
                body_start = header_end + 4;
                const std::size_t space = head.find(' ');
                if (space != std::string::npos) {
                    try {
                        status = std::stoi(head.substr(space + 1));
                    } catch (...) {
                        status = 0;
                    }
                }
                std::size_t pos = 0;
                while (pos < head.size()) {
                    const std::size_t line_end = head.find("\r\n", pos);
                    const std::string line = head.substr(
                        pos, line_end == std::string::npos ? std::string::npos
                                                           : line_end - pos);
                    const std::size_t colon = line.find(':');
                    if (colon != std::string::npos) {
                        std::string key = line.substr(0, colon);
                        std::string value = line.substr(colon + 1);
                        while (!value.empty() && (value.front() == ' ' ||
                                                  value.front() == '\t')) {
                            value.erase(value.begin());
                        }
                        for (auto& c : key) {
                            c = static_cast<char>(
                                std::tolower(static_cast<unsigned char>(c)));
                        }
                        if (key == "content-length") {
                            try {
                                expected_length = static_cast<std::size_t>(
                                    std::stoull(value));
                            } catch (...) {
                                expected_length = 0;
                            }
                        } else if (key == "transfer-encoding" && value == "chunked") {
                            chunked = true;
                        }
                    }
                    if (line_end == std::string::npos) break;
                    pos = line_end + 2;
                }
                headers_done = true;
            } else {
                continue;
            }
        }

        if (status != 200) {
            done = true;
            break;
        }

        // Process available payload bytes.
        std::size_t cursor = body_start;
        std::size_t search_from = body_start;
        while (true) {
            std::size_t line_end = buffer.find("\n", search_from);
            if (line_end == std::string::npos) break;
            const std::string line = buffer.substr(cursor, line_end - cursor);
            cursor = line_end + 1;
            search_from = cursor;
            if (line.compare(0, 6, "data: ") == 0) {
                std::string payload = line.substr(6);
                if (payload.rfind("\r", payload.size() - 1) == payload.size() - 1) {
                    payload.resize(payload.size() - 1);
                }
                if (payload == "[DONE]") {
                    saw_done = true;
                    done = true;
                    break;
                }
                delivered_any = true;
                if (!on_payload(payload)) {
                    done = true;
                    break;
                }
            }
        }
        // Erase consumed bytes so the next read does not re-process them.
        if (cursor > 0) {
            buffer.erase(0, cursor);
        }
        body_start = 0;

        if (!chunked && expected_length > 0 && buffer.size() >= body_start + expected_length) {
            done = true;
        }
    }

    socket_close(handle);
    if (status_out) *status_out = status;
    if (stalled_out) *stalled_out = stalled;
    if (cancelled) return true;
    if (stalled) {
        if (error_out) {
            *error_out = delivered_any
                ? "provider stalled: no data for " +
                      std::to_string(timeout_ms / 1000) + "s mid-stream"
                : "provider stalled: no response for " +
                      std::to_string(timeout_ms / 1000) + "s";
        }
        return false;
    }
    if (!headers_done) {
        if (error_out) *error_out = "connection closed before the provider sent a response";
        return false;
    }
    if (status != 200) {
        if (error_out) {
            // Capture what of the error body we already have (llama.cpp's
            // 503 carries {"error":{"message":"Loading model",...}}), so the
            // client can show the provider's own reason.
            std::string detail;
            if (buffer.size() > body_start) {
                std::size_t limit = buffer.size() - body_start;
                if (limit > 240) limit = 240;
                detail = buffer.substr(body_start, limit);
            }
            *error_out = "provider returned HTTP " + std::to_string(status);
            if (!detail.empty()) *error_out += " " + detail;
        }
        return false;
    }
    if (!saw_done) {
        if (error_out) {
            *error_out = delivered_any
                ? "provider closed the stream before [DONE]"
                : "provider closed the stream before the first payload";
        }
        return false;
    }
    return true;
}

bool get_body(const std::string& url,
              const std::map<std::string, std::string>& headers,
              std::string& out,
              long long timeout_ms) {
    const Url parsed = parse_url(url);
    if (!parsed.valid) return false;

    SocketHandle handle = socket_connect(parsed.host, parsed.port, timeout_ms);
    if (!socket_valid(handle)) return false;
    socket_set_read_timeout(handle, timeout_ms);

    std::ostringstream request;
    request << "GET " << parsed.path << " HTTP/1.1\r\n";
    request << "Host: " << parsed.host << "\r\n";
    request << "Connection: close\r\n";
    for (const auto& [key, value] : headers) {
        request << key << ": " << value << "\r\n";
    }
    request << "\r\n";

    if (!socket_write_all(handle, request.str())) {
        socket_close(handle);
        return false;
    }

    std::string buffer;
    char chunk[8192];
    std::size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        const int count = socket_read(handle, chunk, sizeof(chunk));
        if (count <= 0) break;
        buffer.append(chunk, static_cast<std::size_t>(count));
        header_end = buffer.find("\r\n\r\n");
    }
    if (header_end == std::string::npos) {
        socket_close(handle);
        return false;
    }
    out = buffer.substr(header_end + 4);
    // Keep reading until EOF (Connection: close) so the whole body is captured.
    while (true) {
        const int count = socket_read(handle, chunk, sizeof(chunk));
        if (count <= 0) break;
        out.append(chunk, static_cast<std::size_t>(count));
    }
    socket_close(handle);
    return true;
}

namespace {

// ── Transport: the raw fd (http) or a vendored-TLS session (https) ──────────
//
// NYAXUS_HAS_MBEDTLS is defined by the build recipes (zig + CMake) whenever
// the vendored mbedtls is compiled in. Without it the build keeps the legacy
// behavior: https:// is a caller-level fallback, not a transport.
#if defined(NYAXUS_HAS_MBEDTLS)
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

// A single-use TLS session over a client fd. mbedtls owns no fd here - the
// caller's socket code stays the source of truth for connect/close/timeouts,
// so the framing code above keeps its existing buffering behavior byte-for-
// byte.
struct TlsSession {
    SocketHandle fd = invalid_socket();
    bool active = false;
    mbedtls_ssl_context ssl{};
    mbedtls_ssl_config config{};
    mbedtls_x509_crt cas{};
    mbedtls_entropy_context entropy{};
    mbedtls_ctr_drbg_context drbg{};

    ~TlsSession() {
        if (active) {
            // Best-effort close-notify; the socket close below ends the TCP
            // side either way.
            mbedtls_ssl_close_notify(&ssl);
            mbedtls_ssl_free(&ssl);
            mbedtls_ssl_config_free(&config);
            mbedtls_x509_crt_free(&cas);
            mbedtls_ctr_drbg_free(&drbg);
            mbedtls_entropy_free(&entropy);
        }
        active = false;
    }
};

std::string tls_error(int code) {
    char buffer[160] = {0};
    mbedtls_strerror(code, buffer, sizeof(buffer));
    return std::string(buffer);
}

// Operator-facing TLS visibility (0 = off): debug output rides stderr
// unless a caller redirects via the conf_dbg ctx. Sits in the anonymous
// namespace so tests can drive it through the http-scope wrapper below.
static int tls_debug_level = 0;
void set_tls_debug_internal(int level) { tls_debug_level = level; }
void tls_default_debug(void* ctx, int level, const char* file, int line, const char* str) {
    (void)level;
    if (tls_debug_level <= 0) return;
    std::FILE* out = ctx ? static_cast<std::FILE*>(ctx) : stderr;
    std::fprintf(out, "tls[%s:%d] %s", file, line, str);
}
#else
struct TlsSession {
    SocketHandle fd = invalid_socket();
    bool active = false;
};
#endif  // NYAXUS_HAS_MBEDTLS

struct Transport {
    SocketHandle fd = invalid_socket();
    TlsSession tls{};
    bool is_tls = false;
};

bool transport_send_all(Transport& t, const std::string& data) {
#if defined(NYAXUS_HAS_MBEDTLS)
    if (t.is_tls && t.tls.active) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const int n = mbedtls_ssl_write(
                &t.tls.ssl,
                reinterpret_cast<const unsigned char*>(data.data()) + sent,
                data.size() - sent);
            if (n <= 0) return false;
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }
#endif
    return socket_write_all(t.fd, data);
}

int transport_read(Transport& t, char* buffer, std::size_t size) {
#if defined(NYAXUS_HAS_MBEDTLS)
    if (t.is_tls && t.tls.active) {
        return mbedtls_ssl_read(&t.tls.ssl,
                                reinterpret_cast<unsigned char*>(buffer), size);
    }
#endif
    return socket_read(t.fd, buffer, size);
}

// A 0/negative read is a timeout only for the raw-socket path's SO_RCVTIMEO
// bookkeeping; the TLS path reports its own semantics through mbedtls codes.
bool transport_read_timed_out(const Transport& t, int read_result) {
    (void)read_result;
#if defined(NYAXUS_HAS_MBEDTLS)
    if (t.is_tls && t.tls.active) {
        return read_result == MBEDTLS_ERR_SSL_TIMEOUT ||
               read_result == MBEDTLS_ERR_SSL_WANT_READ ||
               read_result == MBEDTLS_ERR_SSL_WANT_WRITE;
    }
#endif
    return socket_read_timed_out();
}

void transport_close(Transport& t) {
#if defined(NYAXUS_HAS_MBEDTLS)
    if (t.is_tls && t.tls.active) {
        // The TLS session's destructor runs close-notify + frees all crypto
        // state; the TCP fd is closed here exactly like the raw path.
        const SocketHandle fd = t.fd;
        t = Transport{};
        socket_close(fd);
        return;
    }
#endif
    const SocketHandle fd = t.fd;
    t = Transport{};
    socket_close(fd);
}

// System CA bundle locations on Linux; the vendored Mozilla set is the
// fallback everywhere else. Cheap: called once per TLS connect. The vendored
// copy is resolved cwd-relative first (repo root launches) and then relative
// to the daemon's own directory, so a service started from any cwd still
// finds it.
std::string exe_directory() {
#if defined(_WIN32)
    char path[MAX_PATH] = {0};
    const DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return std::string();
#else
    char path[4096] = {0};
    const ssize_t len =
        ::readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len <= 0) return std::string();
    path[len] = '\0';
#endif
    std::string full(path);
    const std::size_t slash = full.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : full.substr(0, slash + 1);
}

bool file_readable(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return false;
    std::fclose(file);
    return true;
}

std::string default_ca_bundle() {
#if !defined(_WIN32)
    const std::string system_bundle = "/etc/ssl/certs/ca-certificates.crt";
    if (file_readable(system_bundle)) return system_bundle;
#endif
    const std::string vendored = "harness/third_party/mbedtls/cacert.pem";
    if (file_readable(vendored)) return vendored;
    const std::string exe_relative =
        exe_directory() + "third_party/mbedtls/cacert.pem";
    if (file_readable(exe_relative)) return exe_relative;
    return std::string();
}

// Open `url` over the transport: raw socket for http://, socket + vendored
// TLS handshake (and verification against `ca_path`, empty = vendored
// default) for https://. `timeout_ms` bounds connect and socket reads; the
// handshake inherits them through the fd's SO_RCVTIMEO.
bool transport_connect(const Url& parsed, long long timeout_ms,
                       const std::string& ca_path, Transport& t,
                       std::string* error_out) {
    t = Transport{};
    t.fd = socket_connect(parsed.host, parsed.port, timeout_ms);
    if (!socket_valid(t.fd)) {
        if (error_out) *error_out = "could not connect to " + parsed.host;
        return false;
    }
    if (!parsed.tls) {
        socket_set_read_timeout(t.fd, timeout_ms);
        return true;
    }
#if defined(NYAXUS_HAS_MBEDTLS)
    socket_set_read_timeout(t.fd, timeout_ms);
    TlsSession& session = t.tls;
    session.fd = t.fd;
    mbedtls_ssl_init(&session.ssl);
    mbedtls_ssl_config_init(&session.config);
    mbedtls_x509_crt_init(&session.cas);
    mbedtls_entropy_init(&session.entropy);
    mbedtls_ctr_drbg_init(&session.drbg);
    if (mbedtls_ctr_drbg_seed(&session.drbg, mbedtls_entropy_func,
                              &session.entropy, nullptr, 0) != 0) {
        if (error_out) *error_out = "tls: entropy seed failed";
        transport_close(t);
        return false;
    }
    if (mbedtls_ssl_config_defaults(&session.config, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        if (error_out) *error_out = "tls: config defaults failed";
        transport_close(t);
        return false;
    }
    std::string bundle = ca_path.empty() ? default_ca_bundle() : ca_path;
    if (bundle.empty()) {
        if (error_out) *error_out = "tls: no CA bundle found";
        transport_close(t);
        return false;
    }
    if (mbedtls_x509_crt_parse_file(&session.cas, bundle.c_str()) != 0) {
        if (error_out) *error_out = "tls: could not parse CA bundle " + bundle;
        transport_close(t);
        return false;
    }
    mbedtls_ssl_conf_ca_chain(&session.config, &session.cas, nullptr);
    mbedtls_ssl_conf_authmode(&session.config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_rng(&session.config, mbedtls_ctr_drbg_random,
                         &session.drbg);
    mbedtls_ssl_conf_dbg(&session.config, tls_default_debug, nullptr);
    if (mbedtls_ssl_setup(&session.ssl, &session.config) != 0 ||
        mbedtls_ssl_set_hostname(&session.ssl, parsed.host.c_str()) != 0) {
        if (error_out) *error_out = "tls: session setup failed";
        transport_close(t);
        return false;
    }
    session.active = true;
    t.is_tls = true;  // route transports through the session from here on
    // The BIOs ride the fd the caller owns; mbedtls never opens its own. The
    // custom recv maps fd-level timeouts to WANT_READ so the handshake loop
    // owns the retry, matching the socket code's poll style.
    mbedtls_ssl_set_bio(&session.ssl, &session.fd,
                        [](void* ctx, const unsigned char* buf, size_t len) {
            const SocketHandle fd = *static_cast<SocketHandle*>(ctx);
            const std::string chunk(reinterpret_cast<const char*>(buf), len);
            return socket_write_all(fd, chunk) ? static_cast<int>(len)
                                               : MBEDTLS_ERR_NET_SEND_FAILED;
        },
                        [](void* ctx, unsigned char* buf, size_t len) {
            const SocketHandle fd = *static_cast<SocketHandle*>(ctx);
            const int n = socket_read(fd, reinterpret_cast<char*>(buf), len);
            if (n > 0) return n;
            if (n == 0) return 0;
            return socket_read_timed_out() ? MBEDTLS_ERR_SSL_WANT_READ
                                           : MBEDTLS_ERR_NET_RECV_FAILED;
        },
                        nullptr);
    // Handshake: WANT_READ under a read timeout is retryable - but bounded
    // by the same total deadline as the rest of the fetch, so a server that
    // completes the TCP handshake and then goes silent cannot wedge the
    // tool forever (each WANT_READ maps back to one full SO_RCVTIMEO wait).
    const long long deadline_ms = timeout_ms > 0 ? timeout_ms : 30000;
    const auto started = std::chrono::steady_clock::now();
    for (;;) {
        const int rc = mbedtls_ssl_handshake(&session.ssl);
        if (rc == 0) break;
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started)
                    .count() > deadline_ms) {
                if (error_out) {
                    *error_out = "tls handshake with " + parsed.host +
                                 " timed out (" +
                                 std::to_string(deadline_ms) + " ms)";
                }
                transport_close(t);
                return false;
            }
            continue;
        }
        if (error_out) {
            *error_out = "tls handshake with " + parsed.host + " failed: " +
                         tls_error(rc);
        }
        transport_close(t);
        return false;
    }
    return true;
#else
    (void)ca_path;
    if (error_out) *error_out = "tls not built in this binary";
    socket_close(t.fd);
    t = Transport{};
    return false;
#endif
}

}  // namespace

void set_tls_debug(int level) { set_tls_debug_internal(level); }

namespace {

std::string lower_copy(std::string value) {
    for (auto& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

std::string trim_copy(std::string value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                              value.front() == '\r' || value.front() == '\n')) {
        value.erase(value.begin());
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                              value.back() == '\r' || value.back() == '\n')) {
        value.pop_back();
    }
    return value;
}

// Resolve a (possibly relative) Location against the current URL. The
// scheme is preserved (an http:→https: redirect hops transports at the
// loop's next iteration; the reverse too).
std::string resolve_location(const std::string& current, const std::string& location) {
    const std::string trimmed = trim_copy(location);
    if (trimmed.rfind("http://", 0) == 0 || trimmed.rfind("https://", 0) == 0) {
        return trimmed;
    }
    const Url base = parse_url(current);
    std::string origin = base.tls ? "https://" : "http://";
    origin += base.host;
    const int default_port = base.tls ? 443 : 80;
    if (base.port != default_port) origin += ":" + std::to_string(base.port);
    if (!trimmed.empty() && trimmed.front() == '/') return origin + trimmed;
    const std::size_t slash = base.path.find_last_of('/');
    const std::string dir = slash == std::string::npos ? "/" : base.path.substr(0, slash + 1);
    return origin + dir + trimmed;
}

}  // namespace

bool get_full(const std::string& url,
              const std::map<std::string, std::string>& headers,
              int* status_out,
              std::string& out,
              long long timeout_ms,
              long long max_body_bytes,
              std::string* error_out,
              const std::string& ca_path) {
    const std::size_t cap = max_body_bytes > 0
                                ? static_cast<std::size_t>(max_body_bytes)
                                : 0;  // 0 = no cap
    std::string current = url;
    for (int hop = 0; hop <= 5; ++hop) {
        const Url parsed = parse_url(current);
        if (!parsed.valid) {
            if (error_out) *error_out = "invalid url";
            return false;
        }

        Transport transport;
        if (!transport_connect(parsed, timeout_ms, ca_path, transport,
                               error_out)) {
            return false;
        }

        // Read with a stall tolerance: a read timeout inside a body is a
        // server pause (a slow generation, an aria snapshot being composed),
        // not an EOF. Track cumulative silent time so a genuinely dead
        // server still aborts after the overall timeout.
        const auto read_started = std::chrono::steady_clock::now();
        long long silent_ms = 0;
        auto transport_read_tolerant = [&](char* buffer,
                                           std::size_t size) -> int {
            const long long cap_ms =
                timeout_ms > 0
                    ? timeout_ms
                    : 30000;  // legacy no-timeout callers keep the older cap
            const int count = transport_read(transport, buffer, size);
            if (count > 0) {
                silent_ms = 0;
                return count;
            }
            if (!transport_read_timed_out(transport, count)) return count;
            silent_ms += 500;
            if (silent_ms >= cap_ms) return -1;
            const long long elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - read_started)
                    .count();
            if (elapsed >= cap_ms * 4) return -1;  // hard wall
            return 0;  // still waiting; the caller loops
        };

        std::ostringstream request;
        request << "GET " << parsed.path << " HTTP/1.1\r\n";
        request << "Host: " << parsed.host << "\r\n";
        request << "Connection: close\r\n";
        for (const auto& [key, value] : headers) {
            request << key << ": " << value << "\r\n";
        }
        request << "\r\n";
        if (!transport_send_all(transport, request.str())) {
            transport_close(transport);
            if (error_out) *error_out = "send failed";
            return false;
        }

        std::string buffer;
        char chunk[8192];
        std::size_t header_end = std::string::npos;
        while (header_end == std::string::npos) {
            const int count =
                transport_read_tolerant(chunk, sizeof(chunk));
            if (count <= 0) break;
            buffer.append(chunk, static_cast<std::size_t>(count));
            header_end = buffer.find("\r\n\r\n");
        }
        if (header_end == std::string::npos) {
            transport_close(transport);
            if (error_out)
                *error_out = "connection closed before the response headers";
            return false;
        }

        // Status line + the headers that shape body framing.
        const std::string head = buffer.substr(0, header_end);
        int status = 0;
        std::size_t content_length = 0;
        bool chunked = false;
        std::string location;
        const std::size_t space = head.find(' ');
        if (space != std::string::npos) {
            try {
                status = std::stoi(head.substr(space + 1));
            } catch (...) {
                status = 0;
            }
        }
        std::size_t pos = 0;
        while (pos < head.size()) {
            const std::size_t line_end = head.find("\r\n", pos);
            const std::string line = head.substr(
                pos, line_end == std::string::npos ? std::string::npos
                                                    : line_end - pos);
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                const std::string key =
                    lower_copy(line.substr(0, colon));
                std::string value = trim_copy(line.substr(colon + 1));
                if (key == "content-length") {
                    try {
                        content_length = static_cast<std::size_t>(std::stoull(value));
                    } catch (...) {
                        content_length = 0;
                    }
                } else if (key == "transfer-encoding") {
                    chunked = lower_copy(value) == "chunked";
                } else if (key == "location") {
                    location = value;
                }
            }
            if (line_end == std::string::npos) break;
            pos = line_end + 2;
        }
        if (status_out) *status_out = status;

        // Redirect: drop this response and follow the Location.
        if (status >= 300 && status < 400 && !location.empty() && hop < 5) {
            transport_close(transport);
            current = resolve_location(current, location);
            continue;
        }

        // The cursor-start body bytes may sit in this buffer already; the
        // content-length branch pre-fills from it. The chunked branch must
        // NOT pre-fill (it re-decodes from the cursor), and the EOF branch
        // must not either (it re-reads until the socket closes).
        std::string body;
        if (!chunked && content_length > 0 && buffer.size() > header_end + 4) {
            body = buffer.substr(header_end + 4);
            if (cap > 0 && body.size() > cap) body.resize(cap);
        }

        auto refill = [&]() -> bool {
            char tmp[8192];
            const int count = transport_read_tolerant(tmp, sizeof(tmp));
            if (count < 0) return false;
            if (count == 0) return true;  // stalling, caller loops again
            buffer.append(tmp, static_cast<std::size_t>(count));
            return true;
        };

        if (chunked) {
            // Decode HTTP chunked framing: <hex-size>\r\n<data>\r\n ... 0\r\n.
            std::size_t cursor = header_end + 4;
            bool done = false;
            while (!done && (cap == 0 || body.size() < cap)) {
                const std::size_t line_end = buffer.find("\r\n", cursor);
                if (line_end == std::string::npos) {
                    if (!refill()) { done = true; break; }
                    continue;
                }
                std::string size_str = buffer.substr(cursor, line_end - cursor);
                const std::size_t semi = size_str.find(';');
                if (semi != std::string::npos) size_str = size_str.substr(0, semi);
                size_str = trim_copy(size_str);
                unsigned long long size = 0;
                try {
                    size = std::stoull(size_str, nullptr, 16);
                } catch (...) {
                    done = true;
                    break;
                }
                if (size == 0) {
                    done = true;
                    break;
                }
                const std::size_t data_start = line_end + 2;
                if (buffer.size() < data_start + size + 2) {
                    if (!refill()) { done = true; break; }
                    continue;
                }
                const std::size_t take =
                    cap == 0 ? size : std::min<std::size_t>(size, cap - body.size());
                body.append(buffer, data_start, take);
                cursor = data_start + size + 2;
                if (take < size) done = true;  // cap hit mid-chunk
            }
        } else if (content_length > 0) {
            // The body may already be partly present in `buffer` and was
            // pre-filled above: continue from the END of the buffer, not
            // from the cursor start, or the first chunk is appended twice
            // (a header+body single-segment response mangles the body with
            // the leading chunk duplicated mid-document).
            std::size_t cursor = buffer.size();
            while (body.size() < content_length && (cap == 0 || body.size() < cap)) {
                const std::size_t available =
                    buffer.size() > cursor ? buffer.size() - cursor : 0;
                if (available == 0) {
                    if (!refill()) break;
                    continue;
                }
                const std::size_t take =
                    std::min({available, content_length - body.size(),
                              cap == 0 ? available : cap - body.size()});
                body.append(buffer, cursor, take);
                cursor += take;
            }
            if (body.size() > content_length) body.resize(content_length);
        } else {
            // No framing headers: the body is EOF-delimited.
            while (cap == 0 || body.size() < cap) {
                char tmp[8192];
                const int count =
                    transport_read_tolerant(tmp, sizeof(tmp));
                if (count < 0) break;
                if (count == 0) continue;  // stalling; bounded by the cap
                const std::size_t take = cap == 0
                                             ? static_cast<std::size_t>(count)
                                             : std::min<std::size_t>(
                                                   static_cast<std::size_t>(count),
                                                   cap - body.size());
                body.append(tmp, take);
            }
        }

        transport_close(transport);
        out = std::move(body);
        return true;
    }
    if (error_out) *error_out = "too many redirects";
    return false;
}

bool request_body(const std::string& method,
                  const std::string& url,
                  const std::string& body,
                  const std::map<std::string, std::string>& headers,
                  std::string& out,
                  long long timeout_ms,
                  int* status_out) {
    const Url parsed = parse_url(url);
    if (!parsed.valid) return false;

    SocketHandle handle = socket_connect(parsed.host, parsed.port, timeout_ms);
    if (!socket_valid(handle)) return false;
    socket_set_read_timeout(handle, timeout_ms);

    std::ostringstream request;
    request << method << " " << parsed.path << " HTTP/1.1\r\n";
    request << "Host: " << parsed.host << "\r\n";
    request << "Connection: close\r\n";
    for (const auto& [key, value] : headers) {
        request << key << ": " << value << "\r\n";
    }
    request << "Content-Length: " << body.size() << "\r\n\r\n";
    request << body;

    if (!socket_write_all(handle, request.str())) {
        socket_close(handle);
        return false;
    }

    std::string buffer;
    char chunk[8192];
    std::size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        const int count = socket_read(handle, chunk, sizeof(chunk));
        if (count <= 0) break;
        buffer.append(chunk, static_cast<std::size_t>(count));
        header_end = buffer.find("\r\n\r\n");
        if (buffer.size() > (1u << 20)) break;  // head too large
    }
    if (header_end == std::string::npos) {
        socket_close(handle);
        return false;
    }
    const std::string head = buffer.substr(0, header_end);
    std::size_t body_start = header_end + 4;
    std::size_t expected_length = 0;
    int status = 0;
    {
        const std::size_t space = head.find(' ');
        if (space != std::string::npos) {
            try {
                status = std::stoi(head.substr(space + 1));
            } catch (...) {
                status = 0;
            }
        }
        std::size_t pos = 0;
        while (pos < head.size()) {
            const std::size_t line_end = head.find("\r\n", pos);
            const std::string line = head.substr(
                pos, line_end == std::string::npos ? std::string::npos
                                                   : line_end - pos);
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = line.substr(0, colon);
                std::string value = line.substr(colon + 1);
                while (!value.empty() && (value.front() == ' ' ||
                                          value.front() == '\t')) {
                    value.erase(value.begin());
                }
                for (auto& c : key) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (key == "content-length") {
                    try {
                        expected_length = static_cast<std::size_t>(std::stoull(value));
                    } catch (...) {
                        expected_length = 0;
                    }
                }
            }
            if (line_end == std::string::npos) break;
            pos = line_end + 2;
        }
    }
    out = buffer.substr(body_start);
    if (status_out) *status_out = status;
    // Keep reading until EOF (Connection: close) or the declared length.
    while (true) {
        if (expected_length > 0 && out.size() >= expected_length) break;
        const int count = socket_read(handle, chunk, sizeof(chunk));
        if (count <= 0) break;
        out.append(chunk, static_cast<std::size_t>(count));
    }
    socket_close(handle);
    return true;
}

bool post_body(const std::string& url,
               const std::string& body,
               const std::map<std::string, std::string>& headers,
               std::string& out,
               long long timeout_ms,
               int* status_out) {
    return request_body("POST", url, body, headers, out, timeout_ms, status_out);
}

}  // namespace http
}  // namespace nyaxus
