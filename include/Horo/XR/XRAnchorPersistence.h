#pragma once

/** @file XRAnchorPersistence.h
 * @brief Optional XR Save participant for protected local durable anchor references.
 */

#include "Horo/Runtime/Save/SaveRestoreTransaction.h"
#include "Horo/XR/XRSpatialAnchors.h"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <thread>

namespace Horo::XR {
    struct XRAnchorStoreProviderTag;
    struct XRAnchorStorePartitionTag;
    struct XRProtectedAnchorReferenceTag;

    /** @brief Stable family of the selected local provider, independent of its live XR generation. */
    using XRAnchorStoreProvider = Runtime::PersistentSaveIdentity<XRAnchorStoreProviderTag>;
    /** @brief Opaque product-local protected partition, independent of cloud or platform account names. */
    using XRAnchorStorePartition = Runtime::PersistentSaveIdentity<XRAnchorStorePartitionTag>;
    /** @brief Non-secret protected-store lookup key. It is not a provider token or a native pointer. */
    using XRProtectedAnchorReference = Runtime::PersistentSaveIdentity<XRProtectedAnchorReferenceTag>;

    /** @brief Trusted host routing and authorization for one protected local partition. */
    struct XRAnchorStoreScope final {
        XRAnchorStoreProvider provider;
        XRAnchorStorePartition partition;
        std::uint32_t providerVersion{}; /**< Exact schema understood by the selected protected provider. */
        std::uint64_t accessRevision{};  /**< Current purpose/access revision minted by the host. */
    };

    /** @brief Durable metadata for a protected reference. No native handle or cached pose crosses this boundary. */
    struct XRProtectedAnchorEntry final {
        XRProtectedAnchorReference reference;
        std::uint64_t expiresAtUnixSeconds{};
    };

    /** @brief Bounded protected local store selected by the host, with no filesystem or network fallback. */
    class IXRProtectedAnchorStore {
    public:
        virtual ~IXRProtectedAnchorStore() = default;

        /** @brief Checks current consent, provider and scope without waiting or reading durable storage.
         * @param scope Host-owned partition and access evidence.
         * @return Success only while the exact scope is current and authorized.
         */
        [[nodiscard]] virtual Result<void> Admit(const XRAnchorStoreScope &scope) const = 0;
        /** @brief Returns the host's checked durable clock in Unix seconds, or zero when unavailable. */
        [[nodiscard]] virtual std::uint64_t CurrentUnixSeconds() const noexcept = 0;
        /** @brief Finds the protected reference already issued for one exact live localized anchor.
         * @param scope Exact local partition and current access revision.
         * @param anchor Exact localized live identity, also validated by XR before use.
         * @return Inert protected lookup and trusted expiry, or a typed store failure.
         */
        [[nodiscard]] virtual Result<XRProtectedAnchorEntry> FindLive(const XRAnchorStoreScope &scope,
                                                                      const XRSpatialAnchorId &anchor) const = 0;
        /** @brief Resolves trusted expiry metadata on the cold restore lane.
         * @param scope Exact local partition and current access revision.
         * @param reference Lookup key whose membership must be checked against that scope.
         * @return Trusted entry, empty if absent, or a typed store failure.
         */
        [[nodiscard]] virtual Result<std::optional<XRProtectedAnchorEntry>> Resolve(const XRAnchorStoreScope &scope,
                                                                                    const XRProtectedAnchorReference &reference) const = 0;
    };

    /** @brief Observable restore disposition. Only new live localization supplies a world-space pose. */
    enum class XRDurableAnchorStatus : std::uint8_t {
        Missing,
        Stale,
        Unlocalized
    };

    /** @brief Durable owner publication, never a usable live anchor or cached transform. */
    struct XRDurableAnchorState final {
        XRDurableAnchorStatus status{XRDurableAnchorStatus::Missing};
        std::optional<XRProtectedAnchorReference> reference;
        std::uint64_t expiresAtUnixSeconds{};
    };

    /** @brief One already-validated pair of restore interfaces. Host keeps the check borrowed while Save owns the receipt. */
    struct XRStagedAnchorRestore final {
        std::unique_ptr<Runtime::IStagedRestoreParticipant> participant;
        Runtime::IStagedRestoreAggregatePublication *publication{}; /**< Exact receipt's final check and no-fail publication port. */
    };

