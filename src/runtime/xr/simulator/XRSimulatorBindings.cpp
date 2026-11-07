#include "Horo/Foundation/Sha256.h"
#include "XRSimulatorState.h"

#include <algorithm>

namespace Horo::XR {
    namespace {
        void Number(Sha256Builder &builder, const std::uint64_t value) noexcept {
            std::array<std::byte, 8> bytes{};
            for (std::size_t index = 0; index < bytes.size(); ++index)
                bytes[index] = static_cast<std::byte>((value >> (index * 8)) & 0xffU);
            static_cast<void>(builder.Update(bytes));
        }

        bool Text(Sha256Builder &builder, const std::string_view value) noexcept {
            if (value.empty() || value.size() > XRActionBindingLimits::MaximumNameBytes)
                return false;
            Number(builder, value.size());
            return builder.Update(std::as_bytes(std::span{value.data(), value.size()}));
        }

        bool BindingIdentity(Sha256Builder &builder, const XRResolvedActionBinding &binding) noexcept {
            Number(builder, static_cast<std::uint64_t>(binding.valueType));
            Number(builder, static_cast<std::uint64_t>(binding.role));
            Number(builder, binding.profile.value);
            Number(builder, binding.control.value);
            return Text(builder, binding.action.Value()) && Text(builder, binding.context.Value());
        }

        std::optional<XRTrackedActionKind> Kind(const Input::ActionValueType type) noexcept {
            switch (type) {
                case Input::ActionValueType::Digital:
                    return XRTrackedActionKind::Boolean;
                case Input::ActionValueType::Axis1D:
                    return XRTrackedActionKind::Scalar;
                case Input::ActionValueType::Axis2D:
                    return XRTrackedActionKind::Vector2;
            }
            return {};
        }

        bool CurrentCoordinator(const XRActionBindingCoordinator *coordinator, const XRSessionId &session) noexcept {
            return coordinator && coordinator->Session() == session && coordinator->Current() && coordinator->Revision() != 0;
        }

        bool Nonzero(const XRSimulatorBindingFingerprint &fingerprint) noexcept {
            return std::ranges::any_of(fingerprint, [](const auto value) {
                return value != std::byte{};
            });
        }
    }  // namespace

    /** @copydoc RuntimeInternal::XRSimulatorHostAccess::BindingFingerprint */
    Result<XRSimulatorBindingFingerprint> RuntimeInternal::XRSimulatorHostAccess::BindingFingerprint(const XRResolvedActionBindings &plan) {
        if (plan.schemaVersion == 0 || plan.bindings.size() > XRFakeLimits::MaximumActions)
            return Result<XRSimulatorBindingFingerprint>::Failure(MakeError(XRErrors::OperationInvalid));
        Sha256Builder builder;
        static_cast<void>(Text(builder, "Horo.XRSimulator.Bindings.v1"));
        Number(builder, 1);
        Number(builder, plan.schemaVersion);
        Number(builder, plan.bindings.size());
        for (const auto &binding : plan.bindings)
            if (!BindingIdentity(builder, binding))
                return Result<XRSimulatorBindingFingerprint>::Failure(MakeError(XRErrors::OperationInvalid));
        const auto digest = builder.Finalize();
        XRSimulatorBindingFingerprint result;
        std::ranges::transform(digest.bytes, result.begin(), [](const auto byte) {
            return static_cast<std::byte>(byte);
        });
        return Result<XRSimulatorBindingFingerprint>::Success(result);
    }

    Result<void> XRSimulatorRuntime::Impl::CopyBindings(const XRResolvedActionBindings &plan) {
        for (std::size_t index = 0; index < plan.bindings.size(); ++index) {
            const auto kind = Kind(plan.bindings[index].valueType);
            if (!kind)
                return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported));
            candidateBindings.values[index] = {plan.bindings[index].role, *kind};
        }
        return Result<void>::Success();
    }

    Result<void> XRSimulatorRuntime::Impl::PrepareBindings(const XRSessionId &candidate) {
        candidateBindings = {};
        candidateBindings.session = candidate;
        if (!query)
            return Nonzero(expectedFingerprint) ? Result<void>::Failure(MakeError(XRErrors::OperationUnsupported))
                                                : Result<void>::Success();
        const auto before = host.Snapshot().session;
        const auto *coordinator = query();
        if (host.Snapshot().session != before || shutdownRequested)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        if (!CurrentCoordinator(coordinator, candidate))
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        const auto &plan = *coordinator->Current();
        const auto fingerprint = RuntimeInternal::XRSimulatorHostAccess::BindingFingerprint(plan);
        if (fingerprint.HasError())
            return Result<void>::Failure(fingerprint.ErrorValue());
        if (Nonzero(expectedFingerprint) && fingerprint.Value() != expectedFingerprint)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (auto copied = CopyBindings(plan); copied.HasError())
            return copied;
        candidateBindings.owner = coordinator;
        candidateBindings.revision = coordinator->Revision();
        candidateBindings.count = plan.bindings.size();
        candidateBindings.fingerprint = fingerprint.Value();
        return Result<void>::Success();
    }

    Result<void> XRSimulatorRuntime::Impl::Prepare(const XRSessionPreparation stage, const XRSessionId &candidate,
                                                   const XRFeaturePlan &plan) {
        if (children)
            if (auto prepared = children->Prepare(stage, candidate, plan); prepared.HasError())
                return prepared;
        if (stage != XRSessionPreparation::InputAndUi)
            return Result<void>::Success();
        try {
            auto bindings = PrepareBindings(candidate);
            if (bindings.HasError() && children)
                children->Release(stage, candidate);
            return bindings;
        } catch (...) {
            if (children)
                children->Release(stage, candidate);
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        }
    }

    void XRSimulatorRuntime::Impl::Release(const XRSessionPreparation stage, const XRSessionId &session) noexcept {
        if (stage == XRSessionPreparation::InputAndUi) {
            if (candidateBindings.session == session)
                candidateBindings = {};
            if (activeBindings.session == session)
                activeBindings = {};
        }
        if (children)
            children->Release(stage, session);
    }

    Result<void> XRSimulatorRuntime::Impl::CheckBindings() const {
        if (!query)
            return Result<void>::Success();
        const auto before = host.Snapshot().session;
        const XRActionBindingCoordinator *coordinator{};
        try {
            coordinator = query();
        } catch (...) {
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        }
        if (shutdownRequested || host.Snapshot().session != before || before != activeBindings.session ||
            coordinator != activeBindings.owner)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        if (!CurrentCoordinator(coordinator, before) || coordinator->Revision() != activeBindings.revision)
            return Result<void>::Failure(MakeError(XRErrors::CapabilityStale));
        return Result<void>::Success();
    }

    Result<void> XRSimulatorRuntime::Impl::ValidateControlBindings(const XRSimulatorStep &step) const {
        for (const auto &action : std::span{step.actions}.first(step.actionCount)) {
            if (action.bindingIndex >= activeBindings.count)
                return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported));
            const auto &binding = activeBindings.values[action.bindingIndex];
            if (action.role != binding.role || action.kind != binding.kind)
                return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        }
        return Result<void>::Success();
    }
}  // namespace Horo::XR
