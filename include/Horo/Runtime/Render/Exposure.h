#pragma once

/** @file Exposure.h
 * @brief Inert view exposure settings, next-submission identity and portable GPU dispatch ABI.
 */
#include "Horo/Foundation/ErrorCode.h"
#include "Horo/Runtime/Render/TemporalHistory.h"

#include <array>
#include <cstdint>
#include <optional>

namespace Horo::Render {
    /** @brief Explicit authored meter selection; unknown values fail validation. */
    enum class ExposureMode : std::uint8_t {
        Manual,
        AutomaticHistogram
    };
    /** @brief Clock domain whose admitted delta the host supplies. */
    enum class ExposureTimeSource : std::uint8_t {
        Simulation,
        View
    };
    /** @brief Declared initial/empty-sample recovery policy for the future GPU meter. */
    enum class ExposureResetPolicy : std::uint8_t {
        InitialValue,
        RetainLastValid
    };
    /** @brief Diagnostic reason associated with an already issued state. */
    enum class ExposureSampleReason : std::uint8_t {
        Valid,
        Initial,
        Empty,
        Invalid
    };

    /** @brief Positive EV increases light: scale=exp2(EV). All EV bounds are finite and within [-32,32]. */
    struct ExposureSettings final {
        ExposureMode mode{ExposureMode::Manual};
        ExposureTimeSource timeSource{ExposureTimeSource::View};
        ExposureResetPolicy reset{ExposureResetPolicy::InitialValue};
        float manualEv{}, compensationEv{}, initialEv{};
        float minimumEv{-16.0F}, maximumEv{16.0F};
        float minimumLogLuminance{-16.0F}, maximumLogLuminance{16.0F};
        float lowPercentile{0.01F}, highPercentile{0.99F};
        float neutralTarget{0.18F};
        float brightenEvPerSecond{3.0F}, darkenEvPerSecond{1.0F};
        /** @brief Compares all authored settings. @param other Settings to compare. @return Exact value equality. */
        bool operator==(const ExposureSettings &other) const = default;
    };

    /** @brief Metadata for a host-issued exposure resource, never evidence of GPU completion.
     * Automatic state consumes submission N's GPU result in submission N+1 of the SAME view. Initial/reset exposure is
     * represented as Manual until a real GPU-produced buffer is admitted. Diagnostics are optional later safe-point copies;
     * production automatic exposure reads the device-local buffer, never this CPU value. Owners retain buffer residency.
     */
    struct ExposureState final {
        ExposureMode mode{ExposureMode::Manual};
        RenderViewId view;
        std::uint64_t generation{}, settingsGeneration{}, deviceGeneration{};
        std::uint64_t sourceSubmission{}, consumeSubmission{};
        RenderBufferHandle buffer;
        std::optional<float> diagnosticEv;
        ExposureSampleReason reason{ExposureSampleReason::Initial};
    };

    /** @brief Five 16-byte lanes shared with a future cooked GPU meter; no native dispatch is supplied here.
     * Region is x/y/width/height in the unexposed source. Limits are min/max log2 luminance and low/high percentile.
     * Adaptation is min/max EV and brighten/darken EV per second. Timing is delta seconds, neutral target, compensation,
     * initial EV. Histogram has exactly 256 uint32 bins; float32 reduction excludes non-finite samples and preserves last valid
     * state on empty/invalid input according to reset policy. Controls are ABI version=1, histogram bins=256, time-source enum,
     * reset-policy enum. Native bindings/completion remain a later implementation.
     */
    struct alignas(16) ExposureDispatchParameters final {
        std::array<std::uint32_t, 4> region{};
        std::array<float, 4> limits{};
        std::array<float, 4> adaptation{};
        std::array<float, 4> timing{};
        std::array<std::uint32_t, 4> controls{};
    };

    static_assert(sizeof(ExposureDispatchParameters) == 80);
    /** @brief Fixed portable GPU histogram bin count. */
    inline constexpr std::uint32_t ExposureHistogramBins = 256;

    /** @brief Validates closed policies and finite bounded metering/adaptation settings.
     * @param settings Authored settings. @return Success or typed invalid-settings failure.
     */
    [[nodiscard]] Result<void> ValidateExposureSettings(const ExposureSettings &settings);
    /** @brief Validates exact view/device/exposure generations and next-submission structure only; issues no GPU receipt.
     * @param state Host-issued metadata. @param compatibility Exact current view/history metadata.
     * @param settingsGeneration Exact authored settings revision. @param submission Receiving view submission ordinal.
     * @return Success or typed incompatible-state failure. No native buffer contents or completion are examined.
     */
    [[nodiscard]] Result<void> ValidateExposureState(const ExposureState &state, const TemporalHistoryCompatibility &compatibility,
                                                     std::uint64_t settingsGeneration, std::uint64_t submission);
    /** @brief Packs a bounded region and selected time delta; no pixels are read or GPU work submitted.
     * @param settings Automatic histogram settings. @param source Actual unexposed input extent.
     * @param region In-bounds x/y/width/height, at most 16M samples. @param deltaSeconds Selected clock delta in [0,1].
     * @return Portable ABI value or typed settings/dispatch failure.
     */
    [[nodiscard]] Result<ExposureDispatchParameters> PrepareExposureDispatch(const ExposureSettings &settings, FramebufferExtent source,
                                                                             std::array<std::uint32_t, 4> region, float deltaSeconds);
    /** @brief Reference math only, never a production CPU scene meter or GPU adaptation result.
     * @param exposureEv Finite Horo stop offset in [-32,32]. @return exp2(EV) or typed invalid-settings failure.
     */
    [[nodiscard]] Result<float> ReferenceExposureScale(float exposureEv);

    namespace ExposureErrors {
        extern const ErrorCodeDescriptor InvalidSettings;   /**< Malformed or unbounded authored settings. */
        extern const ErrorCodeDescriptor IncompatibleState; /**< Stale/foreign/invalid view exposure identity. */
        extern const ErrorCodeDescriptor InvalidDispatch;   /**< Invalid region, clock delta or dispatch mode. */
    }  // namespace ExposureErrors
}  // namespace Horo::Render
