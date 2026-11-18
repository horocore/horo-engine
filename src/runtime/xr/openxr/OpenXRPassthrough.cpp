#include "host/OpenXRNativeSession.h"

#include <bit>

namespace Horo::XR::OpenXRInternal {
    namespace {
        constexpr std::string_view Extension = XR_FB_PASSTHROUGH_EXTENSION_NAME;
        constexpr std::uint32_t PropertiesVersion = 3;

        /** @brief Fences a synchronous native borrow against reentrant retirement. */
        struct PassthroughOperation final {
            explicit PassthroughOperation(bool &active) noexcept : active_(active), previous_(active) {
                active_ = true;
            }

            ~PassthroughOperation() {
                active_ = previous_;
            }

            bool &active_;
            bool previous_;
        };

        /** @brief Resolves the official extension dispatch through the exact instance. */
        template <typename Function>
        bool Resolve(PFN_xrGetInstanceProcAddr getProc, XrInstance instance, const char *name, Function &function) {
            PFN_xrVoidFunction raw{};
            if (getProc(instance, name, &raw) != XR_SUCCESS || !raw)
                return false;
            function = std::bit_cast<Function>(raw);
            return true;
        }

        /** @brief The consent is for visual composition in one live World and one exact session. */
        bool ValidScope(const XRPassthroughScope &scope) noexcept {
            return scope.session.IsValid() && scope.grant.IsValid() && scope.world.id.IsValid() &&
                   scope.world.id.session == scope.session && scope.world.kind == XRSpaceKind::World &&
                   scope.world.worldOriginRevision.IsValid() &&
                   scope.world.convention == XRCoordinateConvention::RightHandedYUpNegativeZForward;
        }

        /** @brief Keeps platform denial, unsupported capability and native resource failure distinct. */
        Error PassthroughFailure(const char *operation, XrResult result) {
            switch (result) {
                case XR_ERROR_PERMISSION_INSUFFICIENT:
                case XR_ERROR_NOT_PERMITTED_PASSTHROUGH_FB:
                    return MakeError(XRErrors::PermissionDenied, operation);
                case XR_ERROR_FEATURE_UNSUPPORTED:
                case XR_ERROR_FUNCTION_UNSUPPORTED:
                    return MakeError(XRErrors::OperationUnsupported, operation);
                case XR_ERROR_FEATURE_ALREADY_CREATED_PASSTHROUGH_FB:
                case XR_ERROR_INSUFFICIENT_RESOURCES_PASSTHROUGH_FB:
                    return MakeError(XRErrors::CapacityExceeded, operation);
                case XR_ERROR_SESSION_LOST:
                case XR_ERROR_INSTANCE_LOST:
                case XR_SESSION_LOSS_PENDING:
                    return MakeError(XRErrors::IdentityStale, operation);
                default:
                    return MakeError(XRErrors::OperationUnavailable, operation);
            }
        }

