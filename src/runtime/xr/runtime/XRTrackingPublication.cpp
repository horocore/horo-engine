#include "host/XRTrackingPublication.h"

#include <algorithm>
#include <limits>
#include <memory>

namespace Horo::XR::RuntimeInternal {
    namespace {
        bool IsHand(const XRTrackedDeviceRole role) noexcept {
            return role == XRTrackedDeviceRole::LeftHand || role == XRTrackedDeviceRole::RightHand;
        }

        bool CapabilitiesMatch(const XRTrackedDeviceRole role, const XRTrackedDeviceCapabilities &capabilities) {
            using enum Horo::XR::XRTrackedDeviceCapability;
            XRTrackedDeviceCapabilities expected;
            if (role == XRTrackedDeviceRole::Head)
                expected.values[static_cast<std::size_t>(HeadPose)] = true;
            else if (IsHand(role)) {
                expected.values[static_cast<std::size_t>(HandJoints)] = true;
                expected.values[static_cast<std::size_t>(PalmPose)] = capabilities.Has(PalmPose);
            } else {
                expected.values[static_cast<std::size_t>(GripPose)] = capabilities.Has(GripPose);
                expected.values[static_cast<std::size_t>(AimPose)] = capabilities.Has(AimPose);
                if (!capabilities.Has(GripPose) && !capabilities.Has(AimPose))
                    return false;
            }
            return expected == capabilities;
        }

        bool ValidPublicationLimits(const XRTrackingSnapshotLimits &limits) noexcept {
            return limits.maximumDevices > 0 && limits.maximumDevices <= 5 && limits.maximumPoses > 0 &&
                   limits.maximumPoses <= XRTrackingSnapshotHardLimits::MaximumPoses &&
                   limits.maximumActions <= XRTrackingSnapshotHardLimits::MaximumActions &&
                   limits.maximumHandJoints <= XRHandTrackingHardLimits::MaximumJoints;
        }

        /** @brief Maps only the supported normalized physical role/pose pairs, never a default capability. */
        std::optional<XRTrackedDeviceCapability> SourceCapability(const TrackingSourceEvidence &source) noexcept {
            using enum Horo::XR::XRTrackedDeviceCapability;
            if (source.role == XRTrackedDeviceRole::Head && source.kind == XRTrackedPoseKind::Head)
                return HeadPose;
            if (source.role == XRTrackedDeviceRole::LeftController || source.role == XRTrackedDeviceRole::RightController) {
                if (source.kind == XRTrackedPoseKind::Grip)
                    return GripPose;
                if (source.kind == XRTrackedPoseKind::Aim)
                    return AimPose;
            }
            if (IsHand(source.role) && source.kind == XRTrackedPoseKind::Palm)
                return PalmPose;
            return std::nullopt;
        }

        bool Located(const XRPoseDescriptor &pose) noexcept {
            return pose.components.positionMeters.validity != XRPoseComponentValidity::Invalid ||
                   pose.components.orientation.validity != XRPoseComponentValidity::Invalid;
        }

        bool Velocity(const XRPoseDescriptor &pose) noexcept {
            return pose.components.linearVelocityMetersPerSecond.validity != XRPoseComponentValidity::Invalid ||
                   pose.components.angularVelocityRadiansPerSecond.validity != XRPoseComponentValidity::Invalid;
        }

        bool ValidSourceShape(const TrackingSourceEvidence &source) noexcept {
            return SourceCapability(source).has_value() && (source.active || (!Located(source.pose) && !Velocity(source.pose))) &&
                   (source.role != XRTrackedDeviceRole::Head || source.active == Located(source.pose));
        }

        bool ValidSourceTime(const XRPoseDescriptor &pose, const XRTimeEvidence &first) noexcept {
            return pose.purpose == XRPosePurpose::SimulationInput && pose.time.simulation && !pose.time.renderPrediction &&
                   pose.time == first;
        }

        /** @brief Validates a complete normalized packet before any canonical device generation can change.
         * @param sources Bounded same-cutoff physical observations. @param session Exact active owner.
         * @param origin Exact current reference-space origin. @return Original typed failure or complete admission.
         */
        Result<void> ValidateSourceEvidence(const std::span<const TrackingSourceEvidence> sources, const XRSessionId &session,
                                            const XRWorldOriginRevision origin) {
            if (sources.size() > 7)
                return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
            for (std::size_t index = 0; index < sources.size(); ++index) {
                const auto &source = sources[index];
                const auto &pose = source.pose;
                if (!ValidSourceShape(source))
                    return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
                if (std::ranges::any_of(sources.first(index), [&](const TrackingSourceEvidence &prior) {
                    return prior.role == source.role && prior.kind == source.kind;
                }))
                    return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
                if (!ValidSourceTime(pose, sources.front().pose.time))
                    return Result<void>::Failure(MakeError(XRErrors::TimeDomainIncompatible));
                if (const auto valid = XRPoseSample::Create(pose, session, origin); valid.HasError())
                    return Result<void>::Failure(valid.ErrorValue());
            }
            return Result<void>::Success();
        }

