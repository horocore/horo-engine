#include "Horo/XR/XRTrackingSnapshot.h"

#include "Horo/XR/XRErrors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <utility>

namespace Horo::XR {
    namespace {
        /** @brief Creates a failed result from a stable XR descriptor. */
        template <typename Value> [[nodiscard]] Result<Value> Reject(const ErrorCodeDescriptor &descriptor) {
            return Result<Value>::Failure(MakeError(descriptor));
        }

        /** @brief Maps a pose purpose to the capability required to publish it. */
        [[nodiscard]] XRTrackedDeviceCapability RequiredCapability(const XRTrackedPoseKind kind) noexcept {
            using enum XRTrackedDeviceCapability;
            switch (kind) {
                case XRTrackedPoseKind::Head:
                    return HeadPose;
                case XRTrackedPoseKind::Grip:
                    return GripPose;
                case XRTrackedPoseKind::Aim:
                    return AimPose;
                case XRTrackedPoseKind::Palm:
                    return PalmPose;
                case XRTrackedPoseKind::Tracker:
                    return TrackerPose;
                case XRTrackedPoseKind::Count:
                    return Count;
            }
            return Count;
        }

        /** @brief Checks whether a role is permitted to expose a semantic pose purpose. */
        [[nodiscard]] bool RoleSupportsPose(const XRTrackedDeviceRole role, const XRTrackedPoseKind kind) noexcept {
            using enum XRTrackedPoseKind;
            switch (role) {
                case XRTrackedDeviceRole::Head:
                    return kind == Head;
                case XRTrackedDeviceRole::LeftController:
                case XRTrackedDeviceRole::RightController:
                    return kind == Grip || kind == Aim || kind == Palm;
                case XRTrackedDeviceRole::GenericTracker:
                    return kind == Tracker;
                case XRTrackedDeviceRole::Count:
                    return false;
            }
            return false;
        }

        /** @brief Looks up one canonical device without allocating. */
        [[nodiscard]] const XRTrackedDeviceRecord *FindDevice(const std::span<const XRTrackedDeviceRecord> devices,
                                                              const XRDeviceId &id) noexcept {
            const auto found = std::ranges::lower_bound(devices, id, {}, &XRTrackedDeviceRecord::id);
            return found != devices.end() && found->id == id ? std::to_address(found) : nullptr;
        }

        /** @brief Detects replacement of a device slot retained by a caller. */
        [[nodiscard]] bool HasReplacedDeviceSlot(const std::span<const XRTrackedDeviceRecord> devices, const XRDeviceId &id) noexcept {
            return std::ranges::any_of(devices, [&id](const XRTrackedDeviceRecord &record) {
                return record.id.session == id.session && record.id.slot.index == id.slot.index && record.id.slot != id.slot;
            });
        }

        /** @brief Looks up one canonical device-pose key without allocating. */
        [[nodiscard]] const XRTrackedPoseRecord *FindPose(const std::span<const XRTrackedPoseRecord> poses, const XRDeviceId &device,
                                                          const XRTrackedPoseKind kind) noexcept {
            const auto key = std::pair{device, kind};
            const auto found = std::ranges::lower_bound(poses, key, {}, [](const XRTrackedPoseRecord &record) {
                return std::pair{record.device, record.kind};
            });
            return found != poses.end() && found->device == device && found->kind == kind ? std::to_address(found) : nullptr;
        }

        /** @brief Validates fixed bounds before immutable storage allocation. */
        [[nodiscard]] bool ValidLimits(const XRTrackingSnapshotDescriptor &descriptor) noexcept {
            return descriptor.limits.maximumDevices > 0 &&
                   descriptor.limits.maximumDevices <= XRTrackingSnapshotHardLimits::MaximumDevices && descriptor.limits.maximumPoses > 0 &&
                   descriptor.limits.maximumPoses <= XRTrackingSnapshotHardLimits::MaximumPoses &&
                   descriptor.devices.size() <= descriptor.limits.maximumDevices &&
                   descriptor.poses.size() <= descriptor.limits.maximumPoses &&
                   descriptor.limits.maximumActions <= XRTrackingSnapshotHardLimits::MaximumActions &&
                   descriptor.actions.size() <= descriptor.limits.maximumActions;
        }

