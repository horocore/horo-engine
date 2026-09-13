#pragma once

/**
 * @file SaveProjectPolicy.h
 * @brief Typed project-authored save modes and immutable cooked runtime policy.
 */

#include "Horo/Foundation/Result.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>

namespace Horo::Runtime {
    /** @brief Stable project-configured save and load behavior categories. */
    enum class SavePolicyMode : std::uint8_t {
        Manual = 0,
        Quick = 1,
        Auto = 2,
        Checkpoint = 3,
        Suspend = 4,
        Load = 5,
        Count = 6
    };

    /** @brief Runtime-state eligibility required before one mode may be admitted. */
    enum class SaveModeEligibility : std::uint8_t {
        StableRuntime,
        ActiveGameplay,
        PausedOrMenu,
        SuspendTransition
    };

    /** @brief Catalog replacement behavior for one save category. */
    enum class SaveRotationStrategy : std::uint8_t {
        None,
        ReplaceSingle,
        ReplaceOldest
    };

    /** @brief Backend-neutral UI exposure owned by the product policy. */
    enum class SavePresentationVisibility : std::uint8_t {
        Hidden,
        Listed,
        PrimaryAction,
        AutomaticNotice
    };

    /** @brief Thumbnail requirement without renderer or editor ownership. */
    enum class SaveThumbnailPolicy : std::uint8_t {
        Disabled,
        Optional,
        Required
    };

    /** @brief Confirmation behavior presented by an adapter before admission. */
    enum class SaveConfirmationPolicy : std::uint8_t {
        None,
        OnOverwrite,
        Always
    };

    /** @brief Project choice when a runtime composition does not implement an enabled mode. */
    enum class SaveUnsupportedModeFallback : std::uint8_t {
        RejectProject,
        DisableMode
    };

    /** @brief Project choice when optional thumbnail capture is unavailable. */
    enum class SaveThumbnailFallback : std::uint8_t {
        RejectProject,
        Omit
    };

    /** @brief Finite category capacity and overwrite policy. */
    struct SaveRotationPolicy final {
        SaveRotationStrategy strategy{SaveRotationStrategy::None};
        std::uint16_t maximumRetainedSlots{};
        bool preserveLastSuccessful{};

        [[nodiscard]] auto operator<=>(const SaveRotationPolicy &) const noexcept = default;
    };

    /** @brief Monotonic scheduling policy interpreted by the owning runtime scheduler. */
    struct SaveCooldownPolicy final {
        std::uint64_t minimumIntervalMilliseconds{};
        bool coalesceWhenBusy{};

        [[nodiscard]] auto operator<=>(const SaveCooldownPolicy &) const noexcept = default;
    };

    /** @brief Presentation hints that never own UI or renderer state. */
    struct SavePresentationPolicy final {
        SavePresentationVisibility visibility{SavePresentationVisibility::Hidden};
        SaveThumbnailPolicy thumbnail{SaveThumbnailPolicy::Disabled};
        SaveConfirmationPolicy confirmation{SaveConfirmationPolicy::None};

        [[nodiscard]] auto operator<=>(const SavePresentationPolicy &) const noexcept = default;
    };

    /** @brief Complete project-authored policy for one stable mode. */
    struct SaveModeProjectPolicy final {
        bool enabled{};
        SaveModeEligibility eligibility{SaveModeEligibility::StableRuntime};
        SaveRotationPolicy rotation;
        SaveCooldownPolicy cooldown;
        SavePresentationPolicy presentation;
        SaveUnsupportedModeFallback unsupportedMode{SaveUnsupportedModeFallback::RejectProject};
        SaveThumbnailFallback unsupportedThumbnail{SaveThumbnailFallback::RejectProject};

        [[nodiscard]] auto operator<=>(const SaveModeProjectPolicy &) const noexcept = default;
    };

    /** @brief Portable project value cooked without consulting editor settings or ambient state. */
    struct SaveProjectPolicy final {
        static constexpr std::uint32_t CurrentSchemaVersion = 1;
        static constexpr std::size_t ModeCount = static_cast<std::size_t>(SavePolicyMode::Count);
        static_assert(ModeCount <= 32, "SavePolicyMode count exceeds cooked policy mask capacity");

