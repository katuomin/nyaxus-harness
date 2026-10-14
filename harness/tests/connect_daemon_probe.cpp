// Isolated probe of the daemon's own socket_connect (the numeric fast path
// + select-with-exceptfds). `times` connects in ONE process, to reproduce
// the per-process wedge: dial 1 (live), dial 2 (refused), dial 3 (refused).
#include <cstdio>
#include <chrono>
#include <nyaxus/http.hpp>

int main(int argc, char** argv) {
    nyaxus::http::socket_init();
    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    const int port = argc > 2 ? atoi(argv[2]) : 9446;
    const int times = argc > 3 ? atoi(argv[3]) : 1;
    for (int i = 1; i <= times; ++i) {
        const auto started = std::chrono::steady_clock::now();
        const nyaxus::http::SocketHandle fd =
            nyaxus::http::socket_connect(host, port, 15000);
        const int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        printf("connect #%d fd=%lld valid=%d after %dms\n", i, (long long)fd,
               nyaxus::http::socket_valid(fd) ? 1 : 0, ms);
        if (nyaxus::http::socket_valid(fd)) nyaxus::http::socket_close(fd);
        fflush(stdout);
    }
    return 0;
}
