#pragma once

/** @file D3D12Initialization.h
 * @brief Private, native-free D3D12 device initialization and host admission seam.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Runtime/Render/RenderAdapter.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace Horo::Render {
    /** @brief Optional independent queues; the direct graphics queue is always required. */
    enum class D3D12QueueKind {
        Direct,
        Compute,
        Copy
    };

    /** @brief Exact machine-local selection and diagnostic configuration. */
    struct D3D12InitializationRequest {
        std::optional<RenderAdapterId> adapter;
        std::uint32_t maxAdapters{16};
        bool enableDebugLayer{false};
        bool enableComputeQueue{false};
        bool enableCopyQueue{false};
        bool allowSoftwareForTests{false};
        CancellationToken cancellation;
    };

    /**
     * @brief Host-owned trust and driver policy, borrowed until initialization completes.
     *
     * VerifyRuntime must validate the executable's fixed SDK 619 / package 1.619.5
     * activation contract and verified payload before any native D3D12 use. It
     * returns repair/update errors rather than relying on inbox runtime fallback.
     * This component never selects paths, downloads dependencies, or exports SDK settings.
     */
    class ID3D12HostAdmission {
    public:
        virtual ~ID3D12HostAdmission() = default;
        /** @brief Validates the fixed host/runtime release contract. */
        [[nodiscard]] virtual Result<void> VerifyRuntime() = 0;
        /** @brief Applies versioned restrictive driver policy to the exact adapter and driver. */
        [[nodiscard]] virtual Result<void> AdmitDriver(const RenderAdapterId &adapter, std::uint64_t driverVersion) = 0;
    };

    /** @brief Native-free discovery facts in DXGI default preference order. */
    struct D3D12AdapterCandidate {
        RenderAdapterId id;
        std::uint64_t driverVersion{0};
        bool software{false};
        bool remote{false};
    };

    /**
     * @brief Backend-private runtime seam for deterministic failure and lifecycle coverage.
     * Calls run synchronously on the render-capable thread with host-initialized COM.
     * Release is idempotent, tolerates partial acquisition, and releases queues before
     * device, adapters and factory. No command submission is exposed by this ticket,
     * so teardown has no outstanding GPU work and performs no GPU wait.
     */
    class ID3D12InitializationRuntime {
    public:
        virtual ~ID3D12InitializationRuntime() = default;
        /** @brief Checks OS/architecture/COM and creates the factory after explicit debug setup. */
        [[nodiscard]] virtual Result<void> Open(bool enableDebugLayer) = 0;
        /** @brief Enumerates a finite set without creating devices; overflow is a typed failure. */
        [[nodiscard]] virtual Result<std::vector<D3D12AdapterCandidate>> Enumerate(std::uint32_t maxAdapters) = 0;
        /** @brief Creates the exact adapter at FL 12_0 and independently requires Shader Model 6.0.
         * Failure releases the attempted device. Baseline rejections use feature_level_unsupported
         * or shader_model_unsupported so automatic selection may qualify the next hardware adapter.
         */
        [[nodiscard]] virtual Result<void> CreateDevice(const RenderAdapterId &adapter) = 0;
        /** @brief Creates one explicitly requested queue; failure retains the native cause. */
        [[nodiscard]] virtual Result<void> CreateQueue(D3D12QueueKind kind) = 0;
        /** @brief Releases every acquired native object, including partial device/queue state. */
        virtual void Release() noexcept = 0;
    };

    /**
     * @brief Owns a startup-only D3D12 device session on its construction thread.
     *
     * The host must construct, initialize, stop and destroy this object on its
     * render-capable owner thread. No async work or borrowed native handles escape.
     * Failed/cancelled initialization rolls back and may be retried; Shutdown closes
     * admission permanently. Device-ready here does not imply presentation or parity.
     */
    class D3D12Initialization final {
    public:
        /** @brief Takes exclusive ownership of an inert runtime. @param runtime Non-null owned runtime. */
        explicit D3D12Initialization(std::unique_ptr<ID3D12InitializationRuntime> runtime);
        ~D3D12Initialization();
        D3D12Initialization(const D3D12Initialization &) = delete;
        D3D12Initialization &operator=(const D3D12Initialization &) = delete;
        /** @brief Initializes once; preserves host/native errors and rolls back any failed attempt.
         * @param host Borrowed runtime/driver admission policy.
         * @param request Exact selection, queue and cancellation settings.
         * @return Selected adapter, or a typed startup error.
         */
        [[nodiscard]] Result<RenderAdapterId> Initialize(ID3D12HostAdmission &host, const D3D12InitializationRequest &request);
        /** @brief Releases native state and closes admission. @return Wrong-thread error or success. */
        [[nodiscard]] Result<void> Shutdown();

    private:
        enum class State {
            Idle,
            Initializing,
            Ready,
            Stopped
        };
        struct Rollback;
        std::unique_ptr<ID3D12InitializationRuntime> runtime_;
        std::thread::id owner_{std::this_thread::get_id()};
        State state_{State::Idle};
    };

    /** @brief Creates an inert native runtime, or an unsupported-host runtime outside native Windows x64. */
    [[nodiscard]] std::unique_ptr<ID3D12InitializationRuntime> CreateD3D12InitializationRuntime();

    /** @brief Produces a backend-scoped actionable error preserving stage and optional HRESULT. */
    [[nodiscard]] Error D3D12InitializationError(const char *code, const char *message, std::int64_t nativeCause = 0);
}  // namespace Horo::Render