        struct DeviceUpdate final {
            XRTrackedDeviceRole role;
            XRTrackedDeviceState state;
            XRTrackedDeviceCapabilities capabilities;
        };

        struct CollectedRole final {
            XRTrackedDeviceCapabilities capabilities;
            bool observed{};
            bool located{};
        };

        CollectedRole CollectRole(const std::span<const TrackingSourceEvidence> sources, const XRTrackedDeviceRole role,
                                  XRTrackedDeviceCapabilities capabilities) {
            CollectedRole result{capabilities};
            for (const auto &source : sources) {
                if (source.role != role || !source.active)
                    continue;
                result.capabilities.values[static_cast<std::size_t>(*SourceCapability(source))] = true;
                result.observed = true;
                result.located |= Located(source.pose);
            }
            return result;
        }

        XRTrackedDeviceState SourceState(const CollectedRole &role, const XRTrackedDeviceRecord *prior) noexcept {
            using enum Horo::XR::XRTrackedDeviceState;
            if (!role.observed && prior && prior->state == Disconnected)
                return Disconnected;
            return role.located ? Tracked : TrackingLost;
        }

        std::optional<DeviceUpdate> CollectDeviceUpdate(const std::span<const TrackingSourceEvidence> sources,
                                                        const XRTrackedDeviceRole role, const XRTrackedDeviceRecord *prior) {
            auto collected = CollectRole(sources, role, prior ? prior->capabilities : XRTrackedDeviceCapabilities{});
            if (!collected.observed && !prior)
                return std::nullopt;
            if (IsHand(role))
                collected.capabilities.values[static_cast<std::size_t>(XRTrackedDeviceCapability::HandJoints)] = true;
            return DeviceUpdate{role, SourceState(collected, prior), collected.capabilities};
        }

        bool Replaced(const XRTrackedDeviceRecord &prior, const XRTrackedDeviceState state,
                      const XRTrackedDeviceCapabilities &capabilities) noexcept {
            return prior.capabilities != capabilities ||
                   (prior.state == XRTrackedDeviceState::Disconnected && state != XRTrackedDeviceState::Disconnected);
        }

        Result<XRDeviceId> UpdateDevice(XRTrackedDeviceRecord &record, const XRTrackedDeviceState state,
                                        const XRTrackedDeviceCapabilities capabilities) {
            const bool replaced = Replaced(record, state, capabilities);
            if (replaced && record.id.slot.generation == std::numeric_limits<std::uint32_t>::max())
                return Result<XRDeviceId>::Failure(MakeError(XRErrors::IdentityStale));
            if (replaced)
                ++record.id.slot.generation;
            record.state = state;
            record.capabilities = capabilities;
            return Result<XRDeviceId>::Success(record.id);
        }

        struct DeviceUpdates final {
            std::array<DeviceUpdate, 5> values{};
            std::size_t count{};
            std::size_t newCount{};
        };

        Result<DeviceUpdates> PrepareDeviceUpdates(const std::span<const TrackingSourceEvidence> sources,
                                                   const std::span<const XRTrackedDeviceRecord> existing) {
            using enum Horo::XR::XRTrackedDeviceRole;
            DeviceUpdates result;
            constexpr std::array roles{Head, LeftController, RightController, LeftHand, RightHand};
            for (const auto role : roles) {
                const auto prior = std::ranges::find(existing, role, &XRTrackedDeviceRecord::role);
                const auto update = CollectDeviceUpdate(sources, role, prior == existing.end() ? nullptr : std::to_address(prior));
                if (!update)
                    continue;
                if (prior == existing.end())
                    ++result.newCount;
                else if (Replaced(*prior, update->state, update->capabilities) &&
                         prior->id.slot.generation == std::numeric_limits<std::uint32_t>::max())
                    return Result<DeviceUpdates>::Failure(MakeError(XRErrors::IdentityStale));
                result.values[result.count++] = *update;
            }
            return Result<DeviceUpdates>::Success(result);
        }

        Result<void> ApplyDeviceUpdates(XRTrackingPublication &owner, const DeviceUpdates &updates) {
            for (const auto &update : std::span{updates.values}.first(updates.count)) {
                if (auto result = owner.ObserveDevice(update.role, update.state, update.capabilities); result.HasError())
                    return Result<void>::Failure(result.ErrorValue());
            }
            return Result<void>::Success();
        }

