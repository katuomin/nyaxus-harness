// Isolated probe of the refused-connect behavior on Windows: FIONBIO +
// connect() to a dead loopback port + select(write, except) - which set
// fires, when, and what SO_ERROR says.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#endif
#include <cstdio>
#include <chrono>

int main(int argc, char** argv) {
    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    const int port = argc > 2 ? atoi(argv[2]) : 9446;
    long long timeout_ms = argc > 3 ? atoll(argv[3]) : 15000;
#ifdef _WIN32
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
    const int fd = (int)::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = inet_addr(host);
    const auto started = std::chrono::steady_clock::now();
    const int rc = ::connect(fd, (sockaddr*)&addr, sizeof(addr));
    const int connect_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    printf("connect rc=%d after %dms\n", rc, connect_ms);
    if (rc == 0) { printf("connected instantly\n"); return 0; }
    fd_set w, e;
    FD_ZERO(&w); FD_SET(fd, &w);
    FD_ZERO(&e); FD_SET(fd, &e);
    timeval tv{(long)(timeout_ms / 1000), (long)((timeout_ms % 1000) * 1000)};
    const auto sel_started = std::chrono::steady_clock::now();
    const int ready = select(fd + 1, nullptr, &w, &e, &tv);
    const int sel_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - sel_started).count();
    printf("select ready=%d after %dms (w=%d e=%d)\n", ready, sel_ms,
           ready > 0 && FD_ISSET(fd, &w), ready > 0 && FD_ISSET(fd, &e));
    if (ready > 0) {
        int so = 0; socklen_t l = sizeof(so);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&so, &l);
        printf("so_error=%d\n", so);
    }
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
    return 0;
}
