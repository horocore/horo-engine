#pragma once

/** @file XRSpatialAnchors.h
 * @brief Session-local anchor identities and bounded localization evidence owned by XRRuntime.
 */

#include "Horo/Foundation/Time.h"
#include "Horo/XR/XRSessionLifecycle.h"
#include "Horo/XR/XRSpacePose.h"

#include <array>
#include <memory>
#include <optional>
#include <thread>

namespace Horo::XR {
    struct XRAnchorProviderTag;
    struct XRAnchorGrantTag;
    struct XRAnchorSlotTag;

    /** @brief Identity of the selected live anchor provider incarnation. */
    using XRAnchorProviderGeneration = XRGeneration<XRAnchorProviderTag>;
    /** @brief Host-issued purpose/consumer authorization, unique and increasing over this lifecycle. */
    using XRAnchorGrant = XRGeneration<XRAnchorGrantTag>;

    /** @brief Exact producer, session, coordinate space and authorized consumer lifetime. */
    struct XRAnchorScope final {
        XRCoordinateSpace space;
        XRAnchorProviderGeneration provider;
        XRAnchorGrant grant;
        constexpr auto operator<=>(const XRAnchorScope &) const noexcept = default;
    };

    /** @brief Opaque live anchor identity. It has no portable Scene, persistence or native-handle meaning. */
    class XRSpatialAnchorId final {
    public:
        XRSpatialAnchorId() = default;

        /** @return Whether this value has a valid representation. The owner checks liveness separately. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return object_.IsValid() && grant_.IsValid();
        }

        constexpr auto operator<=>(const XRSpatialAnchorId &) const noexcept = default;

    private:
        friend class XRSpatialAnchors;
        XRSessionObjectId<XRAnchorSlotTag> object_;
        XRAnchorGrant grant_;
    };

    /** @brief Current session localization state, independent of authored gameplay or durable identity. */
    enum class XRAnchorLocalization : std::uint8_t {
        Locating,
        Localized,
        Limited,
        Lost,
        Expired
    };

    /** @brief Complete located pose in the admitted target space. Only Localized evidence may expose it. */
    struct XRAnchorPose final {
        Math::Vec3 positionMeters;
        Math::Quaternion orientation{Math::Quaternion::Identity()};
    };

    /** @brief One provider observation. No velocities, native token or unrelated sensor history may cross this boundary. */
    struct XRAnchorObservation final {
        XRRuntimeTime sampledAt;
        XRAnchorLocalization state{XRAnchorLocalization::Locating};
        std::optional<XRAnchorPose> pose;
    };

    /** @brief Detached immutable evidence. Re-read from its owner at the destination safe point. */
    class XRAnchorSnapshot final {
    public:
        /** @return Exact live identity at the time of publication. */
        [[nodiscard]] const XRSpatialAnchorId &Id() const noexcept {
            return id_;
        }

        /** @return Session, reference space, provider and authorization provenance. */
        [[nodiscard]] const XRAnchorScope &Scope() const noexcept {
            return scope_;
        }

        /** @return Localization state and time, with a pose only while localized. */
        [[nodiscard]] const XRAnchorObservation &Observation() const noexcept {
            return observation_;
        }

    private:
        friend class XRSpatialAnchors;

        XRAnchorSnapshot(const XRSpatialAnchorId &id, const XRAnchorScope &scope, const XRAnchorObservation &observation) noexcept
            : id_(id), scope_(scope), observation_(observation) {}

        XRSpatialAnchorId id_;
        XRAnchorScope scope_;
        XRAnchorObservation observation_;
    };

    /** @brief Application-owned finite limits for live slots and localization evidence freshness. */
    struct XRAnchorLimits final {
        std::uint32_t maximumAnchors{};
        Duration maximumSampleAge;
    };

    /** @brief Reason the host closes one exact admission. */
    enum class XRAnchorWithdrawal : std::uint8_t {
        ProviderStopped,
        AccessRevoked
    };

