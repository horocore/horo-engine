#pragma once

/** @file OpenXRProtectedAnchorStore.h @brief Private binding between the OpenXR store connection and canonical Save. */
#include "Horo/XR/XRAnchorPersistence.h"
#include "OpenXRNativeSession.h"

namespace Horo::XR::OpenXRInternal {
    /** @brief Provider/version, secure partition, and live consent are supplied by the application owner. */
    class OpenXRProtectedAnchorStore final : public IXRProtectedAnchorStore {
    public:
        using Clock = std::uint64_t (*)() noexcept;

        /** @brief Stable provider family for the selected OpenXR persistence route. */
        [[nodiscard]] static XRAnchorStoreProvider Provider();

        /** @brief Opens the separately authorized cold store without retaining the native session owner. */
        [[nodiscard]] static Result<std::shared_ptr<OpenXRProtectedAnchorStore>> Open(const std::shared_ptr<OpenXRNativeSession> &owner,
                                                                                      const XRAnchorScope &liveScope, XrSpace world,
                                                                                      const XRAnchorStoreScope &storeScope,
                                                                                      const CancellationSource &consent, Clock clock);

        /** @brief Persists a localized anchor under a new host-minted opaque lookup ID. Cold owner lane only. */
        [[nodiscard]] Result<void> Record(const XRSpatialAnchorId &anchor, XRRuntimeTime now, XRProtectedAnchorEntry entry);

        /** @brief Removes this adapter's exact unpublished write, preserving the slot if compensation needs retry. */
        [[nodiscard]] Result<void> Discard(const XRSpatialAnchorId &anchor);

        /** @brief Creates a new Locating anchor after the canonical restore publishes the durable reference. */
        [[nodiscard]] Result<std::optional<XRSpatialAnchorId>> Relocalize(const XRAnchorPersistence &participant, XRRuntimeTime now) const;

        [[nodiscard]] Result<void> Admit(const XRAnchorStoreScope &scope) const override;
        [[nodiscard]] std::uint64_t CurrentUnixSeconds() const noexcept override;
        [[nodiscard]] Result<XRProtectedAnchorEntry> FindLive(const XRAnchorStoreScope &scope,
                                                              const XRSpatialAnchorId &anchor) const override;
        [[nodiscard]] Result<std::optional<XRProtectedAnchorEntry>> Resolve(const XRAnchorStoreScope &scope,
                                                                            const XRProtectedAnchorReference &reference) const override;

    private:
        struct Entry final {
            XRSpatialAnchorId anchor;
            XRProtectedAnchorEntry stored;
        };

        OpenXRProtectedAnchorStore(const std::shared_ptr<OpenXRNativeSession> &owner, XRAnchorScope liveScope, XrSpace world,
                                   XRAnchorStoreScope storeScope, Clock clock);

        [[nodiscard]] Result<std::shared_ptr<OpenXRNativeSession>> Borrow(const XRAnchorStoreScope &scope) const;

        std::weak_ptr<OpenXRNativeSession> owner_;
        const std::thread::id lane_{std::this_thread::get_id()};
        XRAnchorScope liveScope_;
        XrSpace world_;
        XRAnchorStoreScope scope_;
        Clock clock_;
        std::uint64_t connection_{};
        std::array<Entry, XRSpatialAnchors::MaximumAnchors> live_{};
        mutable std::uint64_t lastUnixSeconds_{};
    };
}  // namespace Horo::XR::OpenXRInternal