        /** @brief Checks that advertised device capabilities agree with the semantic role. */
        [[nodiscard]] bool ValidCapabilities(const XRTrackedDeviceRecord &device) noexcept {
            using enum XRTrackedDeviceCapability;
            if (const bool controller =
                    device.role == XRTrackedDeviceRole::LeftController || device.role == XRTrackedDeviceRole::RightController;
                device.capabilities.Has(HeadPose) != (device.role == XRTrackedDeviceRole::Head) ||
                device.capabilities.Has(TrackerPose) != (device.role == XRTrackedDeviceRole::GenericTracker) ||
                (device.capabilities.Has(GripPose) || device.capabilities.Has(AimPose) || device.capabilities.Has(PalmPose)) !=
                    controller ||
                (device.capabilities.Has(Haptics) && !controller))
                return false;
            return device.role == XRTrackedDeviceRole::Head || device.role == XRTrackedDeviceRole::GenericTracker ||
                   device.capabilities.Has(GripPose) || device.capabilities.Has(AimPose) || device.capabilities.Has(PalmPose);
        }

        /** @brief Validates canonical device identities, roles, states, and uniqueness. */
        [[nodiscard]] Result<void> ValidateDevices(const std::span<const XRTrackedDeviceRecord> devices, const XRSessionId &activeSession) {
            std::array<bool, static_cast<std::size_t>(XRTrackedDeviceRole::Count)> assignedRoles{};
            for (std::size_t index = 0; index < devices.size(); ++index) {
                const auto &device = devices[index];
                if (auto identity = ValidateXRSessionObject(device.id, activeSession); identity.HasError())
                    return identity;
                if (device.role >= XRTrackedDeviceRole::Count || device.state >= XRTrackedDeviceState::Count || !ValidCapabilities(device))
                    return Reject<void>(XRErrors::TrackingSnapshotInvalid);
                if (device.role != XRTrackedDeviceRole::GenericTracker) {
                    const auto roleIndex = static_cast<std::size_t>(device.role);
                    if (assignedRoles[roleIndex])
                        return Reject<void>(XRErrors::TrackingSnapshotInvalid);
                    assignedRoles[roleIndex] = true;
                }
                if (index > 0 && !(devices[index - 1].id < device.id))
                    return Reject<void>(XRErrors::TrackingSnapshotInvalid);
            }
            return Result<void>::Success();
        }

        /** @brief Validates canonical pose evidence against the referenced device records. */
        [[nodiscard]] Result<void> ValidatePoses(const XRTrackingSnapshotDescriptor &descriptor, const XRSessionId &activeSession,
                                                 const XRWorldOriginRevision activeOriginRevision) {
            for (std::size_t index = 0; index < descriptor.poses.size(); ++index) {
                const auto &pose = descriptor.poses[index];
                if (pose.kind >= XRTrackedPoseKind::Count)
                    return Reject<void>(XRErrors::TrackingSnapshotInvalid);
                if (auto identity = ValidateXRSessionObject(pose.device, activeSession); identity.HasError())
                    return identity;
                if (index > 0 &&
                    !(std::pair{descriptor.poses[index - 1].device, descriptor.poses[index - 1].kind} < std::pair{pose.device, pose.kind}))
                    return Reject<void>(XRErrors::TrackingSnapshotInvalid);

                if (const auto *device = FindDevice(descriptor.devices, pose.device);
                    device == nullptr || device->state != XRTrackedDeviceState::Tracked || !RoleSupportsPose(device->role, pose.kind) ||
                    !device->capabilities.Has(RequiredCapability(pose.kind)))
                    return Reject<void>(XRErrors::TrackingSnapshotInvalid);
                if (pose.sample.Session() != descriptor.session || pose.sample.Purpose() != XRPosePurpose::SimulationInput ||
                    pose.sample.Time().runtimeSample != descriptor.sampledAt)
                    return Reject<void>(XRErrors::TrackingSnapshotInvalid);
                if (auto current = XRPoseSample::Create({.session = pose.sample.Session(),
                                                         .source = pose.sample.Source(),
                                                         .target = pose.sample.Target(),
                                                         .purpose = pose.sample.Purpose(),
                                                         .time = pose.sample.Time(),
                                                         .components = pose.sample.Components()},
                                                        activeSession, activeOriginRevision);
                    current.HasError())
                    return Result<void>::Failure(current.ErrorValue());
            }
            return Result<void>::Success();
        }

