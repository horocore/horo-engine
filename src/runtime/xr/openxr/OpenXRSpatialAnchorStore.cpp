#include "host/OpenXRNativeSession.h"

#include <algorithm>
#include <bit>
#include <limits>

namespace Horo::XR::OpenXRInternal {
    namespace {
        constexpr auto Extension = XR_MSFT_SPATIAL_ANCHOR_PERSISTENCE_EXTENSION_NAME;
        constexpr std::size_t MaximumStoreNames = 64;

        struct StoreCall final {
            explicit StoreCall(bool &inProgress) noexcept : inProgress_(inProgress) {
                inProgress_ = true;
            }

            ~StoreCall() {
                inProgress_ = false;
            }

            StoreCall(const StoreCall &) = delete;
            StoreCall &operator=(const StoreCall &) = delete;

        private:
            bool &inProgress_;
        };

        template <typename Function>
        bool Resolve(PFN_xrGetInstanceProcAddr getProc, XrInstance instance, const char *name, Function &function) {
            PFN_xrVoidFunction raw{};
            if (getProc(instance, name, &raw) != XR_SUCCESS || !raw)
                return false;
            function = std::bit_cast<Function>(raw);
            return true;
        }

        Error StoreFailure(XrResult status) {
            switch (status) {
                case XR_ERROR_PERMISSION_INSUFFICIENT:
                    return MakeError(XRErrors::PermissionDenied);
                case XR_ERROR_LIMIT_REACHED:
                case XR_ERROR_SIZE_INSUFFICIENT:
                case XR_ERROR_OUT_OF_MEMORY:
                    return MakeError(XRErrors::CapacityExceeded);
                case XR_ERROR_SPATIAL_ANCHOR_NAME_INVALID_MSFT:
                    return MakeError(XRErrors::OperationInvalid);
                case XR_ERROR_FUNCTION_UNSUPPORTED:
                case XR_ERROR_FEATURE_UNSUPPORTED:
                    return MakeError(XRErrors::OperationUnsupported);
                case XR_SESSION_LOSS_PENDING:
                case XR_ERROR_SESSION_LOST:
                case XR_ERROR_INSTANCE_LOST:
                    return MakeError(XRErrors::IdentityStale);
                default:
                    return MakeError(XRErrors::OperationUnavailable);
            }
        }

        bool ValidName(std::string_view name) {
            return !name.empty() && name.size() < XR_MAX_SPATIAL_ANCHOR_NAME_SIZE_MSFT && std::ranges::all_of(name, [](const char ch) {
                return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
            });
        }

        XrSpatialAnchorPersistenceNameMSFT NativeName(std::string_view name) {
            XrSpatialAnchorPersistenceNameMSFT result{};
            std::ranges::copy(name, std::begin(result.name));
            return result;
        }

        std::string_view ReadName(const XrSpatialAnchorPersistenceNameMSFT &name) {
            const auto end = std::ranges::find(name.name, '\0');
            return {name.name, static_cast<std::size_t>(end - std::begin(name.name))};
        }
    }  // namespace