    /** @brief XR-owned adapter for one optional protected anchor record on the canonical Save owner lane.
     * @details The application binds one participant per durable anchor record. Operations validate the active
     * XR lifecycle and route only protected local saves. The store never returns raw
     * persistence tokens or cached poses. Capture and restore are lifecycle work, outside frame polling.
     */
    class XRAnchorPersistence final : public Runtime::ICanonicalStateAdapter, public std::enable_shared_from_this<XRAnchorPersistence> {
    public:
        XRAnchorPersistence(const XRAnchorPersistence &) = delete;
        XRAnchorPersistence &operator=(const XRAnchorPersistence &) = delete;

        /** @brief Binds the exact live scope and qualified protected local provider.
         * @param participant Stable canonical Save owner for this one optional record.
         * @param record Stable canonical Save record identity.
         * @param anchors Host-owned live anchor registry observed by the participant.
         * @param liveScope Exact focused live session, space and grant.
         * @param storeScope Exact trusted protected partition and access revision.
         * @param store Host-selected protected local store. Absent provider fails closed.
         * @return Bound adapter or typed admission/allocation failure.
         */
        [[nodiscard]] static Result<std::shared_ptr<XRAnchorPersistence>> Bind(Runtime::SaveParticipantId participant,
                                                                               Runtime::SaveRecordId record,
                                                                               const std::shared_ptr<const XRSpatialAnchors> &anchors,
                                                                               XRAnchorScope liveScope, XRAnchorStoreScope storeScope,
                                                                               std::shared_ptr<const IXRProtectedAnchorStore> store);

        /** @brief Returns inert participant metadata for explicit host registration.
         * @return One optional canonical Save record with its finite schema limits.
         */
        [[nodiscard]] Runtime::CanonicalStateParticipantDescriptor Descriptor() const;
        [[nodiscard]] static Runtime::CanonicalStateParticipantDescriptor Describe(Runtime::SaveParticipantId participant,
                                                                                   Runtime::SaveRecordId record);

        /** @brief Selects an already protected reference for one exact currently localized live identity.
         * @param id Live anchor whose protected reference the store already owns.
         * @param now Current runtime clock used only to verify fresh live evidence.
         * @return Success or a typed failure that keeps the old durable publication.
         */
        [[nodiscard]] Result<void> RecordLocalized(const XRSpatialAnchorId &id, XRRuntimeTime now);

        /** @brief Reads the current durable status after checking live scope, durable clock and access policy.
         * @return Detached durable disposition and an optional lookup key, or a typed admission failure.
         */
        [[nodiscard]] Result<XRDurableAnchorState> Read() const;

        /** @brief Reads a committed restore for the exact bound provider connection. */
        [[nodiscard]] Result<XRDurableAnchorState> ReadRestored(const IXRProtectedAnchorStore &store) const;

        /** @copydoc Runtime::ICanonicalStateAdapter::Capture */
        [[nodiscard]] Result<Runtime::CanonicalCaptureDisposition> Capture(const Runtime::CanonicalCaptureContext &context,
                                                                           Runtime::ICanonicalCaptureSink &sink) const override;

        /** @brief Copies one bounded detached saved record, or an empty span for an absent optional record.
         * @param bytes Exact detached canonical record owned by this adapter, or empty.
         * @return Inactive restore receipt and its borrowed final publication port, or a typed failure.
         * @details The host must pass the returned publication through the same aggregate Activate call that owns
         * the receipt. The check can also compose into a larger host aggregate before its commit gate.
         */
        [[nodiscard]] Result<XRStagedAnchorRestore> StageRestore(std::span<const std::byte> bytes);

    private:
        class Restore;

        XRAnchorPersistence(Runtime::SaveParticipantId participant, Runtime::SaveRecordId record,
                            const std::shared_ptr<const XRSpatialAnchors> &anchors, XRAnchorScope liveScope, XRAnchorStoreScope storeScope,
                            std::shared_ptr<const IXRProtectedAnchorStore> store);

        [[nodiscard]] Result<std::uint64_t> CheckCurrent() const;

        Runtime::StagedRestoreParticipantRequirement requirement_;
        Runtime::SaveRecordId record_;
        std::weak_ptr<const XRSpatialAnchors> anchors_;
        XRAnchorScope liveScope_;
        XRAnchorStoreScope storeScope_;
        std::shared_ptr<const IXRProtectedAnchorStore> store_;
        const std::thread::id owner_{std::this_thread::get_id()};
        mutable std::uint64_t lastUnixSeconds_{}; /**< Owner-lane floor prevents a clock correction from reviving expiry. */
        XRDurableAnchorState active_;
        std::uint64_t revision_{1};
        bool restored_{};
    };
}  // namespace Horo::XR
