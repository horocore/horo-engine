#include "Horo/Assets/AssetPreviewService.h"

#include "../AssetErrors.h"
#include "Horo/Foundation/Sha256.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace Horo::Assets {
    namespace {
        constexpr std::size_t kMaximumIdentityBytes = 256;
        constexpr std::size_t kReadChunkBytes = 64U * 1024U;

        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor, std::string message = {}) {
            auto error = MakeError(descriptor, std::move(message));
            return Result<T>::Failure(std::move(error));
        }

        [[nodiscard]] bool IsTerminal(const AssetPreviewState state) noexcept {
            using enum AssetPreviewState;
            return state == Succeeded || state == Failed || state == Cancelled;
        }

        [[nodiscard]] Result<std::size_t> PayloadSize(const std::filesystem::path &path, const std::size_t maximumBytes) {
            std::error_code error;
            if (const auto status = std::filesystem::symlink_status(path, error);
                error || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status))
                return Failure<std::size_t>(AssetErrors::PreviewReadFailed);
            const std::uintmax_t fileBytes = std::filesystem::file_size(path, error);
            if (error)
                return Failure<std::size_t>(AssetErrors::PreviewReadFailed, error.message());
            if (fileBytes == 0 || fileBytes > maximumBytes || fileBytes > std::numeric_limits<std::size_t>::max())
                return Failure<std::size_t>(AssetErrors::PreviewInputTooLarge);
            return Result<std::size_t>::Success(static_cast<std::size_t>(fileBytes));
        }

        [[nodiscard]] Result<std::vector<std::uint8_t>> ReadPayload(const std::filesystem::path &path, const std::size_t maximumBytes,
                                                                    const CancellationToken &cancellation) {
            if (cancellation.IsCancellationRequested())
                return Failure<std::vector<std::uint8_t>>(AssetErrors::PreviewCancelled);
            auto payloadSize = PayloadSize(path, maximumBytes);
            if (payloadSize.HasError())
                return Result<std::vector<std::uint8_t>>::Failure(payloadSize.ErrorValue());
            std::ifstream input(path, std::ios::binary);
            if (!input)
                return Failure<std::vector<std::uint8_t>>(AssetErrors::PreviewReadFailed);
            std::vector<std::uint8_t> bytes(std::move(payloadSize).Value());
            for (std::size_t remaining = bytes.size(); remaining != 0;) {
                if (cancellation.IsCancellationRequested())
                    return Failure<std::vector<std::uint8_t>>(AssetErrors::PreviewCancelled);
                const std::size_t offset = bytes.size() - remaining;
                const std::size_t requestedBytes = std::min(kReadChunkBytes, remaining);
                input.read(reinterpret_cast<char *>(bytes.data() + offset), static_cast<std::streamsize>(requestedBytes));
                if (input.gcount() != static_cast<std::streamsize>(requestedBytes))
                    return Failure<std::vector<std::uint8_t>>(AssetErrors::PreviewReadFailed);
                remaining -= requestedBytes;
            }
            return Result<std::vector<std::uint8_t>>::Success(std::move(bytes));
        }

        void AppendField(std::vector<std::byte> &bytes, const std::string_view value) {
            const std::uint64_t size = value.size();
            for (unsigned shift = 0; shift < 64; shift += 8)
                bytes.push_back(static_cast<std::byte>((size >> shift) & 0xffU));
            const auto field = std::as_bytes(std::span{value});
            bytes.insert(bytes.end(), field.begin(), field.end());
        }

        void AppendU32(std::vector<std::byte> &bytes, const std::uint32_t value) {
            for (unsigned shift = 0; shift < 32; shift += 8)
                bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
        }

        [[nodiscard]] std::string CacheKey(const AssetPreviewRequest &request, const std::span<const std::uint8_t> payload) {
            std::vector<std::byte> preimage;
            preimage.reserve(request.contributionId.size() + request.moduleId.size() + request.moduleVersion.size() +
                             request.providerVersion.size() + request.assetType.Value().size() + 64U);
            AppendField(preimage, "HoroAssetPreviewCacheV1");
            AppendField(preimage, request.contributionId);
            AppendField(preimage, request.moduleId);
            AppendField(preimage, request.moduleVersion);
            AppendField(preimage, request.providerVersion);
            AppendField(preimage, request.assetType.Value());
            AppendU32(preimage, request.width);
            AppendU32(preimage, request.height);
            const Sha256Digest payloadDigest = ComputeSha256(std::as_bytes(payload));
            for (const std::uint8_t byte : payloadDigest.bytes)
                preimage.push_back(static_cast<std::byte>(byte));
            return FormatSha256(ComputeSha256(preimage));
        }

        [[nodiscard]] bool IsValidOutput(const AssetPreviewImage &image, const std::uint32_t maximumDimension) noexcept {
            if (image.width == 0 || image.height == 0 || image.width > maximumDimension || image.height > maximumDimension)
                return false;
            const std::size_t width = image.width;
            const std::size_t height = image.height;
            if (width > std::numeric_limits<std::size_t>::max() / height)
                return false;
            const std::size_t pixels = width * height;
            return pixels <= std::numeric_limits<std::size_t>::max() / 4U && image.pixels.size() == pixels * 4U;
        }

        [[nodiscard]] bool IsValidRequest(const AssetPreviewRequest &request, const AssetPreviewServiceLimits &limits) {
            const std::array identities{std::string_view{request.contributionId}, std::string_view{request.moduleId},
                                        std::string_view{request.moduleVersion}, std::string_view{request.providerVersion},
                                        std::string_view{request.assetType.Value()}};
            const bool identitiesValid = std::ranges::all_of(identities, [](const std::string_view identity) {
                return !identity.empty() && identity.size() <= kMaximumIdentityBytes;
            });
            const bool dimensionsValid = request.width > 0 && request.height > 0 && request.width <= limits.maximumDimension &&
                                         request.height <= limits.maximumDimension;
            return request.provider != nullptr && identitiesValid && request.absoluteAssetPath.is_absolute() && dimensionsValid;
        }

        struct PreviewControl {
            std::atomic<JobSystem *> jobs{};
        };
    }  // namespace

    struct AssetPreviewHandle::Request {
        AssetPreviewRequest input;
        std::shared_ptr<PreviewControl> control;
        std::shared_ptr<JobHandle> job;
        std::atomic<AssetPreviewState> state{AssetPreviewState::Queued};
        std::mutex resultMutex;
        std::optional<Result<AssetPreviewResult>> result;
        bool consumed{};

        [[nodiscard]] JobSystem *ActiveJobs() const noexcept {
            return control ? control->jobs.load() : nullptr;
        }

        void CancelBeforeExecution() {
            if (AssetPreviewState expected = AssetPreviewState::Queued;
                !state.compare_exchange_strong(expected, AssetPreviewState::Cancelled))
                return;
            std::scoped_lock lock{resultMutex};
            result = Failure<AssetPreviewResult>(AssetErrors::PreviewCancelled);
        }

        void Complete(Result<AssetPreviewResult> value, const CancellationToken &cancellation) {
            using enum AssetPreviewState;
            std::scoped_lock lock{resultMutex};
            if (cancellation.IsCancellationRequested()) {
                result = Failure<AssetPreviewResult>(AssetErrors::PreviewCancelled);
                state.store(Cancelled);
                return;
            }
            if (value.HasError()) {
                const bool cancelled = value.ErrorValue().code.Value() == "asset.preview.cancelled";
                result = std::move(value);
                state.store(cancelled ? Cancelled : Failed);
                return;
            }
            result = std::move(value);
            state.store(Succeeded);
        }
    };

    struct AssetPreviewService::State {
        struct CacheEntry {
            CacheEntry(const AssetPreviewImage &sourceImage, const std::uint64_t sequence, const std::size_t imageBytes)
                : image(sourceImage), useSequence(sequence), bytes(imageBytes) {}

            AssetPreviewImage image;
            std::uint64_t useSequence{};
            std::size_t bytes{};
        };

        State(JobSystem &jobSystem, const AssetPreviewServiceLimits &configuredLimits) : jobs(jobSystem), limits(configuredLimits) {
            control->jobs.store(&jobs);
        }

        [[nodiscard]] std::optional<AssetPreviewImage> FindCached(const std::string &key) {
            std::scoped_lock lock{mutex};
            const auto found = cache.find(key);
            if (found == cache.end())
                return std::nullopt;
            found->second.useSequence = ++sequence;
            return found->second.image;
        }

        void InsertCached(std::string key, const AssetPreviewImage &image) {
            const std::size_t imageBytes = image.pixels.size();
            if (limits.maximumCacheEntries == 0 || imageBytes > limits.maximumCacheBytes)
                return;
            std::scoped_lock lock{mutex};
            while (!cache.empty() && (cache.size() >= limits.maximumCacheEntries || cachedBytes > limits.maximumCacheBytes - imageBytes)) {
                const auto victim = std::ranges::min_element(cache, {}, [](const auto &entry) {
                    return entry.second.useSequence;
                });
                cachedBytes -= victim->second.bytes;
                cache.erase(victim);
            }
            if (const auto existing = cache.find(key); existing != cache.end()) {
                cachedBytes -= existing->second.bytes;
                cache.erase(existing);
            }
            cache.try_emplace(std::move(key), image, ++sequence, imageBytes);
            cachedBytes += imageBytes;
        }

        void Execute(const std::shared_ptr<AssetPreviewHandle::Request> &pending, const CancellationToken &cancellation) {
            if (AssetPreviewState expected = AssetPreviewState::Queued;
                !pending->state.compare_exchange_strong(expected, AssetPreviewState::Running))
                return;
            auto payload = ReadPayload(pending->input.absoluteAssetPath, limits.maximumInputBytes, cancellation);
            if (payload.HasError()) {
                pending->Complete(Result<AssetPreviewResult>::Failure(payload.ErrorValue()), cancellation);
                return;
            }
            std::string key = CacheKey(pending->input, payload.Value());
            if (auto cached = FindCached(key)) {
                pending->Complete(Result<AssetPreviewResult>::Success({std::move(*cached), true}), cancellation);
                return;
            }
            Result<AssetPreviewImage> generated = Generate(pending, payload.Value(), cancellation);
            if (generated.HasError()) {
                pending->Complete(Result<AssetPreviewResult>::Failure(generated.ErrorValue()), cancellation);
                return;
            }
            AssetPreviewImage image = std::move(generated).Value();
            if (!IsValidOutput(image, limits.maximumDimension)) {
                pending->Complete(Failure<AssetPreviewResult>(AssetErrors::PreviewOutputInvalid), cancellation);
                return;
            }
            if (cancellation.IsCancellationRequested()) {
                pending->Complete(Failure<AssetPreviewResult>(AssetErrors::PreviewCancelled), cancellation);
                return;
            }
            InsertCached(std::move(key), image);
            pending->Complete(Result<AssetPreviewResult>::Success({std::move(image), false}), cancellation);
        }

        [[nodiscard]] static Result<AssetPreviewImage> Generate(const std::shared_ptr<AssetPreviewHandle::Request> &pending,
                                                                const std::span<const std::uint8_t> payload,
                                                                const CancellationToken &cancellation) {
            try {
                return pending->input.provider->GeneratePreview(
                    AssetPreviewInput{
                        .editorPayload = payload,
                        .absoluteAssetPath = pending->input.absoluteAssetPath.string(),
                        .assetType = pending->input.assetType,
                        .width = pending->input.width,
                        .height = pending->input.height,
                    },
                    cancellation);
            } catch (...) {  // NOSONAR(cpp:S2738) Extension code cannot be permitted to escape the host boundary.
                return Result<AssetPreviewImage>::Failure(MakeError(AssetErrors::PreviewProviderFailed));
            }
        }

        JobSystem &jobs;
        AssetPreviewServiceLimits limits;
        std::shared_ptr<PreviewControl> control{std::make_shared<PreviewControl>()};
        std::mutex mutex;
        std::vector<std::shared_ptr<AssetPreviewHandle::Request>> requests;
        std::map<std::string, CacheEntry, std::less<>> cache;
        std::size_t cachedBytes{};
        std::uint64_t sequence{};
        bool accepting{true};
    };

    /** @copydoc AssetPreviewHandle::State */
    AssetPreviewState AssetPreviewHandle::State() const noexcept {
        return request_ ? request_->state.load() : AssetPreviewState::Failed;
    }

    /** @copydoc AssetPreviewHandle::RequestCancel */
    Result<void> AssetPreviewHandle::RequestCancel() {
        if (!request_ || !request_->job)
            return Result<void>::Failure(MakeError(AssetErrors::PreviewShutdown));
        const JobSystem *const jobs = request_->ActiveJobs();
        if (jobs == nullptr)
            return Result<void>::Failure(MakeError(AssetErrors::PreviewShutdown));
        request_->CancelBeforeExecution();
        const Result<void> cancelled = jobs->RequestCancel(request_->job->Id());
        return cancelled.HasValue() || IsTerminal(request_->state.load()) ? Result<void>::Success() : cancelled;
    }

    /** @copydoc AssetPreviewHandle::Wait */
    Result<void> AssetPreviewHandle::Wait() const {
        if (!request_ || !request_->job)
            return Result<void>::Failure(MakeError(AssetErrors::PreviewShutdown));
        const Result<void> waited = request_->job->Wait();
        return waited.HasError() && request_->state.load() != AssetPreviewState::Cancelled ? waited : Result<void>::Success();
    }

    /** @copydoc AssetPreviewHandle::TakeResult */
    Result<AssetPreviewResult> AssetPreviewHandle::TakeResult() {
        if (!request_)
            return Failure<AssetPreviewResult>(AssetErrors::PreviewShutdown);
        if (!IsTerminal(request_->state.load()))
            return Failure<AssetPreviewResult>(AssetErrors::PreviewNotReady);
        std::scoped_lock lock{request_->resultMutex};
        if (request_->consumed)
            return Failure<AssetPreviewResult>(AssetErrors::PreviewConsumed);
        request_->consumed = true;
        return request_->result ? std::move(*request_->result) : Failure<AssetPreviewResult>(AssetErrors::PreviewCancelled);
    }

    /** @copydoc AssetPreviewService::AssetPreviewService */
    AssetPreviewService::AssetPreviewService(JobSystem &jobs, AssetPreviewServiceLimits limits)
        : state_(std::make_unique<State>(jobs, limits)) {}

    AssetPreviewService::~AssetPreviewService() {
        Shutdown();
    }

    /** @copydoc AssetPreviewService::Submit */
    Result<AssetPreviewHandle> AssetPreviewService::Submit(AssetPreviewRequest request, const CancellationToken &parentCancellation) {
        if (parentCancellation.IsCancellationRequested())
            return Failure<AssetPreviewHandle>(AssetErrors::PreviewCancelled);
        if (!IsValidRequest(request, state_->limits))
            return Failure<AssetPreviewHandle>(AssetErrors::PreviewRequestInvalid);

        std::scoped_lock lock{state_->mutex};
        if (!state_->accepting)
            return Failure<AssetPreviewHandle>(AssetErrors::PreviewShutdown);
        std::erase_if(state_->requests, [](const auto &pending) {
            return IsTerminal(pending->state.load());
        });
        if (state_->requests.size() >= state_->limits.maximumOutstanding)
            return Failure<AssetPreviewHandle>(AssetErrors::PreviewQueueFull);

        auto pending = std::make_shared<AssetPreviewHandle::Request>();
        pending->input = std::move(request);
        pending->control = state_->control;
        State *const serviceState = state_.get();
        Result<JobHandle> submitted = state_->jobs.Submit(JobDescriptor{.parentCancellation = parentCancellation},
                                                          [pending, serviceState](const CancellationToken &cancellation) {
            serviceState->Execute(pending, cancellation);
        });
        if (submitted.HasError()) {
            const ErrorCodeDescriptor &descriptor =
                submitted.ErrorValue().code.Value() == "job.queue_full" ? AssetErrors::PreviewQueueFull : AssetErrors::PreviewShutdown;
            return Failure<AssetPreviewHandle>(descriptor, submitted.ErrorValue().message);
        }
        pending->job = std::make_shared<JobHandle>(std::move(submitted).Value());
        state_->requests.push_back(pending);
        return Result<AssetPreviewHandle>::Success(AssetPreviewHandle{std::move(pending)});
    }

    /** @copydoc AssetPreviewService::Shutdown */
    void AssetPreviewService::Shutdown() noexcept {
        if (!state_)
            return;
        std::vector<std::shared_ptr<AssetPreviewHandle::Request>> requests;
        {
            std::scoped_lock lock{state_->mutex};
            if (!state_->accepting && state_->requests.empty())
                return;
            state_->accepting = false;
            requests = state_->requests;
        }
        std::ranges::for_each(requests, [](const auto &request) {
            AssetPreviewHandle handle{request};
            static_cast<void>(handle.RequestCancel());
        });
        std::ranges::for_each(requests, [](const auto &request) {
            if (request->job)
                static_cast<void>(request->job->Wait());
        });
        state_->control->jobs.store(nullptr);
        std::scoped_lock lock{state_->mutex};
        state_->requests.clear();
        state_->cache.clear();
        state_->cachedBytes = 0;
    }
}  // namespace Horo::Assets
