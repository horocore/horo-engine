#include "Horo/Release/UpdateTransferErrors.h"
#include "HoroEditor/app/ConfiguredEditorUpdateManifestSource.h"

#include <catch2/catch_test_macros.hpp>

#if defined(__linux__)

#include <arpa/inet.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <netinet/in.h>
#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

using namespace Horo;
using namespace Horo::Editor;

namespace {
    void Check(const bool condition, const char *message) {
        if (!condition)
            throw std::runtime_error(message);
    }

    /** @brief Owns one ephemeral TLS connection and keeps its generated private key in memory. */
    class LoopbackManifestTlsServer final {
    public:
        enum class Reply {
            Ok,
            Redirect,
            Oversized,
            Empty,
            Unavailable
        };

        enum class Protocol {
            Tls13,
            Tls12Only,
            Tls12To13
        };

        explicit LoopbackManifestTlsServer(const Reply reply, const Protocol protocol = Protocol::Tls13)
            : reply_(reply), context_(SSL_CTX_new(TLS_server_method()), &SSL_CTX_free) {
            Check(context_ != nullptr, "TLS context");
            const int minimumVersion = protocol == Protocol::Tls13 ? TLS1_3_VERSION : TLS1_2_VERSION;
            const int maximumVersion = protocol == Protocol::Tls12Only ? TLS1_2_VERSION : TLS1_3_VERSION;
            Check(SSL_CTX_set_min_proto_version(context_.get(), minimumVersion) == 1, "TLS minimum version");
            Check(SSL_CTX_set_max_proto_version(context_.get(), maximumVersion) == 1, "TLS maximum version");
            CreateCertificate();
            StartListener();
            worker_ = std::jthread([this] {
                ServeOnce();
            });
        }

        ~LoopbackManifestTlsServer() {
            if (listener_ >= 0) {
                shutdown(listener_, SHUT_RDWR);
            }
            if (worker_.joinable())
                worker_.join();
            if (listener_ >= 0)
                close(listener_);
            std::error_code ignored;
            std::filesystem::remove(certificatePath_, ignored);
            std::filesystem::remove(directory_, ignored);
        }

        LoopbackManifestTlsServer(const LoopbackManifestTlsServer &) = delete;
        LoopbackManifestTlsServer &operator=(const LoopbackManifestTlsServer &) = delete;

        [[nodiscard]] std::string Url() const {
            return "https://127.0.0.1:" + std::to_string(port_) + "/manifest.json";
        }

        [[nodiscard]] const std::filesystem::path &CertificatePath() const noexcept {
            return certificatePath_;
        }

