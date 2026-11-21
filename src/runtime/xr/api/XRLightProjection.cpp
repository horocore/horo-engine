#include "Horo/XR/XRLightProjection.h"

#include "Horo/XR/XRErrors.h"

#include <cmath>
#include <utility>

namespace Horo::XR {
    namespace {
        /** @brief Validates non-color policy fields before admitting a grant. */
        bool ValidPolicy(const XRLightProjectionPolicy &policy) noexcept {
            using enum XRLightDirectionConvention;
            return (policy.direction == TowardScene || policy.direction == TowardSource) && std::isfinite(policy.ambientScale) &&
                   policy.ambientScale > 0.0F && std::isfinite(policy.directionalScale) && policy.directionalScale > 0.0F &&
                   std::isfinite(policy.maximumAbsoluteChannel) && policy.maximumAbsoluteChannel > 0.0F &&
                   policy.directionalIdentity != 0 && policy.maximumAge.ToNanoseconds() > 0 && policy.smoothingWindow.ToNanoseconds() >= 0;
        }

        /** @brief Converts one explicitly interpreted provider-relative intensity into bounded working RGB. */
        Result<Math::Vec3> ConvertIntensity(const XRLightEstimateIntensity &intensity, Render::SceneColorSourcePolicy color, float scale,
                                            float ceiling) {
            using Outcome = Result<Math::Vec3>;
            const auto rgb = intensity.relativeRgb;
            if (intensity.confidence != XRLightEstimateConfidence::ProviderValid || !Math::IsFinite(rgb) || rgb.x < 0 || rgb.y < 0 ||
                rgb.z < 0)
                return Outcome::Failure(MakeError(XRErrors::OperationInvalid));
            const auto converted = Render::ConvertSceneColorToAcesCg(rgb, color);
            if (converted.HasError())
                return Outcome::Failure(converted.ErrorValue());
            const auto value = converted.Value() * scale;
            if (!Math::IsFinite(value) || std::abs(value.x) > ceiling || std::abs(value.y) > ceiling || std::abs(value.z) > ceiling)
                return Outcome::Failure(MakeError(XRErrors::OperationInvalid));
            return Outcome::Success(value);
        }

        /** @brief Produces only valid canonical light values, without mutating any held presentation. */
        Result<XRProjectedLighting> Project(const XRLightEstimate &estimate, const XRLightProjectionPolicy &policy) {
            using Outcome = Result<XRProjectedLighting>;
            XRProjectedLighting result;
            if (estimate.ambient) {
                const auto ambient =
                    ConvertIntensity(*estimate.ambient, policy.ambientColor, policy.ambientScale, policy.maximumAbsoluteChannel);
                if (ambient.HasError())
                    return Outcome::Failure(ambient.ErrorValue());
                result.ambient = ambient.Value();
            }
            if (estimate.directional) {
                const auto &sample = *estimate.directional;
                const auto rgb =
                    ConvertIntensity(sample.intensity, policy.directionalColor, policy.directionalScale, policy.maximumAbsoluteChannel);
                if (rgb.HasError())
                    return Outcome::Failure(rgb.ErrorValue());
                const auto direction =
                    policy.direction == XRLightDirectionConvention::TowardScene ? sample.providerDirection : -sample.providerDirection;
                const Render::RenderLight light{.direction = direction, .color = rgb.Value(), .stableIdentity = policy.directionalIdentity};
                if (!light.IsValid())
                    return Outcome::Failure(MakeError(XRErrors::OperationInvalid));
                result.directional = light;
            }
            return Outcome::Success(result);
        }

        /** @brief Interpolates finite values without overflowing intermediate differences. */
        Math::Vec3 Blend(const Math::Vec3 &previous, const Math::Vec3 &current, float weight) noexcept {
            return {std::lerp(previous.x, current.x, weight), std::lerp(previous.y, current.y, weight),
                    std::lerp(previous.z, current.z, weight)};
        }

