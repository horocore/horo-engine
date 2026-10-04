#include "ConfiguredEditorUpdateManifestSource.h"

#include "Horo/Release/UpdateDiscoveryErrors.h"
#include "Horo/Release/UpdateTransferErrors.h"

#include <algorithm>
#include <curl/curl.h>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

namespace Horo::Editor {
    namespace {
        constexpr std::size_t MaximumManifestBytes = 128U * 1024U;
        constexpr std::size_t MaximumEndpointBytes = 2048U;
        constexpr std::size_t MaximumEndpoints = 16U;

        using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;

        /** @brief An endpoint is a literal HTTPS URL with no userinfo, fragment, or bearer query. */
        [[nodiscard]] bool ValidEndpoint(const std::string_view url) {
            return url.starts_with("https://") && url.size() <= MaximumEndpointBytes && url.size() > 8U &&
                   std::ranges::none_of(url, [](const unsigned char value) {
                return value <= 0x20U || value == 0x7fU || value == '@' || value == '#' || value == '?';
            });
        }

        /** @brief Rejects empty managed identity and any offline selection for HTTPS endpoints. */
        [[nodiscard]] bool ValidSelection(const EditorUpdateManifestEndpoint &endpoint) {
            if (!Release::IsValidDistributionIdentity(endpoint.signedChannel) || !ValidEndpoint(endpoint.url))
                return false;
            using enum EditorUpdateChannelKind;
            switch (endpoint.selection.kind) {
                case Stable:
                    return endpoint.selection.sourceId.empty() && endpoint.signedChannel == "stable";
                case Preview:
                    return endpoint.selection.sourceId.empty() && endpoint.signedChannel == "preview";
                case Nightly:
                    return endpoint.selection.sourceId.empty() && endpoint.signedChannel == "nightly";
                case Enterprise:
                    return Release::IsValidDistributionIdentity(endpoint.selection.sourceId);
                case Offline:
                    return false;
            }
            return false;
        }

        /** @brief One request's bounded body and cooperative cancellation state. */
        struct ManifestResponse final {
            std::string body;
            CancellationToken cancellation;
            bool oversized{};
        };

        /** @brief Appends one bounded body chunk without allowing an exception across the C callback. */
        std::size_t ReceiveBody(const char *data, const std::size_t size, const std::size_t count, ManifestResponse &response) noexcept {
            if (size == 0U || count > (std::numeric_limits<std::size_t>::max)() / size)
                return 0U;
            const std::size_t bytes = size * count;
            if (bytes > MaximumManifestBytes - response.body.size()) {
                response.oversized = true;
                return 0U;
            }
            try {
                response.body.append(data, bytes);
            } catch (...) {
                return 0U;
            }
            return bytes;
        }

        /** @brief Requests transfer termination when the owning job is cancelled. */
        int ReportTransfer(const ManifestResponse &response) noexcept {
            return response.cancellation.IsCancellationRequested() ? 1 : 0;
        }

        // The C callbacks have libcurl's fixed signature; keep the request logic typed.
        constexpr curl_write_callback ReceiveBodyCallback = [](auto *data, const std::size_t size, const std::size_t count,
                                                               auto *context) noexcept -> std::size_t {
            auto *response = static_cast<ManifestResponse *>(context);
            return response == nullptr ? 0U : ReceiveBody(data, size, count, *response);
        };
        constexpr curl_xferinfo_callback ReportTransferCallback = [](auto *context, curl_off_t, curl_off_t, curl_off_t,
                                                                     curl_off_t) noexcept {
            const auto *response = static_cast<const ManifestResponse *>(context);
            return response == nullptr ? 0 : ReportTransfer(*response);
        };