        /** @brief Validates action provenance and normalized payloads independently of pose validity. */
        [[nodiscard]] Result<void> ValidateActions(const XRTrackingSnapshotDescriptor &descriptor) {
            if ((descriptor.limits.maximumActions == 0) != (descriptor.bindingRevision == 0))
                return Reject<void>(XRErrors::TrackingSnapshotInvalid);
            for (std::size_t index = 0; index < descriptor.actions.size(); ++index) {
                const auto &action = descriptor.actions[index];
                const auto *device = FindDevice(descriptor.devices, action.device);
                if (!device || action.kind >= XRTrackedActionKind::Count ||
                    action.bindingIndex >= XRTrackingSnapshotHardLimits::MaximumActions || !std::isfinite(action.x) ||
                    !std::isfinite(action.y) || std::abs(action.x) > 1.0F || std::abs(action.y) > 1.0F ||
                    (action.kind != XRTrackedActionKind::Vector2 && action.y != 0.0F) ||
                    (action.kind == XRTrackedActionKind::Boolean && action.x != 0.0F && action.x != 1.0F) ||
                    (!action.active && (action.x != 0.0F || action.y != 0.0F)))
                    return Reject<void>(XRErrors::TrackingSnapshotInvalid);
                if (action.active && (descriptor.focus != XRTrackingFocusState::Focused ||
                                      (descriptor.state != XRTrackingState::Active && descriptor.state != XRTrackingState::TrackingLost) ||
                                      device->state == XRTrackedDeviceState::Disconnected))
                    return Reject<void>(XRErrors::TrackingSnapshotInvalid);
                if (index > 0 && !(std::pair{descriptor.actions[index - 1].bindingIndex, descriptor.actions[index - 1].device} <
                                   std::pair{action.bindingIndex, action.device}))
                    return Reject<void>(XRErrors::TrackingSnapshotInvalid);
            }
            return Result<void>::Success();
        }

        /** @brief Validates loss-state rules that prevent stale pose fabrication. */
        [[nodiscard]] bool ValidAggregateState(const XRTrackingSnapshotDescriptor &descriptor) noexcept {
            using enum XRTrackingState;
            if (descriptor.state == Active)
                return true;
            if (!descriptor.poses.empty())
                return false;
            if (descriptor.state == TrackingLost) {
                return std::ranges::none_of(descriptor.devices, [](const XRTrackedDeviceRecord &device) {
                    return device.state == XRTrackedDeviceState::Tracked;
                });
            }
            return descriptor.devices.empty();
        }
    }  // namespace

