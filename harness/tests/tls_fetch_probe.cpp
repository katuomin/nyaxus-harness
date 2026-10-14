// Direct probe of the TLS transport through http::get_full.
#include <cstdio>
#include <map>
#include <string>
#include <nyaxus/http.hpp>

int main(int argc, char** argv) {
    const std::string url = argc > 1 ? argv[1] : "https://example.com/";
    const std::string ca = argc > 2 ? argv[2] : std::string();
    nyaxus::http::socket_init();
    nyaxus::http::set_tls_debug(argc > 3 ? std::atoi(argv[3]) : 2);
    int status = 0;
    std::string body;
    std::string error;
    const std::map<std::string, std::string> headers = {
        {"User-Agent", "Nyaxus/1.0"}, {"Connection", "close"}};
    const bool ok = nyaxus::http::get_full(url, headers, &status, body, 15000,
                                           0, &error,
                                           ca.empty() ? std::string() : ca);
    std::printf("ok=%d status=%d bytes=%zu error=%s\n", ok ? 1 : 0, status,
                body.size(), error.c_str());
    if (ok) std::printf("%.*s\n", 200, body.c_str());
    return ok ? 0 : 1;
}
