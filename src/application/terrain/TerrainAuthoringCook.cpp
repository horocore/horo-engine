#include "TerrainAuthoringCookInternal.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>

namespace Horo::Application {
    namespace TerrainAuthoringCookErrors {
        const ErrorDomainId Domain{"horo.application.terrain_cook"};
        const ErrorCodeDescriptor Invalid{Domain, ErrorCode{"terrain.cook.invalid"}, ErrorSeverity::Error,
                                          "Terrain cook inputs or host capabilities are invalid.",
                                          "Select a saved source and an explicit supported cook profile."};
        const ErrorCodeDescriptor Busy{Domain, ErrorCode{"terrain.cook.busy"}, ErrorSeverity::Error,
                                       "The terrain cook attempt or retained completion is still owned.",
                                       "Cancel or consume the existing attempt first."};
        const ErrorCodeDescriptor Closed{Domain, ErrorCode{"terrain.cook.closed"}, ErrorSeverity::Error,
                                         "Terrain cook admission is retired.", "Use the current workspace source owner."};
        const ErrorCodeDescriptor Stale{Domain, ErrorCode{"terrain.cook.stale"}, ErrorSeverity::Error,
                                        "Terrain source or dependency meaning changed since capture.",
                                        "Save and request a cook for the current source."};
        const ErrorCodeDescriptor Failed{Domain, ErrorCode{"terrain.cook.failed"}, ErrorSeverity::Error,
                                         "Terrain cook control failed before a terminal publication receipt was returned.",
                                         "Inspect the retained source and cook diagnostic."};
        const ErrorCodeDescriptor Cancelled{Domain, ErrorCode{"terrain.cook.cancelled"}, ErrorSeverity::Error,
                                            "Terrain cook was cancelled before publication.",
                                            "Request a new cook when the source is ready."};
    }  // namespace TerrainAuthoringCookErrors

    namespace {
        struct Work final {
            TerrainAuthoringCookRequest request;
            std::shared_ptr<CancellationSource> cancellation;
            Error failed;
            Error capacity;
        };

        static_assert(std::is_nothrow_move_constructible_v<Work>);
        static_assert(std::is_nothrow_move_constructible_v<TerrainAuthoringCookCompletion>);

        bool ValidEnvironment(const TerrainAuthoringCookEnvironment &environment) noexcept {
            return environment.jobs && environment.registry && environment.mutations && environment.cacheFiles &&
                   environment.publicationFiles && environment.newPublicationOperationId;
        }

        bool ValidFence(const TerrainAuthoringCookFence &fence) noexcept {
            return fence.workspace.IsValid() && fence.session.IsValid() && fence.dataset.IsValid() && fence.source.IsValid() &&
                   fence.capability.IsValid();
        }

        Result<Assets::AssetCookReport> Run(const TerrainAuthoringCookEnvironment &environment, Work &work) noexcept {
            using Return = Result<Assets::AssetCookReport>;
            try {
                return TerrainCookDetail::CookAttempt(environment, work.request, work.cancellation->Token());
            } catch (const std::bad_alloc &) {
                return Return::Failure(std::move(work.capacity));
            } catch (...) {
                return Return::Failure(std::move(work.failed));
            }
        }
    }  // namespace

    /** @brief Transfer state is bounded to one request/completion; native work never holds the transfer mutex. */
    struct TerrainAuthoringCookHost::State final {
        TerrainAuthoringCookEnvironment environment;
        std::mutex transfer;
        std::condition_variable wake;
        std::optional<Work> pending;
        std::optional<TerrainAuthoringCookCompletion> completed;
        std::atomic<bool> running{};
        std::atomic<bool> stopping{};
        std::jthread control;

        explicit State(TerrainAuthoringCookEnvironment services)
            : environment(std::move(services)), control([this] {
                  Loop();
              }) {}

        std::optional<Work> Await() {
            std::unique_lock lock(transfer);
            wake.wait(lock, [this] {
                return stopping.load() || pending.has_value();
            });
            if (!pending)
                return {};
            return std::exchange(pending, {});
        }

