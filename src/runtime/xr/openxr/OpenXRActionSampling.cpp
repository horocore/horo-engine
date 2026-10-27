#include "host/OpenXRActionBindings.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>

namespace Horo::XR::OpenXRInternal {
    namespace {
        /** @brief Preserves exact native action-query failure evidence at the backend boundary. */
        Error SamplingFailure(const char *phase, const XrResult result) {
            const auto &descriptor = result == XR_ERROR_FUNCTION_UNSUPPORTED || result == XR_ERROR_PATH_UNSUPPORTED
                                         ? XRErrors::OperationUnsupported
                                         : XRErrors::OperationUnavailable;
            return MakeError(descriptor, std::format("OpenXR {} failed; native result={}", phase, static_cast<std::int32_t>(result)));
        }

        /** @brief Clears the operation fence on success, native error and foreign callback exception. */
        struct SamplingGuard final {
            SamplingGuard(const SamplingGuard &) = delete;
            SamplingGuard &operator=(const SamplingGuard &) = delete;

            bool &active;

            explicit SamplingGuard(bool &value) noexcept : active(value) {
                value = true;
            }

            ~SamplingGuard() {
                active = false;
            }
        };

        /** @brief Calls one official scalar query without interpreting undefined inactive payload fields. */
        template <typename State, typename Function>
        Result<State> ReadState(Function function, const XrSession session, const XrActionStateGetInfo &info, const XrStructureType type) {
            State state{type};
            if (const auto result = function(session, &info, &state); result != XR_SUCCESS)
                return Result<State>::Failure(SamplingFailure("sample action", result));
            if (state.type != type || state.next != nullptr || (state.isActive != XR_TRUE && state.isActive != XR_FALSE))
                return Result<State>::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
            return Result<State>::Success(state);
        }

        [[nodiscard]] bool Normalized(const XRTrackedActionRecord &record) noexcept {
            return std::isfinite(record.x) && std::isfinite(record.y) && std::abs(record.x) <= 1.0F && std::abs(record.y) <= 1.0F;
        }
    }  // namespace

    /** @copydoc OpenXRActionBindings::PrepareScalarPlan */
    Result<OpenXRActionBindings::ScalarPlan> OpenXRActionBindings::PrepareScalarPlan(
        const XRResolvedActionBindings &plan, const std::span<const XRTrackedDeviceRecord> devices) const {
        if (plan.schemaVersion != schemaVersion_ || plan.bindings.size() > XRActionBindingLimits::MaximumActions ||
            devices.size() > XRTrackingSnapshotHardLimits::MaximumDevices)
            return Result<ScalarPlan>::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
        for (std::size_t index = 0; index < devices.size(); ++index) {
            if (auto identity = ValidateXRSessionObject(devices[index].id, owner_); identity.HasError())
                return Result<ScalarPlan>::Failure(identity.ErrorValue());
            if (devices[index].role >= XRTrackedDeviceRole::Count || devices[index].state >= XRTrackedDeviceState::Count ||
                (devices[index].role != XRTrackedDeviceRole::GenericTracker &&
                 std::ranges::find(devices.first(index), devices[index].role, &XRTrackedDeviceRecord::role) != devices.first(index).end()))
                return Result<ScalarPlan>::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
        }
        ScalarPlan result;
        for (std::size_t index = 0; index < plan.bindings.size(); ++index) {
            const auto &binding = plan.bindings[index];
            const auto action = std::ranges::find(actions_, binding.action, &Action::id);
            if (action == actions_.end() || action->type != binding.valueType || action->role != binding.role ||
                binding.profile.value == 0 || binding.control.value == 0 ||
                (index > 0 && !(plan.bindings[index - 1].action.Value() < binding.action.Value())))
                return Result<ScalarPlan>::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
            const auto device = std::ranges::find(devices, binding.role, &XRTrackedDeviceRecord::role);
            if (device == devices.end() || device->state == XRTrackedDeviceState::Disconnected)
                continue;
            result.entries[result.count++] = {std::to_address(action), *device, static_cast<std::uint32_t>(index), binding.profile,
                                              binding.fallback};
        }
        return Result<ScalarPlan>::Success(std::move(result));
    }

    /** @copydoc OpenXRActionBindings::ReadBoolean */
    Result<XRTrackedActionRecord> OpenXRActionBindings::ReadBoolean(const XrActionStateGetInfo &info, XRTrackedActionRecord record) const {
        auto state = ReadState<XrActionStateBoolean>(dispatch_.booleanState, native_.session, info, XR_TYPE_ACTION_STATE_BOOLEAN);
        if (state.HasError())
            return Result<XRTrackedActionRecord>::Failure(state.ErrorValue());
        record.kind = XRTrackedActionKind::Boolean;
        record.active = state.Value().isActive == XR_TRUE;
        if (record.active) {
            if (state.Value().currentState != XR_TRUE && state.Value().currentState != XR_FALSE)
                return Result<XRTrackedActionRecord>::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
            record.x = state.Value().currentState == XR_TRUE ? 1.0F : 0.0F;
        }
        return Result<XRTrackedActionRecord>::Success(record);
    }

