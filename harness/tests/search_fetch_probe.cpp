// Direct probe of the web.search request path: the exact get_full call
// web_search_tool makes (same headers, timeout, cap), then the same
// json::parse + results extraction. Prints where the chain breaks.
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <nyaxus/http.hpp>
#include <nyaxus/json.hpp>

int main(int argc, char** argv) {
    const std::string endpoint =
        argc > 1 ? argv[1] : "http://<deployment-lan>:8080";
    const std::string query = argc > 2 ? argv[2] : "test";
    nyaxus::http::socket_init();
    std::string ep = endpoint;
    while (ep.size() > 1 && ep.back() == '/') ep.pop_back();
    const std::string url =
        ep + "/search?q=" + query + "&format=json";
    int status = 0;
    std::string body;
    std::string error;
    const bool ok = nyaxus::http::get_full(
        url, {{"User-Agent", "Nyaxus/1.0"}}, &status, body, 15000,
        256 * 1024, &error);
    std::printf("url=%s\n", url.c_str());
    std::printf("ok=%d status=%d bytes=%zu error=%s\n", ok ? 1 : 0, status,
                body.size(), error.c_str());
    if (!ok) return 1;
    std::printf("body_head=[%.120s]\n", body.c_str());
    if (argc > 3) {
        std::ofstream dump(argv[3], std::ios::binary);
        dump.write(body.data(), static_cast<std::streamsize>(body.size()));
    }
    nyaxus::json::Value parsed;
    try {
        parsed = nyaxus::json::parse(body);
    } catch (const std::exception& e) {
        std::printf("parse: threw %s\n", e.what());
        return 2;
    }
    std::printf("type=%d\n", static_cast<int>(parsed.type()));
    if (parsed.type() != nyaxus::json::Value::Type::Object) return 3;
    const auto& results = parsed["results"];
    std::printf("results_type=%d count=%zu\n",
                static_cast<int>(results.type()),
                results.type() == nyaxus::json::Value::Type::Array
                    ? results.as_array().size()
                    : 0);
    if (results.type() == nyaxus::json::Value::Type::Array &&
        !results.as_array().empty()) {
        const auto& first = results.as_array().front();
        std::printf("first_url=%s\n",
                    first["url"].as_string("").c_str());
    }
    return 0;
}
