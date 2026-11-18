#pragma once

/** @file NativeAudioDriver.h
 * @brief Non-installed native primitives beneath the shared Horo lifecycle and PCM adapter.
 */

#include "Horo/Audio/Internal/AudioBackend.h"

#include <optional>

namespace Horo::Audio::Backend {
    /**
     * @brief Resolve an admitted exact-rate float tuple and complete semantic channel permutation.
     * @param request Preserved open policy and epoch.
     * @param native Actual native semantic channel order and rate.
     * @param revision Complete discovery revision used to open the device.
     * @param period Actual native/Horo callback period in frames.
     * @return Candidate or typed unsupported failure; no resampling, remapping loss or implicit layout conversion.
     */
    [[nodiscard]] Result<AudioNegotiatedDeviceFormat> NegotiateNativeFloatOutput(const Open &request, const AudioProcessingFormat &native,
                                                                                 std::uint64_t revision, std::uint32_t period);

    /** @brief Retained adapter entry; native driver serializes all calls and writes no state after Stop success. */
    struct NativeAudioRenderPort final {
        void *context{};
        void (*process)(void *, std::span<float>, std::uint32_t) noexcept {};
    };

    /** @brief Native retirement progress; Pending preserves the entire ownership island and is never detachment proof. */
    enum class NativeRetirementProgress : std::uint8_t {
        Pending,
        Complete
    };

    /** @brief A bounded retirement poll, carrying either explicit progress or a preserved native failure. */
    using NativeRetirementResult = Result<NativeRetirementProgress>;

    /**
     * @brief Equal-peer native primitives; no platform API crosses this non-installed Horo boundary.
     *
     * Control methods run on one declared owner thread. Open prepares native interleaved binary32 at the
     * reported semantic rate/layout, with each native render request bounded by MaximumAudioCallbackFrames.
     * Open/Start failures report their remaining ownership through Resources. Stop Complete proves
     * all readers finished and future entry impossible; Pending/failure retains all callback/native state. Close
     * is called only after successful Stop or an Open that never started. Driver factories are inert.
     * Native callbacks may only render bounded PCM or latch preallocated native facts; they never allocate,
     * log, enumerate or invoke recovery. No driver wraps another concrete Horo backend.
     */
    class NativeAudioDriver {
    public:
        virtual ~NativeAudioDriver() = default;
        /** @brief Fixed concrete peer identity. @return WASAPI, CoreAudio or PipeWire. */
        [[nodiscard]] virtual AudioBackendKind Kind() const noexcept = 0;
        /** @brief Read native ownership without OS calls. @return Closed, CallbackDetached, or Retained until callback/listener proof. */
        [[nodiscard]] virtual ResourceDisposition Resources() const noexcept = 0;
        /** @brief Read required native-fact backpressure without OS calls or mutation. @return True while loss must be accepted before a
         * new Open/Start/activation can reuse epoch state. */
        [[nodiscard]] virtual bool HasUndrainedRequiredFacts() const noexcept = 0;
        /** @brief Query native service availability off-callback. @return Complete probe or native-cause failure. */
        [[nodiscard]] virtual Result<AudioBackendProbe> Probe() = 0;
        /** @brief Enumerate complete owned facts preserving unchanged IDs. @return Catalog or failure; no partial publication. */
        [[nodiscard]] virtual Result<AudioDeviceSnapshot> Enumerate() = 0;
        /** @brief Acquire a stopped float output; failure attempts cleanup and reports unproven retirement through Resources. @param
         * request Preserved intent. @return Candidate. */
        [[nodiscard]] virtual Result<Opened> Open(const Backend::Open &request) = 0;
        /** @brief Attach the retained adapter; Resources distinguishes detached failure from retained native readers. @param render
         * Retained serial entry. @return Native start result.
         */
        [[nodiscard]] virtual Result<void> Start(NativeAudioRenderPort render) = 0;
        /** @brief Poll native stop/join without turning Pending into detachment proof. @param deadline Host steady-clock deadline.
         * @return Complete only after all native readers retire; Pending/failure retains port, driver, workers and code. */
        [[nodiscard]] virtual NativeRetirementResult Stop(AudioMonotonicTimestamp deadline) = 0;
        /** @brief Release detached/open-only resources. @return Complete only after native cleanup, otherwise Pending or failure. */
        [[nodiscard]] virtual NativeRetirementResult Close() = 0;
        /** @brief Drain bounded native facts on control. @param output Caller storage. @return Written count. */
        [[nodiscard]] virtual std::size_t DrainEvents(std::span<Event> output) noexcept = 0;
    };
}  // namespace Horo::Audio::Backend
