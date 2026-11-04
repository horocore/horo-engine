#pragma once
#include "Horo/XR/XRTrackingSnapshot.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::XR::SimulatorTests {
    template <class Type> Type Value(const std::uint64_t value) {
        const auto result = Type::Create(value);
        REQUIRE(result.HasValue());
        return result.Value();
    }

    inline XRWorldOriginRevision Origin() {
        return Value<XRWorldOriginRevision>(1);
    }

    inline XRPoseDescriptor Pose(const XRSessionId &session, const std::int64_t time = 100, const bool tracked = true) {
        XRPoseDescriptor descriptor{.session = session,
                                    .source = {{session, {2, 1}}, XRSpaceKind::Action, Origin()},
                                    .target = {{session, {3, 1}}, XRSpaceKind::Local, Origin()},
                                    .purpose = XRPosePurpose::SimulationInput,
                                    .time = {Value<XRRuntimeTime>(time), Value<XRSimulationTime>(time), {}}};
        if (tracked) {
            descriptor.components.positionMeters = {Math::Vec3{1, 2, 3}, XRPoseComponentValidity::Tracked};
            descriptor.components.orientation = {Math::Quaternion::Identity(), XRPoseComponentValidity::Tracked};
            descriptor.components.confidence = XRTrackingConfidence::High;
            descriptor.components.loss = XRTrackingLossState::None;
        }
        return descriptor;
    }

    inline XRHandJointRecord Joint(const XRSessionId &session, const XRDeviceId &device, const XRHandedness hand = XRHandedness::Left,
                                   const XRHandJoint joint = XRHandJoint::Palm, const std::int64_t time = 100) {
        const auto sample = XRPoseSample::Create(Pose(session, time), session, Origin());
        REQUIRE(sample.HasValue());
        return {device, hand, joint, sample.Value(), 0.01F};
    }

    template <class T> bool ErrorIs(const Result<T> &result, const ErrorCodeDescriptor &error) {
        return result.HasError() && result.ErrorValue().domain.Value() == error.domain.Value() &&
               result.ErrorValue().code.Value() == error.code.Value();
    }
}  // namespace Horo::XR::SimulatorTests
