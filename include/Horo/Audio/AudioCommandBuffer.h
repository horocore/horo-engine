#pragma once

/** @file AudioCommandBuffer.h
 * @brief Bounded single-control-producer, single-callback-consumer command publication.
 */

#include "Horo/Audio/AudioCommands.h"

#include <memory>

namespace Horo::Audio {
    inline constexpr std::uint32_t MaximumAudioCommandSlots = 65'536;

    /** @brief Ordered normalized intent; gaps may represent coalesced parameter snapshots, never reordered operations. */
    struct AudioCommandRecord {
        std::uint64_t sequence{};
        AudioCommand command;
    };

    /** @brief Fixed-size publication outcome; unsuccessful publication retains all work with the caller. */
    enum class AudioCommandPublishStatus : std::uint8_t {
        Published,
        OrdinaryFull,
        CriticalRetry,
        InvalidCommand,
        InvalidSequence,
        Closed,
        Inactive
    };

    /** @brief Preparation-only queue dimensions and exact callback generation. */
    struct AudioCommandBufferDescriptor {
        AudioRuntimeId owner;
        AudioMemoryPoolId storageIdentity;
        std::uint64_t epoch{};
        std::uint32_t slots{};         /**< Power of two in [2, MaximumAudioCommandSlots]. */
        std::uint32_t criticalSlots{}; /**< Positive reserve smaller than slots; reserve is not a priority queue. */
        std::size_t budgetBytes{};     /**< Covers payload and pool slot metadata; fixed owner/cursor overhead is excluded. */
    };

    /**
     * @brief Preallocated SPSC FIFO for one control owner and one callback consumer.
     * No method creates a thread or retains producer pointers. Only TryConsume and IsDrained run on the callback.
     * The host must join producers and detach/join the callback before moving or destroying this owner.
     */
    class AudioCommandBuffer final {
    public:
        /** @brief Prepare all ring storage off-callback, initially empty and open.
         * @param descriptor Valid dimensions, nonzero owner/epoch and host-unique storage identity.
         * @return Prepared owner or typed configuration, memory budget or allocation failure.
         */
        [[nodiscard]] static Result<AudioCommandBuffer> Create(const AudioCommandBufferDescriptor &descriptor);
        /** @brief Destroy quiescent storage off-callback; closing alone does not establish quiescence. */
        ~AudioCommandBuffer();
        /** @brief Move a quiescent owner. @param other Source, left inert. */
        AudioCommandBuffer(AudioCommandBuffer &&other) noexcept;
        /** @brief Replace a quiescent owner off-callback. @param other Source. @return This buffer. */
        AudioCommandBuffer &operator=(AudioCommandBuffer &&other) noexcept;
        AudioCommandBuffer(const AudioCommandBuffer &) = delete;
        AudioCommandBuffer &operator=(const AudioCommandBuffer &) = delete;

        /**
         * @brief Control-only validation/normalization and release-publication of one ordered record.
         * @param record Exact owner/epoch and strictly increasing nonzero sequence; no sequence wrap is admitted.
         * @return Published or a fixed-size rejection. Failed attempts never consume the sequence or mutate queued records.
         * Ordinary records stop at slots-criticalSlots occupancy; critical records may use the complete ring.
         * CriticalRetry means NOT accepted: control must retain/retry or explicitly reconcile the work, never discard it.
         * The caller must not skip a rejected earlier record to publish later work across a lifecycle boundary.
         * @pre Exactly one control producer; live registry identities, leases and closed-scene admission already validated.
         */
        [[nodiscard]] AudioCommandPublishStatus TryPublish(const AudioCommandRecord &record) noexcept;
        /** @brief Callback-only acquire/copy of at most one record, releasing its ring slot only after the copy.
         * @param record Output copied on success, unchanged when empty/inert.
         * @return True for one owned record; false means no currently published record. Never allocates, locks or frees.
         * Storage reuse is not permission to release the external resources identified by the copied command.
         */
        [[nodiscard]] bool TryConsume(AudioCommandRecord &record) noexcept;
        /** @brief Control-only close after the final publication; accepted records remain consumable. Idempotent. */
        void Close() noexcept;
        /** @brief Consumer-only terminal check; not a callback-detachment or external-resource acknowledgement.
         * @return True only when closed and all published records consumed, or when moved-from/inert.
         */
        [[nodiscard]] bool IsDrained() const noexcept;
        /** @brief Samples bounded SPSC occupancy without blocking either owner. @return Approximate queued records, or zero when inert. */
        [[nodiscard]] std::uint32_t Depth() const noexcept;

    private:
        struct State;
        /** @brief Publish fully prepared storage. */
        explicit AudioCommandBuffer(std::unique_ptr<State> state);
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Audio