    Result<void> OpenXRNativeSession::OpenSpatialAnchorStore(const XRAnchorScope &scope, XrSpace world, const CancellationSource &grant) {
        if (controlThread_ != std::this_thread::get_id() || retirementCall_ || anchorCall_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        const StoreCall call{anchorCall_};
        if (anchorStore_.connection != XR_NULL_HANDLE)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        if (anchorStoreGeneration_ == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        auto consent = grant.Token();
        if (consent.IsCancellationRequested())
            return Result<void>::Failure(MakeError(XRErrors::PermissionDenied));
        const auto publication = BorrowAnchorPublication(scope, world);
        if (publication.HasError())
            return Result<void>::Failure(publication.ErrorValue());
        if (auto current = publication.Value()->CheckScope(scope); current.HasError())
            return current;
        const auto extension = ReadExtensionAdmission(scope.space.id.session, Extension);
        if (extension.HasError())
            return Result<void>::Failure(extension.ErrorValue());
        if (extension.Value().state == NativeExtensionState::Denied)
            return Result<void>::Failure(MakeError(XRErrors::PermissionDenied));
        if (extension.Value().state == NativeExtensionState::VersionIncompatible)
            return Result<void>::Failure(MakeError(XRErrors::ContractVersionIncompatible));
        if (extension.Value().state != NativeExtensionState::Enabled)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported));
        SpatialAnchorStore store;
        PFN_xrCreateSpatialAnchorStoreConnectionMSFT open{};
        const auto proc = dispatch_.getProc;
        if (!Resolve(proc, instance_, "xrCreateSpatialAnchorStoreConnectionMSFT", open) ||
            !Resolve(proc, instance_, "xrDestroySpatialAnchorStoreConnectionMSFT", store.destroy) ||
            !Resolve(proc, instance_, "xrPersistSpatialAnchorMSFT", store.persist) ||
            !Resolve(proc, instance_, "xrEnumeratePersistedSpatialAnchorNamesMSFT", store.enumerate) ||
            !Resolve(proc, instance_, "xrCreateSpatialAnchorFromPersistedNameMSFT", store.restore) ||
            !Resolve(proc, instance_, "xrUnpersistSpatialAnchorMSFT", store.remove))
            return Result<void>::Failure(MakeError(XRErrors::LoaderIncompatible));
        if (consent.IsCancellationRequested())
            return Result<void>::Failure(MakeError(XRErrors::PermissionDenied));
        if (auto current = publication.Value()->CheckScope(scope); current.HasError())
            return current;
        const auto status = open(session_, &store.connection);
        if (XR_FAILED(status))
            return Result<void>::Failure(StoreFailure(status));
        if (store.connection == XR_NULL_HANDLE)
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        store.consent = std::move(consent);
        anchorStore_ = std::move(store);
        const auto current = publication.Value()->CheckScope(scope);
        if (status != XR_SUCCESS || current.HasError() || anchorStore_.consent.IsCancellationRequested()) {
            const bool revoked = anchorStore_.consent.IsCancellationRequested();
            if (const auto closed = RetireSpatialAnchorStore(); closed.HasError())
                return closed;
            if (revoked)
                return Result<void>::Failure(MakeError(XRErrors::PermissionDenied));
            return current.HasError() ? current : Result<void>::Failure(StoreFailure(status));
        }
        anchorStore_.admitted = true;
        ++anchorStoreGeneration_;
        return Result<void>::Success();
    }

