#include "Horo/Assets/AssetProvider.h"

#include "../AssetErrors.h"
#include "AssetProviderRead.h"

#include <algorithm>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace Horo::Assets {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor, std::string message = {}) {
            return Result<T>::Failure(MakeError(descriptor, std::move(message)));
        }

        [[nodiscard]] bool IsTerminal(const AssetLoadState state) noexcept {
            using enum AssetLoadState;
            return state == Succeeded || state == Failed || state == Cancelled;
        }

        struct AssetLoadControl {
            std::atomic<JobSystem *> jobs{};
        };
    }  // namespace

    /** @copydoc Internal::ReadExactArtifact */
    Result<std::vector<std::uint8_t>> Internal::ReadExactArtifact(std::istream &input, const std::size_t expectedBytes,
                                                                  const CancellationToken &cancellation) {
        std::vector<std::uint8_t> bytes(expectedBytes);
        constexpr std::size_t kChunkBytes = 64U * 1024U;
        std::size_t offset{};
        while (offset < bytes.size()) {
            if (cancellation.IsCancellationRequested())
                return Failure<std::vector<std::uint8_t>>(AssetErrors::LoadCancelled);
            const std::size_t count = std::min(kChunkBytes, bytes.size() - offset);
            input.read(reinterpret_cast<char *>(bytes.data() + offset), static_cast<std::streamsize>(count));
            if (input.gcount() != static_cast<std::streamsize>(count))
                return Failure<std::vector<std::uint8_t>>(AssetErrors::ProviderReadFailed,
                                                          "Cooked artifact changed or was truncated during the read.");
            offset += count;
        }
        return Result<std::vector<std::uint8_t>>::Success(std::move(bytes));
    }

    /** @copydoc FilesystemAssetProvider::FilesystemAssetProvider */
    FilesystemAssetProvider::FilesystemAssetProvider(std::filesystem::path cookedRoot, const AssetProviderLimits limits)
        : cookedRoot_(std::move(cookedRoot)), limits_(limits) {}

    /** @copydoc IAssetProvider::Exists */
    Result<bool> FilesystemAssetProvider::Exists(const AssetId id, const CancellationToken &cancellation) const {
        if (cancellation.IsCancellationRequested())
            return Failure<bool>(AssetErrors::LoadCancelled);
        if (!id.IsValid())
            return Failure<bool>(AssetErrors::IdentityInvalid);
        const std::filesystem::path path = cookedRoot_ / (id.ToString() + ".cooked");
        std::error_code error;
        const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
        if (error == std::errc::no_such_file_or_directory)
            return Result<bool>::Success(false);
        if (error)
            return Failure<bool>(AssetErrors::ProviderReadFailed, error.message());
        return Result<bool>::Success(std::filesystem::is_regular_file(status) && !std::filesystem::is_symlink(status));
    }

    /** @copydoc IAssetProvider::Load */
    Result<std::vector<std::uint8_t>> FilesystemAssetProvider::Load(const AssetId id, const CancellationToken &cancellation) const {
        const Result<bool> exists = Exists(id, cancellation);
        if (exists.HasError())
            return Result<std::vector<std::uint8_t>>::Failure(exists.ErrorValue());
        if (!exists.Value())
            return Failure<std::vector<std::uint8_t>>(AssetErrors::ProviderNotFound);
        const std::filesystem::path path = cookedRoot_ / (id.ToString() + ".cooked");
        std::error_code error;
        const std::uintmax_t size = std::filesystem::file_size(path, error);
        if (error)
            return Failure<std::vector<std::uint8_t>>(AssetErrors::ProviderReadFailed, error.message());
        if (size > limits_.maximumAssetBytes)
            return Failure<std::vector<std::uint8_t>>(AssetErrors::ProviderTooLarge);
        std::ifstream input(path, std::ios::binary);
        if (!input)
            return Failure<std::vector<std::uint8_t>>(AssetErrors::ProviderReadFailed);
        return Internal::ReadExactArtifact(input, size, cancellation);
    }

    class MemoryAssetProvider::State {
    public:
        void Insert(const AssetId id, std::vector<std::uint8_t> bytes) {
            std::scoped_lock lock{mutex_};
            assets_.insert_or_assign(id, std::move(bytes));
        }

        void Remove(const AssetId id) {
            std::scoped_lock lock{mutex_};
            assets_.erase(id);
        }

        [[nodiscard]] bool Contains(const AssetId id) const {
            std::scoped_lock lock{mutex_};
            return assets_.contains(id);
        }

        [[nodiscard]] std::optional<std::vector<std::uint8_t>> Find(const AssetId id) const {
            std::scoped_lock lock{mutex_};
            const auto found = assets_.find(id);
            if (found == assets_.end())
                return std::nullopt;
            return found->second;
        }

    private:
        std::unordered_map<AssetId, std::vector<std::uint8_t>, AssetIdHash> assets_;
        mutable std::mutex mutex_;
    };

    MemoryAssetProvider::MemoryAssetProvider() : state_(std::make_unique<State>()) {}

    MemoryAssetProvider::~MemoryAssetProvider() = default;

    /** @copydoc MemoryAssetProvider::Insert */
    void MemoryAssetProvider::Insert(const AssetId id, std::vector<std::uint8_t> bytes) {
        state_->Insert(id, std::move(bytes));
    }

    /** @copydoc MemoryAssetProvider::Remove */
    void MemoryAssetProvider::Remove(const AssetId id) {
        state_->Remove(id);
    }

    /** @copydoc IAssetProvider::Exists */
    Result<bool> MemoryAssetProvider::Exists(const AssetId id, const CancellationToken &cancellation) const {
        if (cancellation.IsCancellationRequested())
            return Failure<bool>(AssetErrors::LoadCancelled);
        return Result<bool>::Success(state_->Contains(id));
    }

    /** @copydoc IAssetProvider::Load */
    Result<std::vector<std::uint8_t>> MemoryAssetProvider::Load(const AssetId id, const CancellationToken &cancellation) const {
        if (cancellation.IsCancellationRequested())
            return Failure<std::vector<std::uint8_t>>(AssetErrors::LoadCancelled);
        const auto found = state_->Find(id);
        if (!found)
            return Failure<std::vector<std::uint8_t>>(AssetErrors::ProviderNotFound);
        return Result<std::vector<std::uint8_t>>::Success(*found);
    }

    struct AssetLoadHandle::Request {
        AssetId id;
        AssetRegistryRevision revision;
        std::optional<AssetRecord> record;
        std::shared_ptr<AssetLoadControl> control;
        std::shared_ptr<JobHandle> job;
        std::atomic<AssetLoadState> state{AssetLoadState::Queued};
        std::mutex resultMutex;
        std::optional<Result<AssetLoadResult>> result;
        bool consumed{};

        void Complete(Result<std::vector<std::uint8_t>> &loaded, const CancellationToken &cancel) {
            using enum AssetLoadState;
            std::scoped_lock lock{resultMutex};
            if (loaded.HasError()) {
                const bool wasCancelled = cancel.IsCancellationRequested() || loaded.ErrorValue().code.Value() == "asset.load.cancelled";
                result = Result<AssetLoadResult>::Failure(loaded.ErrorValue());
                state.store(wasCancelled ? Cancelled : Failed);
            } else {
                result = Result<AssetLoadResult>::Success(AssetLoadResult{id, revision, std::move(loaded).Value()});
                state.store(Succeeded);
            }
        }

        void Fail(const ErrorCodeDescriptor &error) {
            using enum AssetLoadState;
            std::scoped_lock lock{resultMutex};
            result = Result<AssetLoadResult>::Failure(MakeError(error));
            state.store(Failed);
        }
    };

    /** @copydoc AssetLoadHandle::State */
    AssetLoadState AssetLoadHandle::State() const noexcept {
        return request_ ? request_->state.load() : AssetLoadState::Failed;
    }

    /** @copydoc AssetLoadHandle::RequestCancel */
    Result<void> AssetLoadHandle::RequestCancel() {  // NOSONAR(cpp:S5817)
        if (!request_ || !request_->job)
            return Result<void>::Failure(MakeError(AssetErrors::LoadShutdown));
        const JobSystem *jobs = request_->control ? request_->control->jobs.load() : nullptr;
        if (!jobs)
            return Result<void>::Failure(MakeError(AssetErrors::LoadShutdown));
        if (AssetLoadState expected = AssetLoadState::Queued;
            request_->state.compare_exchange_strong(expected, AssetLoadState::Cancelled)) {
            std::scoped_lock lock{request_->resultMutex};
            request_->result = Result<AssetLoadResult>::Failure(MakeError(AssetErrors::LoadCancelled));
        }
        if (const Result<void> cancelled = jobs->RequestCancel(request_->job->Id());
            cancelled.HasError() && !IsTerminal(request_->state.load()))
            return cancelled;
        return Result<void>::Success();
    }

    /** @copydoc AssetLoadHandle::Wait */
    Result<void> AssetLoadHandle::Wait() const {
        using enum AssetLoadState;
        if (!request_ || !request_->job)
            return Result<void>::Failure(MakeError(AssetErrors::LoadShutdown));
        const Result<void> waited = request_->job->Wait();
        if (waited.HasError() && request_->state.load() == Queued)
            request_->state.store(Cancelled);
        return waited.HasError() && request_->state.load() != Cancelled ? waited : Result<void>::Success();
    }

    /** @copydoc AssetLoadHandle::TakeResult */
    Result<AssetLoadResult> AssetLoadHandle::TakeResult() {  // NOSONAR(cpp:S5817)
        if (!request_)
            return Failure<AssetLoadResult>(AssetErrors::LoadShutdown);
        if (!IsTerminal(request_->state.load()))
            return Failure<AssetLoadResult>(AssetErrors::LoadNotReady);
        std::scoped_lock lock{request_->resultMutex};
        if (request_->consumed)
            return Failure<AssetLoadResult>(AssetErrors::LoadConsumed);
        request_->consumed = true;
        if (!request_->result)
            return Failure<AssetLoadResult>(AssetErrors::LoadCancelled);
        return std::move(*request_->result);
    }

    struct AssetLoadService::State {
        State(JobSystem &jobSystem, const IAssetProvider &assetProvider, const std::size_t limit)
            : jobs(jobSystem), provider(assetProvider), maximumOutstanding(limit) {
            control->jobs.store(&jobs);
        }

        JobSystem &jobs;
        const IAssetProvider &provider;
        std::size_t maximumOutstanding;
        std::shared_ptr<AssetLoadControl> control{std::make_shared<AssetLoadControl>()};
        std::mutex mutex;
        std::vector<std::shared_ptr<AssetLoadHandle::Request>> requests;
        bool accepting{true};
    };

    AssetLoadService::AssetLoadService(JobSystem &jobs, const IAssetProvider &provider, const std::size_t maximumOutstanding)
        : state_(std::make_unique<State>(jobs, provider, maximumOutstanding)) {}

    AssetLoadService::~AssetLoadService() {
        Shutdown();
    }

    /** @copydoc AssetLoadService::LoadAsync */
    Result<AssetLoadHandle> AssetLoadService::LoadAsync(const AssetRegistrySnapshot &snapshot, const AssetId id) {
        return LoadAsync(snapshot, id, {});
    }

    /** @copydoc AssetLoadService::LoadAsync */
    Result<AssetLoadHandle> AssetLoadService::LoadAsync(const AssetRegistrySnapshot &snapshot, const AssetId id,
                                                        const CancellationToken &parentCancellation) {
        if (parentCancellation.IsCancellationRequested())
            return Failure<AssetLoadHandle>(AssetErrors::LoadCancelled);
        const AssetRecord *record = snapshot.Find(id);
        if (!record)
            return Failure<AssetLoadHandle>(AssetErrors::ProviderNotFound,
                                            "Asset identity is not present in the captured registry snapshot.");
        static_cast<void>(record);
        std::scoped_lock lock{state_->mutex};
        if (!state_->accepting)
            return Failure<AssetLoadHandle>(AssetErrors::LoadShutdown);
        std::erase_if(state_->requests, [](const auto &request) {
            return IsTerminal(request->state.load());
        });
        if (state_->requests.size() >= state_->maximumOutstanding)
            return Failure<AssetLoadHandle>(AssetErrors::LoadQueueFull);

        auto request = std::make_shared<AssetLoadHandle::Request>();
        request->id = id;
        request->revision = snapshot.Revision();
        request->record = *record;
        request->control = state_->control;
        Result<JobHandle> submitted = state_->jobs.Submit(JobDescriptor{.parentCancellation = parentCancellation},
                                                          [request, provider = &state_->provider](const CancellationToken &cancellation) {
            using enum AssetLoadState;
            if (AssetLoadState expected = Queued; !request->state.compare_exchange_strong(expected, Running))
                return;
            try {
                Result<std::vector<std::uint8_t>> loaded = provider->Load(request->id, cancellation);
                request->Complete(loaded, cancellation);
            } catch (const std::exception &) {  // NOSONAR(cpp:S1181)
                request->Fail(AssetErrors::ProviderReadFailed);
            }
        });

        if (submitted.HasError()) {
            const ErrorCodeDescriptor &descriptor =
                submitted.ErrorValue().code.Value() == "job.queue_full" ? AssetErrors::LoadQueueFull : AssetErrors::LoadShutdown;
            return Failure<AssetLoadHandle>(descriptor, submitted.ErrorValue().message);
        }
        request->job = std::make_shared<JobHandle>(std::move(submitted).Value());
        state_->requests.push_back(request);
        return Result<AssetLoadHandle>::Success(AssetLoadHandle{std::move(request)});
    }

    /** @copydoc AssetLoadService::Shutdown */
    void AssetLoadService::Shutdown() noexcept {
        if (!state_)
            return;
        std::vector<std::shared_ptr<AssetLoadHandle::Request>> requests;
        {
            std::scoped_lock lock{state_->mutex};
            if (!state_->accepting && state_->requests.empty())
                return;
            state_->accepting = false;
            requests = state_->requests;
        }
        for (const auto &request : requests) {
            AssetLoadHandle handle{request};
            static_cast<void>(handle.RequestCancel());
        }
        for (const auto &request : requests)
            if (request->job)
                static_cast<void>(request->job->Wait());
        state_->control->jobs.store(nullptr);
        std::scoped_lock lock{state_->mutex};
        state_->requests.clear();
    }
}  // namespace Horo::Assets