    private:
        void CreateCertificate() {
            std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> keyContext{EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr),
                                                                                   &EVP_PKEY_CTX_free};
            Check(keyContext != nullptr && EVP_PKEY_keygen_init(keyContext.get()) == 1, "EC key generation");
            Check(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(keyContext.get(), NID_X9_62_prime256v1) == 1, "EC curve");
            EVP_PKEY *rawKey = nullptr;
            Check(EVP_PKEY_keygen(keyContext.get(), &rawKey) == 1, "EC key");
            std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{rawKey, &EVP_PKEY_free};
            std::unique_ptr<X509, decltype(&X509_free)> certificate{X509_new(), &X509_free};
            Check(certificate != nullptr && X509_set_version(certificate.get(), 2) == 1, "certificate");
            Check(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) == 1, "serial");
            Check(X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) != nullptr, "not before");
            Check(X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600) != nullptr, "not after");
            Check(X509_set_pubkey(certificate.get(), key.get()) == 1, "public key");
            X509_NAME *subject = X509_get_subject_name(certificate.get());
            Check(subject != nullptr && X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                                                   reinterpret_cast<const unsigned char *>("127.0.0.1"), -1, -1, 0) == 1,
                  "subject");
            Check(X509_set_issuer_name(certificate.get(), subject) == 1, "issuer");
            X509V3_CTX extensionContext{};
            X509V3_set_ctx(&extensionContext, certificate.get(), certificate.get(), nullptr, nullptr, 0);
            AddExtension(certificate.get(), extensionContext, NID_basic_constraints, "critical,CA:TRUE");
            AddExtension(certificate.get(), extensionContext, NID_key_usage, "critical,digitalSignature,keyCertSign");
            AddExtension(certificate.get(), extensionContext, NID_subject_alt_name, "IP:127.0.0.1");
            Check(X509_sign(certificate.get(), key.get(), EVP_sha256()) > 0, "signature");
            Check(SSL_CTX_use_certificate(context_.get(), certificate.get()) == 1, "server certificate");
            Check(SSL_CTX_use_PrivateKey(context_.get(), key.get()) == 1, "server private key");

            std::string directoryTemplate = (std::filesystem::temp_directory_path() / "horo-manifest-tls-XXXXXX").string();
            const char *createdDirectory = mkdtemp(directoryTemplate.data());
            Check(createdDirectory != nullptr, "certificate directory");
            directory_ = createdDirectory;
            certificatePath_ = directory_ / "ca.pem";
            std::unique_ptr<BIO, decltype(&BIO_free)> output{BIO_new_file(certificatePath_.string().c_str(), "w"), &BIO_free};
            Check(output != nullptr && PEM_write_bio_X509(output.get(), certificate.get()) == 1, "CA certificate");
        }

        static void AddExtension(X509 *certificate, X509V3_CTX &context, const int identifier, const char *value) {
            std::string configuration{value};
            std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension{X509V3_EXT_conf_nid(nullptr, &context, identifier,
                                                                                                          configuration.data()),
                                                                                      &X509_EXTENSION_free};
            Check(extension != nullptr && X509_add_ext(certificate, extension.get(), -1) == 1, "certificate extension");
        }

        void StartListener() {
            listener_ = socket(AF_INET, SOCK_STREAM, 0);
            Check(listener_ >= 0, "loopback socket");
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = 0;
            Check(bind(listener_, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0, "loopback bind");
            Check(listen(listener_, 1) == 0, "loopback listen");
            socklen_t length = sizeof(address);
            Check(getsockname(listener_, reinterpret_cast<sockaddr *>(&address), &length) == 0, "loopback port");
            port_ = ntohs(address.sin_port);
        }

        void ServeOnce() {
            // Only this worker blocks SIGPIPE; a client may close while rejecting an oversized response.
            sigset_t blocked{};
            sigemptyset(&blocked);
            sigaddset(&blocked, SIGPIPE);
            pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
            pollfd pending{listener_, POLLIN, 0};
            if (poll(&pending, 1, 5000) <= 0 || (pending.revents & POLLIN) == 0)
                return;
            const int client = accept(listener_, nullptr, nullptr);
            if (client < 0)
                return;
            timeval timeout{5, 0};
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            std::unique_ptr<SSL, decltype(&SSL_free)> ssl{SSL_new(context_.get()), &SSL_free};
            if (ssl && SSL_set_fd(ssl.get(), client) == 1 && SSL_accept(ssl.get()) == 1) {
                char request[2048]{};
                if (SSL_read(ssl.get(), request, sizeof(request)) > 0) {
                    const std::string response = Response();
                    std::size_t sent = 0;
                    while (sent < response.size()) {
                        const int written = SSL_write(ssl.get(), response.data() + sent, static_cast<int>(response.size() - sent));
                        if (written <= 0)
                            break;  // The client may have deliberately rejected the oversized body.
                        sent += static_cast<std::size_t>(written);
                    }
                }
            }
            close(client);
        }

        [[nodiscard]] std::string Response() const {
            if (reply_ == Reply::Redirect)
                return "HTTP/1.1 302 Found\r\nLocation: https://127.0.0.1:1/redirected\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            if (reply_ == Reply::Unavailable)
                return "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            if (reply_ == Reply::Empty)
                return "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            const std::string body = reply_ == Reply::Oversized ? std::string(128U * 1024U + 1U, 'x') : "signed-document";
            return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
                   "\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n" + body;
        }

        Reply reply_;
        std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context_;
        std::filesystem::path directory_;
        std::filesystem::path certificatePath_;
        int listener_{-1};
        unsigned short port_{};
        std::jthread worker_;
    };
}  // namespace

