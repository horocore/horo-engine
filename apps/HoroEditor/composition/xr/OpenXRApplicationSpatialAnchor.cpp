#include "../play/save/EditorPlaySaveIdentityIssuing.h"
#include "OpenXRApplicationSession.h"
#include "OpenXRProtectedAnchorStore.h"

#include <chrono>
#include <limits>
#include <new>

namespace Horo::Application::Internal {
    using namespace XR;
    using namespace XR::OpenXRInternal;

    namespace {
        constexpr XRAnchorLimits PreviewLimits{1, Duration::FromMilliseconds(250)};
        constexpr auto SampleInterval = Duration::FromMilliseconds(100);
        constexpr std::string_view AnchorParticipant = "horo.xr.world_anchor";
        constexpr std::string_view AnchorRecord = "17a778c9-686f-42fd-888c-d29b57aaf104";
        constexpr std::uint64_t AnchorRetentionSeconds = 24 * 60 * 60;

        std::uint64_t AnchorUnixSeconds() noexcept {
            const auto seconds =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            return seconds > 0 ? static_cast<std::uint64_t>(seconds) : 0;
        }
    }  // namespace

    bool OpenXRApplicationSession::MayArmSpatialAnchor() const {
        if (!settings_.AllowsSpatialAnchorPreview() || !native_)
            return false;
        const auto route = native_->ReadExtensionAdmission(selection_.attempt, XR_MSFT_SPATIAL_ANCHOR_EXTENSION_NAME);
        return route.HasValue() && route.Value().state == NativeExtensionState::Enabled;
    }

    bool OpenXRApplicationSession::MayArmSpatialAnchorStorage() const {
        if (!settings_.AllowsSpatialAnchorPersistence() || !native_)
            return false;
        const auto route = native_->ReadExtensionAdmission(selection_.attempt, XR_MSFT_SPATIAL_ANCHOR_PERSISTENCE_EXTENSION_NAME);
        return route.HasValue() && route.Value().state == NativeExtensionState::Enabled;
    }

    bool OpenXRApplicationSession::HasSpatialAnchorStorageConsent() const noexcept {
        return spatialAnchor_.storeGrant.has_value();
    }

    Result<void> OpenXRApplicationSession::SetSpatialAnchorConsent(bool preview, bool storage,
                                                                   const std::shared_ptr<const XRSessionLifecycle> &lifecycle) {
        RevokeSpatialAnchor();
        if (!preview && !storage)
            return Result<void>::Success();
        if (spatialAnchor_.scope || !lifecycle || (preview && !MayArmSpatialAnchor()) || (storage && !MayArmSpatialAnchorStorage()))
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        if (spatialAnchor_.grant == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        try {
            auto publication = std::make_shared<XRSpatialAnchors>(lifecycle);
            if (storage)
                spatialAnchor_.storeGrant.emplace();
            spatialAnchor_.publication = std::move(publication);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        }
        ++spatialAnchor_.grant;
        spatialAnchor_.allowed = true;
        spatialAnchor_.preview = preview;
        return Result<void>::Success();
    }

    Result<OpenXRApplicationAnchorSave> OpenXRApplicationSession::BindSpatialAnchorSave(const XRAnchorStoreScope &storeScope,
                                                                                        std::uint64_t (*unixSeconds)() noexcept) {
        using Outcome = Result<OpenXRApplicationAnchorSave>;
        if (const auto current = Current(); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (!settings_.AllowsSpatialAnchorPersistence())
            return Outcome::Failure(MakeError(XRErrors::OperationUnsupported));
        if (!native_ || !spatialAnchor_.storeGrant || !spatialAnchor_.scope || !spatialAnchor_.publication)
            return Outcome::Failure(MakeError(XRErrors::PermissionDenied));

        const auto live = *spatialAnchor_.scope;
        const auto world = BorrowWorldSpace(live.space);
        if (world.HasError())
            return Outcome::Failure(world.ErrorValue());
        if (!native_ || !spatialAnchor_.storeGrant || spatialAnchor_.scope != live || !spatialAnchor_.publication)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));

        const auto owner = native_;
        const auto anchors = spatialAnchor_.publication;
        const auto consent = *spatialAnchor_.storeGrant;
        auto store = OpenXRProtectedAnchorStore::Open(owner, live, world.Value(), storeScope, consent, unixSeconds);
        if (store.HasError())
            return Outcome::Failure(store.ErrorValue());
        spatialAnchor_.storeOpen = true;
        const auto participant = Runtime::SaveParticipantId::Parse(AnchorParticipant);
        const auto record = Runtime::SaveRecordId::Parse(AnchorRecord);
        auto adapter = XRAnchorPersistence::Bind(participant.Value(), record.Value(), anchors, live, storeScope, store.Value());
        if (adapter.HasError()) {
            const auto closed = owner->CloseSpatialAnchorStore(live);
            spatialAnchor_.storeOpen = closed.HasError();
            return Outcome::Failure(closed.HasError() ? closed.ErrorValue() : adapter.ErrorValue());
        }
        return Outcome::Success({std::move(adapter).Value(), std::move(store).Value()});
    }

