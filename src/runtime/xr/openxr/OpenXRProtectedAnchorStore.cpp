#include "host/OpenXRProtectedAnchorStore.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <new>

namespace Horo::XR::OpenXRInternal {
    namespace {
        using StoredEntry = Result<std::optional<XRProtectedAnchorEntry>>;

        bool SameScope(const XRAnchorStoreScope &left, const XRAnchorStoreScope &right) noexcept {
            return left.provider == right.provider && left.partition == right.partition && left.providerVersion == right.providerVersion &&
                   left.accessRevision == right.accessRevision;
        }

        /** @brief A native name carries metadata inside the provider. Save receives only the opaque reference. */
        std::string NamePrefix(const XRAnchorStoreScope &scope, const XRProtectedAnchorReference &reference) {
            return std::format("horo-a1-{}-{:016x}-{}-", scope.partition.ToString(), scope.accessRevision, reference.ToString());
        }

        std::string NativeName(const XRAnchorStoreScope &scope, const XRProtectedAnchorEntry &entry) {
            return std::format("{}{:016x}", NamePrefix(scope, entry.reference), entry.expiresAtUnixSeconds);
        }

        StoredEntry FindRecord(std::span<const XrSpatialAnchorPersistenceNameMSFT> names, std::string_view prefix,
                               const XRProtectedAnchorReference &reference) {
            std::optional<XRProtectedAnchorEntry> entry;
            for (const auto &record : names) {
                const std::string_view name{record.name};  // The native owner verified termination before copying names.
                if (!name.starts_with(prefix))
                    continue;
                const auto expiry = name.substr(prefix.size());
                std::uint64_t seconds{};
                const auto end = expiry.data() + expiry.size();
                const auto parsed = std::from_chars(expiry.data(), end, seconds, 16);
                if (entry || expiry.size() != 16 || parsed.ec != std::errc{} || parsed.ptr != end || seconds == 0)
                    return StoredEntry::Failure(MakeError(XRErrors::OperationInvalid));
                entry = XRProtectedAnchorEntry{reference, seconds};
            }
            return StoredEntry::Success(entry);
        }
    }  // namespace

    XRAnchorStoreProvider OpenXRProtectedAnchorStore::Provider() {
        return XRAnchorStoreProvider::FromBytes(
                   {0x56, 0xd2, 0x83, 0x34, 0x84, 0x31, 0x47, 0x82, 0x97, 0x8b, 0x3d, 0x26, 0x45, 0x9b, 0x56, 0x7f})
            .Value();
    }

    OpenXRProtectedAnchorStore::OpenXRProtectedAnchorStore(const std::shared_ptr<OpenXRNativeSession> &owner, XRAnchorScope liveScope,
                                                           XrSpace world, XRAnchorStoreScope storeScope, Clock clock)
        : owner_(owner), liveScope_(liveScope), world_(world), scope_(storeScope), clock_(clock) {}