    /** @brief The selected XRRuntime owner for live spatial anchors on the canonical session control lane.
     * @details One application-owned instance observes the canonical lifecycle without retaining it. The host calls
     * Activate only from an actual admitted provider and purpose grant. It withdraws before retiring the provider or
     * changing privacy, runtime, reference space or origin. No observer may write Scene directly. Methods do not call
     * native APIs, perform I/O or allocate on success. This owner retains only one current observation per live slot.
     */
    class XRSpatialAnchors final {
    public:
        static constexpr std::size_t MaximumAnchors = 64;

        /** @brief Observes the exact canonical session authority. @param lifecycle Shared by the host on the session control lane. */
        explicit XRSpatialAnchors(const std::shared_ptr<const XRSessionLifecycle> &lifecycle) noexcept;
        XRSpatialAnchors(const XRSpatialAnchors &) = delete;
        XRSpatialAnchors &operator=(const XRSpatialAnchors &) = delete;

        /** @brief Atomically activates or replaces the provider and access scope after complete validation.
         * @param scope Admitted current target space, provider incarnation and unique host grant.
         * @param limits Finite host-approved limits.
         * @param support Selected provider's current support and permission evidence.
         * @return Success invalidates old anchors. Failure leaves the old admission intact. */
        [[nodiscard]] Result<void> Activate(const XRAnchorScope &scope, const XRAnchorLimits &limits, XRCapabilityState support);

        /** @brief Allocates one live identity for a privately prepared provider anchor.
         * @param scope Exact active producer and authorization.
         * @param createdAt Runtime clock at creation. Older observations cannot localize the resulting identity.
         * @return New Locating identity or typed stale, invalid or capacity failure. */
        [[nodiscard]] Result<XRSpatialAnchorId> Create(const XRAnchorScope &scope, XRRuntimeTime createdAt);

        /** @brief Commits one validated provider observation for its exact live anchor.
         * @param scope Scope captured when the producer started location work.
         * @param id Exact live anchor to update.
         * @param observation Observation sampled strictly after creation and the last accepted sample. A failed update retains the previous
         * evidence.
         * @return Success or typed invalid, stale or time-domain failure. */
        [[nodiscard]] Result<void> Publish(const XRAnchorScope &scope, const XRSpatialAnchorId &id, const XRAnchorObservation &observation);

        /** @brief Reads detached localization evidence while its authority and lifetime are current.
         * @param scope Exact captured scope of the authorized consumer.
         * @param id Exact live anchor to read.
         * @param now Current runtime clock. Expired data is replaced with an explicit state and no pose.
         * @return Copy of current evidence, or typed invalid, stale, denied or unavailable failure. */
        [[nodiscard]] Result<XRAnchorSnapshot> Read(const XRAnchorScope &scope, const XRSpatialAnchorId &id, XRRuntimeTime now) const;

        /** @brief Invalidates one exact live ID before the provider frees its native object. */
        [[nodiscard]] Result<void> Retire(const XRAnchorScope &scope, const XRSpatialAnchorId &id);

        /** @brief Wipes all observations for the exact scope, even after native session admission closes.
         * @param scope Exact scope to invalidate. Stale withdrawals cannot stop a replacement.
         * @param reason Why the host closes this authorization. Access revocation yields PermissionDenied on read.
         * @return Success or typed invalid/stale/wrong-thread failure. */
        [[nodiscard]] Result<void> Withdraw(const XRAnchorScope &scope, XRAnchorWithdrawal reason);

    private:
        friend class XRAnchorPersistence;

        struct Slot final {
            XRSpatialAnchorId id;
            XRAnchorObservation observation;
        };

        [[nodiscard]] Result<std::shared_ptr<const XRSessionLifecycle>> ResolveLifecycle() const;

        [[nodiscard]] Result<void> CheckScope(const XRAnchorScope &scope) const;
        [[nodiscard]] Result<std::size_t> Find(const XRAnchorScope &scope, const XRSpatialAnchorId &id) const;

        std::weak_ptr<const XRSessionLifecycle> lifecycle_;
        const std::thread::id owner_;
        XRAnchorScope scope_;
        XRAnchorLimits limits_;
        std::uint64_t continuity_{};
        std::uint32_t lastSlotGeneration_{};
        std::optional<XRAnchorWithdrawal> withdrawal_;
        std::array<Slot, MaximumAnchors> slots_{};
    };
}  // namespace Horo::XR
