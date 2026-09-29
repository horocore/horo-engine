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
        [[nodiscard]] virtual Result<std::string> Get(const std::string &url, const EditorUpdateManifestHttpPolicy &policy,
                                                      CancellationToken cancellation) = 0;
    };

    /** @brief TLS-verified, no-redirect, size-bounded manifest transport. */
    class CurlEditorUpdateManifestHttpClient final : public IEditorUpdateManifestHttpClient {
    public:
        [[nodiscard]] Result<std::string> Get(const std::string &url, const EditorUpdateManifestHttpPolicy &policy,
                                              CancellationToken cancellation) override;
    };

    /** @brief Maps an editor channel only to an exact installer-selected HTTPS endpoint. */
    class ConfiguredEditorUpdateManifestSource final : public IEditorUpdateManifestSource {
    public:
        ConfiguredEditorUpdateManifestSource(std::vector<EditorUpdateManifestEndpoint> endpoints, EditorUpdateManifestHttpPolicy policy,
                                             IEditorUpdateManifestHttpClient &client);
        [[nodiscard]] Result<EditorUpdateMetadata> Fetch(const EditorUpdateChannel &channel, CancellationToken cancellation) override;

    private:
        std::vector<EditorUpdateManifestEndpoint> endpoints_;
        EditorUpdateManifestHttpPolicy policy_;
        IEditorUpdateManifestHttpClient &client_;
    };
}  // namespace Horo::Editor
