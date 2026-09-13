#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveProjectPolicy.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <string>

namespace Horo::Runtime {
    namespace {
        [[nodiscard]] SaveModeProjectPolicy SaveMode(const SaveRotationStrategy rotation, const std::uint16_t slots) {
            return {.enabled = true,
                    .eligibility = SaveModeEligibility::StableRuntime,
                    .rotation = {.strategy = rotation,
                                 .maximumRetainedSlots = slots,
                                 .preserveLastSuccessful = rotation == SaveRotationStrategy::ReplaceOldest},
                    .presentation = {.visibility = SavePresentationVisibility::Listed}};
        }

        [[nodiscard]] SaveModeProjectPolicy LoadMode() {
            return {.enabled = true,
                    .eligibility = SaveModeEligibility::PausedOrMenu,
                    .presentation = {.visibility = SavePresentationVisibility::PrimaryAction}};
        }

        TEST_CASE("Cooked save policy owns all six typed modes without editor state", "[unit][save][policy]") {
            SaveProjectPolicy project;
            project.modes[static_cast<std::size_t>(SavePolicyMode::Manual)] = SaveMode(SaveRotationStrategy::None, 16);
            project.modes[static_cast<std::size_t>(SavePolicyMode::Quick)] = SaveMode(SaveRotationStrategy::ReplaceSingle, 1);
            project.modes[static_cast<std::size_t>(SavePolicyMode::Auto)] = SaveMode(SaveRotationStrategy::ReplaceOldest, 4);
            project.modes[static_cast<std::size_t>(SavePolicyMode::Auto)].cooldown = {.minimumIntervalMilliseconds = 30'000,
                                                                                      .coalesceWhenBusy = true};
            project.modes[static_cast<std::size_t>(SavePolicyMode::Checkpoint)] = SaveMode(SaveRotationStrategy::ReplaceOldest, 3);
            project.modes[static_cast<std::size_t>(SavePolicyMode::Checkpoint)].cooldown.coalesceWhenBusy = true;
            project.modes[static_cast<std::size_t>(SavePolicyMode::Suspend)] = SaveMode(SaveRotationStrategy::ReplaceSingle, 1);
            project.modes[static_cast<std::size_t>(SavePolicyMode::Suspend)].eligibility = SaveModeEligibility::SuspendTransition;
            project.modes[static_cast<std::size_t>(SavePolicyMode::Suspend)].cooldown.coalesceWhenBusy = true;
            project.modes[static_cast<std::size_t>(SavePolicyMode::Load)] = LoadMode();

            auto cooked = CookedSaveProjectPolicy::Create(project, SaveRuntimeCapabilities::AllSupported());
            REQUIRE(cooked.HasValue());
            for (std::size_t index = 0; index < SaveProjectPolicy::ModeCount; ++index)
                REQUIRE(cooked.Value().IsEnabled(static_cast<SavePolicyMode>(index)));
            REQUIRE(cooked.Value().BackgroundIntentModeCount() == 3);
            REQUIRE(cooked.Value().HasBackgroundIntentWork());

            project.modes[static_cast<std::size_t>(SavePolicyMode::Manual)] = {};
            REQUIRE(cooked.Value().Mode(SavePolicyMode::Manual)->enabled);
            REQUIRE(cooked.Value().Mode(static_cast<SavePolicyMode>(255)) == nullptr);
        }

        TEST_CASE("Invalid save project policy fails with actionable mode context", "[unit][save][policy]") {
            SaveProjectPolicy project;
            project.modes[static_cast<std::size_t>(SavePolicyMode::Quick)] = SaveMode(SaveRotationStrategy::ReplaceOldest, 2);
            REQUIRE(ValidateSaveProjectPolicy(project).HasError());
            auto invalid = CookedSaveProjectPolicy::Create(project, SaveRuntimeCapabilities::AllSupported());
            REQUIRE(invalid.HasError());
            REQUIRE(invalid.ErrorValue().code.Value() == SaveErrors::PolicyInvalid.code.Value());
            REQUIRE(SaveErrors::PolicyInvalid.userActionable);
            REQUIRE(invalid.ErrorValue().message.find("quick") != std::string::npos);
            REQUIRE(invalid.ErrorValue().message.find("one replaceable logical slot") != std::string::npos);

            project = {};
            auto &disabledAuto = project.modes[static_cast<std::size_t>(SavePolicyMode::Auto)];
            disabledAuto.cooldown.minimumIntervalMilliseconds = 1;
            invalid = CookedSaveProjectPolicy::Create(project, SaveRuntimeCapabilities::AllSupported());
            REQUIRE(invalid.HasError());
            REQUIRE(invalid.ErrorValue().message.find("zero-work") != std::string::npos);

            project = {};
            project.schemaVersion = 0;
            invalid = CookedSaveProjectPolicy::Create(project, SaveRuntimeCapabilities::AllSupported());
            REQUIRE(invalid.HasError());
            REQUIRE(invalid.ErrorValue().message.find("recook") != std::string::npos);
        }