TEST_CASE("Editor HTTPS manifest client accepts TLS 1.3 from strict and mixed-version servers", "[editor][update][tls]") {
    CurlEditorUpdateManifestHttpClient client;
    for (const auto protocol : {LoopbackManifestTlsServer::Protocol::Tls13, LoopbackManifestTlsServer::Protocol::Tls12To13}) {
        LoopbackManifestTlsServer server{LoopbackManifestTlsServer::Reply::Ok, protocol};
        const auto result = client.Get(server.Url(), {.certificateAuthorityBundle = server.CertificatePath()}, {});
        REQUIRE(result.HasValue());
        CHECK(result.Value() == "signed-document");
    }
}

TEST_CASE("Editor HTTPS manifest client rejects redirects and oversized bodies", "[editor][update][tls]") {
    CurlEditorUpdateManifestHttpClient client;
    {
        LoopbackManifestTlsServer server{LoopbackManifestTlsServer::Reply::Redirect};
        auto result = client.Get(server.Url(), {.certificateAuthorityBundle = server.CertificatePath()}, {});
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == Release::UpdateTransferErrors::InvalidResponse.code.Value());
    }
    {
        LoopbackManifestTlsServer server{LoopbackManifestTlsServer::Reply::Oversized};
        auto result = client.Get(server.Url(), {.certificateAuthorityBundle = server.CertificatePath()}, {});
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == Release::UpdateTransferErrors::InvalidResponse.code.Value());
    }
}

TEST_CASE("Editor HTTPS manifest client rejects an untrusted test authority", "[editor][update][tls]") {
    LoopbackManifestTlsServer server{LoopbackManifestTlsServer::Reply::Ok};
    CurlEditorUpdateManifestHttpClient client;
    auto result = client.Get(server.Url(), {}, {});
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == Release::UpdateTransferErrors::TransportFailed.code.Value());
}

TEST_CASE("Editor HTTPS manifest client refuses a TLS 1.2-only server", "[editor][update][tls]") {
    LoopbackManifestTlsServer server{LoopbackManifestTlsServer::Reply::Ok, LoopbackManifestTlsServer::Protocol::Tls12Only};
    CurlEditorUpdateManifestHttpClient client;
    auto result = client.Get(server.Url(), {.certificateAuthorityBundle = server.CertificatePath()}, {});
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == Release::UpdateTransferErrors::TransportFailed.code.Value());
}

TEST_CASE("Editor HTTPS manifest client rejects empty metadata and unsuccessful HTTP status", "[editor][update][tls]") {
    CurlEditorUpdateManifestHttpClient client;
    for (const auto reply : {LoopbackManifestTlsServer::Reply::Empty, LoopbackManifestTlsServer::Reply::Unavailable}) {
        LoopbackManifestTlsServer server{reply};
        const auto result = client.Get(server.Url(), {.certificateAuthorityBundle = server.CertificatePath()}, {});
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == Release::UpdateTransferErrors::InvalidResponse.code.Value());
    }
}

TEST_CASE("Editor HTTPS manifest client rejects a trusted certificate for another host", "[editor][update][tls]") {
    LoopbackManifestTlsServer server{LoopbackManifestTlsServer::Reply::Ok};
    CurlEditorUpdateManifestHttpClient client;
    std::string mismatchedUrl = server.Url();
    mismatchedUrl.replace(mismatchedUrl.find("127.0.0.1"), std::string_view{"127.0.0.1"}.size(), "localhost");
    const auto result = client.Get(mismatchedUrl, {.certificateAuthorityBundle = server.CertificatePath()}, {});
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == Release::UpdateTransferErrors::TransportFailed.code.Value());
}

#else

TEST_CASE("Editor HTTPS manifest loopback fixture requires Linux", "[editor][update][tls]") {
    SKIP("The runtime-generated OpenSSL loopback TLS fixture is Linux-only.");
}

#endif