    Result<std::shared_ptr<XRSpatialAnchors>> OpenXRNativeSession::BorrowAnchorStore(const XRAnchorScope &scope, XrSpace world) const {
        using Outcome = Result<std::shared_ptr<XRSpatialAnchors>>;
        if (!anchorStore_.admitted || anchorStore_.pendingRemoval)
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable));
        if (anchorStore_.consent.IsCancellationRequested())
            return Outcome::Failure(MakeError(XRErrors::PermissionDenied));
        auto publication = BorrowAnchorPublication(scope, world);
        if (publication.HasError())
            return publication;
        const auto current = publication.Value()->CheckScope(scope);
        if (current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (anchorStore_.consent.IsCancellationRequested())
            return Outcome::Failure(MakeError(XRErrors::PermissionDenied));
        return publication;
    }

    Result<std::uint64_t> OpenXRNativeSession::CheckSpatialAnchorStore(const XRAnchorScope &scope, XrSpace world) {
        using Outcome = Result<std::uint64_t>;
        if (controlThread_ != std::this_thread::get_id() || retirementCall_ || anchorCall_)
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible));
        const StoreCall call{anchorCall_};
        const auto current = BorrowAnchorStore(scope, world);
        return current.HasError() ? Outcome::Failure(current.ErrorValue()) : Outcome::Success(anchorStoreGeneration_);
    }

    Result<std::size_t> OpenXRNativeSession::SpatialAnchorStore::Read(std::span<XrSpatialAnchorPersistenceNameMSFT> names) const {
        using Outcome = Result<std::size_t>;
        std::uint32_t count{};
        const auto status = enumerate(connection, static_cast<std::uint32_t>(names.size()), &count, names.data());
        if (status != XR_SUCCESS)
            return Outcome::Failure(StoreFailure(status));
        if (count > names.size())
            return Outcome::Failure(MakeError(XRErrors::CapacityExceeded));
        return Outcome::Success(count);
    }

    Result<std::size_t> OpenXRNativeSession::ReadSpatialAnchorStore(const XRAnchorScope &scope, XrSpace world,
                                                                    std::span<XrSpatialAnchorPersistenceNameMSFT> names) {
        using Outcome = Result<std::size_t>;
        if (controlThread_ != std::this_thread::get_id() || retirementCall_ || anchorCall_)
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible));
        const StoreCall call{anchorCall_};
        if (names.empty() || names.size() > MaximumStoreNames)
            return Outcome::Failure(MakeError(XRErrors::CapacityExceeded));
        auto active = BorrowAnchorStore(scope, world);
        if (active.HasError())
            return Outcome::Failure(active.ErrorValue());
        std::array<XrSpatialAnchorPersistenceNameMSFT, MaximumStoreNames> scratch{};
        const auto read = anchorStore_.Read(std::span{scratch}.first(names.size()));
        if (read.HasError())
            return read;
        active = BorrowAnchorStore(scope, world);
        if (active.HasError())
            return Outcome::Failure(active.ErrorValue());
        const auto entries = std::span{scratch}.first(read.Value());
        if (std::ranges::any_of(entries, [](const auto &entry) {
            const auto name = ReadName(entry);
            return name.empty() || name.size() == XR_MAX_SPATIAL_ANCHOR_NAME_SIZE_MSFT;
        }))
            return Outcome::Failure(MakeError(XRErrors::OperationInvalid));
        std::ranges::copy(entries, names.begin());
        return read;
    }

    Result<void> OpenXRNativeSession::PersistSpatialAnchor(const XRAnchorScope &scope, XrSpace world, const XRSpatialAnchorId &id,
                                                           XRRuntimeTime now, std::string_view name) {
        if (controlThread_ != std::this_thread::get_id() || retirementCall_ || anchorCall_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        const StoreCall call{anchorCall_};
        if (!ValidName(name))
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        const auto selectedName = NativeName(name);
        auto active = BorrowAnchorStore(scope, world);
        if (active.HasError())
            return Result<void>::Failure(active.ErrorValue());
        const auto anchor = active.Value()->Read(scope, id, now);
        if (anchor.HasError())
            return Result<void>::Failure(anchor.ErrorValue());
        if (anchor.Value().Observation().state != XRAnchorLocalization::Localized)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        const auto slot = std::ranges::find(anchors_.slots, id, &SpatialAnchors::Slot::id);
        if (slot == anchors_.slots.end() || anchorStore_.pendingRemoval)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        std::array<XrSpatialAnchorPersistenceNameMSFT, MaximumStoreNames> names{};
        const auto entries = anchorStore_.Read(names);
        if (entries.HasError())
            return Result<void>::Failure(entries.ErrorValue());
        if (entries.Value() == names.size())
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        if (std::ranges::any_of(std::span{names}.first(entries.Value()), [&selectedName](const auto &entry) {
            return ReadName(entry) == ReadName(selectedName);
        }))
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        active = BorrowAnchorStore(scope, world);
        if (active.HasError())
            return Result<void>::Failure(active.ErrorValue());
        const XrSpatialAnchorPersistenceInfoMSFT info{XR_TYPE_SPATIAL_ANCHOR_PERSISTENCE_INFO_MSFT, nullptr, selectedName, slot->anchor};
        const auto status = anchorStore_.persist(anchorStore_.connection, &info);
        if (XR_FAILED(status))
            return Result<void>::Failure(StoreFailure(status));
        active = BorrowAnchorStore(scope, world);
        if (status == XR_SUCCESS && active.HasValue()) {
            const auto current = active.Value()->Read(scope, id, now);
            if (current.HasValue() && current.Value().Observation().state == XRAnchorLocalization::Localized)
                return Result<void>::Success();
        }
        anchorStore_.pendingRemoval = info.spatialAnchorPersistenceName;
        if (auto undone = anchorStore_.UndoPendingWrite(); undone.HasError())
            return undone;
        if (status == XR_SUCCESS && active.HasValue())
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        return Result<void>::Failure(active.HasError() ? active.ErrorValue() : StoreFailure(status));
    }

    Result<void> OpenXRNativeSession::DiscardSpatialAnchorWrite(const XRAnchorScope &scope, std::uint64_t generation,
                                                                std::string_view name) {
        if (controlThread_ != std::this_thread::get_id() || retirementCall_ || anchorCall_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        const StoreCall call{anchorCall_};
        if (!scope.grant.IsValid() || scope != anchors_.scope || generation != anchorStoreGeneration_ ||
            anchorStore_.connection == XR_NULL_HANDLE)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        if (!ValidName(name))
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        if (anchorStore_.pendingRemoval && ReadName(*anchorStore_.pendingRemoval) != name)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        anchorStore_.pendingRemoval = NativeName(name);
        return anchorStore_.UndoPendingWrite();
    }

    Result<std::optional<XRSpatialAnchorId>> OpenXRNativeSession::RestoreSpatialAnchor(const XRAnchorScope &scope, XrSpace world,
                                                                                       XRRuntimeTime now, std::string_view name) {
        using Outcome = Result<std::optional<XRSpatialAnchorId>>;
        if (controlThread_ != std::this_thread::get_id() || retirementCall_ || anchorCall_)
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible));
        const StoreCall call{anchorCall_};
        if (!ValidName(name))
            return Outcome::Failure(MakeError(XRErrors::OperationInvalid));
        const auto selectedName = NativeName(name);
        auto active = BorrowAnchorStore(scope, world);
        if (active.HasError())
            return Outcome::Failure(active.ErrorValue());
        auto slots = std::span{anchors_.slots}.first(anchors_.maximumAnchors);
        const auto slot = std::ranges::find_if(slots, [](const auto &entry) {
            return entry.anchor == XR_NULL_HANDLE && entry.space == XR_NULL_HANDLE;
        });
        if (slot == slots.end())
            return Outcome::Failure(MakeError(XRErrors::CapacityExceeded));
        const auto id = active.Value()->Create(scope, now);
        if (id.HasError())
            return Outcome::Failure(id.ErrorValue());
        const XrSpatialAnchorFromPersistedAnchorCreateInfoMSFT info{XR_TYPE_SPATIAL_ANCHOR_FROM_PERSISTED_ANCHOR_CREATE_INFO_MSFT, nullptr,
                                                                    anchorStore_.connection, selectedName};
        XrSpatialAnchorMSFT restored{XR_NULL_HANDLE};
        auto status = anchorStore_.restore(session_, &info, &restored);
        if (XR_SUCCEEDED(status))
            slot->anchor = restored;
        auto current = BorrowAnchorStore(scope, world);
        if (status == XR_SUCCESS && slot->anchor != XR_NULL_HANDLE && current.HasValue()) {
            status = anchors_.AttachSpace(*slot, session_);
            current = BorrowAnchorStore(scope, world);
        }
        if (status == XR_SUCCESS && slot->space != XR_NULL_HANDLE && current.HasValue()) {
            slot->id = id.Value();
            slot->lastSample = now;
            return Outcome::Success(id.Value());
        }
        (void)active.Value()->Retire(scope, id.Value());
        auto failure = current.HasError() ? current.ErrorValue() : StoreFailure(status);
        if (auto retired = anchors_.Release(*slot); retired.HasError())
            return Outcome::Failure(WithCause(std::move(retired).ErrorValue(), std::move(failure)));
        if (current.HasValue() && status == XR_ERROR_SPATIAL_ANCHOR_NAME_NOT_FOUND_MSFT)
            return Outcome::Success(std::nullopt);
        return Outcome::Failure(std::move(failure));
    }

    Result<void> OpenXRNativeSession::SpatialAnchorStore::UndoPendingWrite() {
        if (!pendingRemoval)
            return Result<void>::Success();
        const auto status = remove(connection, &*pendingRemoval);
        if (status != XR_SUCCESS && status != XR_ERROR_SPATIAL_ANCHOR_NAME_NOT_FOUND_MSFT)
            return Result<void>::Failure(StoreFailure(status));
        pendingRemoval.reset();
        return Result<void>::Success();
    }

    Result<void> OpenXRNativeSession::CloseSpatialAnchorStore(const XRAnchorScope &scope) {
        if (controlThread_ != std::this_thread::get_id() || retirementCall_ || anchorCall_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        const StoreCall call{anchorCall_};
        if (!scope.grant.IsValid() || scope != anchors_.scope)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        return RetireSpatialAnchorStore();
    }

    Result<void> OpenXRNativeSession::RetireSpatialAnchorStore() {
        anchorStore_.admitted = false;
        if (anchorStore_.connection == XR_NULL_HANDLE)
            return Result<void>::Success();
        if (auto undone = anchorStore_.UndoPendingWrite(); undone.HasError())
            return undone;
        const auto status = anchorStore_.destroy(anchorStore_.connection);
        if (XR_FAILED(status))
            return Result<void>::Failure(StoreFailure(status));
        anchorStore_ = {};
        return Result<void>::Success();
    }
}  // namespace Horo::XR::OpenXRInternal