        /** @brief Verifies runtime reconstruction and two native compositor slots on the selected system. */
        Result<void> CheckPassthroughSystem(PFN_xrGetSystemProperties query, const NativeSessionBorrow &native) {
            XrSystemPassthroughProperties2FB passthrough{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES2_FB};
            XrSystemProperties system{XR_TYPE_SYSTEM_PROPERTIES, &passthrough};
            const auto result = query(native.instance, native.system, &system);
            if (result != XR_SUCCESS)
                return Result<void>::Failure(PassthroughFailure("xrGetSystemProperties(passthrough)", result));
            if (system.type != XR_TYPE_SYSTEM_PROPERTIES || system.next != &passthrough || system.systemId != native.system ||
                passthrough.type != XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES2_FB || passthrough.next)
                return Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "Malformed passthrough system properties"));
            if ((passthrough.capabilities & XR_PASSTHROUGH_CAPABILITY_BIT_FB) == 0)
                return Result<void>::Failure(
                    MakeError(XRErrors::OperationUnsupported, "Selected system has no reconstruction passthrough"));
            if (system.graphicsProperties.maxLayerCount < 2)
                return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded, "Projection and passthrough need two native layers"));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc OpenXRNativeSession::StartPassthrough */
    Result<void> OpenXRNativeSession::StartPassthrough(const XRPassthroughScope &scope) {
        if (controlThread_ != std::this_thread::get_id() || passthroughCall_ || lightCall_ || retirementCall_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (passthrough_.feature != XR_NULL_HANDLE)
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded, "Retire the previous passthrough feature first"));
        if (!ValidScope(scope))
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        if (scope.grant <= lastPassthroughGrant_)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));

        const PassthroughOperation operation{passthroughCall_};
        const auto admission = ReadExtensionAdmission(scope.session, Extension);
        if (admission.HasError())
            return Result<void>::Failure(admission.ErrorValue());
        const auto &extension = admission.Value();
        if (extension.state == NativeExtensionState::Denied)
            return Result<void>::Failure(MakeError(XRErrors::PermissionDenied));
        if (extension.state == NativeExtensionState::VersionIncompatible ||
            (extension.state == NativeExtensionState::Enabled && extension.discoveredVersion < PropertiesVersion))
            return Result<void>::Failure(MakeError(XRErrors::ContractVersionIncompatible));
        if (extension.state != NativeExtensionState::Enabled || extension.minimumVersion == 0)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported, "Passthrough was not requested and enabled"));
        const auto native = Borrow(scope.session);
        if (native.HasError())
            return Result<void>::Failure(native.ErrorValue());
        PFN_xrGetSystemProperties system{};
        PFN_xrCreatePassthroughFB create{};
        PFN_xrCreatePassthroughLayerFB createLayer{};
        PFN_xrPassthroughStartFB start{};
        PFN_xrPassthroughLayerResumeFB resume{};
        Passthrough candidate{.scope = scope};
        const auto get = dispatch_.getProc;
        const auto instance = native.Value().instance;
        if (!Resolve(get, instance, "xrGetSystemProperties", system) || !Resolve(get, instance, "xrCreatePassthroughFB", create) ||
            !Resolve(get, instance, "xrCreatePassthroughLayerFB", createLayer) || !Resolve(get, instance, "xrPassthroughStartFB", start) ||
            !Resolve(get, instance, "xrPassthroughLayerResumeFB", resume) ||
            !Resolve(get, instance, "xrPassthroughPauseFB", candidate.pause) ||
            !Resolve(get, instance, "xrDestroyPassthroughLayerFB", candidate.destroyLayer) ||
            !Resolve(get, instance, "xrDestroyPassthroughFB", candidate.destroy))
            return Result<void>::Failure(MakeError(XRErrors::LoaderIncompatible, "Passthrough dispatch is incomplete"));
        if (auto supported = CheckPassthroughSystem(system, native.Value()); supported.HasError())
            return supported;
        if (const auto current = Borrow(scope.session); current.HasError())
            return Result<void>::Failure(current.ErrorValue());

        lastPassthroughGrant_ = scope.grant;
        const auto session = native.Value().session;
        const auto prepare = [&]() -> Result<void> {
            const XrPassthroughCreateInfoFB feature{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
            XrPassthroughFB created{XR_NULL_HANDLE};
            auto result = create(session, &feature, &created);
            if (XR_SUCCEEDED(result))
                candidate.feature = created;
            if (result != XR_SUCCESS)
                return Result<void>::Failure(PassthroughFailure("xrCreatePassthroughFB", result));
            if (candidate.feature == XR_NULL_HANDLE)
                return Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "Passthrough returned no feature"));

            const XrPassthroughLayerCreateInfoFB layer{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB, nullptr, candidate.feature, 0,
                                                       XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB};
            XrPassthroughLayerFB createdLayer{XR_NULL_HANDLE};
            result = createLayer(session, &layer, &createdLayer);
            if (XR_SUCCEEDED(result))
                candidate.layer = createdLayer;
            if (result != XR_SUCCESS)
                return Result<void>::Failure(PassthroughFailure("xrCreatePassthroughLayerFB", result));
            if (candidate.layer == XR_NULL_HANDLE)
                return Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "Passthrough returned no layer"));

            result = start(candidate.feature);
            candidate.started = XR_SUCCEEDED(result);
            if (result != XR_SUCCESS)
                return Result<void>::Failure(PassthroughFailure("xrPassthroughStartFB", result));
            result = resume(candidate.layer);
            if (result != XR_SUCCESS)
                return Result<void>::Failure(PassthroughFailure("xrPassthroughLayerResumeFB", result));
            const auto current = Borrow(scope.session);
            return current.HasError() ? Result<void>::Failure(current.ErrorValue()) : Result<void>::Success();
        }();
        passthrough_ = candidate;
        if (prepare.HasError()) {
            if (auto retired = RetirePassthrough(); retired.HasError())
                return retired;
            return prepare;
        }
        passthrough_.admitted = true;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRNativeSession::StopPassthrough */
    Result<void> OpenXRNativeSession::StopPassthrough(const XRPassthroughScope &scope) {
        if (controlThread_ != std::this_thread::get_id() || passthroughCall_ || lightCall_ || retirementCall_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (!ValidScope(scope))
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        if (scope != passthrough_.scope)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        const PassthroughOperation operation{passthroughCall_};
        return RetirePassthrough();
    }

    /** @copydoc OpenXRNativeSession::RevokePassthrough */
    void OpenXRNativeSession::RevokePassthrough() noexcept {
        passthrough_.admitted = false;
        if (passthrough_.started) {
            const PassthroughOperation operation{passthroughCall_};
            passthrough_.started = false;
            const auto result = passthrough_.pause(passthrough_.feature);
            passthrough_.started = XR_FAILED(result);
        }
    }

    /** @copydoc OpenXRNativeSession::RetirePassthrough */
    Result<void> OpenXRNativeSession::RetirePassthrough() {
        RevokePassthrough();
        if (passthrough_.layer != XR_NULL_HANDLE) {
            const auto result = passthrough_.destroyLayer(passthrough_.layer);
            if (result != XR_SUCCESS)
                return Result<void>::Failure(PassthroughFailure("xrDestroyPassthroughLayerFB", result));
            passthrough_.layer = XR_NULL_HANDLE;
        }
        if (passthrough_.feature != XR_NULL_HANDLE) {
            const auto result = passthrough_.destroy(passthrough_.feature);
            if (result != XR_SUCCESS)
                return Result<void>::Failure(PassthroughFailure("xrDestroyPassthroughFB", result));
        }
        passthrough_ = {};
        return Result<void>::Success();
    }

    /** @copydoc OpenXRNativeSession::EndPassthroughFrame */
    Result<XrResult> OpenXRNativeSession::EndPassthroughFrame(const XRPassthroughScope &scope, XrSession session,
                                                              const XrFrameEndInfo &projection, PFN_xrEndFrame dispatch) {
        using Outcome = Result<XrResult>;
        if (controlThread_ != std::this_thread::get_id() || passthroughCall_ || lightCall_ || retirementCall_)
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible));
        if (scope != passthrough_.scope)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        if (!passthrough_.admitted)
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable, "The passthrough grant has ended"));
        const PassthroughOperation operation{passthroughCall_};
        const auto native = Borrow(scope.session);
        if (native.HasError() || native.Value().session != session) {
            RevokePassthrough();
            return Outcome::Failure(native.HasError() ? native.ErrorValue() : MakeError(XRErrors::IdentityStale));
        }
        if (!passthrough_.admitted)
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable, "The passthrough grant has ended"));
        const XrCompositionLayerPassthroughFB underlay{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB, nullptr,
                                                       XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, XR_NULL_HANDLE,
                                                       passthrough_.layer};
        const std::array layers{std::bit_cast<const XrCompositionLayerBaseHeader *>(&underlay), projection.layers[0]};
        auto combined = projection;
        combined.layerCount = static_cast<std::uint32_t>(layers.size());
        combined.layers = layers.data();
        const auto result = dispatch(session, &combined);
        if (result != XR_SUCCESS)
            RevokePassthrough();
        return Outcome::Success(result);
    }
}  // namespace Horo::XR::OpenXRInternal
