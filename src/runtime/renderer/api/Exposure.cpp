#include "Horo/Runtime/Render/Exposure.h"

#include "RenderErrorDescriptor.h"

#include <cmath>
#include <limits>

namespace Horo::Render {
    namespace {
        /** @brief Checks one finite inclusive numeric interval. */
        bool Bounded(float value, float low, float high) noexcept {
            return std::isfinite(value) && value >= low && value <= high;
        }

        /** @brief Checks canonical authored stop bounds and initial value. */
        bool ValidEv(const ExposureSettings &s) noexcept {
            const std::array values{s.manualEv, s.compensationEv, s.initialEv, s.minimumEv, s.maximumEv};
            for (float value : values)
                if (!Bounded(value, -32.0F, 32.0F))
                    return false;
            return s.minimumEv <= s.initialEv && s.initialEv <= s.maximumEv && s.minimumEv <= s.maximumEv;
        }

        /** @brief Checks finite ordered histogram limits and percentiles. */
        bool ValidMeter(const ExposureSettings &s) noexcept {
            const bool logs = Bounded(s.minimumLogLuminance, -32, 32) && Bounded(s.maximumLogLuminance, -32, 32) &&
                              s.minimumLogLuminance < s.maximumLogLuminance;
            const bool percentiles =
                Bounded(s.lowPercentile, 0, 1) && Bounded(s.highPercentile, 0, 1) && s.lowPercentile < s.highPercentile;
            return logs && percentiles;
        }

        /** @brief Checks the neutral target and finite adaptation rates. */
        bool ValidRates(const ExposureSettings &s) noexcept {
            return Bounded(s.neutralTarget, 0.000001F, 65504.0F) && Bounded(s.brightenEvPerSecond, 0, 64) &&
                   Bounded(s.darkenEvPerSecond, 0, 64);
        }

        /** @brief Checks the structural next-submission relation without GPU completion evidence. */
        bool ValidAutomaticIdentity(const ExposureState &s) noexcept {
            return s.buffer.IsValid() && s.sourceSubmission > 0 && s.sourceSubmission < std::numeric_limits<std::uint64_t>::max() &&
                   s.consumeSubmission == s.sourceSubmission + 1;
        }

        /** @brief Checks overflow-safe source bounds and the finite histogram sample ceiling. */
        bool ValidRegion(FramebufferExtent source, std::array<std::uint32_t, 4> region) noexcept {
            if (region[0] >= source.width || region[1] >= source.height || region[2] == 0 || region[3] == 0)
                return false;
            if (region[2] > source.width - region[0] || region[3] > source.height - region[1])
                return false;
            return static_cast<std::uint64_t>(region[2]) * region[3] <= 16U * 1024U * 1024U;
        }

        /** @brief Checks the closed diagnostic vocabulary and mode-specific state structure. */
        bool ValidStateValue(const ExposureState &s) noexcept {
            if (static_cast<unsigned>(s.reason) > static_cast<unsigned>(ExposureSampleReason::Invalid))
                return false;
            if (s.diagnosticEv && !Bounded(*s.diagnosticEv, -32, 32))
                return false;
            if (s.mode == ExposureMode::Manual)
                return !s.buffer.IsValid() && s.sourceSubmission == 0 && s.diagnosticEv.has_value();
            if (s.mode != ExposureMode::AutomaticHistogram)
                return false;
            return ValidAutomaticIdentity(s);
        }
    }  // namespace