    /** @copydoc XRTrackingSnapshot::Create */
    Result<XRTrackingSnapshot> XRTrackingSnapshot::Create(const XRTrackingSnapshotDescriptor &descriptor, const XRSessionId &activeSession,
                                                          const XRWorldOriginRevision activeOriginRevision) {
        if (auto version = RequireXRContractVersion(CurrentXRContractVersion, descriptor.contractVersion); version.HasError())
            return Result<XRTrackingSnapshot>::Failure(version.ErrorValue());
        if (auto session = ValidateXRSession(descriptor.session, activeSession); session.HasError())
            return Result<XRTrackingSnapshot>::Failure(session.ErrorValue());
        if (!descriptor.revision.IsValid() || descriptor.sequence == 0 || !descriptor.sampledAt.IsValid() ||
            !descriptor.worldOriginRevision.IsValid() || !activeOriginRevision.IsValid() ||
            descriptor.focus >= XRTrackingFocusState::Count || descriptor.visibility >= XRTrackingVisibilityState::Count ||
            descriptor.state >= XRTrackingState::Count)
            return Reject<XRTrackingSnapshot>(XRErrors::TrackingSnapshotInvalid);
        if (descriptor.worldOriginRevision != activeOriginRevision)
            return Reject<XRTrackingSnapshot>(XRErrors::OriginRevisionStale);
        if (!ValidLimits(descriptor))
            return Reject<XRTrackingSnapshot>(XRErrors::CapacityExceeded);
        if (!ValidAggregateState(descriptor))
            return Reject<XRTrackingSnapshot>(XRErrors::TrackingSnapshotInvalid);
        if (auto devices = ValidateDevices(descriptor.devices, activeSession); devices.HasError())
            return Result<XRTrackingSnapshot>::Failure(devices.ErrorValue());
        if (auto poses = ValidatePoses(descriptor, activeSession, activeOriginRevision); poses.HasError())
            return Result<XRTrackingSnapshot>::Failure(poses.ErrorValue());
        if (auto actions = ValidateActions(descriptor); actions.HasError())
            return Result<XRTrackingSnapshot>::Failure(actions.ErrorValue());
        return Result<XRTrackingSnapshot>::Success(XRTrackingSnapshot{descriptor});
    }

    std::array<XRTrackedPoseRecord, XRTrackingSnapshotHardLimits::MaximumPoses> XRTrackingSnapshot::EmptyPoses() noexcept {
        return []<std::size_t... Indices>(std::index_sequence<Indices...>) {
            return std::array<XRTrackedPoseRecord, sizeof...(Indices)>{
                {(static_cast<void>(Indices), XRTrackedPoseRecord{{}, XRTrackedPoseKind::Tracker, XRPoseSample{XRPoseDescriptor{}}})...}};
        }(std::make_index_sequence<XRTrackingSnapshotHardLimits::MaximumPoses>{});
    }

    XRTrackingSnapshot::XRTrackingSnapshot(const XRTrackingSnapshotDescriptor &descriptor)
        : contractVersion_(descriptor.contractVersion), session_(descriptor.session), revision_(descriptor.revision),
          sequence_(descriptor.sequence), sampledAt_(descriptor.sampledAt), worldOriginRevision_(descriptor.worldOriginRevision),
          focus_(descriptor.focus), visibility_(descriptor.visibility), state_(descriptor.state), limits_(descriptor.limits),
          deviceCount_(descriptor.devices.size()), poseCount_(descriptor.poses.size()), actionCount_(descriptor.actions.size()),
          bindingRevision_(descriptor.bindingRevision) {
        std::ranges::copy(descriptor.devices, devices_.begin());
        std::ranges::copy(descriptor.poses, poses_.begin());
        std::ranges::copy(descriptor.actions, actions_.begin());
    }

    /** @copydoc XRTrackingSnapshot::ContractVersion */
    XRContractVersion XRTrackingSnapshot::ContractVersion() const noexcept {
        return contractVersion_;
    }

    /** @copydoc XRTrackingSnapshot::Session */
    const XRSessionId &XRTrackingSnapshot::Session() const noexcept {
        return session_;
    }

    /** @copydoc XRTrackingSnapshot::Revision */
    XRTrackingSnapshotRevision XRTrackingSnapshot::Revision() const noexcept {
        return revision_;
    }

    /** @copydoc XRTrackingSnapshot::Sequence */
    std::uint64_t XRTrackingSnapshot::Sequence() const noexcept {
        return sequence_;
    }

    /** @copydoc XRTrackingSnapshot::SampledAt */
    XRRuntimeTime XRTrackingSnapshot::SampledAt() const noexcept {
        return sampledAt_;
    }

    /** @copydoc XRTrackingSnapshot::WorldOriginRevision */
    XRWorldOriginRevision XRTrackingSnapshot::WorldOriginRevision() const noexcept {
        return worldOriginRevision_;
    }

    /** @copydoc XRTrackingSnapshot::Focus */
    XRTrackingFocusState XRTrackingSnapshot::Focus() const noexcept {
        return focus_;
    }

