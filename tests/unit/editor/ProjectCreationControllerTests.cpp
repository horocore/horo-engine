#include "Horo/Editor/ProjectCreationController.h"
#include "editor/project_model/RendererAvailability.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
    const Horo::Editor::RendererAvailabilitySnapshot &DefaultAvailability() {
        using namespace Horo::Editor;
        static const RendererAvailabilitySnapshot
            availability{{RendererBackendAvailability{"opengl", "OpenGL", RendererAvailabilityState::Active, {}}}, "opengl"};
        return availability;
    }

    class TemporaryDirectory {
    public:
        TemporaryDirectory() {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path_ = std::filesystem::temp_directory_path() / ("horo-project-creation-" + std::to_string(stamp));
            std::filesystem::create_directories(path_);
        }

        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }

        [[nodiscard]] const std::filesystem::path &Path() const {
            return path_;
        }

    private:
        std::filesystem::path path_;
    };

    void HasDiagnostic(const Horo::Editor::ProjectCreationValidation &validation, const Horo::Editor::ProjectCreationDiagnosticCode code) {
        for (const auto &diagnostic : validation.diagnostics) {
            if (diagnostic.code == code) {
                return;
            }
        }
        REQUIRE((false && "expected validation diagnostic"));
    }

    TEST_CASE("Provides Document Defaults", "[unit][editor]") {
        using namespace Horo::Editor;

        const ProjectCreationDraft draft = ProjectCreationController{DefaultAvailability()}.Draft();
        REQUIRE((draft.templateId == "3d-starter"));
        REQUIRE((draft.projectVersion == "0.1.0"));
        REQUIRE((draft.defaultScene == "Assets/Scenes/main.horo"));
        REQUIRE((draft.renderBackend == "opengl"));
        REQUIRE((draft.physicsEnabled));
        REQUIRE((draft.targetFrameRate == 60));
        REQUIRE((draft.buildProfile == "desktop-debug"));
        REQUIRE((draft.assetCompression == "lz4"));
        REQUIRE((draft.textureCompression == "bc7"));
        REQUIRE((draft.targetPlatform == "host"));
        REQUIRE((draft.compilerFamily == "default"));
        REQUIRE((draft.minimumCxxStandard == 20));
        REQUIRE((!ProjectCreationController{DefaultAvailability()}.IsDirty()));
    }

    TEST_CASE("Template Transitions Keep Starter Scene State Coherent", "[unit][editor][project-creation]") {
        using namespace Horo::Editor;

        ProjectCreationController controller{DefaultAvailability()};
        controller.SetTemplateId("empty");
        REQUIRE_FALSE((controller.Draft().includeStarterContent));
        REQUIRE((controller.Draft().defaultScene.empty()));

        controller.SetTemplateId("custom");
        REQUIRE((controller.Draft().includeStarterContent));
        REQUIRE((controller.Draft().defaultScene == "Assets/Scenes/main.horo"));

        controller.SetTemplateId("empty");
        controller.SetTemplateId("package-based");
        REQUIRE((controller.Draft().includeStarterContent));
        REQUIRE((controller.Draft().defaultScene == "Assets/Scenes/main.horo"));

        controller.SetIncludeStarterContent(false);
        REQUIRE((controller.Draft().defaultScene.empty()));
        controller.SetIncludeStarterContent(true);
        REQUIRE((controller.Draft().defaultScene == "Assets/Scenes/main.horo"));
    }

    TEST_CASE("Rejects Blank And Path Like Names", "[unit][editor]") {
        using namespace Horo::Editor;

        TemporaryDirectory temporaryDirectory;
        ProjectCreationController controller{DefaultAvailability()};
        controller.SetProjectPath((temporaryDirectory.Path() / "new-project").string());

        controller.SetProjectName("   ");
        HasDiagnostic(controller.Validate(), ProjectCreationDiagnosticCode::ProjectNameRequired);

        controller.SetProjectName("bad/name");
        HasDiagnostic(controller.Validate(), ProjectCreationDiagnosticCode::ProjectNameContainsPathSeparator);
    }

    TEST_CASE("Distinguishes Occupied Empty And Missing Destinations", "[unit][editor]") {
        using namespace Horo::Editor;

        TemporaryDirectory temporaryDirectory;
        const auto occupied = temporaryDirectory.Path() / "occupied";
        const auto empty = temporaryDirectory.Path() / "empty";
        const auto missing = temporaryDirectory.Path() / "missing";
        std::filesystem::create_directories(occupied);
        std::filesystem::create_directories(empty);
        std::ofstream{occupied / "existing.txt"} << "occupied";

        REQUIRE((InspectProjectCreationLocation(occupied).kind == ProjectCreationLocationKind::OccupiedDirectory));
        REQUIRE((InspectProjectCreationLocation(empty).kind == ProjectCreationLocationKind::EmptyDirectory));
        REQUIRE((InspectProjectCreationLocation(missing).kind == ProjectCreationLocationKind::Missing));

        ProjectCreationController controller{DefaultAvailability()};
        controller.SetProjectName("NewProject");
        controller.SetProjectPath(occupied.string());
        HasDiagnostic(controller.Validate(), ProjectCreationDiagnosticCode::ProjectPathOccupied);

        controller.SetProjectPath(empty.string());
        REQUIRE((controller.Validate().IsValid()));
        REQUIRE((controller.BuildCreationRequest().has_value()));

        controller.SetProjectPath(missing.string());
        REQUIRE((controller.Validate().IsValid()));
        REQUIRE((controller.BuildCreationRequest().has_value()));
        REQUIRE((!std::filesystem::exists(missing)));
    }

    TEST_CASE("Tracks Draft Leave Intent", "[unit][editor]") {
        using namespace Horo::Editor;

        ProjectCreationController controller{DefaultAvailability()};
        REQUIRE((controller.LeaveIntent() == ProjectCreationLeaveIntent::Allow));

        controller.SetProjectName("Changed");
        REQUIRE((controller.IsDirty()));
        REQUIRE((controller.LeaveIntent() == ProjectCreationLeaveIntent::RequireDiscardConfirmation));

        controller.DiscardDraft();
        REQUIRE((!controller.IsDirty()));
        REQUIRE((controller.LeaveIntent() == ProjectCreationLeaveIntent::Allow));
    }

    TEST_CASE("Defaults To Active Backend And Rejects Unavailable Selection", "[unit][editor]") {
        using namespace Horo::Editor;
        const RendererAvailabilitySnapshot
            availability{{
                             RendererBackendAvailability{"opengl", "OpenGL", RendererAvailabilityState::Available, {}},
                             RendererBackendAvailability{"metal", "Metal", RendererAvailabilityState::Active, {}},
                             RendererBackendAvailability{"vulkan", "Vulkan", RendererAvailabilityState::NotInstalled,
                                                         "Renderer component is not installed."},
                         },
                         "metal"};
        ProjectCreationController controller{availability};
        REQUIRE((controller.Draft().renderBackend == "metal"));

        TemporaryDirectory temporaryDirectory;
        controller.SetProjectName("BackendProject");
        controller.SetProjectPath((temporaryDirectory.Path() / "backend-project").string());
        controller.SetRenderBackend("vulkan");
        HasDiagnostic(controller.Validate(), ProjectCreationDiagnosticCode::RendererBackendUnavailable);
        REQUIRE((!controller.BuildCreationRequest().has_value()));
    }
}  // namespace