    Runtime::CanonicalStateParticipantDescriptor OpenXRApplicationSession::SpatialAnchorSaveDescriptor() {
        return XRAnchorPersistence::Describe(Runtime::SaveParticipantId::Parse(AnchorParticipant).Value(),
                                             Runtime::SaveRecordId::Parse(AnchorRecord).Value());
    }

    Result<std::shared_ptr<const Runtime::ICanonicalStateAdapter>> OpenXRApplicationSession::PrepareSpatialAnchorCapture(
        Runtime::SaveWorldId world, Security::SecureRandomSource &entropy) {
        using Outcome = Result<std::shared_ptr<const Runtime::ICanonicalStateAdapter>>;
        if (!spatialAnchor_.storeGrant || !spatialAnchor_.localized)
            return Outcome::Success({});
        auto acquired = AcquireSpatialAnchorSave(world);
        if (acquired.HasError())
            return Outcome::Failure(acquired.ErrorValue());
        const auto binding = std::move(acquired).Value();
        if (!spatialAnchor_.recorded) {
            auto id = IssuePlaySaveIdentity<XRProtectedAnchorReference>(entropy);
            if (id.HasError())
                return Outcome::Failure(id.ErrorValue());
            const auto now = AnchorUnixSeconds();
            if (now == 0 || now > std::numeric_limits<std::uint64_t>::max() - AnchorRetentionSeconds)
                return Outcome::Failure(MakeError(XRErrors::TimeDomainIncompatible));
            const auto time = spatialAnchor_.cutoff;
            const auto anchor = spatialAnchor_.id;
            const auto written = binding.store->Record(anchor, time, {id.Value(), now + AnchorRetentionSeconds});
            if (written.HasError())
                return Outcome::Failure(written.ErrorValue());
            spatialAnchor_.recorded = true;
        }
        const auto selected = binding.participant->RecordLocalized(spatialAnchor_.id, spatialAnchor_.cutoff);
        return selected.HasError() ? Outcome::Failure(selected.ErrorValue()) : Outcome::Success(binding.participant);
    }

