#include "Horo/Release/UpdateHttpDownload.h"

#include "Horo/Release/UpdateTransferCheckpointStore.h"
#include "Horo/Release/UpdateTransferErrors.h"
#include "UpdateHttpResponse.h"

#include <curl/curl.h>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace Horo::Release {
    namespace {
        /** @brief Request-local resources and admitted durable response state. */
        struct DownloadState final {
            const UpdatePackageRecord &package;
            const UpdateDownloadPaths &paths;
            const UpdateDownloadLimits &limits;
            NativeDurableFileSystem &files;
            CancellationToken cancellation;
            const UpdateDownloadProgress &progress;
            UpdateHttpResponseHeaders headers;
            std::optional<UpdateDownloadSession> session;
            std::optional<Error> failure;
            bool transportFailure{};
            CURL *curl{};
        };

        /** @brief Admits the final response at the header/body boundary. */
        [[nodiscard]] bool AcceptHeader(DownloadState &state, const std::string_view line) {
            char *effectiveUrl = nullptr;
            if (curl_easy_getinfo(state.curl, CURLINFO_EFFECTIVE_URL, &effectiveUrl) != CURLE_OK)
                return false;
            auto parsed = state.headers.Feed(line, effectiveUrl == nullptr ? std::string_view{} : std::string_view{effectiveUrl});
            if (parsed.HasError()) {
                state.failure = parsed.ErrorValue();
                return false;
            }
            if (!parsed.Value())
                return true;
            auto started =
                UpdateDownloadSession::Begin(state.package, *parsed.Value(), state.paths, state.limits, state.files, state.cancellation);
            if (started.HasError()) {
                state.failure = started.ErrorValue();
                return false;
            }
            state.session.emplace(std::move(started).Value());
            return true;
        }

        std::size_t ReceiveHeader(char *data, const std::size_t size, const std::size_t count, void *userData) noexcept {
            auto *state = static_cast<DownloadState *>(userData);
            if (state == nullptr || size == 0U || count > (std::numeric_limits<std::size_t>::max)() / size)
                return 0U;
            const std::size_t bytes = size * count;
            try {
                if (!AcceptHeader(*state, std::string_view{data, bytes}))
                    return 0U;
            } catch (...) {
                state->transportFailure = true;
                return 0U;
            }
            return bytes;
        }

        std::size_t ReceiveBody(char *data, const std::size_t size, const std::size_t count, void *userData) noexcept {
            auto *state = static_cast<DownloadState *>(userData);
            if (state == nullptr || size == 0U || count > (std::numeric_limits<std::size_t>::max)() / size)
                return 0U;
            const std::size_t bytes = size * count;
            try {
                if (!state->session) {
                    state->failure = MakeError(UpdateTransferErrors::InvalidResponse);
                    return 0U;
                }
                auto checkpoint = state->session->Append(std::as_bytes(std::span{data, bytes}));
                if (checkpoint.HasError()) {
                    state->failure = checkpoint.ErrorValue();
                    return 0U;
                }
                if (state->progress)
                    state->progress(checkpoint.Value().durableBytes, state->package.size);
            } catch (...) {
                state->transportFailure = true;
                return 0U;
            }
            return bytes;
        }

        int ReportTransfer(void *userData, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
            const auto *state = static_cast<const DownloadState *>(userData);
            return state != nullptr && state->cancellation.IsCancellationRequested() ? 1 : 0;
        }

        /** @brief Configures a TLS-verified HTTPS GET with strict final-source and bounded callback behavior. */
        [[nodiscard]] bool Configure(CURL *curl, DownloadState &state, const UpdateHttpDownloadPolicy &policy) {
            if (curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_3) != CURLE_OK)
                return false;
            return curl_easy_setopt(curl, CURLOPT_URL, state.package.url.c_str()) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_USERAGENT, "horo-update/1") == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https") == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https") == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity") == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(policy.connectTimeoutSeconds)) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(policy.requestTimeoutSeconds)) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, ReceiveHeader) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_HEADERDATA, &state) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ReceiveBody) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ReportTransfer) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state) == CURLE_OK;
        }

        /** @brief Applies one exact If-Range request for a durable partial package. */
        [[nodiscard]] bool ConfigureResume(CURL *curl, const UpdateTransferCheckpoint &checkpoint, curl_slist *&headers,
                                           std::string &range) {
            range = std::format("{}-", checkpoint.durableBytes);
            headers = curl_slist_append(nullptr, std::format("If-Range: {}", checkpoint.strongEtag).c_str());
            return headers != nullptr && curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str()) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) == CURLE_OK;
        }

        /** @brief Returns a typed cancellation, callback, transport, or completed-body result. */
        [[nodiscard]] Result<UpdateTransferCheckpoint> FinishDownload(DownloadState &state, const CURLcode outcome,
                                                                      const Security::ArtifactVerifier &verifier) {
            if (state.failure)
                return Result<UpdateTransferCheckpoint>::Failure(*state.failure);
            if (state.transportFailure)
                return Result<UpdateTransferCheckpoint>::Failure(MakeError(UpdateTransferErrors::TransportFailed));
            if (state.cancellation.IsCancellationRequested())
                return Result<UpdateTransferCheckpoint>::Failure(MakeError(UpdateTransferErrors::Cancelled));
            if (outcome != CURLE_OK)
                return Result<UpdateTransferCheckpoint>::Failure(MakeError(UpdateTransferErrors::TransportFailed));
            if (!state.session)
                return Result<UpdateTransferCheckpoint>::Failure(MakeError(UpdateTransferErrors::InvalidResponse));
            auto complete = state.session->Finish(verifier);
            if (complete.HasError())
                return complete;
            if (complete.Value().durableBytes != state.package.size)
                return Result<UpdateTransferCheckpoint>::Failure(MakeError(UpdateTransferErrors::InvalidResponse));
            return complete;
        }
    }  // namespace

    /** @copydoc DownloadUpdatePackageHttps */
    Result<UpdateTransferCheckpoint> DownloadUpdatePackageHttps(const UpdateHttpsDownloadRequest &request, NativeDurableFileSystem &files,
                                                                const Security::ArtifactVerifier &verifier, CancellationToken cancellation,
                                                                const UpdateDownloadProgress &progress) {
        const auto &[package, paths, limits, policy] = request;
        if (!package.url.starts_with("https://") || package.size == 0U || package.size > limits.maximumPackageBytes ||
            policy.connectTimeoutSeconds == 0U || policy.requestTimeoutSeconds < policy.connectTimeoutSeconds ||
            policy.requestTimeoutSeconds > static_cast<std::uint64_t>((std::numeric_limits<long>::max)()))
            return Result<UpdateTransferCheckpoint>::Failure(MakeError(UpdateTransferErrors::InvalidResponse));
        if (cancellation.IsCancellationRequested())
            return Result<UpdateTransferCheckpoint>::Failure(MakeError(UpdateTransferErrors::Cancelled));
        auto prior = LoadUpdateTransferCheckpoint(paths.partialFile, paths.checkpointFile);
        if (prior.HasError())
            return Result<UpdateTransferCheckpoint>::Failure(prior.ErrorValue());
        if (prior.Value() && prior.Value()->durableBytes == package.size) {
            auto verified = VerifyCompletedUpdateTransfer(package, *prior.Value(), paths.partialFile, verifier);
            return verified.HasError() ? Result<UpdateTransferCheckpoint>::Failure(verified.ErrorValue())
                                       : Result<UpdateTransferCheckpoint>::Success(*prior.Value());
        }

        if (static const bool CurlReady = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK; !CurlReady)
            return Result<UpdateTransferCheckpoint>::Failure(MakeError(UpdateTransferErrors::TransportFailed));
        auto *rawCurl = curl_easy_init();
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl{rawCurl, &curl_easy_cleanup};
        if (!curl)
            return Result<UpdateTransferCheckpoint>::Failure(MakeError(UpdateTransferErrors::TransportFailed));
        DownloadState state{package, paths, limits, files, cancellation, progress, UpdateHttpResponseHeaders{package.url}};
        state.curl = curl.get();
        if (!Configure(curl.get(), state, policy))
            return Result<UpdateTransferCheckpoint>::Failure(MakeError(UpdateTransferErrors::TransportFailed));
        // libcurl borrows CAINFO until curl_easy_perform completes.
        const std::string caBundle = policy.certificateAuthorityBundle.string();
        if (!caBundle.empty() && curl_easy_setopt(curl.get(), CURLOPT_CAINFO, caBundle.c_str()) != CURLE_OK)
            return Result<UpdateTransferCheckpoint>::Failure(MakeError(UpdateTransferErrors::TransportFailed));
        curl_slist *rawHeaders = nullptr;
        std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers{nullptr, &curl_slist_free_all};
        std::string range;
        if (prior.Value()) {
            if (prior.Value()->strongEtag.empty() || !ConfigureResume(curl.get(), *prior.Value(), rawHeaders, range)) {
                if (rawHeaders != nullptr)
                    curl_slist_free_all(rawHeaders);
                return Result<UpdateTransferCheckpoint>::Failure(MakeError(UpdateTransferErrors::ResumeMismatch));
            }
            headers.reset(rawHeaders);
        }
        const CURLcode outcome = curl_easy_perform(curl.get());
        if (char *effectiveUrl = nullptr; curl_easy_getinfo(curl.get(), CURLINFO_EFFECTIVE_URL, &effectiveUrl) != CURLE_OK ||
                                          effectiveUrl == nullptr || package.url != effectiveUrl)
            return Result<UpdateTransferCheckpoint>::Failure(MakeError(UpdateTransferErrors::InvalidResponse));
        return FinishDownload(state, outcome, verifier);
    }
}  // namespace Horo::Release