        void Complete(const TerrainAuthoringCookFence &fence, Result<Assets::AssetCookReport> result) {
            std::lock_guard lock(transfer);
            completed.emplace(TerrainAuthoringCookCompletion{fence, std::move(result)});
            running.store(false);
        }

        void Loop() {
            while (auto work = Await()) {
                auto result = Run(environment, *work);
                Complete(work->request.fence, std::move(result));
            }
        }

        void Stop() noexcept {
            stopping.store(true);
            wake.notify_one();
            if (control.joinable())
                control.join();
        }

        ~State() {
            Stop();
        }
    };

    /** @copydoc TerrainAuthoringCookHost::TerrainAuthoringCookHost */
    TerrainAuthoringCookHost::TerrainAuthoringCookHost(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    /** @copydoc TerrainAuthoringCookHost::Create */
    Result<std::unique_ptr<TerrainAuthoringCookHost>> TerrainAuthoringCookHost::Create(TerrainAuthoringCookEnvironment environment) {
        using Return = Result<std::unique_ptr<TerrainAuthoringCookHost>>;
        if (!ValidEnvironment(environment))
            return Return::Failure(MakeError(TerrainAuthoringCookErrors::Invalid));
        try {
            auto state = std::make_unique<State>(std::move(environment));
            return Return::Success(std::unique_ptr<TerrainAuthoringCookHost>(new TerrainAuthoringCookHost(std::move(state))));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(Terrain::TerrainSourceErrors::LimitExceeded));
        } catch (...) {
            return Return::Failure(MakeError(TerrainAuthoringCookErrors::Failed));
        }
    }

    /** @copydoc TerrainAuthoringCookHost::~TerrainAuthoringCookHost */
    TerrainAuthoringCookHost::~TerrainAuthoringCookHost() {
        Drain();
    }

    /** @copydoc TerrainAuthoringCookHost::Start */
    Result<void> TerrainAuthoringCookHost::Start(TerrainAuthoringCookRequest request) {
        if (retired_)
            return Result<void>::Failure(MakeError(TerrainAuthoringCookErrors::Closed));
        if (!ValidFence(request.fence) || !request.asset.IsValid())
            return Result<void>::Failure(MakeError(TerrainAuthoringCookErrors::Invalid));
        std::unique_lock lock(state_->transfer, std::try_to_lock);
        if (!lock.owns_lock())
            return Result<void>::Failure(MakeError(TerrainAuthoringCookErrors::Busy));
        if (state_->running.load() || state_->completed)
            return Result<void>::Failure(MakeError(TerrainAuthoringCookErrors::Busy));
        try {
            auto cancellation = std::make_shared<CancellationSource>();
            Work work{std::move(request), cancellation, MakeError(TerrainAuthoringCookErrors::Failed),
                      MakeError(Terrain::TerrainSourceErrors::LimitExceeded)};
            state_->pending.emplace(std::move(work));
            cancellation_ = std::move(cancellation);
            state_->running.store(true);
            lock.unlock();
            state_->wake.notify_one();
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(Terrain::TerrainSourceErrors::LimitExceeded));
        }
    }

    /** @copydoc TerrainAuthoringCookHost::Cancel */
    void TerrainAuthoringCookHost::Cancel() noexcept {
        if (cancellation_)
            cancellation_->RequestCancellation();
    }

    /** @copydoc TerrainAuthoringCookHost::Retire */
    void TerrainAuthoringCookHost::Retire() noexcept {
        retired_ = true;
        Cancel();
    }

    /** @copydoc TerrainAuthoringCookHost::Poll */
    std::optional<TerrainAuthoringCookCompletion> TerrainAuthoringCookHost::Poll() {
        std::unique_lock lock(state_->transfer, std::try_to_lock);
        if (!lock.owns_lock())
            return {};
        return std::exchange(state_->completed, {});
    }

    /** @copydoc TerrainAuthoringCookHost::IsRunning */
    bool TerrainAuthoringCookHost::IsRunning() const noexcept {
        return state_->running.load();
    }

    /** @copydoc TerrainAuthoringCookHost::Drain */
    void TerrainAuthoringCookHost::Drain() noexcept {
        Retire();
        state_->Stop();
    }
}  // namespace Horo::Application