        /** @brief Configures the exact endpoint, verification, bounded callbacks, and no ambient credentials. */
        [[nodiscard]] bool ConfigureRequest(const CurlHandle &handle, const std::string &url, const EditorUpdateManifestHttpPolicy &policy,
                                            ManifestResponse &response) {
            auto *curl = handle.get();
            return curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_USERAGENT, "horo-update/1") == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https") == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https") == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_HTTPAUTH, 0L) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity") == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(policy.connectTimeoutSeconds)) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(policy.requestTimeoutSeconds)) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ReceiveBodyCallback) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ReportTransferCallback) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &response) == CURLE_OK;
        }
    }  // namespace

    /** @copydoc CurlEditorUpdateManifestHttpClient::Get */
    Result<std::string> CurlEditorUpdateManifestHttpClient::Get(const std::string &url, const EditorUpdateManifestHttpPolicy &policy,
                                                                const CancellationToken cancellation) {
        if (!ValidEndpoint(url) || policy.connectTimeoutSeconds == 0U || policy.requestTimeoutSeconds < policy.connectTimeoutSeconds ||
            policy.requestTimeoutSeconds > static_cast<std::uint64_t>((std::numeric_limits<long>::max)()))
            return Result<std::string>::Failure(MakeError(Release::UpdateDiscoveryErrors::InvalidPolicy));
        if (cancellation.IsCancellationRequested())
            return Result<std::string>::Failure(MakeError(Release::UpdateTransferErrors::Cancelled));
        if (static const bool CurlReady = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK; !CurlReady)
            return Result<std::string>::Failure(MakeError(Release::UpdateTransferErrors::TransportFailed));
        ManifestResponse response{{}, cancellation};
        CURL *const curl = curl_easy_init();
        CurlHandle handle{curl, &curl_easy_cleanup};
        if (curl == nullptr)
            return Result<std::string>::Failure(MakeError(Release::UpdateTransferErrors::TransportFailed));
        // The owning transfer scope enforces TLS 1.3 before any configuration or perform call.
        if (curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_3) != CURLE_OK)
            return Result<std::string>::Failure(MakeError(Release::UpdateTransferErrors::TransportFailed));
        if (!ConfigureRequest(handle, url, policy, response))
            return Result<std::string>::Failure(MakeError(Release::UpdateTransferErrors::TransportFailed));
        // libcurl copies CAINFO when the option is set.
        if (const std::string caBundle = policy.certificateAuthorityBundle.string();
            !caBundle.empty() && curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle.c_str()) != CURLE_OK)
            return Result<std::string>::Failure(MakeError(Release::UpdateTransferErrors::TransportFailed));
        const CURLcode outcome = curl_easy_perform(curl);
        if (response.oversized)
            return Result<std::string>::Failure(MakeError(Release::UpdateTransferErrors::InvalidResponse));
        if (cancellation.IsCancellationRequested())
            return Result<std::string>::Failure(MakeError(Release::UpdateTransferErrors::Cancelled));
        char *effectiveUrl = nullptr;
        long status = 0L;
        if (outcome != CURLE_OK)
            return Result<std::string>::Failure(MakeError(Release::UpdateTransferErrors::TransportFailed));
        if (curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effectiveUrl) != CURLE_OK || effectiveUrl == nullptr || url != effectiveUrl ||
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) != CURLE_OK || status != 200L || response.body.empty())
            return Result<std::string>::Failure(MakeError(Release::UpdateTransferErrors::InvalidResponse));
        return Result<std::string>::Success(std::move(response.body));
    }

    /** @copydoc ConfiguredEditorUpdateManifestSource::ConfiguredEditorUpdateManifestSource */
    ConfiguredEditorUpdateManifestSource::ConfiguredEditorUpdateManifestSource(std::vector<EditorUpdateManifestEndpoint> endpoints,
                                                                               EditorUpdateManifestHttpPolicy policy,
                                                                               IEditorUpdateManifestHttpClient &client)
        : endpoints_(std::move(endpoints)), policy_(std::move(policy)), client_(client) {}

    /** @copydoc ConfiguredEditorUpdateManifestSource::Fetch */
    Result<EditorUpdateMetadata> ConfiguredEditorUpdateManifestSource::Fetch(const EditorUpdateChannel &channel,
                                                                             const CancellationToken cancellation) {
        if (cancellation.IsCancellationRequested())
            return Result<EditorUpdateMetadata>::Failure(MakeError(Release::UpdateTransferErrors::Cancelled));
        if (endpoints_.empty() || endpoints_.size() > MaximumEndpoints)
            return Result<EditorUpdateMetadata>::Failure(MakeError(Release::UpdateDiscoveryErrors::InvalidPolicy));
        const EditorUpdateManifestEndpoint *selected = nullptr;
        for (const auto &endpoint : endpoints_) {
            if (!ValidSelection(endpoint))
                return Result<EditorUpdateMetadata>::Failure(MakeError(Release::UpdateDiscoveryErrors::InvalidPolicy));
            if (std::ranges::count_if(endpoints_, [&](const auto &other) {
                return other.selection == endpoint.selection;
            }) != 1)
                return Result<EditorUpdateMetadata>::Failure(MakeError(Release::UpdateDiscoveryErrors::InvalidPolicy));
            if (endpoint.selection != channel)
                continue;
            if (selected != nullptr)
                return Result<EditorUpdateMetadata>::Failure(MakeError(Release::UpdateDiscoveryErrors::InvalidPolicy));
            selected = &endpoint;
        }
        if (selected == nullptr)
            return Result<EditorUpdateMetadata>::Failure(MakeError(Release::UpdateDiscoveryErrors::InvalidPolicy));
        auto document = client_.Get(selected->url, policy_, cancellation);
        if (document.HasError())
            return Result<EditorUpdateMetadata>::Failure(document.ErrorValue());
        if (document.Value().empty() || document.Value().size() > MaximumManifestBytes)
            return Result<EditorUpdateMetadata>::Failure(MakeError(Release::UpdateTransferErrors::InvalidResponse));
        return Result<EditorUpdateMetadata>::Success({std::move(document).Value(), selected->signedChannel});
    }
}  // namespace Horo::Editor
