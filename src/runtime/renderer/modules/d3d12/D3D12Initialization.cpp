#include "D3D12Initialization.h"

#include <algorithm>
#include <format>
#include <set>

namespace Horo::Render {
    namespace {
        /** @brief Qualifies hardware in host DXGI order; exact requests never try another adapter. */
        Result<RenderAdapterId> CreateSelectedDevice(ID3D12InitializationRuntime &runtime, ID3D12HostAdmission &host,
                                                     const std::vector<D3D12AdapterCandidate> &adapters,
                                                     const D3D12InitializationRequest &request) {
            std::optional<Error> unsupported;
            for (const auto &adapter : adapters) {
                if ((request.adapter && adapter.id != *request.adapter) || adapter.remote ||
                    (adapter.software && !request.allowSoftwareForTests)) {
                    continue;
                }
                if (request.cancellation.IsCancellationRequested()) {
                    return Result<RenderAdapterId>::Failure(
                        D3D12InitializationError("render.d3d12.cancelled", "D3D12 adapter qualification was cancelled."));
                }
                if (auto admitted = host.AdmitDriver(adapter.id, adapter.driverVersion); admitted.HasError()) {
                    return Result<RenderAdapterId>::Failure(std::move(admitted).ErrorValue());
                }
                if (request.cancellation.IsCancellationRequested()) {
                    return Result<RenderAdapterId>::Failure(
                        D3D12InitializationError("render.d3d12.cancelled", "D3D12 initialization was cancelled before device creation."));
                }
                auto created = runtime.CreateDevice(adapter.id);
                if (created.HasValue()) {
                    return Result<RenderAdapterId>::Success(adapter.id);
                }
                if (const Error &error = created.ErrorValue();
                    request.adapter || (error.code.Value() != "render.d3d12.feature_level_unsupported" &&
                                        error.code.Value() != "render.d3d12.shader_model_unsupported")) {
                    return Result<RenderAdapterId>::Failure(error);
                }
                unsupported = std::move(created).ErrorValue();
            }
            return Result<RenderAdapterId>::Failure(unsupported.value_or(
                D3D12InitializationError("render.d3d12.adapter_unavailable",
                                         "Select an available hardware adapter; no automatic software fallback is allowed.")));
        }

        /** @brief Creates only explicitly required queues with cancellation between native calls. */
        Result<void> CreateQueues(ID3D12InitializationRuntime &runtime, const D3D12InitializationRequest &request) {
            using enum D3D12QueueKind;
            for (const auto kind : {Direct, Compute, Copy}) {
                if ((kind == Compute && !request.enableComputeQueue) || (kind == Copy && !request.enableCopyQueue)) {
                    continue;
                }
                if (request.cancellation.IsCancellationRequested()) {
                    return Result<void>::Failure(
                        D3D12InitializationError("render.d3d12.cancelled", "D3D12 initialization was cancelled during queue setup."));
                }
                auto queue = runtime.CreateQueue(kind);
                if (queue.HasError()) {
                    return queue;
                }
            }
            return Result<void>::Success();
        }

        /** @brief Acquires bounded adapter/device/queue state under the caller's rollback transaction. */
        Result<RenderAdapterId> CreateNativeSession(ID3D12InitializationRuntime &runtime, ID3D12HostAdmission &host,
                                                    const D3D12InitializationRequest &request) {
            const auto fail = [](const char *code, const char *message) {
                return Result<RenderAdapterId>::Failure(D3D12InitializationError(code, message));
            };
            if (auto opened = runtime.Open(request.enableDebugLayer); opened.HasError()) {
                return Result<RenderAdapterId>::Failure(std::move(opened).ErrorValue());
            }
            if (request.cancellation.IsCancellationRequested()) {
                return fail("render.d3d12.cancelled", "D3D12 initialization was cancelled before adapter discovery.");
            }
            auto enumeration = runtime.Enumerate(request.maxAdapters);
            if (enumeration.HasError()) {
                return Result<RenderAdapterId>::Failure(std::move(enumeration).ErrorValue());
            }
            const auto &adapters = enumeration.Value();
            if (std::set<RenderAdapterId> identities;
                adapters.size() > request.maxAdapters || !std::ranges::all_of(adapters, [&](const auto &adapter) {
                return adapter.id.IsValid() && identities.insert(adapter.id).second;
            })) {
                return fail("render.d3d12.invalid_adapters",
                            "Refresh discovery; the driver returned invalid or excessive adapter identities.");
            }
            auto device = CreateSelectedDevice(runtime, host, adapters, request);
            if (device.HasError()) {
                return Result<RenderAdapterId>::Failure(std::move(device).ErrorValue());
            }
            if (auto queues = CreateQueues(runtime, request); queues.HasError()) {
                return Result<RenderAdapterId>::Failure(std::move(queues).ErrorValue());
            }
            if (request.cancellation.IsCancellationRequested()) {
                return fail("render.d3d12.cancelled", "D3D12 initialization was cancelled before publishing device readiness.");
            }
            return device;
        }
    }  // namespace