    Result<std::shared_ptr<OpenXRProtectedAnchorStore>> OpenXRProtectedAnchorStore::Open(const std::shared_ptr<OpenXRNativeSession> &owner,
                                                                                         const XRAnchorScope &liveScope, XrSpace world,
                                                                                         const XRAnchorStoreScope &storeScope,
                                                                                         const CancellationSource &consent, Clock clock) {
        using Outcome = Result<std::shared_ptr<OpenXRProtectedAnchorStore>>;
        if (!owner || !clock || !storeScope.partition.IsValid() || storeScope.accessRevision == 0)
            return Outcome::Failure(MakeError(XRErrors::OperationInvalid));
        if (storeScope.provider != Provider() || storeScope.providerVersion != 1)
            return Outcome::Failure(MakeError(XRErrors::ContractVersionIncompatible));
        try {
            auto store =
                std::shared_ptr<OpenXRProtectedAnchorStore>{new OpenXRProtectedAnchorStore(owner, liveScope, world, storeScope, clock)};
            if (store->CurrentUnixSeconds() == 0)
                return Outcome::Failure(MakeError(XRErrors::TimeDomainIncompatible));
            const auto opened = owner->OpenSpatialAnchorStore(liveScope, world, consent);
            if (opened.HasError())
                return Outcome::Failure(opened.ErrorValue());
            const auto connection = owner->CheckSpatialAnchorStore(liveScope, world);
            if (connection.HasError()) {
                const auto closed = owner->CloseSpatialAnchorStore(liveScope);
                return Outcome::Failure(closed.HasError() ? closed.ErrorValue() : connection.ErrorValue());
            }
            store->connection_ = connection.Value();
            return Outcome::Success(std::move(store));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(XRErrors::CapacityExceeded));
        }
    }

    Result<std::shared_ptr<OpenXRNativeSession>> OpenXRProtectedAnchorStore::Borrow(const XRAnchorStoreScope &scope) const {
        using Outcome = Result<std::shared_ptr<OpenXRNativeSession>>;
        if (std::this_thread::get_id() != lane_)
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible));
        if (!SameScope(scope, scope_))
            return Outcome::Failure(MakeError(XRErrors::PermissionDenied));
        auto owner = owner_.lock();
        if (!owner)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        const auto current = owner->CheckSpatialAnchorStore(liveScope_, world_);
        if (current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (current.Value() != connection_)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        return Outcome::Success(std::move(owner));
    }

    Result<void> OpenXRProtectedAnchorStore::Admit(const XRAnchorStoreScope &scope) const {
        const auto owner = Borrow(scope);
        return owner.HasError() ? Result<void>::Failure(owner.ErrorValue()) : Result<void>::Success();
    }

    std::uint64_t OpenXRProtectedAnchorStore::CurrentUnixSeconds() const noexcept {
        if (std::this_thread::get_id() != lane_)
            return 0;
        const auto now = clock_();
        if (now < lastUnixSeconds_)
            return 0;
        lastUnixSeconds_ = now;
        return now;
    }

    Result<void> OpenXRProtectedAnchorStore::Record(const XRSpatialAnchorId &anchor, XRRuntimeTime now, XRProtectedAnchorEntry entry) {
        const auto owner = Borrow(scope_);
        if (owner.HasError())
            return Result<void>::Failure(owner.ErrorValue());
        const auto unixSeconds = CurrentUnixSeconds();
        if (unixSeconds == 0 || !entry.reference.IsValid() || entry.expiresAtUnixSeconds <= unixSeconds)
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        if (std::ranges::find(live_, anchor, &Entry::anchor) != live_.end())
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        const auto slot = std::ranges::find(live_, XRSpatialAnchorId{}, &Entry::anchor);
        if (slot == live_.end())
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        const auto existing = Resolve(scope_, entry.reference);
        if (existing.HasError())
            return Result<void>::Failure(existing.ErrorValue());
        if (existing.Value())
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        const auto written = owner.Value()->PersistSpatialAnchor(liveScope_, world_, anchor, now, NativeName(scope_, entry));
        if (written.HasValue())
            *slot = {anchor, entry};
        return written;
    }

    Result<XRProtectedAnchorEntry> OpenXRProtectedAnchorStore::FindLive(const XRAnchorStoreScope &scope,
                                                                        const XRSpatialAnchorId &anchor) const {
        using Outcome = Result<XRProtectedAnchorEntry>;
        const auto owner = Borrow(scope);
        if (owner.HasError())
            return Outcome::Failure(owner.ErrorValue());
        const auto found = std::ranges::find(live_, anchor, &Entry::anchor);
        if (!anchor.IsValid() || found == live_.end())
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        return Outcome::Success(found->stored);
    }

    StoredEntry OpenXRProtectedAnchorStore::Resolve(const XRAnchorStoreScope &scope, const XRProtectedAnchorReference &reference) const {
        const auto owner = Borrow(scope);
        if (owner.HasError())
            return StoredEntry::Failure(owner.ErrorValue());
        if (!reference.IsValid())
            return StoredEntry::Failure(MakeError(XRErrors::OperationInvalid));
        std::array<XrSpatialAnchorPersistenceNameMSFT, 64> names{};
        const auto count = owner.Value()->ReadSpatialAnchorStore(liveScope_, world_, names);
        if (count.HasError())
            return StoredEntry::Failure(count.ErrorValue());
        return FindRecord(std::span{names}.first(count.Value()), NamePrefix(scope, reference), reference);
    }

    Result<std::optional<XRSpatialAnchorId>> OpenXRProtectedAnchorStore::Relocalize(const XRAnchorPersistence &participant,
                                                                                    XRRuntimeTime now) const {
        using Outcome = Result<std::optional<XRSpatialAnchorId>>;
        const auto owner = Borrow(scope_);
        if (owner.HasError())
            return Outcome::Failure(owner.ErrorValue());
        const auto published = participant.ReadRestored(*this);
        if (published.HasError())
            return Outcome::Failure(published.ErrorValue());
        const auto &restored = published.Value();
        if (restored.status != XRDurableAnchorStatus::Unlocalized || !restored.reference)
            return Outcome::Success(std::nullopt);
        const auto current = CurrentUnixSeconds();
        if (current == 0)
            return Outcome::Failure(MakeError(XRErrors::TimeDomainIncompatible));
        if (restored.expiresAtUnixSeconds <= current)
            return Outcome::Success(std::nullopt);
        const auto entry = Resolve(scope_, *restored.reference);
        if (entry.HasError())
            return Outcome::Failure(entry.ErrorValue());
        if (!entry.Value() || entry.Value()->expiresAtUnixSeconds != restored.expiresAtUnixSeconds)
            return Outcome::Success(std::nullopt);
        return owner.Value()->RestoreSpatialAnchor(liveScope_, world_, now, NativeName(scope_, *entry.Value()));
    }
}  // namespace Horo::XR::OpenXRInternal
