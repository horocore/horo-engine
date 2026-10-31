#include "Horo/XR/XRInputProjection.h"

#include "Horo/XR/XRErrors.h"

#include <algorithm>
#include <bitset>

namespace Horo::XR {
    namespace {
        [[nodiscard]] XRTrackedActionKind ActionKind(const Input::ActionValueType type) noexcept {
            using enum Horo::XR::XRTrackedActionKind;
            switch (type) {
                using enum Horo::Input::ActionValueType;
                case Digital:
                    return Boolean;
                case Axis1D:
                    return Scalar;
                case Axis2D:
                    return Vector2;
            }
            return Count;
        }
    }  // namespace

    /** @copydoc XRInputProjection::Create */
    Result<XRInputProjection> XRInputProjection::Create(const Input::InputActionCollection &port, const XRSessionId &session,
                                                        const XRResolvedActionBindings &plan, const std::uint64_t bindingRevision) {
        if (!session.IsValid() || bindingRevision == 0)
            return Result<XRInputProjection>::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
        auto prepared = Prepare(port, plan);
        if (prepared.HasError())
            return prepared;
        auto projection = std::move(prepared).Value();
        if (auto bound = projection.Bind(session, bindingRevision); bound.HasError())
            return Result<XRInputProjection>::Failure(bound.ErrorValue());
        return Result<XRInputProjection>::Success(std::move(projection));
    }

    /** @copydoc XRInputProjection::Prepare */
    Result<XRInputProjection> XRInputProjection::Prepare(const Input::InputActionCollection &port, const XRResolvedActionBindings &plan) {
        if (plan.schemaVersion == 0 || plan.bindings.empty())
            return Result<XRInputProjection>::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
        if (plan.bindings.size() > XRActionBindingLimits::MaximumActions)
            return Result<XRInputProjection>::Failure(MakeError(XRErrors::CapacityExceeded));
        std::vector<Input::ActionDescriptor> schemas;
        schemas.reserve(plan.bindings.size());
        for (const auto &binding : plan.bindings) {
            if (binding.role >= XRTrackedDeviceRole::Count || ActionKind(binding.valueType) == XRTrackedActionKind::Count)
                return Result<XRInputProjection>::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
            schemas.push_back({.id = binding.action, .valueType = binding.valueType, .context = binding.context});
        }
        auto registered = port.RegisterSource(schemas);
        if (registered.HasError())
            return Result<XRInputProjection>::Failure(registered.ErrorValue());
        XRInputProjection result{std::move(registered).Value()};
        result.count_ = plan.bindings.size();
        for (std::size_t index = 0; index < result.count_; ++index)
            result.bindings_[index] = {plan.bindings[index].role, ActionKind(plan.bindings[index].valueType)};
        return Result<XRInputProjection>::Success(std::move(result));
    }

    /** @copydoc XRInputProjection::Bind */
    Result<void> XRInputProjection::Bind(const XRSessionId &session, const std::uint64_t bindingRevision) {
        if (!session.IsValid() || bindingRevision == 0)
            return Result<void>::Failure(MakeError(XRErrors::IdentityInvalid));
        if (session_.IsValid())
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (auto current = source_.Validate(); current.HasError())
            return current;
        session_ = session;
        bindingRevision_ = bindingRevision;
        return Result<void>::Success();
    }

    /** @copydoc XRInputProjection::Append */
    Result<void> XRInputProjection::Append(const XRTrackingSnapshot &snapshot, const XRSessionId &activeSession,
                                           const XRTrackingSnapshotRevision expectedSnapshotRevision,
                                           const XRWorldOriginRevision activeOriginRevision, const std::uint64_t currentBindingRevision,
                                           const Input::InputCollectionFrame &frame) const {
        if (auto valid = ValidateXRTrackingSnapshot(snapshot, activeSession, expectedSnapshotRevision, activeOriginRevision);
            valid.HasError())
            return valid;
        if (activeSession != session_ || currentBindingRevision != bindingRevision_ || snapshot.BindingRevision() != bindingRevision_)
            return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotStale));
        std::array<Input::CollectedActionValue, XRActionBindingLimits::MaximumActions> values{};
        std::bitset<XRActionBindingLimits::MaximumActions> selected;
        std::size_t count = 0;
        for (const auto &sample : snapshot.Actions()) {
            if (sample.bindingIndex >= count_ || selected.test(sample.bindingIndex))
                return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
            const auto device = std::ranges::find(snapshot.Devices(), sample.device, &XRTrackedDeviceRecord::id);
            if (const auto &binding = bindings_[sample.bindingIndex];
                device == snapshot.Devices().end() || binding.role != device->role || binding.kind != sample.kind)
                return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
            selected.set(sample.bindingIndex);
            values[count++] = {static_cast<std::uint16_t>(sample.bindingIndex), sample.x, sample.y, sample.active};
        }
        return frame.Append(source_, std::span{values}.first(count));
    }

    /** @copydoc XRInputProjection::Reset */
    Result<void> XRInputProjection::Reset() {
        session_ = {};
        bindingRevision_ = 0;
        return source_.Reset();
    }
}  // namespace Horo::XR
