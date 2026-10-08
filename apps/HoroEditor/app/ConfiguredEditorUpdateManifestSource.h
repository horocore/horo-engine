#pragma once

/**
 * @file ConfiguredEditorUpdateManifestSource.h
 * @brief Installed-host selected HTTPS metadata endpoints for editor updates.
 */

#include "ConfiguredEditorUpdateBackend.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Horo::Editor {
    /** @brief One exact host-selected endpoint and its expected signed channel. */
    struct EditorUpdateManifestEndpoint final {
        EditorUpdateChannel selection;
        std::string signedChannel;
        std::string url;
    };

    /** @brief Bounded TLS settings supplied by the installed host, never by fetched metadata. */
    struct EditorUpdateManifestHttpPolicy final {
        std::uint32_t connectTimeoutSeconds{15U};
        std::uint32_t requestTimeoutSeconds{30U};
        std::filesystem::path certificateAuthorityBundle;
    };

    /** @brief HTTPS transport kept separate from endpoint selection for focused host-policy tests. */
    class IEditorUpdateManifestHttpClient {
    public:
        virtual ~IEditorUpdateManifestHttpClient() = default;
        /**
         * @brief Fetches bounded HTTPS bytes without admitting their signature or product identity.
         * @param url Exact host-selected endpoint, with no credentials, query, or fragment.
         * @param policy Host-selected timeout bounds and optional certificate authority bundle.
         * @param cancellation Cooperative cancellation for this blocking worker operation.
         * @return Nonempty response bytes or a transport, policy, response, or cancellation error.
         */
        [[nodiscard]] virtual Result<std::string> Get(const std::string &url, const EditorUpdateManifestHttpPolicy &policy,
                                                      CancellationToken cancellation) = 0;
    };

    /** @brief TLS-verified, no-redirect, size-bounded manifest transport. */
    class CurlEditorUpdateManifestHttpClient final : public IEditorUpdateManifestHttpClient {
    public:
        /**
         * @brief Performs the request with verified TLS 1.3 or newer, no redirects, and a 128 KiB body limit.
         * @param url Exact HTTPS endpoint supplied by the installed host.
         * @param policy Positive timeout bounds and the host-selected certificate authority bundle.
         * @param cancellation Cooperative cancellation observed during and after transfer.
         * @return A nonempty HTTP 200 body at the exact URL, or a failure without any update admission.
         */
        [[nodiscard]] Result<std::string> Get(const std::string &url, const EditorUpdateManifestHttpPolicy &policy,
                                              CancellationToken cancellation) override;
    };

    /** @brief Maps an editor channel only to an exact installer-selected HTTPS endpoint. */
    class ConfiguredEditorUpdateManifestSource final : public IEditorUpdateManifestSource {
    public:
        /**
         * @brief Captures installed-host policy and borrows its HTTP transport.
         * @param endpoints Complete channel map, validated before every fetch.
         * @param policy Trusted transport settings retained by value.
         * @param client Borrowed transport that must outlive the source and all its worker calls.
         */
        ConfiguredEditorUpdateManifestSource(std::vector<EditorUpdateManifestEndpoint> endpoints, EditorUpdateManifestHttpPolicy policy,
                                             IEditorUpdateManifestHttpClient &client);
        /**
         * @brief Fetches only the exact endpoint selected by a valid, unambiguous channel map.
         * @param channel Requested channel and, for enterprise sources, its managed source ID.
         * @param cancellation Cooperative cancellation for the blocking transport.
         * @return Bounded document bytes and the host's expected signed channel, or the original failure.
         */
        [[nodiscard]] Result<EditorUpdateMetadata> Fetch(const EditorUpdateChannel &channel, CancellationToken cancellation) override;

    private:
        std::vector<EditorUpdateManifestEndpoint> endpoints_;
        EditorUpdateManifestHttpPolicy policy_;
        IEditorUpdateManifestHttpClient &client_;
    };
}  // namespace Horo::Editor
