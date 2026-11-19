#include "host/OpenXRNativeSession.h"

#include <bit>
#include <cmath>

namespace Horo::XR::OpenXRInternal {
    namespace {
        constexpr std::string_view Extension = XR_ANDROID_LIGHT_ESTIMATION_EXTENSION_NAME;

        /** @brief Serializes one borrowed light operation against reentrant use and parent retirement. */
        struct LightOperation final {
            explicit LightOperation(bool &active) noexcept : active_(active) {
                active_ = true;
            }

            ~LightOperation() {
                active_ = false;
            }

            bool &active_;
        };

        /** @brief Resolves an official light function from the admitted instance. */
        template <typename Function>
        bool ResolveLightFunction(PFN_xrGetInstanceProcAddr getProc, XrInstance instance, const char *name, Function &function) {
            PFN_xrVoidFunction raw{};
            if (getProc(instance, name, &raw) != XR_SUCCESS || !raw)
                return false;
            function = std::bit_cast<Function>(raw);
            return true;
        }

        /** @brief Maps provider failure to the actionable portable controller outcome. */
        const ErrorCodeDescriptor &LightFailure(XrResult result) noexcept {
            switch (result) {
                case XR_ERROR_PERMISSION_INSUFFICIENT:
                    return XRErrors::PermissionDenied;
                case XR_ERROR_FEATURE_UNSUPPORTED:
                case XR_ERROR_FUNCTION_UNSUPPORTED:
                    return XRErrors::OperationUnsupported;
                case XR_ERROR_SESSION_LOST:
                case XR_ERROR_INSTANCE_LOST:
                case XR_SESSION_LOSS_PENDING:
                    return XRErrors::IdentityStale;
                case XR_ERROR_TIME_INVALID:
                    return XRErrors::TimeDomainIncompatible;
                default:
                    return XRErrors::OperationUnavailable;
            }
        }