    /** @copydoc OpenXRActionBindings::ReadFloat */
    Result<XRTrackedActionRecord> OpenXRActionBindings::ReadFloat(const XrActionStateGetInfo &info, XRTrackedActionRecord record) const {
        auto state = ReadState<XrActionStateFloat>(dispatch_.floatState, native_.session, info, XR_TYPE_ACTION_STATE_FLOAT);
        if (state.HasError())
            return Result<XRTrackedActionRecord>::Failure(state.ErrorValue());
        record.kind = XRTrackedActionKind::Scalar;
        record.active = state.Value().isActive == XR_TRUE;
        if (record.active)
            record.x = state.Value().currentState;
        return Normalized(record) ? Result<XRTrackedActionRecord>::Success(record)
                                  : Result<XRTrackedActionRecord>::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
    }

    /** @copydoc OpenXRActionBindings::ReadVector */
    Result<XRTrackedActionRecord> OpenXRActionBindings::ReadVector(const XrActionStateGetInfo &info, XRTrackedActionRecord record) const {
        auto state = ReadState<XrActionStateVector2f>(dispatch_.vectorState, native_.session, info, XR_TYPE_ACTION_STATE_VECTOR2F);
        if (state.HasError())
            return Result<XRTrackedActionRecord>::Failure(state.ErrorValue());
        record.kind = XRTrackedActionKind::Vector2;
        record.active = state.Value().isActive == XR_TRUE;
        if (record.active) {
            record.x = state.Value().currentState.x;
            record.y = state.Value().currentState.y;
        }
        return Normalized(record) ? Result<XRTrackedActionRecord>::Success(record)
                                  : Result<XRTrackedActionRecord>::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
    }

    /** @copydoc OpenXRActionBindings::ReadScalar */
    Result<XRTrackedActionRecord> OpenXRActionBindings::ReadScalar(const Action &action, const XRTrackedDeviceRecord &device,
                                                                   const std::uint32_t bindingIndex) const {
        const auto role = action.role == XRTrackedDeviceRole::LeftController ? 0U : 1U;
        const XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, action.native, hands_[role]};
        const XRTrackedActionRecord record{.device = device.id, .bindingIndex = bindingIndex};
        switch (action.type) {
            using enum Horo::Input::ActionValueType;
            case Digital:
                return ReadBoolean(info, record);
            case Axis1D:
                return ReadFloat(info, record);
            case Axis2D:
                return ReadVector(info, record);
        }
        return Result<XRTrackedActionRecord>::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
    }

    /** @copydoc OpenXRActionBindings::Sample */
    Result<NativeScalarActionSamples> OpenXRActionBindings::Sample(const XRSessionId &session, const XRResolvedActionBindings &plan,
                                                                   const std::span<const XRTrackedDeviceRecord> devices) const {
        if (auto lane = ControlLane(); lane.HasError())
            return Result<NativeScalarActionSamples>::Failure(lane.ErrorValue());
        if (!active_ || sampling_ || syncSequence_ == 0 || sampledSequence_ == syncSequence_)
            return Result<NativeScalarActionSamples>::Failure(MakeError(XRErrors::OperationUnavailable));
        // One failed sampling opportunity cannot be retried later against the same synchronization.
        sampledSequence_ = syncSequence_;
        // Snapshot bounded plan/device metadata before any foreign query can mutate caller-owned carriers.
        auto prepared = PrepareScalarPlan(plan, devices);
        if (prepared.HasError())
            return Result<NativeScalarActionSamples>::Failure(prepared.ErrorValue());
        SamplingGuard guard{sampling_};
        if (const auto current = Current(session); current.HasError())
            return Result<NativeScalarActionSamples>::Failure(current.ErrorValue());
        const auto profiles = Profiles(session);
        if (profiles.HasError())
            return Result<NativeScalarActionSamples>::Failure(profiles.ErrorValue());
        NativeScalarActionSamples result;
        for (const auto &candidate : std::span{prepared.Value().entries}.first(prepared.Value().count)) {
            const auto role = candidate.action->role == XRTrackedDeviceRole::LeftController ? 0U : 1U;
            if (const auto profile = profiles.Value()[role].profile;
                candidate.profile != profile && !(profile.value == 0 && candidate.fallback && candidate.profile == fallbackProfile_))
                return Result<NativeScalarActionSamples>::Failure(MakeError(XRErrors::TrackingSnapshotStale));
            auto sample = ReadScalar(*candidate.action, candidate.device, candidate.bindingIndex);
            if (sample.HasError())
                return Result<NativeScalarActionSamples>::Failure(sample.ErrorValue());
            result.records_[result.count_++] = sample.Value();
        }
        if (auto valid = session_->Validate(session); valid.HasError())
            return Result<NativeScalarActionSamples>::Failure(valid.ErrorValue());
        sampledSequence_ = syncSequence_;
        return Result<NativeScalarActionSamples>::Success(result);
    }
}  // namespace Horo::XR::OpenXRInternal
