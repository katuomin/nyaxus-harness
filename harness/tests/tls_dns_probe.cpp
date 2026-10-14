// DNS-resolution probe for the vendored-TLS toolchain (winsock getaddrinfo).
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
int main() {
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) { std::printf("WSAStartup failed\n"); return 1; }
#endif
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const int rc = getaddrinfo("example.com", "443", &hints, &result);
    std::printf("getaddrinfo rc=%d\n", rc);
    if (rc == 0 && result) {
        char host[256] = {0};
        getnameinfo(result->ai_addr, static_cast<socklen_t>(result->ai_addrlen),
                    host, sizeof(host), nullptr, 0, NI_NUMERICHOST);
        std::printf("first=%s\n", host);
        freeaddrinfo(result);
    }
    return rc == 0 ? 0 : 1;
}
