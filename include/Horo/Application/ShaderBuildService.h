#pragma once

/** @file ShaderBuildService.h
 * @brief Worker-only shader compilation projected into the host's Build Output store. */

#include "Horo/Foundation/BuildOutputStore.h"
#include "Horo/Runtime/Render/ShaderCompilerPipeline.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Horo::Application {
    /** @brief Host-admitted mapping from an immutable compiler identity to a navigable source. */
    struct ShaderDiagnosticSource final {
        std::string logicalPath;
        std::filesystem::path absolutePath;
    };

    /** @brief Owned compilation inputs and explicit navigation/correlation authority. */
    struct ShaderBuildRequest final {
        Render::ShaderCompilationRequest compilation;
        Render::ShaderCompilerLimits limits;
        std::vector<ShaderDiagnosticSource> sources;
        std::optional<OperationId> operationId;
    };

    namespace ShaderBuildErrors {
        extern const ErrorCodeDescriptor OwnerThread;    /**< Compilation was requested on the composing thread. */
        extern const ErrorCodeDescriptor Closed;         /**< Producer admission has closed for shutdown. */
        extern const ErrorCodeDescriptor Capacity;       /**< The finite concurrent compiler envelope is full. */
        extern const ErrorCodeDescriptor InvalidSources; /**< Navigation mappings are malformed, duplicated or oversized. */
    }  // namespace ShaderBuildErrors

    /**
     * @brief Explicit host producer over an injected compiler and the existing Build Output store.
     * @details Compile runs synchronously on a host-scheduled worker, never on the constructing
     * thread. At most eight calls are admitted concurrently. The service owns cancellation
     * ancestry, not worker threads; shutdown closes admission, cancels and drains admitted calls.
     * The adapter must support concurrent calls, honor cooperative cancellation and enforce a
     * finite deadline for every external operation. An arbitrary non-cooperative adapter cannot
     * be drained within a bounded time; the service never detaches or abandons admitted work.
     * No renderer backend or executable is discovered.
     */
    class ShaderBuildService final {
    public:
        /**
         * @brief Composes the producer with the sole process/project output authority.
         * @param adapter Shared host-selected compiler, or absent for typed tool-unavailable results.
         * @param output Borrowed store which must outlive the service and all admitted calls.
         */
        ShaderBuildService(std::shared_ptr<const Render::IShaderCompilerAdapter> adapter, BuildOutputStore &output);
        /** @brief Cancels and drains all admitted calls before releasing compiler ownership. */
        ~ShaderBuildService();
        ShaderBuildService(const ShaderBuildService &) = delete;
        ShaderBuildService &operator=(const ShaderBuildService &) = delete;

        /**
         * @brief Compiles on the calling worker with one output session and at most one terminal record.
         * @param request Owned immutable compilation, source mappings and optional application operation.
         * @param cancellation Parent operation cancellation, observed together with service shutdown.
         * @return Complete artifacts or typed failure; diagnostics already emitted remain visible on failure.
         * @throws std::bad_alloc If even construction of the typed allocation-failure result cannot allocate.
         * @details A store allocation failure returns AllocationFailed without retrying a possibly started
         * terminal append. Successful return requires successful terminal publication.
         * @pre Caller and store remain alive until this call returns. The constructing thread must not call Compile.
         */
        [[nodiscard]] Result<Render::ShaderCompilationBatch> Compile(ShaderBuildRequest request,
                                                                     const CancellationToken &cancellation = {}) const;
        /**
         * @brief Closes admission, cancels and waits for every admitted synchronous compiler call.
         * @pre Called outside Compile, before the host joins/stops its workers or destroys the output store.
         * @details Idempotent; concrete process timeouts bound the drain when cancellation needs process termination.
         */
        void Shutdown() const noexcept;

    private:
        struct State;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Application
