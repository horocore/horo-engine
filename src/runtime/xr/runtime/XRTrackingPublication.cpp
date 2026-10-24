#include "host/XRTrackingPublication.h"

#include <algorithm>
#include <limits>

namespace Horo::XR::RuntimeInternal {
    namespace {
        /** @brief Checks only the deliberately supported initial physical source profile. */
        bool CapabilitiesMatch(const XRTrackedDeviceRole role, const XRTrackedDeviceCapabilities &capabilities) {
            const bool head = role == XRTrackedDeviceRole::Head;
            const bool controller = role == XRTrackedDeviceRole::LeftController || role == XRTrackedDeviceRole::RightController;
            return capabilities.Has(XRTrackedDeviceCapability::HeadPose) == head &&
                   (capabilities.Has(XRTrackedDeviceCapability::GripPose) || capabilities.Has(XRTrackedDeviceCapability::AimPose)) ==
                       controller &&
                   !capabilities.Has(XRTrackedDeviceCapability::PalmPose) && !capabilities.Has(XRTrackedDeviceCapability::TrackerPose) &&
                   !capabilities.Has(XRTrackedDeviceCapability::Haptics);
        }

        /** @brief Maps only the supported normalized physical role/pose pairs, never a default capability. */
        std::optional<XRTrackedDeviceCapability> SourceCapability(const TrackingSourceEvidence &source) noexcept {
            if (source.role == XRTrackedDeviceRole::Head && source.kind == XRTrackedPoseKind::Head)
                return XRTrackedDeviceCapability::HeadPose;
            if (source.role == XRTrackedDeviceRole::LeftController || source.role == XRTrackedDeviceRole::RightController) {
                if (source.kind == XRTrackedPoseKind::Grip)
                    return XRTrackedDeviceCapability::GripPose;
                if (source.kind == XRTrackedPoseKind::Aim)
                    return XRTrackedDeviceCapability::AimPose;
            }
            return std::nullopt;
        }

