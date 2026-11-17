#include "EditorAudioPreviewControlHost.h"

#include "Horo/Audio/AudioErrors.h"
#include "Horo/Foundation/Telemetry/Operation.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

namespace Horo::Editor {
    /** @brief Retained control exchange and owned worker lifetime; native render callbacks never access this storage. */
    struct EditorAudioPreviewControlHost::State final {
        struct Preparation final {
            AudioComponentPreviewRequest request;
            Telemetry::OperationContext context;
        };

        static_assert(std::is_nothrow_move_constructible_v<Preparation>);
        enum class Admission : std::uint8_t {
            Idle,
            Reserved,
            Running,
            Retiring
        };
        // UI alone owns view/uiGeneration. The exchange mutex protects generation, admission,
        // pendingStart, published and done between UI producer and control consumer. No SDK,
        // decoding, frontend call or rendering executes while it is held. UI uses try_lock.
        mutable std::mutex exchange;
        std::condition_variable wake;
        std::condition_variable finished;
        std::optional<Preparation> pendingStart;
        AudioComponentPreviewSnapshot published;
        AudioComponentPreviewSnapshot view;
        Admission admission{Admission::Idle};
        std::uint64_t generation{};
        std::uint64_t uiGeneration{};
        bool done{};
        // Release/acquire publishes critical intents independently of exchange contention.
        // Generations prevent an old Stop from cancelling a later accepted request. State and
        // factory code remain owned until actual worker join; no native render path reads them.
        std::atomic<std::uint64_t> stopGeneration{};
        std::atomic<bool> closing{};
        EditorAudioPreviewOutputFactory factory;
        Audio::Backend::AudioRuntimeOwnerIssuer &owners;
        std::thread worker;

        explicit State(Audio::Backend::AudioRuntimeOwnerIssuer &issuer) : owners(issuer) {}

        /** @brief Copy only a matching generation, so an idle tick cannot overwrite a newly accepted request. */
        void Publish(AudioComponentPreviewSnapshot snapshot, const std::uint64_t active) {
            std::lock_guard lock(exchange);
            if (active != generation)
                return;
            if (!snapshot.output || snapshot.output->phase == Audio::AudioFrontendPhase::Closed)
                admission = Admission::Idle;
            else if (snapshot.output->phase == Audio::AudioFrontendPhase::Closing ||
                     snapshot.output->phase == Audio::AudioFrontendPhase::Retained)
                admission = Admission::Retiring;
            else
                admission = Admission::Running;
            published = std::move(snapshot);
        }

        /** @brief Consume one current critical Stop, including an intent arriving while native preparation was in progress. */
        void ApplyStop(EditorAudioPreviewHost &host, const std::uint64_t active, std::uint64_t &handled) {
            if (active != 0 && stopGeneration.load(std::memory_order_acquire) == active && handled != active) {
                host.Stop(host.Snapshot().workspace);
                handled = active;
            }
        }

        /** @brief Wait without holding exchange across any Audio work and consume at most one complete generation. */
        std::optional<Preparation> TakeRequest(const bool live, std::uint64_t &active, const std::uint64_t handledStop) {
            std::unique_lock lock(exchange);
            const auto ready = [&] {
                return closing.load(std::memory_order_acquire) || pendingStart.has_value() ||
                       (active != 0 && stopGeneration.load(std::memory_order_acquire) == active && handledStop != active);
            };
            if (live)
                (void)wake.wait_for(lock, std::chrono::milliseconds(5), ready);
            else
                wake.wait(lock, ready);
            if (closing.load(std::memory_order_acquire) || !pendingStart)
                return std::nullopt;
            auto request = std::move(pendingStart);
            pendingStart.reset();
            active = generation;
            return request;
        }

        /** @brief Run all actual Audio work on this owner; binding outlives the host so native teardown retains its request context. */
        void RunHost(EditorAudioPreviewHost &host, std::optional<Telemetry::ScopedOperationContext> &binding) {
            std::uint64_t active{}, handledStop{};
            bool live{};
            while (!closing.load(std::memory_order_acquire)) {
                auto request = TakeRequest(live, active, handledStop);
                if (closing.load(std::memory_order_acquire))
                    break;
                if (request) {
                    binding.reset();
                    binding.emplace(request->context);
                    if (stopGeneration.load(std::memory_order_acquire) == active) {
                        Publish({.workspace = request->request.workspace,
                                 .revision = request->request.revision,
                                 .object = request->request.object},
                                active);
                        handledStop = active;
                        live = false;
                        continue;
                    }
                    (void)host.Start(request->request);
                }
                // Preparation may block on native acquisition. Reconcile Stop/shutdown BEFORE
                // the first startup pump, so revocation cannot authorize a transient Rendering.
                if (closing.load(std::memory_order_acquire))
                    break;
                ApplyStop(host, active, handledStop);
                (void)host.Pump();
                auto snapshot = host.Snapshot();
                live = snapshot.output && snapshot.output->phase != Audio::AudioFrontendPhase::Closed;
                Publish(std::move(snapshot), active);
            }
        }

        /** @brief Retain a terminal control failure; inner-host unwinding must already have proved native retirement or terminated. */
        void Failed(const Error &error) {
            std::lock_guard lock(exchange);
            published.failure = error;
            published.preparation = AudioComponentPreviewPreparation::None;
            admission = Admission::Retiring;
        }

