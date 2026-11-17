#pragma once

/** @file PipeWireAudioBackend.h
 * @brief Inert Linux PipeWire peer; PipeWire and SPA types remain private.
 */

#include "Horo/Audio/Internal/NativeAudioBackend.h"

namespace Horo::Audio::Backend {
    /** @brief Explicit process-owned native library lease, retained by every PipeWire driver. */
    class PipeWireAudioLibrary final {
    public:
        PipeWireAudioLibrary(const PipeWireAudioLibrary &) = delete;
        PipeWireAudioLibrary &operator=(const PipeWireAudioLibrary &) = delete;
        ~PipeWireAudioLibrary();

    private:
        PipeWireAudioLibrary() = default;
        bool initialized_{};
        friend Result<std::shared_ptr<PipeWireAudioLibrary>> InitializePipeWireAudioLibrary();
    };

    /**
     * @brief Explicitly initialize native PipeWire support at the process composition root.
     * @return Shared native-library lease or allocation failure.
     * @pre The process host calls this once on its composition thread and shares the returned lease.
     * No other component calls pw_init/pw_deinit independently. Last release occurs after all stopped drivers.
     */
    [[nodiscard]] Result<std::shared_ptr<PipeWireAudioLibrary>> InitializePipeWireAudioLibrary();
    /**
     * @brief Construct one explicit PipeWire output peer without connecting to the service.
     * @param config Fixed runtime and host steady-clock domain.
     * @param library Explicit process library lease, retained through native callback detachment.
     * @return Inert backend or typed validation/allocation failure.
     * @pre One declared control thread owns driver methods. This baseline processes on its private
     * PipeWire loop; RT_PROCESS is deliberately absent so loop stop/join proves callback detachment.
     * An owned retirement worker may acquire that loop lock and stop/join it; control polls actual
     * worker exit without blocking. Pending retirement retains the complete driver, port and library lease.
     * Realtime data-thread scheduling remains unqualified until a separate data-loop detach proof exists.
     */
    [[nodiscard]] Result<std::unique_ptr<NativeAudioBackend>> CreatePipeWireAudioBackend(NativeAudioBackendConfig config,
                                                                                         std::shared_ptr<PipeWireAudioLibrary> library);
}  // namespace Horo::Audio::Backend
