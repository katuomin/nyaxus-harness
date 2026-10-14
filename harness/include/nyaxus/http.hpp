#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nyaxus {
namespace http {

// ── Sockets ────────────────────────────────────────────────────────────────

using SocketHandle = intptr_t;

SocketHandle invalid_socket();
bool socket_valid(SocketHandle handle);
void socket_close(SocketHandle handle);
bool socket_init();
void socket_cleanup();
SocketHandle socket_connect(const std::string& host, int port, long long timeout_ms);
bool socket_write_all(SocketHandle handle, const std::string& data);
int socket_read(SocketHandle handle, char* buffer, std::size_t size);

// ── Types ──────────────────────────────────────────────────────────────────

struct Request {
    std::string method;
    std::string path;  // path without the query string
    std::map<std::string, std::string> headers;
    std::string body;
    std::string query;  // raw query string, empty when absent
};;

struct Response {
    int status = 200;
    std::string reason = "OK";
    std::map<std::string, std::string> headers;
    std::string body;
};

using Handler = std::function<Response(const Request&)>;

class SseWriter {
   public:
    explicit SseWriter(SocketHandle handle);
    // When `id` is non-zero it is emitted as the SSE `id:` field, which lets
    // clients resume with `Last-Event-ID` after a reconnect.
    bool write(const std::string& data, std::uint64_t id = 0);
    // Set the browser's EventSource reconnect interval (ms) instead of its
    // ~5s default, so a dropped connection recovers quickly.
    bool retry(std::uint32_t milliseconds) const;
    bool named(const std::string& event, const std::string& data);
    bool close() const;
    // True once the client has disconnected (EOF on the socket). Lets SSE
    // handlers drop a dead connection immediately instead of waiting for a
    // failed write.
    bool client_gone() const;

   private:
    SocketHandle handle_;
};

using SseHandler = std::function<void(SseWriter&, const Request&)>;

// ── Server ─────────────────────────────────────────────────────────────────

// Invoked for every non-SSE request once a response is ready (and for SSE
// streams when they open, with duration_ms = -1). Set before start().
using RequestLogger = std::function<void(const std::string& method,
                                        const std::string& path,
                                        int status,
                                        long long duration_ms)>;

class Server {
   public:
    explicit Server(int port);

    void get(const std::string& path, Handler handler);
    void post(const std::string& path, Handler handler);
    void put(const std::string& path, Handler handler);
    void sse(const std::string& path, SseHandler handler);

    void set_request_logger(RequestLogger logger);

    // One registered route, for the self-report inventory
    // (GET /api/v1/routes). Method is "SSE" for streams.
    struct RouteInfo {
        std::string method;
        std::string path;
    };
    // Every registered route, sorted by (method, path) so the inventory is
    // deterministic. This is the daemon's own view of what it serves - the
    // API parity check compares it against the documented contract.
    std::vector<RouteInfo> route_inventory() const;

    bool start(const std::string& host = "0.0.0.0");
    void run();

   private:
    struct Route {
        Handler handler;
        SseHandler sse_handler;
        bool is_sse = false;
    };
    void handle_connection(SocketHandle handle);
    void log_request(const std::string& method, const std::string& path,
                     int status, long long duration_ms);

    int port_;
    SocketHandle listener_ = invalid_socket();
    std::map<std::string, Route> get_routes_;
    std::map<std::string, Route> post_routes_;
    std::map<std::string, Route> put_routes_;
    RequestLogger request_logger_;
    std::mutex logger_mutex_;
};

// ── Client ─────────────────────────────────────────────────────────────────

struct Url {
    std::string host;
    int port = 80;
    std::string path = "/";
    bool valid = false;
    // False for http:// (or a bare authority), true for https://. The TLS
    // transport lands on the same byte-replacing fd, so callers only branch
    // on the scheme through the transport, not on the request framing.
    bool tls = false;
};

Url parse_url(const std::string& url);

// Operator-facing TLS visibility: 0 (default) keeps handshakes quiet; 1-4
// streams mbedtls's own handshake trace to stderr (diagnostics only).
void set_tls_debug(int level);

// POST a JSON body and invoke `on_payload` for each `data:` line of the
// streaming response (including the raw bytes after `data:`).
//
// Returns false (with `error_out` set) when the response was not HTTP 200,
// when the connection closed before a status line, or when a 200 stream ended
// without delivering a single payload. A 200 stream that delivered at least
// one payload (or a clean [DONE]) returns true even if it was cut short, so a
// partial answer is preserved. `should_cancel`, when provided and returning
// true, stops the stream early and is treated as a client-side stop, not an
// error. `stalled_out`, when non-null, reports that the stream was aborted by
// the silence cap (the provider went quiet for `timeout_ms`), as distinct
// from the provider closing the connection.
bool post_stream(const std::string& url,
                 const std::string& body,
                 const std::map<std::string, std::string>& headers,
                 const std::function<bool(const std::string& payload)>& on_payload,
                 long long timeout_ms = 300000,
                 int* status_out = nullptr,
                 std::string* error_out = nullptr,
                 const std::function<bool()>& should_cancel = nullptr,
                 bool* stalled_out = nullptr);

// GET a URL and return the full response body. Returns false on transport error.
bool get_body(const std::string& url,
              const std::map<std::string, std::string>& headers,
              std::string& out,
              long long timeout_ms = 15000);

// GET a URL and return the status code plus the full response body. Unlike
// get_body it reports the status (a 404 is a successful transport), follows
// redirects (up to 5 hops, scheme-preserving), decodes chunked transfer
// encoding, and stops reading once the body reaches `max_body_bytes`
// (0 = no cap). https:// urls ride the vendored TLS transport (mbedtls,
// verified against `ca_path` when set, else the auto-detected system/vendored
// bundle). Returns false on transport error (with `error_out` set when
// non-null).
bool get_full(const std::string& url,
              const std::map<std::string, std::string>& headers,
              int* status_out,
              std::string& out,
              long long timeout_ms,
              long long max_body_bytes,
              std::string* error_out = nullptr,
              const std::string& ca_path = std::string());

// POST a body and return the full response body (non-streaming). The status
// code is reported via `status_out`; a non-2xx response still returns the
// body so callers can surface the server's error. Returns false on transport
// error.
bool post_body(const std::string& url,
               const std::string& body,
               const std::map<std::string, std::string>& headers,
               std::string& out,
               long long timeout_ms = 15000,
               int* status_out = nullptr);

// Like post_body, but with an explicit HTTP method (e.g. "PUT" for
// Hindsight's create-or-update bank route).
bool request_body(const std::string& method,
                  const std::string& url,
                  const std::string& body,
                  const std::map<std::string, std::string>& headers,
                  std::string& out,
                  long long timeout_ms = 15000,
                  int* status_out = nullptr);

}  // namespace http
}  // namespace nyaxus