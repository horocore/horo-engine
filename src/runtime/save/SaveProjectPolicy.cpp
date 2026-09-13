#include "Horo/Runtime/Save/SaveProjectPolicy.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <bit>
#include <string>
#include <utility>

namespace Horo::Runtime {
    namespace {
        [[nodiscard]] constexpr bool IsKnown(const SavePolicyMode mode) noexcept {
            return mode >= SavePolicyMode::Manual && mode < SavePolicyMode::Count;
        }

        [[nodiscard]] constexpr std::size_t Index(const SavePolicyMode mode) noexcept {
            return static_cast<std::size_t>(mode);
        }

        [[nodiscard]] constexpr std::uint8_t Bit(const SavePolicyMode mode) noexcept {
            return static_cast<std::uint8_t>(1U << Index(mode));
        }

        [[nodiscard]] constexpr bool IsKnown(const SaveModeEligibility value) noexcept {
            return value >= SaveModeEligibility::StableRuntime && value <= SaveModeEligibility::SuspendTransition;
        }

        [[nodiscard]] constexpr bool IsKnown(const SaveRotationStrategy value) noexcept {
            return value >= SaveRotationStrategy::None && value <= SaveRotationStrategy::ReplaceOldest;
        }

        [[nodiscard]] constexpr bool IsKnown(const SavePresentationVisibility value) noexcept {
            return value >= SavePresentationVisibility::Hidden && value <= SavePresentationVisibility::AutomaticNotice;
        }

        [[nodiscard]] constexpr bool IsKnown(const SaveThumbnailPolicy value) noexcept {
            return value >= SaveThumbnailPolicy::Disabled && value <= SaveThumbnailPolicy::Required;
        }

        [[nodiscard]] constexpr bool IsKnown(const SaveConfirmationPolicy value) noexcept {
            return value >= SaveConfirmationPolicy::None && value <= SaveConfirmationPolicy::Always;
        }

        [[nodiscard]] constexpr bool IsKnown(const SaveUnsupportedModeFallback value) noexcept {
            return value >= SaveUnsupportedModeFallback::RejectProject && value <= SaveUnsupportedModeFallback::DisableMode;
        }

        [[nodiscard]] constexpr bool IsKnown(const SaveThumbnailFallback value) noexcept {
            return value >= SaveThumbnailFallback::RejectProject && value <= SaveThumbnailFallback::Omit;
        }

        [[nodiscard]] constexpr bool IsBackgroundMode(const SavePolicyMode mode) noexcept {
            return mode == SavePolicyMode::Auto || mode == SavePolicyMode::Checkpoint || mode == SavePolicyMode::Suspend;
        }

        [[nodiscard]] constexpr const char *ModeName(const SavePolicyMode mode) noexcept {
            using enum SavePolicyMode;
            switch (mode) {
                case Manual:
                    return "manual";
                case Quick:
                    return "quick";
                case Auto:
                    return "auto";
                case Checkpoint:
                    return "checkpoint";
                case Suspend:
                    return "suspend";
                case Load:
                    return "load";
                case Count:
                    break;
            }
            return "unknown";
        }

        [[nodiscard]] Error InvalidPolicy(const SavePolicyMode mode, const char *reason) {
            return MakeError(SaveErrors::PolicyInvalid, std::string{"Save policy mode '"} + ModeName(mode) + "' is invalid: " + reason);
        }

