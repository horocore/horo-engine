#pragma once

/**
 * @file SaveThumbnailCapture.h
 * @brief Bounded asynchronous save presentation capture without renderer ownership.
 */

#include "Horo/Runtime/Save/SaveProjectPolicy.h"
#include "Horo/Runtime/Save/SaveSlotMetadata.h"

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Runtime {
    /** @brief Supported encoded thumbnail format; native textures never cross this boundary. */
    enum class SaveThumbnailFormat : std::uint8_t {
        Png
    };
    /** @brief Host-provided capture availability, independent from project requirement. */
    enum class SaveThumbnailAvailability : std::uint8_t {
        Available,
        Headless,
        RendererUnavailable
    };
    /** @brief Terminal state is immutable until explicitly acknowledged by the owner. */
    enum class SaveThumbnailCaptureState : std::uint8_t {
        Idle,
        Pending,
        Captured,
        Omitted,
        Failed
    };

    /** @brief Actual source evidence; frame is presentation provenance, never the logical save tick. */
    struct SaveThumbnailSource final {
        std::uint64_t runtime{}; /**< Runtime incarnation. */
        std::uint64_t scene{};   /**< Scene incarnation. */
        std::uint64_t view{};    /**< View incarnation; zero when capture is unavailable. */
        std::uint64_t frame{};   /**< Completed source frame, or minimum requested frame. */
        [[nodiscard]] auto operator<=>(const SaveThumbnailSource &) const noexcept = default;
    };

    /** @brief Product-selected bounds constrained by qualified hard ceilings. */
    struct SaveThumbnailLimits final {
        std::uint32_t maximumDimension{1'024};      /**< Hard ceiling 4,096 pixels per axis. */
        std::size_t maximumEncodedBytes{1U << 20U}; /**< Hard ceiling 16 MiB. */
        std::chrono::milliseconds timeout{500};     /**< Positive finite timeout, at most 60 seconds. */
    };

    /** @brief Value-only dispatch request for a renderer-owned asynchronous readback/encoding adapter. */
    struct SaveThumbnailRequest final {
        SaveGameSlotId slot;
        SlotGenerationId generation; /**< Intended publication, assigned before archive finalization. */
        SaveThumbnailId thumbnail;
        SaveThumbnailPolicy policy{SaveThumbnailPolicy::Optional};
        SaveThumbnailFormat format{SaveThumbnailFormat::Png};
        SaveThumbnailSource source;
        std::uint32_t width{320};
        std::uint32_t height{180};
        SaveThumbnailLimits limits;
    };

    /** @brief Detached adapter completion transferred to the owner; failure preserves its typed error. */
    struct SaveThumbnailCompletion final {
        std::uint64_t requestSerial{}; /**< Exact dispatch serial, never reused by this coordinator. */
        SaveGameSlotId slot;
        SlotGenerationId generation; /**< Exact target; protects completions across coordinator replacement. */
        SaveThumbnailId thumbnail;
        SaveThumbnailSource source;
        SaveThumbnailFormat format{SaveThumbnailFormat::Png};
        std::uint32_t width{};
        std::uint32_t height{};
        std::vector<std::byte> encoded; /**< Completed CPU PNG bytes; no fences, mappings or GPU handles. */
        std::optional<Error> error;
    };

    /** @brief Immutable CPU artifact whose ownership cannot keep renderer resources alive. */
    class SaveThumbnailArtifact final {
    public:
        /** @brief Returns retained publication and requested bounds. @return Immutable request. */
        [[nodiscard]] const SaveThumbnailRequest &Request() const noexcept {
            return request_;
        }

        /** @brief Returns actual completed frame evidence. @return Source provenance. */
        [[nodiscard]] SaveThumbnailSource Source() const noexcept {
            return source_;
        }

        /** @brief Returns encoded bytes; consumers must use a bounded image decoder. @return Borrowed PNG bytes. */
        [[nodiscard]] std::span<const std::byte> Bytes() const noexcept {
            return bytes_;
        }

    private:
        friend class SaveThumbnailCapture;
        SaveThumbnailRequest request_;
        SaveThumbnailSource source_;
        std::vector<std::byte> bytes_;
    };

    /** @brief Owner-thread snapshot; optional capture failure is evidence, not a save failure. */
    struct SaveThumbnailCaptureSnapshot final {
        SaveThumbnailCaptureState state{SaveThumbnailCaptureState::Idle};
        std::uint64_t requestSerial{};
        std::optional<SaveThumbnailRequest> request;
        std::shared_ptr<const SaveThumbnailArtifact> artifact;
        std::optional<Error> error;
    };

    /**
     * @brief Owner-thread single-request state machine; it never dispatches, waits or touches a renderer.
     *
     * Host marshals completed CPU results onto the owner thread. The renderer adapter independently
     * owns bounded staging/fence retirement, including timeout, cancellation and late completions.
     * All methods and destruction require the same owner thread; no concurrent calls are permitted.
     */
    class SaveThumbnailCapture final {
    public:
        /** @brief Creates an idle owner-thread coordinator without renderer activation. */
        SaveThumbnailCapture() = default;
        SaveThumbnailCapture(const SaveThumbnailCapture &) = delete;
        SaveThumbnailCapture &operator=(const SaveThumbnailCapture &) = delete;
        /** @brief Admits one request, or immediately omits/fails unavailable capture.
         * @param request Exact save publication and bounded capture request.
         * @param availability Explicit composition capability. @param now Nonnegative monotonic owner time.
         * @return Unique serial or validation/lifecycle error; failed admission does not change state.
         * @post Dispatch only when Snapshot().state is Pending; no dispatch is necessary otherwise.
         */
        [[nodiscard]] Result<std::uint64_t> Request(SaveThumbnailRequest request, SaveThumbnailAvailability availability,
                                                    std::chrono::steady_clock::time_point now);
        /** @brief Polls finite deadline and current source without waiting.
         * @param now Monotonic owner time. @param current Current runtime/scene/view evidence.
         * @return Success or invalid-clock error; optional failures become Omitted, required failures become Failed.
         */
        [[nodiscard]] Result<void> Advance(std::chrono::steady_clock::time_point now, SaveThumbnailSource current);
        /** @brief Transfers one already-completed CPU result; stale serials only discard their own bytes.
         * @param completion Detached result. @param now Monotonic owner time. @param current Current source.
         * @return True if consumed, false for late/unrelated results, or invalid-clock error.
         */
        [[nodiscard]] Result<bool> Complete(SaveThumbnailCompletion completion, std::chrono::steady_clock::time_point now,
                                            SaveThumbnailSource current);
        /** @brief Cancels exactly one pending dispatch; renderer retirement remains adapter-owned.
         * @param serial Exact dispatch serial. @return True only when pending capture was retired.
         */
        [[nodiscard]] bool Cancel(std::uint64_t serial);
        /** @brief Permanently closes admission and retires pending capture without waiting. */
        void BeginShutdown();
        /** @brief Clears only exact terminal state, preserving serial fencing and shutdown.
         * @param serial Exact terminal serial. @return True when cleared, false for stale/pending state.
         */
        [[nodiscard]] bool Acknowledge(std::uint64_t serial) noexcept;

        /** @brief Returns owner-thread immutable state. @return Borrowed snapshot until next owner mutation. */
        [[nodiscard]] const SaveThumbnailCaptureSnapshot &Snapshot() const noexcept {
            return snapshot_;
        }

    private:
        /** @brief Applies product requirement while retaining the original error identity. */
        void Finish(Error error);
        SaveThumbnailCaptureSnapshot snapshot_;
        std::uint64_t nextSerial_{1};
        std::chrono::steady_clock::time_point started_{};
        std::chrono::steady_clock::time_point observed_{};
        bool closed_{};
    };

    /** @brief Bounded advisory display and artifact attached only to an exact committed publication. */
    struct SaveCommittedPresentation final {
        SaveSlotCatalogEntry catalog;
        std::shared_ptr<const SaveThumbnailArtifact> artifact;
    };

    /** @brief Associates terminal capture with one validated committed generation; never edits an archive or index.
     * @param publication Durable metadata whose thumbnail identity already matches the finalized archive.
     * @param display Advisory UTF-8 display values; invalid values are omitted without harming the save.
     * @param capture Terminal capture evidence. @param limits Display validation bounds.
     * @return Detached presentation or pending/required-failure/stale publication error.
     * @pre Caller has independently acknowledged durable publication; this function cannot prove physical durability.
     */
    [[nodiscard]] Result<SaveCommittedPresentation> MakeSaveCommittedPresentation(SaveSlotPublicationMetadata publication,
                                                                                  SaveSlotDisplayMetadata display,
                                                                                  const SaveThumbnailCaptureSnapshot &capture,
                                                                                  const SaveSlotMetadataLimits &limits = {});
}  // namespace Horo::Runtime
