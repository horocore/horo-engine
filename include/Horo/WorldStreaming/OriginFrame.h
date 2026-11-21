#pragma once

/**
 * @file OriginFrame.h
 * @brief Canonical global-to-local origin-frame identity and lifecycle contract.
 */

#include "Horo/Foundation/StrongId.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/Math/WorldCoordinate64.h"
#include "Horo/WorldStreaming/WorldStreamingErrors.h"

#include <atomic>
#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace Horo::WorldStreaming {
    namespace Detail {
        struct OriginFrameIdTag;
        struct OriginFrameRevisionTag;
        struct OriginGenerationTag;
    }  // namespace Detail

    /** @brief Stable identity of one local-coordinate frame owner. */
    using OriginFrameId = Foundation::Detail::NonZeroId64<Detail::OriginFrameIdTag, WorldStreamingErrors::IdentityInvalid>;
    /** @brief Immutable publication revision of one origin-frame owner. */
    using OriginFrameRevision = Foundation::Detail::NonZeroId64<Detail::OriginFrameRevisionTag, WorldStreamingErrors::IdentityInvalid>;
    /** @brief Monotonic local-coordinate generation that never wraps or aliases. */
    using OriginGeneration = Foundation::Detail::NonZeroId64<Detail::OriginGenerationTag, WorldStreamingErrors::IdentityInvalid>;

    /** @brief Advances a valid origin generation without wrapping. @param current Current generation. @return Successor or exhaustion. */
    [[nodiscard]] Result<OriginGeneration> NextOriginGeneration(OriginGeneration current);
    /** @brief Advances a valid frame revision without wrapping. @param current Current revision. @return Successor or exhaustion. */
    [[nodiscard]] Result<OriginFrameRevision> NextOriginFrameRevision(OriginFrameRevision current);

    /** @brief Exact identity fence for one immutable origin-frame publication. */
    struct OriginFrameBinding final {
        OriginFrameId identity;       /**< Stable local-frame owner. */
        OriginFrameRevision revision; /**< Exact immutable frame publication. */
        OriginGeneration generation;  /**< Exact local-coordinate generation. */

        /** @brief Checks reserved values. @return Whether every identity component is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] constexpr auto operator<=>(const OriginFrameBinding &) const noexcept = default;
    };

    /** @brief Generation-tagged fp32 local coordinate that cannot alias canonical global authority. */
    class OriginLocalCoordinate final {
    public:
        /**
         * @brief Validates an externally supplied millimeter-aligned local coordinate.
         * @param value Local meters in Horo scene axes.
         * @param frame Stable frame identity.
         * @param generation Exact origin generation.
         * @return Tagged local value or a typed finite/range/precision failure.
         */
        [[nodiscard]] static Result<OriginLocalCoordinate> Create(Math::Vec3 value, OriginFrameId frame, OriginGeneration generation);

        /** @brief Returns local meters. @return Immutable fp32 value. */
        [[nodiscard]] constexpr Math::Vec3 Value() const noexcept {
            return value_;
        }

        /** @brief Returns the stable owning frame identity. @return Non-zero identity. */
        [[nodiscard]] constexpr OriginFrameId Frame() const noexcept {
            return frame_;
        }

        /** @brief Returns the exact coordinate generation. @return Non-zero generation. */
        [[nodiscard]] constexpr OriginGeneration Generation() const noexcept {
            return generation_;
        }

        [[nodiscard]] constexpr auto operator<=>(const OriginLocalCoordinate &) const noexcept = default;

    private:
        friend class OriginFrame;

        explicit constexpr OriginLocalCoordinate(Math::Vec3 value, OriginFrameId frame, OriginGeneration generation) noexcept
            : value_(value), frame_(frame), generation_(generation) {}

        Math::Vec3 value_{};
        OriginFrameId frame_{};
        OriginGeneration generation_{};
    };

    /** @brief Immutable canonical origin and exact local-frame identity fence. */
    class OriginFrame final {
    public:
        /** @brief Maximum supported magnitude on each local axis, in exact millimeters. */
        static constexpr std::int64_t MaximumLocalHalfExtentMillimeters = 8'192'000;

        /**
         * @brief Validates an immutable origin-frame publication.
         * @param binding Stable identity, revision, and generation.
         * @param origin Canonical global origin; retained by value and never mutated.
         * @return Immutable frame or typed invalid-binding failure.
         */
        [[nodiscard]] static Result<OriginFrame> Create(OriginFrameBinding binding, const Math::WorldCoordinate64 &origin);

        /** @brief Returns the exact frame fence. @return Immutable binding. */
        [[nodiscard]] constexpr const OriginFrameBinding &Binding() const noexcept {
            return binding_;
        }

        /** @brief Returns canonical global origin. @return Immutable exact millimeter coordinate. */
        [[nodiscard]] constexpr const Math::WorldCoordinate64 &Origin() const noexcept {
            return origin_;
        }

        /**
         * @brief Converts canonical global authority to this generation's local fp32 view.
         * @param global Stable global coordinate, left unchanged.
         * @return Tagged local coordinate or typed range failure.
         */
        [[nodiscard]] Result<OriginLocalCoordinate> ToLocal(const Math::WorldCoordinate64 &global) const;
        /**
         * @brief Converts a matching generation-tagged local value to canonical global authority.
         * @param local Tagged local coordinate.
         * @return Exact millimeter global coordinate or typed stale/range failure.
         */
        [[nodiscard]] Result<Math::WorldCoordinate64> ToGlobal(const OriginLocalCoordinate &local) const;

        [[nodiscard]] constexpr auto operator<=>(const OriginFrame &) const noexcept = default;

    private:
        explicit constexpr OriginFrame(OriginFrameBinding binding, const Math::WorldCoordinate64 &origin) noexcept
            : binding_(binding), origin_(origin) {}

        OriginFrameBinding binding_{};
        Math::WorldCoordinate64 origin_{};
    };

    /** @brief Immutable frame observation invalidated by replacement or owner shutdown. */
    class OriginFrameLease final {
    public:
        /** @brief Returns the observed frame while its generation is active. @return Frame or typed stale failure. */
        [[nodiscard]] Result<OriginFrame> Get() const;

    private:
        friend class OriginFrameOwner;

        OriginFrameLease(OriginFrame frame, std::shared_ptr<const std::atomic_bool> active)
            : frame_(std::move(frame)), active_(std::move(active)) {}

        OriginFrame frame_;
        std::shared_ptr<const std::atomic_bool> active_;
    };

    /** @brief Single owner of active and staged origin-frame publications. */
    class OriginFrameOwner final {
    public:
        /** @brief Opaque construction gate restricted to validated owner creation. */
        class ConstructionKey final {
            friend class OriginFrameOwner;
            ConstructionKey() = default;
        };

        /** @brief Creates an active owner. @param initial Initial immutable frame. @return Owner or storage failure. */
        [[nodiscard]] static Result<std::unique_ptr<OriginFrameOwner>> Create(OriginFrame initial);
        /** @brief Captures an immutable generation lease. @return Active lease or lifecycle failure. */
        [[nodiscard]] Result<OriginFrameLease> Lease() const;
        /**
         * @brief Validates and stages the exact successor without changing the active frame.
         * @param candidate Same identity with next revision and generation.
         * @return Success or typed stale/lifecycle failure.
         */
        [[nodiscard]] Result<void> StageReplacement(OriginFrame candidate);
        /** @brief Cancels an unpublished candidate without changing the active frame. */
        void CancelReplacement() noexcept;
        /** @brief Atomically publishes the staged candidate and expires old leases. @return Success or typed lifecycle/storage failure. */
        [[nodiscard]] Result<void> PublishReplacement();
        /** @brief Idempotently cancels staging, expires leases, and closes the owner. */
        void Shutdown() noexcept;
        /** @brief Expires retained generation leases before releasing owner state. */
        ~OriginFrameOwner() noexcept;

        /** @brief Compares exact active authority without creating a lease, allocating or invoking callbacks.
         * @param frame Full immutable binding and global origin to compare. @return True on the owner lane only for that publication.
         */
        [[nodiscard]] bool Publishes(const OriginFrame &frame) const noexcept {
            return active_ && activeFrame_ == frame;
        }

        /** @brief Checks lifecycle state. @return Whether the owner can stage and publish. */
        [[nodiscard]] bool IsActive() const noexcept {
            return active_;
        }

        OriginFrameOwner(const OriginFrameOwner &) = delete;
        OriginFrameOwner &operator=(const OriginFrameOwner &) = delete;

        /** @brief Adopts a validated initial frame and generation lease through the creation-only gate. */
        OriginFrameOwner(ConstructionKey, OriginFrame initial, std::shared_ptr<std::atomic_bool> leaseState) noexcept
            : activeFrame_(std::move(initial)), leaseState_(std::move(leaseState)) {}

    private:
        OriginFrame activeFrame_;
        std::optional<OriginFrame> stagedFrame_;
        std::shared_ptr<std::atomic_bool> leaseState_;
        bool active_{true};
    };
}  // namespace Horo::WorldStreaming