        /** @brief Entry owns both SDK apartment creation and final resource destruction, without borrowing UI/scene objects. */
        void Run() noexcept {
            try {
                std::optional<Telemetry::ScopedOperationContext> binding;
                EditorAudioPreviewHost host{owners, std::move(factory)};
                RunHost(host, binding);
            } catch (const std::bad_alloc &) {
                Failed(MakeError(Audio::AudioErrors::MemoryAllocationFailed));
            } catch (const std::exception &error) {
                Failed(MakeError(Audio::AudioErrors::BackendFailed, error.what()));
            } catch (...) {
                Failed(MakeError(Audio::AudioErrors::BackendFailed));
            }
            {
                std::lock_guard lock(exchange);
                done = true;
            }
            finished.notify_one();
        }

        /** @brief Harvest published values without waiting for native work or a control publication. */
        AudioComponentPreviewSnapshot Read() {
            std::unique_lock lock(exchange, std::try_to_lock);
            if (lock.owns_lock())
                view = published;
            return view;
        }
    };

    /** @copydoc EditorAudioPreviewControlHost::EditorAudioPreviewControlHost */
    EditorAudioPreviewControlHost::EditorAudioPreviewControlHost(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    /** @copydoc EditorAudioPreviewControlHost::Create */
    Result<std::unique_ptr<EditorAudioPreviewControlHost>> EditorAudioPreviewControlHost::Create(
        Audio::Backend::AudioRuntimeOwnerIssuer &owners, EditorAudioPreviewOutputFactory factory) {
        using Outcome = Result<std::unique_ptr<EditorAudioPreviewControlHost>>;
        if (!factory)
            return Outcome::Failure(MakeError(AudioComponentPreviewErrors::ProviderUnavailable));
        try {
            auto state = std::make_unique<State>(owners);
            state->factory = std::move(factory);
            std::unique_ptr<EditorAudioPreviewControlHost> owner{new EditorAudioPreviewControlHost(std::move(state))};
            auto *pinned = owner->state_.get();
            pinned->worker = std::thread([pinned] {
                pinned->Run();
            });
            return Outcome::Success(std::move(owner));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(Audio::AudioErrors::MemoryAllocationFailed));
        } catch (const std::system_error &error) {
            return Outcome::Failure(MakeError(AudioComponentPreviewErrors::ProviderUnavailable,
                                              std::string{"Audio control owner creation failed: "} + error.what() + " (" +
                                                  std::to_string(error.code().value()) + ")"));
        }
    }

    /** @copydoc EditorAudioPreviewControlHost::~EditorAudioPreviewControlHost */
    EditorAudioPreviewControlHost::~EditorAudioPreviewControlHost() {
        if (!state_->worker.joinable())
            return;
        {
            // Pair shutdown publication with the wait mutex, so an idle owner cannot miss its wake.
            std::lock_guard lock(state_->exchange);
            state_->closing.store(true, std::memory_order_release);
        }
        state_->wake.notify_one();
        std::unique_lock lock(state_->exchange);
        if (!state_->finished.wait_for(lock, std::chrono::seconds(5), [this] {
            return state_->done;
        }))
            std::terminate();  // Keep the island alive; time alone never proves native or control-thread retirement.
        lock.unlock();
        state_->worker.join();
    }

    /** @copydoc EditorAudioPreviewControlHost::Start */
    Result<void> EditorAudioPreviewControlHost::Start(const AudioComponentPreviewRequest &request) {
        if (!request.workspace.IsValid() || request.revision == 0 || request.object == 0)
            return Result<void>::Failure(MakeError(AudioComponentPreviewErrors::Stale));
        std::unique_lock lock(state_->exchange, std::try_to_lock);
        if (!lock.owns_lock())
            return Result<void>::Failure(MakeError(AudioComponentPreviewErrors::Busy));
        if (state_->closing.load(std::memory_order_acquire) || state_->done)
            return state_->published.failure ? Result<void>::Failure(*state_->published.failure)
                                             : Result<void>::Failure(MakeError(AudioComponentPreviewErrors::ProviderUnavailable));
        if (state_->admission != State::Admission::Idle)
            return Result<void>::Failure(MakeError(AudioComponentPreviewErrors::Busy));
        if (state_->generation == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(Audio::AudioErrors::HandleGenerationExhausted));
        try {
            State::Preparation copied{request, Telemetry::CaptureOperationContext()};
            state_->pendingStart.emplace(std::move(copied));
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(Audio::AudioErrors::MemoryAllocationFailed));
        }
        state_->uiGeneration = ++state_->generation;
        state_->admission = State::Admission::Reserved;
        state_->published = {.workspace = request.workspace,
                             .revision = request.revision,
                             .object = request.object,
                             .preparation = AudioComponentPreviewPreparation::Pending};
        state_->view = state_->published;
        lock.unlock();
        state_->wake.notify_one();
        return Result<void>::Success();
    }

    /** @copydoc EditorAudioPreviewControlHost::Stop */
    void EditorAudioPreviewControlHost::Stop(const Assets::AssetId workspace) noexcept {
        if (state_->uiGeneration != 0 && state_->view.workspace == workspace) {
            state_->stopGeneration.store(state_->uiGeneration, std::memory_order_release);
            state_->wake.notify_one();
        }
    }

    /** @copydoc EditorAudioPreviewControlHost::Pump */
    Result<void> EditorAudioPreviewControlHost::Pump() {
        const auto snapshot = state_->Read();
        return snapshot.failure ? Result<void>::Failure(*snapshot.failure) : Result<void>::Success();
    }

    /** @copydoc EditorAudioPreviewControlHost::Snapshot */
    AudioComponentPreviewSnapshot EditorAudioPreviewControlHost::Snapshot() const {
        return state_->Read();
    }
}  // namespace Horo::Editor