        /** @brief Validates a complete normalized packet before any canonical device generation can change.
         * @param sources Bounded same-cutoff physical observations. @param session Exact active owner.
         * @param origin Exact current reference-space origin. @return Original typed failure or complete admission.
         */
        Result<void> ValidateSourceEvidence(const std::span<const TrackingSourceEvidence> sources, const XRSessionId &session,
                                            const XRWorldOriginRevision origin) {
            if (sources.size() > 5)
                return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
            for (std::size_t index = 0; index < sources.size(); ++index) {
                const auto &source = sources[index];
                const auto &pose = source.pose;
                const bool located = pose.components.positionMeters.validity != XRPoseComponentValidity::Invalid ||
                                     pose.components.orientation.validity != XRPoseComponentValidity::Invalid;
                const bool velocity = pose.components.linearVelocityMetersPerSecond.validity != XRPoseComponentValidity::Invalid ||
                                      pose.components.angularVelocityRadiansPerSecond.validity != XRPoseComponentValidity::Invalid;
                if (!SourceCapability(source) || (!source.active && (located || velocity)) ||
                    (source.role == XRTrackedDeviceRole::Head && source.active != located) ||
                    std::ranges::any_of(sources.first(index), [&](const TrackingSourceEvidence &prior) {
                    return prior.role == source.role && prior.kind == source.kind;
                }))
                    return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
                if (pose.purpose != XRPosePurpose::SimulationInput || !pose.time.simulation || pose.time.renderPrediction ||
                    (index != 0 && pose.time != sources.front().pose.time))
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

        /** @brief Collects one role's normalized observation without mutating device generations or committed snapshots. */
        std::optional<DeviceUpdate> CollectDeviceUpdate(std::span<const TrackingSourceEvidence> sources, XRTrackedDeviceRole role,
                                                        const XRTrackedDeviceRecord *prior) {
            auto capabilities = prior == nullptr ? XRTrackedDeviceCapabilities{} : prior->capabilities;
            bool observed = false;
            bool located = false;
            for (const auto &source : sources) {
                if (source.role != role || !source.active)
                    continue;
                capabilities.values[static_cast<std::size_t>(*SourceCapability(source))] = true;
                observed = true;
                located |= source.pose.components.positionMeters.validity != XRPoseComponentValidity::Invalid ||
                           source.pose.components.orientation.validity != XRPoseComponentValidity::Invalid;
            }
            if (!observed && prior == nullptr)
                return std::nullopt;
            auto state = located ? XRTrackedDeviceState::Tracked : XRTrackedDeviceState::TrackingLost;
            if (!observed && prior != nullptr && prior->state == XRTrackedDeviceState::Disconnected)
                state = XRTrackedDeviceState::Disconnected;
            return DeviceUpdate{role, state, capabilities};
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
        if (limits.maximumDevices == 0 || limits.maximumDevices > devices_.size() || limits.maximumPoses == 0 ||
            limits.maximumPoses > XRTrackingSnapshotHardLimits::MaximumPoses ||
            limits.maximumActions > XRTrackingSnapshotHardLimits::MaximumActions)
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
        if (!session_.IsValid() || role > XRTrackedDeviceRole::RightController || state >= XRTrackedDeviceState::Count ||
            !CapabilitiesMatch(role, capabilities))
            return Outcome::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
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
        const bool replaced = (found->state == XRTrackedDeviceState::Disconnected && state != XRTrackedDeviceState::Disconnected) ||
                              found->capabilities != capabilities;
        if (replaced && found->id.slot.generation == std::numeric_limits<std::uint32_t>::max())
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        current_.reset();
        if (replaced)
            ++found->id.slot.generation;
        found->state = state;
        found->capabilities = capabilities;
        return Outcome::Success(found->id);
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

        std::array<DeviceUpdate, 3> updates{};
        std::size_t updateCount = 0;
        std::size_t newCount = 0;
        const auto existing = Devices();
        constexpr std::array roles{XRTrackedDeviceRole::Head, XRTrackedDeviceRole::LeftController, XRTrackedDeviceRole::RightController};
        for (const auto role : roles) {
            const auto prior = std::ranges::find(existing, role, &XRTrackedDeviceRecord::role);
            const auto update = CollectDeviceUpdate(sources, role, prior == existing.end() ? nullptr : &*prior);
            if (!update)
                continue;
            if (prior == existing.end()) {
                ++newCount;
            } else if ((prior->capabilities != update->capabilities ||
                        (prior->state == XRTrackedDeviceState::Disconnected && update->state != XRTrackedDeviceState::Disconnected)) &&
                       prior->id.slot.generation == std::numeric_limits<std::uint32_t>::max()) {
                return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
            }
            updates[updateCount++] = *update;
        }
        if (deviceCount_ > limits_.maximumDevices || newCount > limits_.maximumDevices - deviceCount_)
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        // Every possible typed failure is prevalidated before the first mutation. The SAME owner/issuer performs all updates.
        for (const auto &update : std::span{updates}.first(updateCount)) {
            const auto device = ObserveDevice(update.role, update.state, update.capabilities);
            if (device.HasError())
                return Result<void>::Failure(device.ErrorValue());
        }
        return Result<void>::Success();
    }

    /** @copydoc XRTrackingPublication::Devices */
    std::span<const XRTrackedDeviceRecord> XRTrackingPublication::Devices() const noexcept {
        if (std::this_thread::get_id() != lane_)
            return {};
        return std::span{devices_}.first(deviceCount_);
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
            poses_.push_back({evidence.device, evidence.kind, std::move(sample).Value()});
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
        const auto lifecycle = lifecycle_->Snapshot();
        if ((request.focus == XRTrackingFocusState::Focused) != lifecycle.admitsActions || !request.time.runtimeSample.IsValid() ||
            !request.time.simulation || !request.time.simulation->IsValid() || request.time.renderPrediction || request.origin != origin_ ||
            revision_ == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
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
                                                     .actions = request.actions},
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
        const auto snapshot = lifecycle_->Snapshot();
        if (snapshot.session != session_ || !snapshot.admitsFrames ||
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
