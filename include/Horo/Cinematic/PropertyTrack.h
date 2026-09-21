#pragma once

/**
 * @file PropertyTrack.h
 * @brief Typed, bounded cinematic property-track sampling and application.
 */

#include "Horo/Cinematic/CinematicIdentity.h"
#include "Horo/Cinematic/CurveSampling.h"
#include "Horo/Runtime/Scene/PropertyBindingRegistry.h"
#include "Horo/Runtime/Scene/SceneIdentity.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Cinematic {
    /** @brief Scene-owned typed property value reused by cinematic sampling and application. */
    using PropertyBindingValue = Runtime::PropertyBindingValue;

    /** @brief Current binary contract understood by property-track evaluation. */
    struct PropertyTrackVersion final {
        std::uint16_t major{1};
        std::uint16_t minor{};
        constexpr auto operator<=>(const PropertyTrackVersion &) const noexcept = default;
    };

    inline constexpr PropertyTrackVersion CurrentPropertyTrackVersion{1, 0};
    inline constexpr std::size_t MaximumPropertyTracks = 1'024;

    /** @brief Number of scalar channels required by one typed property value. */
    [[nodiscard]] constexpr std::size_t PropertyTrackChannelCount(const Runtime::PropertyBindingType type) noexcept {
        switch (type) {
            case Runtime::PropertyBindingType::Float:
            case Runtime::PropertyBindingType::Boolean:
                return 1;
            case Runtime::PropertyBindingType::Vec2:
                return 2;
            case Runtime::PropertyBindingType::Vec3:
                return 3;
            case Runtime::PropertyBindingType::Vec4:
                return 4;
            case Runtime::PropertyBindingType::Count:
                return 0;
        }
        return 0;
    }

    /** @brief Borrowed scalar curves composing one typed float/vector/bool property. */
    struct PropertyCurveSet final {
        Runtime::PropertyBindingType type{Runtime::PropertyBindingType::Float};
        std::array<std::optional<ScalarCurveView>, 4> channels;
    };

    /** @brief Stable authored property target and borrowed typed curves admitted at activation. */
    struct PropertyTrackDescriptor final {
        PropertyTrackVersion version{CurrentPropertyTrackVersion};
        TrackId track;
        Runtime::SceneObjectId targetObject;
        PropertyBindingId binding;
        PropertyCurveSet curves;
        bool required{true}; /**< Optional tracks remain evaluable but report a typed skipped-binding diagnostic. */
    };

    /** @brief Runtime target snapshot supplied by SceneModel at an owned boundary. */
    struct PropertyBindingTargetSnapshot final {
        PropertyBindingId binding;
        Runtime::SceneObjectId targetObject;
        Gameplay::ComponentTypeId componentType;
        std::uint64_t componentRevision{};
        void *component{}; /**< Borrowed only for the current owner boundary; never persisted or retained by a scene asset. */
    };

    /** @brief Generation fence for a replaceable scene and its property-binding snapshot. */
    struct PropertySceneVersion final {
        std::uint64_t sceneGeneration{};
        std::uint64_t bindingRevision{};
        constexpr auto operator<=>(const PropertySceneVersion &) const noexcept = default;
    };

    /** @brief Per-call scene snapshot used to reject stale component storage before a callback is invoked. */
    struct PropertyEvaluationContext final {
        PropertySceneVersion scene;
        std::span<const PropertyBindingTargetSnapshot> targets;
    };

    /** @brief One sampled property value in stable authored-track order. */
    struct PropertyEvaluationValue final {
        TrackId track;
        PropertyBindingId binding;
        Runtime::SceneObjectId targetObject;
        Runtime::PropertyBindingValue value;
    };

    /** @brief Explicit reason emitted when a property value cannot be applied. */
    enum class PropertyBindingEvaluationOutcome : std::uint8_t {
        Applied,
        BindingMissing,
        BindingStale,
        TargetMissing,
        ComponentMismatch,
        TypeMismatch,
        ValueOutOfRange,
        ReadOnly,
        WriteRejected,
        Count
    };

    /** @brief Caller-owned structured diagnostic for editor Problems or runtime event adapters. */
    struct PropertyEvaluationDiagnostic final {
        TrackId track;
        PropertyBindingId binding;
        Runtime::SceneObjectId targetObject;
        PropertyBindingEvaluationOutcome outcome{PropertyBindingEvaluationOutcome::BindingMissing};
        std::optional<Error> error;
    };

    /** @brief Counts successful applications and surfaced skipped-binding outcomes for one boundary. */
    struct PropertyEvaluationResult final {
        std::size_t sampled{};
        std::size_t applied{};
        std::size_t diagnostics{};
    };

    /**
     * @brief Activation-built property plan with allocation-free random-access sampling.
     * @note Curve views borrow immutable caller-owned keys. The registry and target snapshot
     * must outlive the plan and each evaluation boundary supplies the current target snapshot.
     */
    class PropertyEvaluationPlan final {
    public:
        /**
         * @brief Validates typed bindings, target generations, curves and capacity, then compiles stable track order.
         * @param scene Non-zero scene/binding generation captured at activation.
         * @param tracks Property tracks whose borrowed curve storage outlives the plan.
         * @param targets Exact target component snapshot for every required track.
         * @param registry Frozen property binding registry consumed by the plan.
         * @return Compiled plan or a typed version, binding, type, target, or capacity failure.
         */
        [[nodiscard]] static Result<PropertyEvaluationPlan> Create(PropertySceneVersion scene,
                                                                   std::span<const PropertyTrackDescriptor> tracks,
                                                                   std::span<const PropertyBindingTargetSnapshot> targets,
                                                                   const Runtime::PropertyBindingRegistry &registry);

        /**
         * @brief Samples values directly at an arbitrary time into caller-owned storage.
         * @param time Exact random-access curve time.
         * @param context Current scene generation and target snapshot.
         * @param output Storage with at least TrackCount entries.
         * @return Number of resolved values written or a typed stale, sample, or capacity failure.
         * @note The successful path allocates nothing and mutates no playback history.
         */
        [[nodiscard]] Result<std::size_t> Evaluate(CurveTime time, const PropertyEvaluationContext &context,
                                                   std::span<PropertyEvaluationValue> output) const;

        /**
         * @brief Applies a previously sampled value batch through owner-validated typed setters.
         * @param context Current scene generation and target snapshot.
         * @param values Values returned by Evaluate for this plan.
         * @param diagnostics Caller storage with at least TrackCount entries.
         * @return Applied/diagnostic counts; binding failures are skipped but never hidden.
         */
        [[nodiscard]] Result<PropertyEvaluationResult> Apply(const PropertyEvaluationContext &context,
                                                             std::span<const PropertyEvaluationValue> values,
                                                             std::span<PropertyEvaluationDiagnostic> diagnostics) const;

        /**
         * @brief Samples and applies one boundary with one preflighted value batch.
         * @param time Exact random-access curve time.
         * @param context Current scene generation and target snapshot.
         * @param values Caller storage with at least TrackCount entries.
         * @param diagnostics Caller storage with at least TrackCount entries.
         * @return Counts or a typed sampling/stale/capacity failure.
         */
        [[nodiscard]] Result<PropertyEvaluationResult> EvaluateAndApply(CurveTime time, const PropertyEvaluationContext &context,
                                                                        std::span<PropertyEvaluationValue> values,
                                                                        std::span<PropertyEvaluationDiagnostic> diagnostics) const;

        /** @brief Returns the exact activation scene fence. @return Captured scene version. */
        [[nodiscard]] PropertySceneVersion SceneVersion() const noexcept;
        /** @brief Returns the number of admitted tracks, including optional unresolved tracks. */
        [[nodiscard]] std::size_t TrackCount() const noexcept;

    private:
        struct CompiledTrack final {
            PropertyTrackDescriptor track;
            const Runtime::PropertyBindingDescriptor *binding{};
            std::optional<PropertyBindingTargetSnapshot> activationTarget;
            std::size_t activationTargetIndex{MaximumPropertyTracks};
            PropertyBindingEvaluationOutcome activationOutcome{PropertyBindingEvaluationOutcome::Applied};
        };

        [[nodiscard]] static Result<void> ValidateTargetSnapshots(std::span<const PropertyBindingTargetSnapshot> targets);
        [[nodiscard]] static Result<CompiledTrack> CompileTrack(const PropertyTrackDescriptor &track,
                                                                 std::span<const PropertyTrackDescriptor> priorTracks,
                                                                 std::span<const PropertyBindingTargetSnapshot> targets,
                                                                 const Runtime::PropertyBindingRegistry &registry);
        [[nodiscard]] bool ApplyTrack(const CompiledTrack &compiled, const PropertyEvaluationContext &context,
                                      const PropertyEvaluationValue &value, PropertyEvaluationDiagnostic &diagnostic) const;

        PropertyEvaluationPlan(PropertySceneVersion scene, std::vector<CompiledTrack> orderedTracks) noexcept;

        PropertySceneVersion scene_;
        std::vector<CompiledTrack> orderedTracks_;
    };
}  // namespace Horo::Cinematic