    /** @copydoc XRTrackingSnapshot::Visibility */
    XRTrackingVisibilityState XRTrackingSnapshot::Visibility() const noexcept {
        return visibility_;
    }

    /** @copydoc XRTrackingSnapshot::State */
    XRTrackingState XRTrackingSnapshot::State() const noexcept {
        return state_;
    }

    /** @copydoc XRTrackingSnapshot::Limits */
    const XRTrackingSnapshotLimits &XRTrackingSnapshot::Limits() const noexcept {
        return limits_;
    }

    /** @copydoc XRTrackingSnapshot::Devices */
    std::span<const XRTrackedDeviceRecord> XRTrackingSnapshot::Devices() const noexcept {
        return std::span{devices_}.first(deviceCount_);
    }

    /** @copydoc XRTrackingSnapshot::Poses */
    std::span<const XRTrackedPoseRecord> XRTrackingSnapshot::Poses() const noexcept {
        return std::span{poses_}.first(poseCount_);
    }

    /** @copydoc XRTrackingSnapshot::BindingRevision */
    std::uint64_t XRTrackingSnapshot::BindingRevision() const noexcept {
        return bindingRevision_;
    }

    /** @copydoc XRTrackingSnapshot::Actions */
    std::span<const XRTrackedActionRecord> XRTrackingSnapshot::Actions() const noexcept {
        return std::span{actions_}.first(actionCount_);
    }

    /** @copydoc ValidateXRTrackingSnapshot */
    Result<void> ValidateXRTrackingSnapshot(const XRTrackingSnapshot &snapshot, const XRSessionId &activeSession,
                                            const XRTrackingSnapshotRevision expectedRevision,
                                            const XRWorldOriginRevision activeOriginRevision) {
        if (auto session = ValidateXRSession(snapshot.Session(), activeSession); session.HasError())
            return session;
        if (!expectedRevision.IsValid() || !activeOriginRevision.IsValid())
            return Reject<void>(XRErrors::TrackingSnapshotInvalid);
        if (snapshot.Revision() != expectedRevision)
            return Reject<void>(XRErrors::TrackingSnapshotStale);
        if (snapshot.WorldOriginRevision() != activeOriginRevision)
            return Reject<void>(XRErrors::OriginRevisionStale);
        return Result<void>::Success();
    }

    /** @copydoc QueryXRTrackedPose */
    Result<XRPoseSample> QueryXRTrackedPose(const XRTrackingSnapshot &snapshot, const XRDeviceId &device, const XRTrackedPoseKind kind,
                                            const XRSessionId &activeSession, const XRTrackingSnapshotRevision expectedRevision,
                                            const XRWorldOriginRevision activeOriginRevision) {
        if (auto current = ValidateXRTrackingSnapshot(snapshot, activeSession, expectedRevision, activeOriginRevision); current.HasError())
            return Result<XRPoseSample>::Failure(current.ErrorValue());
        if (auto identity = ValidateXRSessionObject(device, activeSession); identity.HasError())
            return Result<XRPoseSample>::Failure(identity.ErrorValue());
        if (kind >= XRTrackedPoseKind::Count)
            return Reject<XRPoseSample>(XRErrors::OperationInvalid);

        const auto devices = snapshot.Devices();
        const auto *record = FindDevice(devices, device);
        if (record == nullptr) {
            return Reject<XRPoseSample>(HasReplacedDeviceSlot(devices, device) ? XRErrors::IdentityStale : XRErrors::OperationUnavailable);
        }
        if (!record->capabilities.Has(RequiredCapability(kind)) || !RoleSupportsPose(record->role, kind))
            return Reject<XRPoseSample>(XRErrors::OperationUnsupported);
        if (snapshot.State() != XRTrackingState::Active || record->state != XRTrackedDeviceState::Tracked)
            return Reject<XRPoseSample>(XRErrors::OperationUnavailable);
        if (const auto *pose = FindPose(snapshot.Poses(), device, kind); pose != nullptr)
            return Result<XRPoseSample>::Success(pose->sample);
        return Reject<XRPoseSample>(XRErrors::OperationUnavailable);
    }
}  // namespace Horo::XR
