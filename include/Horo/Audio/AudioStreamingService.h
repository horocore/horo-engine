#pragma once

/**
 * @file AudioStreamingService.h
 * @brief Bounded worker-filled audio streams and allocation-free callback consumption.
 */

#include "Horo/Assets/AssetId.h"
#include "Horo/Audio/AudioStreamDecoder.h"
#include "Horo/Audio/AudioVoiceControls.h"
#include "Horo/Foundation/JobSystem.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Horo::Audio {
    struct AudioStreamState;

    /**
     * @brief Package-backed source opened only by a host worker job.
     * @details Open resolves its private state with context.Get<T>() and returns a typed error on mismatch before I/O.
     * The host retains the object and provider code until shutdown completes; context itself grants no ownership.
     */
    struct AudioStreamPackageSource final {
        using OpenFunction = Result<AudioStreamDecoder> (*)(const BorrowedCallbackContext &context, Assets::AssetId asset,
                                                            const AudioStreamDecoderSpec &expected, std::size_t maximumPackageBytes,
                                                            const CancellationToken &cancellation);
        BorrowedCallbackContext context{};      /**< Type-checked borrowed host provider, retained until service shutdown. */
        OpenFunction open{};                    /**< Opens immutable cooked media; must observe cancellation during bounded I/O. */
        std::shared_ptr<const void> ownerLease; /**< Pins external provider code through every fill and decoder release. */
    };

    /** @brief Fixed process admission limits; all stream storage is charged before publication. */
    struct AudioStreamingLimits final {
        std::uint32_t maximumStreams{16};
        std::uint32_t maximumConcurrentFills{4};
        std::size_t maximumBytes{64U << 20U};
        std::uint32_t joinTimeoutMilliseconds{5'000}; /**< Host control-lane cancellation drain deadline. */
    };

    /** @brief Starvation policy selected before a stream becomes callback-visible. */
    enum class AudioStreamUnderrunPolicy : std::uint8_t {
        ContinueWithSilence,
        StopWithSilence,
    };

    /** @brief One immutable package generation and its prefetch reservation. */
    struct AudioStreamRequest final {
        Assets::AssetId asset;
        AudioStreamDecoderSpec decoder;
        std::uint32_t ringFrames{};        /**< Capacity, at least two decode blocks. */
        std::uint32_t lookaheadFrames{};   /**< Pump fills until this many frames are ready. */
        std::size_t maximumPackageBytes{}; /**< Worst-case retained package generation, charged globally. */
        std::uint8_t priority{};           /**< Higher values are scheduled first, ties by slot. */
        AudioStreamUnderrunPolicy underrunPolicy{AudioStreamUnderrunPolicy::ContinueWithSilence};
    };

    /** @brief Service-local generation-checked stream identity. */
    struct AudioStreamHandle final {
        std::uint32_t slot{};
        std::uint64_t generation{};

        [[nodiscard]] bool IsValid() const noexcept {
            return slot != 0 && generation != 0;
        }

        auto operator<=>(const AudioStreamHandle &) const noexcept = default;
    };

    /** @brief Fixed callback result; diagnostics are projected by control outside the callback. */
    struct AudioStreamRenderResult final {
        std::uint32_t availableFrames{};
        std::uint32_t silentFrames{};
        bool ended{};
        bool stopped{};
    };

    /**
     * @brief Borrowed callback endpoint for one admitted stream generation.
     * @details The host must detach every callback using this port before calling Retire or Shutdown.
     * Exactly one port and one callback consumer exist per stream generation. Render only reads
     * prepublished ring samples and writes the supplied planar ADR-063 planes. The service requires
     * lock-free pointer, bool and 64-bit atomics on the target; unsupported targets fail compilation.
     */
    class AudioStreamRenderPort final {
    public:
        AudioStreamRenderPort(const AudioStreamRenderPort &) = delete;
        AudioStreamRenderPort &operator=(const AudioStreamRenderPort &) = delete;

        /** @brief Release an optional retained-port pin on detached control only; never destroy on callback. */
        ~AudioStreamRenderPort();

        AudioStreamRenderPort(AudioStreamRenderPort &&other) noexcept
            : state_(std::exchange(other.state_, nullptr)), retained_(std::exchange(other.retained_, false)) {}

        AudioStreamRenderPort &operator=(AudioStreamRenderPort &&) = delete;
        /**
         * @brief Consumes up to the requested frame count without allocation, locks, decoding or I/O.
         * @param planes Exclusive writable planes in the stream's declared channel order.
         * @param frames Number of frames writable in every plane, within the admitted callback limit.
         * @return Available and positive-zero silent frame counts with terminal state.
         * @pre Plane count and capacities match the admitted format; the host retains output storage.
         */
        [[nodiscard]] AudioStreamRenderResult Render(std::span<AudioSample *const> planes, std::uint32_t frames) const noexcept;

        /** @brief Request one worker-side reposition without decoding, waiting or allocating on callback.
         * @param frame Exact source frame in [0, admitted frameCount]; requires a seekable decoder.
         * @param loop Exclusive worker loop; disabled endpoints are zero, enabled end is within the source.
         * @return True if accepted; false for moved/stopped/nonseekable/out-of-range or outstanding work.
         * @details The sole callback consumer stops reading the ring before publishing this request.
         * Render emits uncounted preparation silence until PositionReady; control must continue Pump.
         * Pending work retains the port, ring, decoder and package lease through normal cancellation/join.
         */
        [[nodiscard]] bool RequestPosition(std::uint64_t frame, AudioVoiceLoop loop = {}) const noexcept;
        /** @brief Read worker acknowledgement, not a guarantee of buffered PCM.
         * @return True when no reposition is outstanding; a worker failure remains pending until control reconciles it.
         */
        [[nodiscard]] bool PositionReady() const noexcept;
        /** @brief Suspend future fills while retaining the ring and any already-running bounded fill.
         * @param suspended True for a virtual voice not awaiting realization; false before worker preparation.
         * @pre Sole callback owner; this is not cancellation or permission to reclaim storage.
         */
        void SuspendFills(bool suspended) const noexcept;
        /** @brief Observe control/underrun stop without consuming PCM.
         * @return True after stop or for a moved port. Worker failures remain control-owned snapshot facts.
         */
        [[nodiscard]] bool IsStopped() const noexcept;
        /** @brief Discard at most the currently published frames without reading PCM or recording an underrun.
         * @param frames Maximum bounded ring frames to consume.
         * @return Exact frames discarded; zero during reposition, on stop or for a moved port.
         * @pre Sole callback consumption, serialized with Render and RequestPosition.
         */
        [[nodiscard]] std::uint32_t Discard(std::uint32_t frames) const noexcept;
        /** @brief Read acquired available ring frames on the sole consumer, without advancing it.
         * @return Bounded available count, zero during positioning, stop or after move.
         */
        [[nodiscard]] std::uint32_t AvailableFrames() const noexcept;

    private:
        friend class AudioStreamingService;

        explicit AudioStreamRenderPort(AudioStreamState *state, bool retained = false) noexcept : state_(state), retained_(retained) {}

        AudioStreamState *state_{};
        bool retained_{}; /**< Control-only pin bookkeeping; Render never reads this flag. */
    };

    /** @brief Control-owner view of worker and callback facts. */
    struct AudioStreamSnapshot final {
        std::uint32_t bufferedFrames{};
        std::uint64_t underrunFrames{};
        std::uint64_t underrunCallbacks{};
        bool sourceEnded{};
        bool failed{};
        bool cancelled{};
        bool stopped{};
        std::optional<Error> failure; /**< Original worker failure, retained on control until retirement. */
    };

    /** @brief Control-only rate-limited starvation fact for diagnostics and telemetry. */
    struct AudioStreamUnderrunReport final {
        std::uint64_t missingFrames{};
        std::uint64_t callbackCount{};
        std::uint64_t observedSampleFrame{};
    };

    /**
     * @brief Owns bounded stream jobs, decoder sessions and rings on the audio control lane.
     * @details The injected JobSystem and package source outlive this service. Pump never waits and
     * schedules at most one fill per stream. All service methods require its single control owner
     * lane. Stop requests cancellation; Retire and Shutdown join workers only after the host has
     * detached callback ports. No method except Render is callback-safe. The host must complete
     * Shutdown while the injected JobSystem and package source remain alive.
     */
    class AudioStreamingService final {
        /** @brief Unforgeable factory authority; only Create can originate a key. */
        class ConstructionKey final {
            friend class AudioStreamingService;
            ConstructionKey() = default;

        public:
            ConstructionKey(const ConstructionKey &) = default;
        };

    public:
        /**
         * @brief Validates limits and reserves fixed stream slots without starting work.
         * @param jobs Process-owned worker scheduler.
         * @param source Host-approved package source with lifetime beyond this service.
         * @param limits Global stream count, concurrent fill and byte budgets.
         * @return Service or a typed invalid/capacity error.
         */
        [[nodiscard]] static Result<std::unique_ptr<AudioStreamingService>> Create(JobSystem &jobs, AudioStreamPackageSource source,
                                                                                   AudioStreamingLimits limits = {});

        /**
         * @brief Factory-only allocation seam for std::make_unique, not a host admission bypass.
         * @param key Private construction authority originated only after Create validates inputs.
         * @param jobs Process-owned worker scheduler retained by reference.
         * @param source Validated source transferred with its provider/code lease.
         * @param limits Validated finite stream storage and join budgets.
         * @throws std::bad_alloc When fixed slot allocation fails; Create translates this failure.
         * @pre Create has validated the source and limits. No worker is started during construction.
         */
        AudioStreamingService(ConstructionKey key, JobSystem &jobs, AudioStreamPackageSource source, AudioStreamingLimits limits);

        AudioStreamingService(const AudioStreamingService &) = delete;
        AudioStreamingService &operator=(const AudioStreamingService &) = delete;
        ~AudioStreamingService();

        /**
         * @brief Preallocates and admits one package stream on the control lane.
         * @param request Validated exact decoder facts and bounded prefetch policy.
         * @return Generation-checked identity or typed invalid/capacity error; no worker starts yet.
         */
        [[nodiscard]] Result<AudioStreamHandle> Admit(AudioStreamRequest request);
        /** @brief Borrows the sole callback endpoint. @param handle Live stream identity. @return Port or stale/duplicate error. */
        [[nodiscard]] Result<AudioStreamRenderPort> RenderPort(AudioStreamHandle handle);
        /** @brief Borrow the sole port with a control-owned retirement pin.
         * @param handle Live exact stream generation.
         * @return Port or typed stale/duplicate failure. Retire/Shutdown retain storage while this port exists.
         * @details The service must outlive the port. Move/destruction occur only on detached control;
         * destruction releases the pin without joining jobs or freeing ring storage. The host may then Retire.
         * Existing RenderPort callers retain their explicit host-detachment contract.
         */
        [[nodiscard]] Result<AudioStreamRenderPort> RetainedRenderPort(AudioStreamHandle handle);
        /** @brief Copy actual admitted decoder facts on control, never caller-trusted replacement metadata.
         * @param handle Live exact stream identity. @return Owned facts or typed stale failure.
         */
        [[nodiscard]] Result<AudioStreamDecoderSpec> DecoderSpec(AudioStreamHandle handle) const;
        /** @brief Reaps completed fills and admits priority-ordered lookahead jobs without waiting. */
        void Pump();
        /** @brief Requests cancellation and closes further fill admission. @param handle Live identity. @return Success or stale-handle
         * error. */
        [[nodiscard]] Result<void> Stop(AudioStreamHandle handle);
        /**
         * @brief Joins cancelled worker work and releases one stream after its callback port is detached.
         * @param handle Live identity with callback quiescence established by the host.
         * @return Success, stale-handle error or bounded worker-join interruption. On join interruption
         * the stream remains retained and may be retried; callback memory is never reclaimed.
         */
        [[nodiscard]] Result<void> Retire(AudioStreamHandle handle);
        /** @brief Reads bounded counters and terminal facts on control. @param handle Live identity. @return Snapshot or stale-handle
         * error. */
        [[nodiscard]] Result<AudioStreamSnapshot> Snapshot(AudioStreamHandle handle) const;
        /**
         * @brief Takes a coalesced underrun fact on control, never on the callback.
         * @param handle Live stream identity.
         * @param sampleFrame Current authoritative Audio sample-frame clock value.
         * @param minimumIntervalFrames Positive diagnostic interval in sample frames.
         * @return New cumulative-delta report when due, empty when none is due, or a typed invalid error.
         */
        [[nodiscard]] Result<std::optional<AudioStreamUnderrunReport>> TakeUnderrunReport(AudioStreamHandle handle,
                                                                                          std::uint64_t sampleFrame,
                                                                                          std::uint64_t minimumIntervalFrames);
        /**
         * @brief Stops admission, cancels and joins all jobs after backend callback detachment.
         * @pre The host has proved every borrowed render port quiescent.
         * @return Success or bounded join interruption. The host must retry or follow the process-fatal
         * retention policy before destroying an incompletely shut down service.
         */
        [[nodiscard]] Result<void> Shutdown();

    private:
        struct Slot;
        [[nodiscard]] AudioStreamState *Find(AudioStreamHandle handle) noexcept;
        [[nodiscard]] const AudioStreamState *Find(AudioStreamHandle handle) const noexcept;
        /** @brief Issue the sole callback port after one shared liveness/duplicate check, with optional control retirement pin. */
        [[nodiscard]] Result<AudioStreamRenderPort> IssueRenderPort(AudioStreamHandle handle, bool retained);

        JobSystem &jobs_;
        AudioStreamPackageSource source_;
        AudioStreamingLimits limits_;
        std::vector<Slot> slots_;
        std::size_t reservedBytes_{};
        bool closed_{};
        bool shutdownComplete_{};
    };
}  // namespace Horo::Audio
