// Vendored-TLS smoke: config init + the vendored CA bundle parses into a
// chain. No sockets yet - that lands with the HTTP client integration.
#include <cstdio>
#include <string>
#include <vector>

#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>

int main(int argc, char** argv) {
    const std::string ca_path = argc > 1 ? argv[1] : "harness/third_party/mbedtls/cacert.pem";
    std::FILE* file = std::fopen(ca_path.c_str(), "rb");
    if (!file) { std::printf("FAIL: ca bundle not found: %s\n", ca_path.c_str()); return 1; }
    std::string pem;
    char buffer[8192];
    std::size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) pem.append(buffer, n);
    std::fclose(file);

    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt cas;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&config);
    mbedtls_x509_crt_init(&cas);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, nullptr, 0) != 0) {
        std::printf("FAIL: ctr_drbg_seed\n"); return 1;
    }
    if (mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        std::printf("FAIL: config_defaults\n"); return 1;
    }
    mbedtls_ssl_conf_ca_chain(&config, &cas, nullptr);
    mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &drbg);
    if (mbedtls_ssl_setup(&ssl, &config) != 0) { std::printf("FAIL: ssl_setup\n"); return 1; }

    std::vector<unsigned char> pem_bytes(pem.begin(), pem.end());
    pem_bytes.push_back(0);
    int parsed = mbedtls_x509_crt_parse(&cas, pem_bytes.data(),
                                        static_cast<int>(pem_bytes.size()));
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&config);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    if (parsed != 0) { std::printf("FAIL: ca parse (%d)\n", parsed); return 1; }
    int certs = 0;
    char subject[256];
    for (mbedtls_x509_crt* c = &cas; c != nullptr; c = c->next) {
        if (certs < 3) {
            mbedtls_x509_dn_gets(subject, sizeof(subject), &c->subject);
            std::printf("  [%d] %s\n", certs, subject);
        }
        ++certs;
    }
    if (certs < 100) { std::printf("FAIL: only %d certs parsed\n", certs); return 1; }
    std::printf("TLS_SMOKE_PASSED (%d ca certs)\n", certs);
    return 0;
}