        [[nodiscard]] Result<void> ValidateEnums(const SavePolicyMode mode, const SaveModeProjectPolicy &policy) {
            if (!IsKnown(policy.eligibility) || !IsKnown(policy.rotation.strategy) || !IsKnown(policy.presentation.visibility) ||
                !IsKnown(policy.presentation.thumbnail) || !IsKnown(policy.presentation.confirmation) || !IsKnown(policy.unsupportedMode) ||
                !IsKnown(policy.unsupportedThumbnail))
                return Result<void>::Failure(InvalidPolicy(mode, "one or more typed policy values are unknown"));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateDisabled(const SavePolicyMode mode, const SaveModeProjectPolicy &policy) {
            if (policy.rotation.strategy != SaveRotationStrategy::None || policy.rotation.maximumRetainedSlots != 0 ||
                policy.rotation.preserveLastSuccessful || policy.cooldown.minimumIntervalMilliseconds != 0 ||
                policy.cooldown.coalesceWhenBusy || policy.presentation.visibility != SavePresentationVisibility::Hidden ||
                policy.presentation.thumbnail != SaveThumbnailPolicy::Disabled ||
                policy.presentation.confirmation != SaveConfirmationPolicy::None)
                return Result<void>::Failure(
                    InvalidPolicy(mode, "disabled modes must use the zero-work rotation, cooldown, and presentation policy"));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateLoadPolicy(const SavePolicyMode mode, const SaveModeProjectPolicy &policy) {
            const SaveRotationPolicy &rotation = policy.rotation;
            if (rotation.strategy != SaveRotationStrategy::None || rotation.maximumRetainedSlots != 0 || rotation.preserveLastSuccessful ||
                policy.cooldown.minimumIntervalMilliseconds != 0 || policy.cooldown.coalesceWhenBusy ||
                policy.presentation.thumbnail != SaveThumbnailPolicy::Disabled)
                return Result<void>::Failure(
                    InvalidPolicy(mode, "load cannot rotate slots, schedule cooldown work, or capture thumbnails"));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateRetentionCapacity(const SavePolicyMode mode, const SaveRotationPolicy &rotation) {
            if (rotation.maximumRetainedSlots == 0 || rotation.maximumRetainedSlots > CookedSaveProjectPolicy::MaximumRetainedSlotsPerMode)
                return Result<void>::Failure(InvalidPolicy(mode, "retained slot capacity must be within the engine ceiling"));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateReplacementContract(const SavePolicyMode mode, const SaveRotationPolicy &rotation) {
            if (rotation.strategy == SaveRotationStrategy::ReplaceSingle && rotation.maximumRetainedSlots != 1)
                return Result<void>::Failure(InvalidPolicy(mode, "single replacement requires exactly one retained slot"));
            if (rotation.strategy == SaveRotationStrategy::ReplaceOldest &&
                (rotation.maximumRetainedSlots < 2 || !rotation.preserveLastSuccessful))
                return Result<void>::Failure(
                    InvalidPolicy(mode, "oldest-slot rotation requires at least two slots and last-success preservation"));
            if (rotation.strategy == SaveRotationStrategy::None && rotation.preserveLastSuccessful)
                return Result<void>::Failure(InvalidPolicy(mode, "last-success preservation requires a replacement strategy"));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateRotation(const SavePolicyMode mode, const SaveModeProjectPolicy &policy) {
            if (mode == SavePolicyMode::Load)
                return ValidateLoadPolicy(mode, policy);
            if (const Result<void> capacity = ValidateRetentionCapacity(mode, policy.rotation); capacity.HasError())
                return capacity;
            return ValidateReplacementContract(mode, policy.rotation);
        }

        [[nodiscard]] Result<void> ValidateCooldown(const SavePolicyMode mode, const SaveModeProjectPolicy &policy) {
            if (policy.cooldown.minimumIntervalMilliseconds > CookedSaveProjectPolicy::MaximumCooldownMilliseconds)
                return Result<void>::Failure(InvalidPolicy(mode, "cooldown exceeds the portable project maximum"));
            if (!IsBackgroundMode(mode) && policy.cooldown.coalesceWhenBusy)
                return Result<void>::Failure(InvalidPolicy(mode, "busy coalescing is only valid for background intent modes"));
            if (mode == SavePolicyMode::Auto && policy.cooldown.minimumIntervalMilliseconds == 0)
                return Result<void>::Failure(InvalidPolicy(mode, "automatic save requires a positive cooldown"));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateModeContract(const SavePolicyMode mode, const SaveModeProjectPolicy &policy) {
            if (mode == SavePolicyMode::Quick &&
                (policy.rotation.strategy != SaveRotationStrategy::ReplaceSingle || policy.rotation.maximumRetainedSlots != 1))
                return Result<void>::Failure(InvalidPolicy(mode, "quick save requires one replaceable logical slot"));
            if (mode == SavePolicyMode::Suspend &&
                (policy.eligibility != SaveModeEligibility::SuspendTransition ||
                 policy.rotation.strategy != SaveRotationStrategy::ReplaceSingle || policy.rotation.maximumRetainedSlots != 1))
                return Result<void>::Failure(
                    InvalidPolicy(mode, "suspend save requires suspend-transition eligibility and one replaceable slot"));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidatePresentation(const SavePolicyMode mode, const SaveModeProjectPolicy &policy) {
            if (IsBackgroundMode(mode) && policy.presentation.confirmation == SaveConfirmationPolicy::Always)
                return Result<void>::Failure(InvalidPolicy(mode, "background modes cannot require blocking confirmation"));
            if (policy.presentation.thumbnail == SaveThumbnailPolicy::Required &&
                policy.unsupportedThumbnail == SaveThumbnailFallback::Omit)
                return Result<void>::Failure(InvalidPolicy(mode, "required thumbnails cannot declare omission fallback"));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateModeSpecific(const SavePolicyMode mode, const SaveModeProjectPolicy &policy) {
            if (const Result<void> cooldown = ValidateCooldown(mode, policy); cooldown.HasError())
                return cooldown;
            if (const Result<void> contract = ValidateModeContract(mode, policy); contract.HasError())
                return contract;
            return ValidatePresentation(mode, policy);
        }
    }  // namespace

    /** @copydoc ValidateSaveProjectPolicy */
    Result<void> ValidateSaveProjectPolicy(const SaveProjectPolicy &project) {
        if (project.schemaVersion != SaveProjectPolicy::CurrentSchemaVersion)
            return Result<void>::Failure(
                MakeError(SaveErrors::PolicyInvalid, "Save project policy schema version is unsupported; recook the project settings."));

        for (std::size_t index = 0; index < project.modes.size(); ++index) {
            const auto mode = static_cast<SavePolicyMode>(index);
            const SaveModeProjectPolicy &policy = project.modes[index];
            if (const Result<void> enums = ValidateEnums(mode, policy); enums.HasError())
                return enums;
            if (!policy.enabled) {
                if (const Result<void> disabled = ValidateDisabled(mode, policy); disabled.HasError())
                    return disabled;
                continue;
            }
            if (const Result<void> rotation = ValidateRotation(mode, policy); rotation.HasError())
                return rotation;
            if (const Result<void> specific = ValidateModeSpecific(mode, policy); specific.HasError())
                return specific;
        }
        return Result<void>::Success();
    }

    /** @copydoc CookedSaveProjectPolicy::Create */
    Result<CookedSaveProjectPolicy> CookedSaveProjectPolicy::Create(SaveProjectPolicy project,
                                                                    const SaveRuntimeCapabilities &capabilities) {
        if (const Result<void> validation = ValidateSaveProjectPolicy(project); validation.HasError())
            return Result<CookedSaveProjectPolicy>::Failure(validation.ErrorValue());

        std::uint8_t enabledMask{};
        std::uint8_t capabilityDisabledMask{};
        std::uint8_t backgroundMask{};
        for (std::size_t index = 0; index < project.modes.size(); ++index) {
            const auto mode = static_cast<SavePolicyMode>(index);
            SaveModeProjectPolicy &policy = project.modes[index];
            if (!policy.enabled)
                continue;

            if (!capabilities.Supports(mode)) {
                if (policy.unsupportedMode == SaveUnsupportedModeFallback::RejectProject)
                    return Result<CookedSaveProjectPolicy>::Failure(
                        MakeError(SaveErrors::PolicyCapabilityUnsupported,
                                  std::string{"Save policy mode '"} + ModeName(mode) +
                                      "' is enabled but unavailable in the selected runtime composition."));
                policy = {};
                capabilityDisabledMask = static_cast<std::uint8_t>(capabilityDisabledMask | Bit(mode));
                continue;
            }

            if (policy.presentation.thumbnail != SaveThumbnailPolicy::Disabled && !capabilities.thumbnailCapture) {
                if (policy.unsupportedThumbnail == SaveThumbnailFallback::RejectProject)
                    return Result<CookedSaveProjectPolicy>::Failure(
                        MakeError(SaveErrors::PolicyCapabilityUnsupported,
                                  std::string{"Save policy mode '"} + ModeName(mode) +
                                      "' requires unavailable thumbnail capture; select omission fallback or another composition."));
                policy.presentation.thumbnail = SaveThumbnailPolicy::Disabled;
            }

            enabledMask = static_cast<std::uint8_t>(enabledMask | Bit(mode));
            if (IsBackgroundMode(mode))
                backgroundMask = static_cast<std::uint8_t>(backgroundMask | Bit(mode));
        }
        return Result<CookedSaveProjectPolicy>::Success(
            CookedSaveProjectPolicy{std::move(project), enabledMask, capabilityDisabledMask, backgroundMask});
    }

    /** @copydoc CookedSaveProjectPolicy::CookedSaveProjectPolicy */
    CookedSaveProjectPolicy::CookedSaveProjectPolicy(SaveProjectPolicy project, const std::uint8_t enabledMask,
                                                     const std::uint8_t capabilityDisabledMask, const std::uint8_t backgroundMask) noexcept
        : project_(std::move(project)), enabledMask_(enabledMask), capabilityDisabledMask_(capabilityDisabledMask),
          backgroundMask_(backgroundMask) {}

    /** @copydoc CookedSaveProjectPolicy::IsEnabled */
    bool CookedSaveProjectPolicy::IsEnabled(const SavePolicyMode mode) const noexcept {
        return IsKnown(mode) && (enabledMask_ & Bit(mode)) != 0;
    }

    /** @copydoc CookedSaveProjectPolicy::WasDisabledByCapability */
    bool CookedSaveProjectPolicy::WasDisabledByCapability(const SavePolicyMode mode) const noexcept {
        return IsKnown(mode) && (capabilityDisabledMask_ & Bit(mode)) != 0;
    }

    /** @copydoc CookedSaveProjectPolicy::Mode */
    const SaveModeProjectPolicy *CookedSaveProjectPolicy::Mode(const SavePolicyMode mode) const noexcept {
        return IsKnown(mode) ? &project_.modes[Index(mode)] : nullptr;
    }

    /** @copydoc CookedSaveProjectPolicy::BackgroundIntentModeCount */
    std::size_t CookedSaveProjectPolicy::BackgroundIntentModeCount() const noexcept {
        return static_cast<std::size_t>(std::popcount(backgroundMask_));
    }

    /** @copydoc CookedSaveProjectPolicy::HasBackgroundIntentWork */
    bool CookedSaveProjectPolicy::HasBackgroundIntentWork() const noexcept {
        return backgroundMask_ != 0;
    }
}  // namespace Horo::Runtime