        bool ValidPublicationTime(const TrackingPublicationRequest &request, const XRSessionSnapshot &lifecycle,
                                  const XRWorldOriginRevision origin, const std::uint64_t revision) noexcept {
            return (request.focus == XRTrackingFocusState::Focused) == lifecycle.admitsActions && request.time.runtimeSample.IsValid() &&
                   request.time.simulation && request.time.simulation->IsValid() && !request.time.renderPrediction &&
                   request.origin == origin && revision != std::numeric_limits<std::uint64_t>::max();
        }
    }  // namespace

    /** @copydoc XRTrackingPublication::XRTrackingPublication */
    XRTrackingPublication::XRTrackingPublication(XRSessionLifecycle &lifecycle) noexcept : lifecycle_(&lifecycle) {}

    /** @copydoc XRTrackingPublication::Lane */
    Result<void> XRTrackingPublication::Lane() const {
        if (std::this_thread::get_id() != lane_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        return Result<void>::Success();
    }

    /** @copydoc XRTrackingPublication::Bind */
    Result<void> XRTrackingPublication::Bind(const XRSessionId &candidate, const XRTrackingSnapshotLimits limits,
                                             const XRWorldOriginRevision origin) {
        if (auto lane = Lane(); lane.HasError())
            return lane;
        if (session_.IsValid() || !candidate.IsValid() || !origin.IsValid())
            return Result<void>::Failure(MakeError(XRErrors::IdentityInvalid));
        if (!ValidPublicationLimits(limits))
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        poses_.reserve(limits.maximumPoses);
        session_ = candidate;
        limits_ = limits;
        origin_ = origin;
        return Result<void>::Success();
    }

    /** @copydoc XRTrackingPublication::ObserveDevice */
    Result<XRDeviceId> XRTrackingPublication::ObserveDevice(const XRTrackedDeviceRole role, const XRTrackedDeviceState state,
                                                            const XRTrackedDeviceCapabilities capabilities) {
        using Outcome = Result<XRDeviceId>;
        if (const auto lane = Lane(); lane.HasError())
            return Outcome::Failure(lane.ErrorValue());
        current_.reset();
        if (auto valid = ValidateDevice(role, state, capabilities); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        const auto records = std::span{devices_}.first(deviceCount_);
        const auto found = std::ranges::find(records, role, &XRTrackedDeviceRecord::role);
        if (found == records.end()) {
            if (deviceCount_ == limits_.maximumDevices)
                return Outcome::Failure(MakeError(XRErrors::CapacityExceeded));
            current_.reset();
            auto &record = devices_[deviceCount_];
            record = {{session_, {static_cast<std::uint32_t>(deviceCount_), 1}}, role, state, capabilities};
            ++deviceCount_;
            return Outcome::Success(record.id);
        }
        return UpdateDevice(*found, state, capabilities);
    }

    Result<void> XRTrackingPublication::ValidateDevice(const XRTrackedDeviceRole role, const XRTrackedDeviceState state,
                                                       const XRTrackedDeviceCapabilities capabilities) const {
        if (!session_.IsValid() || role >= XRTrackedDeviceRole::Count || role == XRTrackedDeviceRole::GenericTracker ||
            state >= XRTrackedDeviceState::Count || !CapabilitiesMatch(role, capabilities))
            return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
        return IsHand(role) ? AdmitHands() : Result<void>::Success();
    }

    Result<void> XRTrackingPublication::AdmitSources(const std::span<const TrackingSourceEvidence> sources) const {
        if (std::ranges::any_of(sources, [](const auto &source) {
            return IsHand(source.role);
        }))
            return AdmitHands();
        return Result<void>::Success();
    }

    /** @copydoc XRTrackingPublication::ObserveSources */
    Result<void> XRTrackingPublication::ObserveSources(const std::span<const TrackingSourceEvidence> sources) {
        if (auto lane = Lane(); lane.HasError())
            return lane;
        current_.reset();
        if (auto admitted = lifecycle_->Admit(session_, true); admitted.HasError())
            return admitted;
        if (auto valid = ValidateSourceEvidence(sources, session_, origin_); valid.HasError())
            return valid;

        if (auto admitted = AdmitSources(sources); admitted.HasError())
            return admitted;
        const auto prepared = PrepareDeviceUpdates(sources, Devices());
        if (prepared.HasError())
            return Result<void>::Failure(prepared.ErrorValue());
        if (deviceCount_ > limits_.maximumDevices || prepared.Value().newCount > limits_.maximumDevices - deviceCount_)
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        return ApplyDeviceUpdates(*this, prepared.Value());
    }

    /** @copydoc XRTrackingPublication::Devices */
    std::span<const XRTrackedDeviceRecord> XRTrackingPublication::Devices() const noexcept {
        if (std::this_thread::get_id() != lane_)
            return {};
        return std::span{devices_}.first(deviceCount_);
    }

    Result<void> XRTrackingPublication::AdmitHands() const {
        const auto plan = lifecycle_->AcceptedPlan();
        if (!plan || limits_.maximumHandJoints == 0)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported));
        const auto decision = plan->Decision(XRCapability::HandTracking).state;
        if (decision != XRFeatureDecisionState::Required && decision != XRFeatureDecisionState::OptionalEnabled)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported));
        return Result<void>::Success();
    }

    Result<void> XRTrackingPublication::AdmitJointRecords(const std::span<const XRHandJointRecord> joints) const {
        return joints.empty() ? Result<void>::Success() : AdmitHands();
    }

    /** @copydoc XRTrackingPublication::PreparePoses */
    Result<void> XRTrackingPublication::PreparePoses(const TrackingPublicationRequest &request) {
        poses_.clear();
        if (request.poses.size() > limits_.maximumPoses)
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        for (const auto &evidence : request.poses) {
            if (evidence.pose.time != request.time)
                return Result<void>::Failure(MakeError(XRErrors::TimeDomainIncompatible));
            auto sample = XRPoseSample::Create(evidence.pose, session_, origin_);
            if (sample.HasError())
                return Result<void>::Failure(sample.ErrorValue());
            poses_.emplace_back(evidence.device, evidence.kind, std::move(sample).Value());
        }
        std::ranges::sort(poses_, {}, [](const XRTrackedPoseRecord &pose) {
            return std::pair{pose.device, pose.kind};
        });
        return Result<void>::Success();
    }

    /** @copydoc XRTrackingPublication::Publish */
    Result<void> XRTrackingPublication::Publish(const TrackingPublicationRequest &request) {
        if (auto lane = Lane(); lane.HasError())
            return lane;
        current_.reset();
        if (auto admitted = lifecycle_->Admit(session_, request.focus == XRTrackingFocusState::Focused); admitted.HasError())
            return admitted;
        if (!ValidPublicationTime(request, lifecycle_->Snapshot(), origin_, revision_))
            return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
        if (auto hands = AdmitJointRecords(request.handJoints); hands.HasError())
            return hands;
        if (auto prepared = PreparePoses(request); prepared.HasError())
            return prepared;
        auto revision = XRTrackingSnapshotRevision::Create(revision_ + 1);
        if (revision.HasError())
            return Result<void>::Failure(revision.ErrorValue());
        const bool unavailable = request.state == XRTrackingState::PermissionLost || request.state == XRTrackingState::SessionLost;
        const auto devices = unavailable ? std::span<const XRTrackedDeviceRecord>{} : Devices();
        auto candidate = XRTrackingSnapshot::Create({.session = session_,
                                                     .revision = revision.Value(),
                                                     .sequence = revision_ + 1,
                                                     .sampledAt = request.time.runtimeSample,
                                                     .worldOriginRevision = origin_,
                                                     .focus = request.focus,
                                                     .visibility = request.visibility,
                                                     .state = request.state,
                                                     .limits = limits_,
                                                     .devices = devices,
                                                     .poses = poses_,
                                                     .bindingRevision = request.bindingRevision,
                                                     .actions = request.actions,
                                                     .handJoints = request.handJoints},
                                                    session_, origin_);
        if (candidate.HasError())
            return Result<void>::Failure(candidate.ErrorValue());
        current_.emplace(std::move(candidate).Value());
        ++revision_;
        return Result<void>::Success();
    }

    /** @copydoc XRTrackingPublication::Current */
    const XRTrackingSnapshot *XRTrackingPublication::Current() const noexcept {
        if (std::this_thread::get_id() != lane_ || !current_)
            return nullptr;
        if (const auto snapshot = lifecycle_->Snapshot(); snapshot.session != session_ || !snapshot.admitsFrames ||
                                                          (current_->Focus() == XRTrackingFocusState::Focused && !snapshot.admitsActions))
            return nullptr;
        return &*current_;
    }

    /** @copydoc XRTrackingPublication::Close */
    void XRTrackingPublication::Close(const XRSessionId &session) noexcept {
        if (std::this_thread::get_id() != lane_ || session != session_)
            return;
        current_.reset();
        poses_.clear();
        devices_ = {};
        deviceCount_ = 0;
        session_ = {};
        limits_ = {};
        origin_ = {};
    }

    /** @copydoc XRTrackingPublication::Revoke */
    void XRTrackingPublication::Revoke() noexcept {
        if (std::this_thread::get_id() == lane_)
            current_.reset();
    }
}  // namespace Horo::XR::RuntimeInternal