        /** @brief Smooths only slots that remained available in consecutive samples within their bounded age. */
        void Smooth(XRProjectedLighting &next, const XRProjectedLighting &previous, float weight) {
            if (next.ambient && previous.ambient)
                next.ambient = Blend(*previous.ambient, *next.ambient, weight);
            if (next.directional && previous.directional) {
                next.directional->color = Blend(previous.directional->color, next.directional->color, weight);
                const auto direction = Math::TryNormalize(Blend(previous.directional->direction, next.directional->direction, weight));
                if (direction.HasValue())
                    next.directional->direction = direction.Value();
            }
        }

        /** @brief Keeps only previously projected components that remain available at the same source timestamp. */
        std::optional<XRProjectedLighting> RetainRepeatedObservation(std::optional<XRProjectedLighting> previous,
                                                                     const XRLightEstimate &estimate) {
            if (!previous)
                return std::nullopt;
            if (!estimate.ambient)
                previous->ambient.reset();
            if (!estimate.directional)
                previous->directional.reset();
            return previous->ambient || previous->directional ? previous : std::nullopt;
        }
    }  // namespace

    /** @copydoc XRLightProjection::Create */
    Result<XRLightProjection> XRLightProjection::Create(XRLightEstimateScope scope, XRLightProjectionPolicy policy) {
        using Outcome = Result<XRLightProjection>;
        if (!scope.IsValid())
            return Outcome::Failure(MakeError(XRErrors::CoordinateSpaceIncompatible));
        if (!ValidPolicy(policy))
            return Outcome::Failure(MakeError(XRErrors::OperationInvalid));
        for (const auto color : {policy.ambientColor, policy.directionalColor}) {
            if (const auto valid = Render::ValidateSceneColorSourcePolicy(color); valid.HasError())
                return Outcome::Failure(valid.ErrorValue());
        }
        return Outcome::Success(XRLightProjection{scope, policy});
    }

    /** @copydoc XRLightProjection::XRLightProjection */
    XRLightProjection::XRLightProjection(XRLightEstimateScope scope, XRLightProjectionPolicy policy) noexcept
        : scope_(scope), policy_(policy) {}

    /** @copydoc XRLightProjection::Update */
    Result<std::optional<XRProjectedLighting>> XRLightProjection::Update(const XRLightEstimateScope &active, XRRuntimeTime now,
                                                                         const XRLightEstimate *estimate) {
        using Outcome = Result<std::optional<XRProjectedLighting>>;
        auto previous = std::exchange(current_, std::nullopt);
        if (active != scope_) {
            scope_ = {};
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        }
        if (!scope_.session.IsValid() || (estimate && estimate->scope != scope_))
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        if (!now.IsValid() || now < lastClock_)
            return Outcome::Failure(MakeError(XRErrors::TimeDomainIncompatible));
        lastClock_ = now;
        if (!estimate)
            return Outcome::Success(std::nullopt);
        if (estimate->provenance != XRLightEstimateProvenance::AndroidOpenXR)
            return Outcome::Failure(MakeError(XRErrors::OperationUnsupported));
        const auto timestamp = estimate->sampledAt;
        if (!timestamp.IsValid() || timestamp > now || timestamp < lastSample_ ||
            now.Nanoseconds() - timestamp.Nanoseconds() > policy_.maximumAge.ToNanoseconds())
            return Outcome::Failure(MakeError(XRErrors::TrackingSnapshotStale));
        if (timestamp == lastSample_) {
            current_ = RetainRepeatedObservation(std::move(previous), *estimate);
            return Outcome::Success(current_);
        }
        auto next = Project(*estimate, policy_);
        if (next.HasError())
            return Outcome::Failure(next.ErrorValue());
        auto projected = std::move(next).Value();
        if (projected.ambient || projected.directional) {
            const auto gap = timestamp.Nanoseconds() - lastSample_.Nanoseconds();
            const auto smoothing = policy_.smoothingWindow.ToNanoseconds();
            if (const auto previousAge = now.Nanoseconds() - lastSample_.Nanoseconds();
                previous && previousAge <= policy_.maximumAge.ToNanoseconds() && gap < smoothing)
                Smooth(projected, *previous, static_cast<float>(static_cast<double>(gap) / static_cast<double>(smoothing)));
            current_ = projected;
        }
        lastSample_ = timestamp;
        return Outcome::Success(current_);
    }
}  // namespace Horo::XR