        /** @brief Verifies the selected system's capacity before creating a sensitive provider child. */
        Result<void> CheckLightSystem(PFN_xrGetSystemProperties query, const NativeSessionBorrow &native) {
            XrSystemLightEstimationPropertiesANDROID light{XR_TYPE_SYSTEM_LIGHT_ESTIMATION_PROPERTIES_ANDROID};
            XrSystemProperties system{XR_TYPE_SYSTEM_PROPERTIES, &light};
            const auto result = query(native.instance, native.system, &system);
            if (result != XR_SUCCESS)
                return Result<void>::Failure(MakeError(LightFailure(result), "xrGetSystemProperties(light estimation)"));
            if (system.type != XR_TYPE_SYSTEM_PROPERTIES || system.next != &light || system.systemId != native.system ||
                light.type != XR_TYPE_SYSTEM_LIGHT_ESTIMATION_PROPERTIES_ANDROID || light.next || light.supportsLightEstimation > XR_TRUE)
                return Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "Malformed native light capability"));
            if (light.supportsLightEstimation == XR_FALSE)
                return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported, "Selected system has no light estimator"));
            return Result<void>::Success();
        }

        /** @brief Rejects unrecognized flags without treating their payload as evidence. */
        bool KnownState(XrLightEstimateStateANDROID state) noexcept {
            return state == XR_LIGHT_ESTIMATE_STATE_VALID_ANDROID || state == XR_LIGHT_ESTIMATE_STATE_INVALID_ANDROID;
        }

        /** @brief Preserves relative provider channels as received, without imposing a renderer color space. */
        bool ValidRgb(const XrVector3f &rgb) noexcept {
            return std::isfinite(rgb.x) && std::isfinite(rgb.y) && std::isfinite(rgb.z) && rgb.x >= 0.0F && rgb.y >= 0.0F && rgb.z >= 0.0F;
        }

        /** @brief Copies only validated fields from either available estimate slot. */
        Result<XRLightEstimate> DecodeLightEstimate(const XRLightEstimateScope &scope, const XrLightEstimateANDROID &native,
                                                    const XrAmbientLightANDROID &ambient, const XrDirectionalLightANDROID &directional) {
            using Outcome = Result<XRLightEstimate>;
            constexpr auto valid = XR_LIGHT_ESTIMATE_STATE_VALID_ANDROID;
            if (native.type != XR_TYPE_LIGHT_ESTIMATE_ANDROID || native.next != &ambient || ambient.type != XR_TYPE_AMBIENT_LIGHT_ANDROID ||
                ambient.next != &directional || directional.type != XR_TYPE_DIRECTIONAL_LIGHT_ANDROID || directional.next ||
                !KnownState(native.state) || !KnownState(ambient.state) || !KnownState(directional.state))
                return Outcome::Failure(MakeError(XRErrors::OperationInvalid, "Malformed native light estimate"));
            if (native.state != valid || (ambient.state != valid && directional.state != valid))
                return Outcome::Failure(MakeError(XRErrors::OperationUnavailable, "No live native light estimate"));

            const auto sampledAt = XRRuntimeTime::Create(native.lastUpdatedTime);
            if (sampledAt.HasError())
                return Outcome::Failure(MakeError(XRErrors::TimeDomainIncompatible, "Native light update time is invalid"));
            XRLightEstimate sample{.scope = scope, .sampledAt = sampledAt.Value()};
            if (ambient.state == valid) {
                if (!ValidRgb(ambient.intensity))
                    return Outcome::Failure(MakeError(XRErrors::OperationInvalid, "Invalid ambient light channels"));
                sample.ambient = XRLightEstimateIntensity{{ambient.intensity.x, ambient.intensity.y, ambient.intensity.z}};
            }
            if (directional.state == valid) {
                const Math::Vec3 direction{directional.direction.x, directional.direction.y, directional.direction.z};
                const float length = Math::Length(direction);
                if (!ValidRgb(directional.intensity) || !std::isfinite(length) || length <= Math::DefaultEpsilon)
                    return Outcome::Failure(MakeError(XRErrors::OperationInvalid, "Invalid directional light evidence"));
                sample.directional =
                    XRDirectionalLightEstimate{{{directional.intensity.x, directional.intensity.y, directional.intensity.z}},
                                               direction / length};
            }
            return Outcome::Success(sample);
        }
    }  // namespace

    /** @copydoc OpenXRNativeSession::StartLightEstimation */
    Result<void> OpenXRNativeSession::StartLightEstimation(const XRLightEstimateScope &scope, const XRLightEstimateLimits &limits) {
        if (controlThread_ != std::this_thread::get_id() || lightCall_ || passthroughCall_ || retirementCall_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (light_.handle != XR_NULL_HANDLE)
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded, "Retire the previous light estimator first"));
        if (!scope.IsValid() || limits.minimumUpdateInterval <= Duration{} || limits.maximumAge <= Duration{})
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        if (scope.grant <= lastLightGrant_)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        const LightOperation operation{lightCall_};
        const auto admission = ReadExtensionAdmission(scope.session, Extension);
        if (admission.HasError())
            return Result<void>::Failure(admission.ErrorValue());
        if (admission.Value().state == NativeExtensionState::Denied)
            return Result<void>::Failure(MakeError(XRErrors::PermissionDenied));
        if (admission.Value().state == NativeExtensionState::VersionIncompatible)
            return Result<void>::Failure(MakeError(XRErrors::ContractVersionIncompatible));
        if (admission.Value().state != NativeExtensionState::Enabled || admission.Value().minimumVersion == 0)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported, "Light estimation route was not requested and enabled"));
        const auto native = Borrow(scope.session);
        if (native.HasError())
            return Result<void>::Failure(native.ErrorValue());

        PFN_xrGetSystemProperties query{};
        PFN_xrCreateLightEstimatorANDROID create{};
        LightEstimator candidate;
        const auto instance = native.Value().instance;
        if (!ResolveLightFunction(dispatch_.getProc, instance, "xrGetSystemProperties", query) ||
            !ResolveLightFunction(dispatch_.getProc, instance, "xrCreateLightEstimatorANDROID", create) ||
            !ResolveLightFunction(dispatch_.getProc, instance, "xrDestroyLightEstimatorANDROID", candidate.destroy) ||
            !ResolveLightFunction(dispatch_.getProc, instance, "xrGetLightEstimateANDROID", candidate.get))
            return Result<void>::Failure(MakeError(XRErrors::LoaderIncompatible, "Light-estimation dispatch is incomplete"));
        if (auto supported = CheckLightSystem(query, native.Value()); supported.HasError())
            return supported;
        if (const auto current = Borrow(scope.session); current.HasError())
            return Result<void>::Failure(current.ErrorValue());

        candidate.scope = scope;
        candidate.limits = limits;
        lastLightGrant_ = scope.grant;
        XrLightEstimatorCreateInfoANDROID info{XR_TYPE_LIGHT_ESTIMATOR_CREATE_INFO_ANDROID};
        const auto created = create(native.Value().session, &info, &candidate.handle);
        if (created != XR_SUCCESS) {
            if (XR_SUCCEEDED(created) && candidate.handle != XR_NULL_HANDLE) {
                light_ = candidate;
                if (auto retired = RetireLightEstimation(); retired.HasError())
                    return retired;
            }
            return Result<void>::Failure(MakeError(LightFailure(created), "xrCreateLightEstimatorANDROID"));
        }
        if (candidate.handle == XR_NULL_HANDLE)
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "Native light estimator returned no handle"));
        light_ = candidate;
        if (const auto current = Borrow(scope.session); current.HasError()) {
            if (auto retired = RetireLightEstimation(); retired.HasError())
                return retired;
            return Result<void>::Failure(current.ErrorValue());
        }
        light_.active = true;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRNativeSession::AdvanceLightEstimation */
    Result<bool> OpenXRNativeSession::AdvanceLightEstimation(const XRLightEstimateScope &scope, XrSpace world, XRRuntimeTime now) {
        using Outcome = Result<bool>;
        if (controlThread_ != std::this_thread::get_id() || lightCall_ || passthroughCall_ || retirementCall_)
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible));
        if (scope != light_.scope)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        if (!light_.active)
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable));
        if (world == XR_NULL_HANDLE || (light_.world != XR_NULL_HANDLE && light_.world != world)) {
            light_.current.reset();
            light_.active = false;
            return Outcome::Failure(MakeError(XRErrors::CoordinateSpaceIncompatible));
        }
        if (!now.IsValid() || now < light_.lastRequest)
            return Outcome::Failure(MakeError(XRErrors::TimeDomainIncompatible));

        const LightOperation operation{lightCall_};
        if (const auto native = Borrow(scope.session); native.HasError()) {
            light_.current.reset();
            light_.active = false;
            return Outcome::Failure(native.ErrorValue());
        }
        if (light_.lastRequest.IsValid() &&
            now.Nanoseconds() - light_.lastRequest.Nanoseconds() < light_.limits.minimumUpdateInterval.ToNanoseconds())
            return Outcome::Success(false);

        XrDirectionalLightANDROID directional{XR_TYPE_DIRECTIONAL_LIGHT_ANDROID, nullptr, XR_LIGHT_ESTIMATE_STATE_INVALID_ANDROID};
        XrAmbientLightANDROID ambient{XR_TYPE_AMBIENT_LIGHT_ANDROID, &directional, XR_LIGHT_ESTIMATE_STATE_INVALID_ANDROID};
        XrLightEstimateANDROID native{XR_TYPE_LIGHT_ESTIMATE_ANDROID, &ambient, XR_LIGHT_ESTIMATE_STATE_INVALID_ANDROID};
        const XrLightEstimateGetInfoANDROID info{XR_TYPE_LIGHT_ESTIMATE_GET_INFO_ANDROID, nullptr, world, now.Nanoseconds()};
        light_.world = world;
        light_.lastRequest = now;
        const auto result = light_.get(light_.handle, &info, &native);
        if (result != XR_SUCCESS) {
            light_.current.reset();
            const auto &failure = LightFailure(result);
            if (&failure == &XRErrors::PermissionDenied || &failure == &XRErrors::IdentityStale ||
                &failure == &XRErrors::OperationUnsupported)
                light_.active = false;
            return Outcome::Failure(MakeError(failure, "xrGetLightEstimateANDROID"));
        }
        if (const auto current = Borrow(scope.session); current.HasError()) {
            light_.current.reset();
            light_.active = false;
            return Outcome::Failure(current.ErrorValue());
        }
        const auto sample = DecodeLightEstimate(scope, native, ambient, directional);
        if (sample.HasError()) {
            light_.current.reset();
            return Outcome::Failure(sample.ErrorValue());
        }
        const auto time = sample.Value().sampledAt;
        if (!time.IsValid() || time > now || time < light_.lastSample ||
            now.Nanoseconds() - time.Nanoseconds() > light_.limits.maximumAge.ToNanoseconds()) {
            light_.current.reset();
            return Outcome::Failure(MakeError(XRErrors::TrackingSnapshotStale, "Native light estimate time is stale or invalid"));
        }
        if (time == light_.lastSample) {
            if (light_.current) {
                if (!sample.Value().ambient)
                    light_.current->ambient.reset();
                if (!sample.Value().directional)
                    light_.current->directional.reset();
                if (!light_.current->ambient && !light_.current->directional)
                    light_.current.reset();
            }
            return Outcome::Success(false);
        }
        light_.lastSample = time;
        light_.current = sample.Value();
        return Outcome::Success(true);
    }

    /** @copydoc OpenXRNativeSession::ReadLightEstimation */
    Result<XRLightEstimate> OpenXRNativeSession::ReadLightEstimation(const XRLightEstimateScope &scope, XRRuntimeTime now) const {
        using Outcome = Result<XRLightEstimate>;
        if (controlThread_ != std::this_thread::get_id() || lightCall_ || passthroughCall_ || retirementCall_)
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible));
        if (scope != light_.scope)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        if (!light_.active || !light_.current)
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable));
        if (!now.IsValid() || now < light_.lastRequest)
            return Outcome::Failure(MakeError(XRErrors::TimeDomainIncompatible));
        const LightOperation operation{lightCall_};
        if (const auto native = Borrow(scope.session); native.HasError())
            return Outcome::Failure(native.ErrorValue());
        if (now.Nanoseconds() - light_.current->sampledAt.Nanoseconds() > light_.limits.maximumAge.ToNanoseconds())
            return Outcome::Failure(MakeError(XRErrors::TrackingSnapshotStale));
        return Outcome::Success(*light_.current);
    }

    /** @copydoc OpenXRNativeSession::StopLightEstimation */
    Result<void> OpenXRNativeSession::StopLightEstimation(const XRLightEstimateScope &scope) {
        if (controlThread_ != std::this_thread::get_id() || lightCall_ || passthroughCall_ || retirementCall_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (scope != light_.scope)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        const LightOperation operation{lightCall_};
        return RetireLightEstimation();
    }

    /** @copydoc OpenXRNativeSession::RetireLightEstimation */
    Result<void> OpenXRNativeSession::RetireLightEstimation() {
        light_.active = false;
        light_.current.reset();
        if (light_.handle != XR_NULL_HANDLE) {
            const auto result = light_.destroy(light_.handle);
            if (result != XR_SUCCESS)
                return Result<void>::Failure(MakeError(LightFailure(result), "xrDestroyLightEstimatorANDROID"));
        }
        light_ = {};
        return Result<void>::Success();
    }
}  // namespace Horo::XR::OpenXRInternal