        std::uint32_t schemaVersion{CurrentSchemaVersion};
        std::array<SaveModeProjectPolicy, ModeCount> modes{};

        [[nodiscard]] auto operator<=>(const SaveProjectPolicy &) const noexcept = default;
    };

    /** @brief Runtime-composition support facts supplied by the application composition root. */
    struct SaveRuntimeCapabilities final {
        std::array<bool, SaveProjectPolicy::ModeCount> supportedModes{};
        bool thumbnailCapture{};

        /** @brief Produces a profile supporting every declared mode and thumbnail capture. @return Full support profile. */
        [[nodiscard]] static constexpr SaveRuntimeCapabilities AllSupported() noexcept {
            SaveRuntimeCapabilities capabilities;
            capabilities.supportedModes.fill(true);
            capabilities.thumbnailCapture = true;
            return capabilities;
        }

        /** @brief Tests exact support for a stable mode. @param mode Mode to inspect. @return False for unknown modes. */
        [[nodiscard]] constexpr bool Supports(SavePolicyMode mode) const noexcept {
            const auto index = static_cast<std::size_t>(mode);
            return index < supportedModes.size() && supportedModes[index];
        }
    };

    /**
     * @brief Validates portable project policy independently from runtime capability selection.
     * @param project Candidate project-authored policy.
     * @return Success or PolicyInvalid with actionable schema and mode context.
     */
    [[nodiscard]] Result<void> ValidateSaveProjectPolicy(const SaveProjectPolicy &project);

    /** @brief Immutable, fixed-storage runtime policy produced from project data and composition facts. */
    class CookedSaveProjectPolicy final {
    public:
        static constexpr std::uint16_t MaximumRetainedSlotsPerMode = 64;
        static constexpr std::uint64_t MaximumCooldownMilliseconds = 86'400'000;

        /**
         * @brief Validates and cooks a complete project policy without editor settings or runtime mutable state.
         * @param project Candidate portable project policy.
         * @param capabilities Runtime-composition capability facts.
         * @return Owned runtime policy, PolicyInvalid, or PolicyCapabilityUnsupported with actionable mode context.
         */
        [[nodiscard]] static Result<CookedSaveProjectPolicy> Create(SaveProjectPolicy project, const SaveRuntimeCapabilities &capabilities);

        /** @brief Reports whether a mode remains enabled after declared fallbacks. @param mode Mode to inspect. @return False for unknown
         * modes. */
        [[nodiscard]] bool IsEnabled(SavePolicyMode mode) const noexcept;
        /** @brief Reports whether capability fallback disabled an authored enabled mode. @param mode Mode to inspect. @return False
         * otherwise. */
        [[nodiscard]] bool WasDisabledByCapability(SavePolicyMode mode) const noexcept;
        /** @brief Returns the immutable cooked mode policy. @param mode Valid stable mode. @return Borrowed policy, or nullptr for an
         * unknown mode. */
        [[nodiscard]] const SaveModeProjectPolicy *Mode(SavePolicyMode mode) const noexcept;
        /** @brief Returns the number of enabled auto/checkpoint/suspend producers. @return Fixed bounded count. */
        [[nodiscard]] std::size_t BackgroundIntentModeCount() const noexcept;
        /** @brief Fast-path test used to avoid scheduling when all background modes are disabled. @return True only when background work
         * can exist. */
        [[nodiscard]] bool HasBackgroundIntentWork() const noexcept;

    private:
        CookedSaveProjectPolicy(SaveProjectPolicy project, std::uint32_t enabledMask, std::uint32_t capabilityDisabledMask,
                                std::uint32_t backgroundMask) noexcept;

        SaveProjectPolicy project_;
        std::uint32_t enabledMask_{};
        std::uint32_t capabilityDisabledMask_{};
        std::uint32_t backgroundMask_{};
    };
}  // namespace Horo::Runtime