    /** @copydoc D3D12InitializationError */
    Error D3D12InitializationError(const char *code, const char *message, const std::int64_t nativeCause) {
        Error error{ErrorCode{code}, ErrorDomainId{"horo.render.d3d12"}, ErrorSeverity::Error, message, {}};
        if (nativeCause != 0) {
            error.message += std::format(" (HRESULT {}).", nativeCause);
        }
        return error;
    }

    /** @brief Keeps failed attempts retryable and releases acquired state exactly once. */
    struct D3D12Initialization::Rollback {
        ID3D12InitializationRuntime &runtime;
        State &state;
        bool acquired{false};
        bool committed{false};

        Rollback(ID3D12InitializationRuntime &ownedRuntime, State &sessionState) : runtime(ownedRuntime), state(sessionState) {}

        Rollback(const Rollback &) = delete;
        Rollback &operator=(const Rollback &) = delete;
        Rollback(Rollback &&) = delete;
        Rollback &operator=(Rollback &&) = delete;

        ~Rollback() {
            if (!committed) {
                if (acquired) {
                    runtime.Release();
                }
                state = State::Idle;
            }
        }
    };

    /** @copydoc D3D12Initialization::D3D12Initialization */
    D3D12Initialization::D3D12Initialization(std::unique_ptr<ID3D12InitializationRuntime> runtime) : runtime_(std::move(runtime)) {
        HORO_INVARIANT_MSG(runtime_ != nullptr, "D3D12 initialization requires an owned runtime.");
    }

    D3D12Initialization::~D3D12Initialization() {
        HORO_INVARIANT_MSG(owner_ == std::this_thread::get_id(), "Destroy D3D12 initialization on its owner thread.");
        runtime_->Release();
    }

    /** @copydoc D3D12Initialization::Initialize */
    Result<RenderAdapterId> D3D12Initialization::Initialize(ID3D12HostAdmission &host, const D3D12InitializationRequest &request) {
        const auto fail = [](const char *code, const char *message) {
            return Result<RenderAdapterId>::Failure(D3D12InitializationError(code, message));
        };
        if (owner_ != std::this_thread::get_id()) {
            return fail("render.d3d12.wrong_thread", "Initialize D3D12 on the host render-capable owner thread.");
        }
        if (state_ != State::Idle) {
            return fail("render.d3d12.invalid_state", "Create a new session after shutdown; do not initialize a ready device twice.");
        }
        if (request.maxAdapters == 0 || request.maxAdapters > 64 || (request.adapter && !request.adapter->IsValid())) {
            return fail("render.d3d12.invalid_request", "Use a valid machine-local adapter and an enumeration limit from 1 to 64.");
        }
        if (request.cancellation.IsCancellationRequested()) {
            return fail("render.d3d12.cancelled", "D3D12 initialization was cancelled before native acquisition.");
        }
        state_ = State::Initializing;

        Rollback rollback{*runtime_, state_};

        if (auto verified = host.VerifyRuntime(); verified.HasError()) {
            return Result<RenderAdapterId>::Failure(std::move(verified).ErrorValue());
        }
        if (request.cancellation.IsCancellationRequested()) {
            return fail("render.d3d12.cancelled", "D3D12 initialization was cancelled after host verification.");
        }

        rollback.acquired = true;
        auto device = CreateNativeSession(*runtime_, host, request);
        if (device.HasError()) {
            return device;
        }
        state_ = State::Ready;
        rollback.committed = true;
        return device;
    }

    /** @copydoc D3D12Initialization::Shutdown */
    Result<void> D3D12Initialization::Shutdown() {
        if (owner_ != std::this_thread::get_id()) {
            return Result<void>::Failure(D3D12InitializationError("render.d3d12.wrong_thread", "Shut down D3D12 on its owner thread."));
        }
        if (state_ == State::Initializing) {
            return Result<void>::Failure(D3D12InitializationError("render.d3d12.initialization_in_progress",
                                                                  "Complete or cancel initialization before shutting down its session."));
        }
        runtime_->Release();
        state_ = State::Stopped;
        return Result<void>::Success();
    }
}  // namespace Horo::Render