    /** @copydoc ValidateExposureSettings */
    Result<void> ValidateExposureSettings(const ExposureSettings &s) {
        const bool enums = static_cast<unsigned>(s.mode) <= static_cast<unsigned>(ExposureMode::AutomaticHistogram) &&
                           static_cast<unsigned>(s.timeSource) <= static_cast<unsigned>(ExposureTimeSource::View) &&
                           static_cast<unsigned>(s.reset) <= static_cast<unsigned>(ExposureResetPolicy::RetainLastValid);
        if (!enums || !ValidEv(s) || !ValidMeter(s) || !ValidRates(s))
            return Result<void>::Failure(MakeError(ExposureErrors::InvalidSettings));
        if (!Bounded(s.manualEv + s.compensationEv, s.minimumEv, s.maximumEv) ||
            !Bounded(s.initialEv + s.compensationEv, s.minimumEv, s.maximumEv))
            return Result<void>::Failure(MakeError(ExposureErrors::InvalidSettings));
        return Result<void>::Success();
    }

    /** @copydoc ValidateExposureState */
    Result<void> ValidateExposureState(const ExposureState &s, const TemporalHistoryCompatibility &c, std::uint64_t settingsGeneration,
                                       std::uint64_t submission) {
        const bool generations = c.IsValid() && s.view == c.view && s.generation == c.exposureGeneration &&
                                 s.deviceGeneration == c.deviceGeneration && s.settingsGeneration == settingsGeneration;
        if (!generations || settingsGeneration == 0 || submission == 0 || s.consumeSubmission != submission || !ValidStateValue(s))
            return Result<void>::Failure(MakeError(ExposureErrors::IncompatibleState));
        return Result<void>::Success();
    }

    /** @copydoc PrepareExposureDispatch */
    Result<ExposureDispatchParameters> PrepareExposureDispatch(const ExposureSettings &s, FramebufferExtent source,
                                                               std::array<std::uint32_t, 4> region, float deltaSeconds) {
        using Return = Result<ExposureDispatchParameters>;
        if (auto settings = ValidateExposureSettings(s); settings.HasError())
            return Return::Failure(settings.ErrorValue());
        if (!source.IsValid() || !Bounded(deltaSeconds, 0, 1) || s.mode != ExposureMode::AutomaticHistogram)
            return Return::Failure(MakeError(ExposureErrors::InvalidDispatch));
        if (!ValidRegion(source, region))
            return Return::Failure(MakeError(ExposureErrors::InvalidDispatch));
        return Return::Success({region,
                                {s.minimumLogLuminance, s.maximumLogLuminance, s.lowPercentile, s.highPercentile},
                                {s.minimumEv, s.maximumEv, s.brightenEvPerSecond, s.darkenEvPerSecond},
                                {deltaSeconds, s.neutralTarget, s.compensationEv, s.initialEv},
                                {1, ExposureHistogramBins, static_cast<std::uint32_t>(s.timeSource), static_cast<std::uint32_t>(s.reset)}});
    }

    /** @copydoc ReferenceExposureScale */
    Result<float> ReferenceExposureScale(float ev) {
        if (!Bounded(ev, -32, 32))
            return Result<float>::Failure(MakeError(ExposureErrors::InvalidSettings));
        return Result<float>::Success(std::exp2(ev));
    }

    namespace ExposureErrors {
        namespace {
            const ErrorDomainId Domain{"render.exposure"};
        }

        const ErrorCodeDescriptor InvalidSettings =
            Detail::MakeErrorDescriptor(Domain, "render.exposure.settings_invalid", ErrorSeverity::Error, "Exposure settings are invalid.",
                                        "Use finite EV bounds, ordered meter percentiles and bounded adaptation rates.");
        const ErrorCodeDescriptor IncompatibleState =
            Detail::MakeErrorDescriptor(Domain, "render.exposure.state_incompatible", ErrorSeverity::Error,
                                        "Exposure identity is incompatible.",
                                        "Supply the same view/device/settings and exact next submitted view generation.");
        const ErrorCodeDescriptor InvalidDispatch = Detail::
            MakeErrorDescriptor(Domain, "render.exposure.dispatch_invalid", ErrorSeverity::Error,
                                "Exposure GPU dispatch metadata is invalid.",
                                "Supply automatic settings, at most 16M bounded samples and a selected delta of at most one second.");
    }  // namespace ExposureErrors
}  // namespace Horo::Render
