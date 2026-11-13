#pragma once

/** @file VisualSafety.h
 * @brief Backend-neutral captured visual safety and typed effect suppression.
 */
#include "Horo/Foundation/ModuleHost.h"

namespace Horo::Accessibility {
    /** @brief Engine-owned effect classification, supplied by Camera/VFX/UI rather than inferred from pixels. */
    enum class VisualEffectIntent : std::uint8_t {
        CameraShake,
        Flash,
        DecorativeMotion,
        EssentialMotion
    };

    /** @brief Frame-stable visual-safety input for independently owned camera, VFX and animation consumers. */
    class VisualSafetySnapshot final {
    public:
        /** @brief Captures one authoritative visual-safety revision.
         * @param snapshot Snapshot with the safety contribution registered.
         * @return Validated view or VisualSafetyErrors::InvalidSettings; failure preserves the consumer's prior view.
         */
        [[nodiscard]] static Result<VisualSafetySnapshot> Capture(ConfigurationSnapshot snapshot);
        /** @brief Returns desired motion reduction. @return Captured preference. */
        [[nodiscard]] bool ReduceMotion() const noexcept;
        /** @brief Returns desired flash suppression. @return Captured preference. */
        [[nodiscard]] bool SuppressFlash() const noexcept;
        /** @brief Evaluates an explicitly tagged effect without quality/backend gates.
         * @param intent Engine owner's typed effect classification.
         * @return Scale in [0,1]; unknown intent fails closed to zero.
         * @details Reduction suppresses shake/decorative motion. Flash suppression disables
         * tagged flashes independently. Essential motion retains logical behavior; the UI
         * owner may substitute an immediate transition while preserving completion ordering.
         */
        [[nodiscard]] float Scale(VisualEffectIntent intent) const noexcept;
        /** @brief Returns authority revision. @return Captured immutable revision. */
        [[nodiscard]] ConfigurationRevision Revision() const noexcept;

    private:
        VisualSafetySnapshot(ConfigurationSnapshot snapshot, bool reduceMotion, bool suppressFlash, float shake) noexcept;
        ConfigurationSnapshot snapshot_;
        bool reduceMotion_{};
        bool suppressFlash_{};
        float shake_{1};
    };

    /** @brief Creates inert descriptors exclusively for accessibility.visual.safety.*.
     * @return User/NextFrame contribution; shake multiplier is integer thousandths [0,1000].
     */
    [[nodiscard]] ModuleConfigurationContribution MakeVisualSafetyContribution();

    namespace VisualSafetyErrors {
        /** @brief Safety preferences are missing, have incorrect types or exceed the admitted ranges. */
        extern const ErrorCodeDescriptor InvalidSettings;
    }  // namespace VisualSafetyErrors
}  // namespace Horo::Accessibility
