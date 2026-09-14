#pragma once

/**
 * @file MotionHistory.h
 * @brief Backend-neutral object, camera, jitter, and disocclusion history contract.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/Runtime/Render/TemporalHistory.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace Horo::Render {
    /** @brief Stable extraction-owned identity for one logical render object or subobject. */
    struct RenderMotionObjectId {
        std::uint64_t value{0};

        /** @brief Reports whether the identity is usable. @return True for a non-zero identity. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderMotionObjectId &) const noexcept = default;
    };

    /** @brief Canonical motion-vector orientation and units consumed by temporal passes. */
    enum class RenderMotionVectorConvention : std::uint8_t {
        PreviousUvMinusCurrentUv,
    };

    /** @brief Current immutable transform sample for one extracted render object. */
    struct RenderMotionObjectSample {
        RenderMotionObjectId object;
        Math::Mat4 localToWorld{Math::Mat4::Identity()};

        /** @brief Validates identity and finite transform values. @return True when the sample is usable. */
        [[nodiscard]] bool IsValid() const noexcept {
            return object.IsValid() && Math::IsFinite(localToWorld);
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderMotionObjectSample &) const noexcept = default;
    };

    /** @brief Current unjittered camera state and explicit normalized projection jitter. */
    struct RenderMotionCameraSample {
        Math::Mat4 unjitteredViewProjection{Math::Mat4::Identity()};
        Math::Vec2 jitterUv{};

        /** @brief Validates finite camera and jitter values. @return True when the sample is usable. */
        [[nodiscard]] bool IsValid() const noexcept {
            return Math::IsFinite(unjitteredViewProjection) && Math::IsFinite(jitterUv);
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderMotionCameraSample &) const noexcept = default;
    };

    /** @brief Same-frame semantic resources used to detect disocclusion and reject invalid reprojection. */
    struct RenderDisocclusionInputs {
        RenderTextureHandle positiveLinearDepth;
        RenderTextureHandle motionVectors;
        std::optional<RenderTextureHandle> reactiveMask;
        std::optional<RenderTextureHandle> transparencyMask;

        /** @brief Validates required and optional exact texture generations. @return True when every supplied handle is valid. */
        [[nodiscard]] bool IsValid() const noexcept {
            const auto validOptional = [](const std::optional<RenderTextureHandle> &handle) {
                return !handle.has_value() || handle->IsValid();
            };
            return positiveLinearDepth.IsValid() && motionVectors.IsValid() && validOptional(reactiveMask) &&
                   validOptional(transparencyMask);
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderDisocclusionInputs &) const noexcept = default;
    };

    /** @brief Explicit discontinuities not derivable from compatibility generations. */
    struct RenderMotionInvalidationSignals {
        bool cameraCut{false};
        bool explicitRequest{false};
        bool invalidInput{false};
        bool suspended{false};
        bool skippedFrame{false};
        bool providerRequested{false};

        [[nodiscard]] constexpr auto operator<=>(const RenderMotionInvalidationSignals &) const noexcept = default;
    };

    /** @brief Finite capacity admitted before tracker storage allocation. */
    struct RenderMotionHistoryLimits {
        static constexpr std::size_t HardMaxObjects = 1'048'576;
        std::size_t maxObjects{65'536};

        /** @brief Validates the finite object bound. @return True for a non-zero bound within the hard limit. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return maxObjects > 0 && maxObjects <= HardMaxObjects;
        }
    };

    /** @brief Complete immutable input for one attempted real render frame. */
    struct RenderMotionFrameRequest {
        TemporalHistoryCompatibility compatibility;
        std::uint64_t frameId{0};
        std::uint64_t predecessorFrameId{0};
        RenderMotionCameraSample camera;
        RenderDisocclusionInputs disocclusion;
        RenderMotionInvalidationSignals invalidation;
        std::span<const RenderMotionObjectSample> objects;
    };

    /** @brief Effective current/previous transform pair for one logical render object. */
    struct RenderMotionObjectPair {
        RenderMotionObjectId object;
        Math::Mat4 currentLocalToWorld{Math::Mat4::Identity()};
        Math::Mat4 previousLocalToWorld{Math::Mat4::Identity()};
        bool hasPrevious{false};

        [[nodiscard]] constexpr auto operator<=>(const RenderMotionObjectPair &) const noexcept = default;
    };

    /** @brief Effective current/previous camera and jitter pair for motion-vector generation. */
    struct RenderMotionCameraPair {
        Math::Mat4 currentUnjitteredViewProjection{Math::Mat4::Identity()};
        Math::Mat4 previousUnjitteredViewProjection{Math::Mat4::Identity()};
        Math::Vec2 currentJitterUv{};
        Math::Vec2 previousJitterUv{};
        bool hasPrevious{false};

        [[nodiscard]] constexpr auto operator<=>(const RenderMotionCameraPair &) const noexcept = default;
    };

    /** @brief Owned motion input for one pending real-frame attempt. */
    struct RenderMotionFrame {
        TemporalHistoryCompatibility compatibility;
        std::uint64_t frameId{0};
        std::uint64_t attempt{0};
        RenderMotionVectorConvention convention{RenderMotionVectorConvention::PreviousUvMinusCurrentUv};
        RenderMotionCameraPair camera;
        RenderDisocclusionInputs disocclusion;
        std::optional<TemporalHistoryResetCause> resetCause;
        std::vector<RenderMotionObjectPair> objects;
    };

    /**
     * @brief Owner-thread transaction that advances camera and object history only after successful real frames.
     *
     * Input objects are copied into bounded canonical storage. BeginFrame never advances published state;
     * Publish commits the exact pending snapshot, while Abandon preserves the prior frame. No native API
     * handles, mutable scene pointers, synthetic frames, or backend fallback enter this contract.
     */
    class RenderMotionHistoryTracker final {
    public:
        RenderMotionHistoryTracker(const RenderMotionHistoryTracker &) = delete;
        RenderMotionHistoryTracker &operator=(const RenderMotionHistoryTracker &) = delete;
        RenderMotionHistoryTracker(RenderMotionHistoryTracker &&other) noexcept;
        RenderMotionHistoryTracker &operator=(RenderMotionHistoryTracker &&other) noexcept;
        ~RenderMotionHistoryTracker();

        /**
         * @brief Creates an owner-thread tracker with one finite object limit.
         * @param limits Maximum current objects retained after publication.
         * @return A tracker, or a typed validation/allocation failure.
         */
        [[nodiscard]] static Result<RenderMotionHistoryTracker> Create(const RenderMotionHistoryLimits &limits);

        /**
         * @brief Begins one real-frame transaction and resolves effective previous samples and reset provenance.
         * @param request Complete compatibility, camera, semantic resources, signals, and unique object samples.
         * @return Owned canonical frame data, or a typed validation/lifecycle failure.
         */
        [[nodiscard]] Result<RenderMotionFrame> BeginFrame(const RenderMotionFrameRequest &request);

        /**
         * @brief Commits the exact pending real frame as the next camera/object history.
         * @param frame Unmodified frame returned by BeginFrame.
         * @return Success, or a typed stale/tampered/lifecycle failure.
         */
        [[nodiscard]] Result<void> Publish(const RenderMotionFrame &frame);

        /**
         * @brief Cancels the exact pending frame without advancing camera, jitter, or object state.
         * @param frame Unmodified frame returned by BeginFrame.
         * @return Success, or a typed stale/tampered/lifecycle failure.
         */
        [[nodiscard]] Result<void> Abandon(const RenderMotionFrame &frame);

        /**
         * @brief Stops the tracker idempotently and releases all bounded CPU history.
         * @return Success, or WrongThread outside the owner-thread safe point.
         */
        [[nodiscard]] Result<void> Shutdown();

    private:
        struct PublishedObject;

        struct PendingState {
            RenderMotionFrame frame;
        };

        RenderMotionHistoryTracker(RenderMotionHistoryLimits limits, std::vector<PublishedObject> objects) noexcept;
        [[nodiscard]] Result<void> ValidateThreadAndState() const;
        /** @brief Builds canonical effective motion pairs from already validated current samples. */
        [[nodiscard]] RenderMotionFrame BuildFrame(const RenderMotionFrameRequest &request,
                                                   std::span<const RenderMotionObjectSample> current,
                                                   std::optional<TemporalHistoryResetCause> reset);
        [[nodiscard]] Result<void> Complete(const RenderMotionFrame &frame, bool publish);
        void ReleaseAll() noexcept;

        RenderMotionHistoryLimits m_limits;
        std::thread::id m_ownerThread;
        std::vector<PublishedObject> m_objects;
        std::optional<TemporalHistoryCompatibility> m_compatibility;
        RenderMotionCameraSample m_camera;
        std::uint64_t m_lastPublishedFrame{0};
        std::uint64_t m_nextAttempt{1};
        std::optional<PendingState> m_pending;
        bool m_stopped{false};
    };
}  // namespace Horo::Render