        TEST_CASE("Runtime capability fallbacks are explicit and preserved in cooked policy", "[unit][save][policy]") {
            SaveProjectPolicy project;
            auto &automatic = project.modes[static_cast<std::size_t>(SavePolicyMode::Auto)];
            automatic = SaveMode(SaveRotationStrategy::ReplaceOldest, 4);
            automatic.cooldown.minimumIntervalMilliseconds = 5'000;
            automatic.unsupportedMode = SaveUnsupportedModeFallback::DisableMode;
            auto &manual = project.modes[static_cast<std::size_t>(SavePolicyMode::Manual)];
            manual = SaveMode(SaveRotationStrategy::None, 8);
            manual.presentation.thumbnail = SaveThumbnailPolicy::Optional;
            manual.unsupportedThumbnail = SaveThumbnailFallback::Omit;

            SaveRuntimeCapabilities capabilities = SaveRuntimeCapabilities::AllSupported();
            capabilities.supportedModes[static_cast<std::size_t>(SavePolicyMode::Auto)] = false;
            capabilities.thumbnailCapture = false;
            auto cooked = CookedSaveProjectPolicy::Create(project, capabilities);
            REQUIRE(cooked.HasValue());
            REQUIRE_FALSE(cooked.Value().IsEnabled(SavePolicyMode::Auto));
            REQUIRE(cooked.Value().WasDisabledByCapability(SavePolicyMode::Auto));
            REQUIRE(cooked.Value().Mode(SavePolicyMode::Auto)->enabled == false);
            REQUIRE(cooked.Value().Mode(SavePolicyMode::Manual)->presentation.thumbnail == SaveThumbnailPolicy::Disabled);

            manual.presentation.thumbnail = SaveThumbnailPolicy::Required;
            manual.unsupportedThumbnail = SaveThumbnailFallback::RejectProject;
            auto rejected = CookedSaveProjectPolicy::Create(project, capabilities);
            REQUIRE(rejected.HasError());
            REQUIRE(rejected.ErrorValue().code.Value() == SaveErrors::PolicyCapabilityUnsupported.code.Value());
            REQUIRE(SaveErrors::PolicyCapabilityUnsupported.userActionable);
            REQUIRE(rejected.ErrorValue().message.find("manual") != std::string::npos);
        }

        TEST_CASE("All-disabled policy creates no runtime background work", "[unit][save][policy]") {
            const SaveProjectPolicy project;
            const SaveRuntimeCapabilities noCapabilities;
            auto cooked = CookedSaveProjectPolicy::Create(project, noCapabilities);
            REQUIRE(cooked.HasValue());
            REQUIRE(cooked.Value().BackgroundIntentModeCount() == 0);
            REQUIRE_FALSE(cooked.Value().HasBackgroundIntentWork());
            for (std::size_t index = 0; index < SaveProjectPolicy::ModeCount; ++index) {
                const auto mode = static_cast<SavePolicyMode>(index);
                REQUIRE_FALSE(cooked.Value().IsEnabled(mode));
                REQUIRE_FALSE(cooked.Value().WasDisabledByCapability(mode));
            }
        }

        TEST_CASE("Contradictory rotation scheduling and presentation policies fail closed", "[unit][save][policy]") {
            SaveProjectPolicy project;
            auto &automatic = project.modes[static_cast<std::size_t>(SavePolicyMode::Auto)];
            automatic = SaveMode(SaveRotationStrategy::ReplaceOldest, 4);
            REQUIRE(CookedSaveProjectPolicy::Create(project, SaveRuntimeCapabilities::AllSupported()).HasError());

            automatic.cooldown.minimumIntervalMilliseconds = 1;
            automatic.presentation.confirmation = SaveConfirmationPolicy::Always;
            REQUIRE(CookedSaveProjectPolicy::Create(project, SaveRuntimeCapabilities::AllSupported()).HasError());

            automatic.presentation.confirmation = SaveConfirmationPolicy::None;
            automatic.presentation.thumbnail = SaveThumbnailPolicy::Required;
            automatic.unsupportedThumbnail = SaveThumbnailFallback::Omit;
            REQUIRE(CookedSaveProjectPolicy::Create(project, SaveRuntimeCapabilities::AllSupported()).HasError());

            project = {};
            auto &load = project.modes[static_cast<std::size_t>(SavePolicyMode::Load)];
            load = LoadMode();
            load.rotation.maximumRetainedSlots = 1;
            REQUIRE(CookedSaveProjectPolicy::Create(project, SaveRuntimeCapabilities::AllSupported()).HasError());
        }
    }  // namespace
}  // namespace Horo::Runtime