    Result<OpenXRApplicationAnchorSave> OpenXRApplicationSession::AcquireSpatialAnchorSave(Runtime::SaveWorldId world) {
        using Outcome = Result<OpenXRApplicationAnchorSave>;
        if (const auto current = Current(); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (spatialAnchor_.save)
            return spatialAnchor_.saveWorld == world ? Outcome::Success(*spatialAnchor_.save)
                                                     : Outcome::Failure(MakeError(XRErrors::IdentityStale));
        const auto partition = XRAnchorStorePartition::FromBytes(world.Bytes());
        if (partition.HasError())
            return Outcome::Failure(partition.ErrorValue());
        auto binding = BindSpatialAnchorSave({OpenXRProtectedAnchorStore::Provider(), partition.Value(), 1, 1}, &AnchorUnixSeconds);
        if (binding.HasError())
            return binding;
        spatialAnchor_.save = binding.Value();
        spatialAnchor_.saveWorld = world;
        return binding;
    }

    Result<OpenXRApplicationAnchorRestore> OpenXRApplicationSession::StageSpatialAnchorRestore(Runtime::SaveWorldId world,
                                                                                               std::span<const std::byte> record) {
        using Outcome = Result<OpenXRApplicationAnchorRestore>;
        if (record.empty() && !spatialAnchor_.save)
            return Outcome::Success({});
        auto binding = AcquireSpatialAnchorSave(world);
        if (binding.HasError())
            return Outcome::Failure(binding.ErrorValue());
        auto staged = binding.Value().participant->StageRestore(record);
        if (staged.HasError())
            return Outcome::Failure(staged.ErrorValue());
        auto receipt = std::move(staged).Value();
        return Outcome::Success({std::move(receipt.participant), receipt.publication});
    }

    void OpenXRApplicationSession::RelocalizeRestoredAnchor() noexcept {
        // The Scene and Save operation already committed. A provider outage leaves the durable result unlocalized.
        try {
            if (!spatialAnchor_.save || RetireRestoredAnchor().HasError())
                return;
            const auto binding = *spatialAnchor_.save;
            const auto restored = binding.store->Relocalize(*binding.participant, spatialAnchor_.cutoff);
            if (restored.HasValue())
                spatialAnchor_.restored = restored.Value();
        } catch (const std::bad_alloc &) {
            return;
        }
    }

    Result<void> OpenXRApplicationSession::RetireRestoredAnchor() {
        if (!spatialAnchor_.restored || !spatialAnchor_.scope)
            return Result<void>::Success();
        const auto scope = *spatialAnchor_.scope;
        const auto world = BorrowWorldSpace(scope.space);
        if (world.HasError())
            return Result<void>::Failure(world.ErrorValue());
        if (spatialAnchor_.scope != scope || !spatialAnchor_.restored)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        const auto retired = native_->RetireSpatialAnchor(scope, world.Value(), *spatialAnchor_.restored);
        if (retired.HasValue())
            spatialAnchor_.restored.reset();
        return retired;
    }

    void OpenXRApplicationSession::ProgressRestoredAnchor(XrSpace world, XRRuntimeTime now) {
        if (spatialAnchor_.restored && spatialAnchor_.storeGrant)
            (void)native_->SampleSpatialAnchor(*spatialAnchor_.scope, world, *spatialAnchor_.restored, now);
    }

    void OpenXRApplicationSession::RevokeSpatialAnchorPreview() noexcept {
        spatialAnchor_.preview = false;
        if (!spatialAnchor_.storeGrant)
            RevokeSpatialAnchor();
    }

    void OpenXRApplicationSession::RevokeSpatialAnchorStorage() noexcept {
        spatialAnchor_.save.reset();
        spatialAnchor_.saveWorld.reset();
        spatialAnchor_.recorded = false;
        if (spatialAnchor_.storeGrant) {
            spatialAnchor_.storeGrant->RequestCancellation();
            spatialAnchor_.storeGrant.reset();
        }
        if (!spatialAnchor_.preview)
            RevokeSpatialAnchor();
    }

    void OpenXRApplicationSession::RevokeSpatialAnchor() noexcept {
        spatialAnchor_.save.reset();
        spatialAnchor_.saveWorld.reset();
        spatialAnchor_.recorded = false;
        spatialAnchor_.preview = false;
        if (spatialAnchor_.storeGrant) {
            spatialAnchor_.storeGrant->RequestCancellation();
            spatialAnchor_.storeGrant.reset();
        }
        spatialAnchor_.allowed = false;
        spatialAnchor_.localized = false;
        spatialAnchor_.sampled = {};
        spatialAnchor_.cutoff = {};
        if (spatialAnchor_.scope && spatialAnchor_.publication)
            (void)spatialAnchor_.publication->Withdraw(*spatialAnchor_.scope, XRAnchorWithdrawal::AccessRevoked);
    }

    Result<void> OpenXRApplicationSession::ProgressSpatialAnchorRetirement() {
        if (!spatialAnchor_.allowed && spatialAnchor_.scope) {
            const auto stopped = native_->StopSpatialAnchors(*spatialAnchor_.scope, XRAnchorWithdrawal::AccessRevoked);
            if (stopped.HasError())
                return stopped;
            spatialAnchor_.storeOpen = false;
            spatialAnchor_.restored.reset();
            spatialAnchor_.scope.reset();
            spatialAnchor_.id = {};
        }
        if (!spatialAnchor_.allowed)
            spatialAnchor_.publication.reset();
        if (spatialAnchor_.storeOpen && !spatialAnchor_.storeGrant && spatialAnchor_.scope) {
            const auto closed = native_->CloseSpatialAnchorStore(*spatialAnchor_.scope);
            if (closed.HasError())
                return closed;
            spatialAnchor_.storeOpen = false;
        }
        return spatialAnchor_.storeGrant ? Result<void>::Success() : RetireRestoredAnchor();
    }

    XRCapabilityState OpenXRApplicationSession::ReadSpatialAnchorState() const noexcept {
        if (!spatialAnchor_.preview)
            return XRCapabilityState::Unavailable;
        const bool fresh = spatialAnchor_.localized && spatialAnchor_.cutoff.Nanoseconds() - spatialAnchor_.sampled.Nanoseconds() <=
                                                           PreviewLimits.maximumSampleAge.ToNanoseconds();
        return fresh ? XRCapabilityState::Available : XRCapabilityState::TemporarilyUnavailable;
    }

    Result<void> OpenXRApplicationSession::StartSpatialAnchor(const XRCoordinateSpace &space, XrSpace world, XRRuntimeTime now) {
        const XRAnchorScope scope{space, XRAnchorProviderGeneration::Create(selection_.attempt.Value()).Value(),
                                  XRAnchorGrant::Create(spatialAnchor_.grant).Value()};
        auto limits = PreviewLimits;
        if (spatialAnchor_.storeGrant)
            limits.maximumAnchors = 2;
        const auto opened = native_->StartSpatialAnchors(scope, world, limits, spatialAnchor_.publication);
        if (opened.HasError())
            return opened;
        spatialAnchor_.scope = scope;
        if (!spatialAnchor_.allowed)
            return Result<void>::Failure(MakeError(XRErrors::PermissionDenied));
        // The preview anchors the calibrated native World origin. No room pose leaves the XR owner.
        const auto created = native_->CreateSpatialAnchor(scope, world, {}, now);
        if (created.HasError())
            return Result<void>::Failure(created.ErrorValue());
        spatialAnchor_.id = created.Value();
        spatialAnchor_.sampled = now;
        return Result<void>::Success();
    }

    void OpenXRApplicationSession::AdvanceSpatialAnchor(const XRTrackingSnapshot &tracking) {
        if (!spatialAnchor_.allowed)
            return;
        if (tracking.Focus() != XRTrackingFocusState::Focused || tracking.State() != XRTrackingState::Active) {
            if (spatialAnchor_.scope)
                RevokeSpatialAnchor();
            return;
        }
        const auto space = WorldSpace(tracking.Session());
        if (space.HasError() || (spatialAnchor_.scope && space.Value() != spatialAnchor_.scope->space)) {
            RevokeSpatialAnchor();
            return;
        }
        const auto world = BorrowWorldSpace(space.Value());
        if (world.HasError() || !spatialAnchor_.allowed) {
            RevokeSpatialAnchor();
            return;
        }
        const auto now = tracking.SampledAt();
        spatialAnchor_.cutoff = now;
        if (!spatialAnchor_.scope) {
            if (StartSpatialAnchor(space.Value(), world.Value(), now).HasError())
                RevokeSpatialAnchor();
            return;
        }
        if (now.Nanoseconds() - spatialAnchor_.sampled.Nanoseconds() < SampleInterval.ToNanoseconds())
            return;
        const auto sampled = native_->SampleSpatialAnchor(*spatialAnchor_.scope, world.Value(), spatialAnchor_.id, now);
        if (sampled.HasError() || !spatialAnchor_.allowed) {
            RevokeSpatialAnchor();
            return;
        }
        const auto anchor = spatialAnchor_.publication->Read(*spatialAnchor_.scope, spatialAnchor_.id, now);
        spatialAnchor_.localized = anchor.HasValue() && anchor.Value().Observation().state == XRAnchorLocalization::Localized;
        spatialAnchor_.sampled = now;
        ProgressRestoredAnchor(world.Value(), now);
    }
}  // namespace Horo::Application::Internal
