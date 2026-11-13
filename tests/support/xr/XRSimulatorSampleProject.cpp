#include "XRSimulatorSampleProject.h"

#include "Horo/Editor/ProjectCreationService.h"
#include "Horo/Foundation/DataBus.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Scene/SceneSource.h"
#include "Horo/Security/SecureMemory.h"

#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <thread>

namespace Horo::Tests {
    namespace {
        /** @brief Supplies stable nonzero identity entropy for the explicitly materialized sample fixture. */
        class SampleRandom final : public Security::SecureRandomSource {
        public:
            Result<void> Fill(const std::span<std::byte> destination) override {
                for (auto &byte : destination)
                    byte = std::byte{0x71};
                return Result<void>::Success();
            }
        };

        void Write(const std::filesystem::path &path, const std::string_view bytes) {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream file{path, std::ios::binary | std::ios::trunc};
            file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            file.close();
            REQUIRE_FALSE(file.fail());
        }

        Editor::ProjectCreationSnapshot Wait(const Editor::ProjectCreationService &service, const Editor::ProjectCreationOperationId id) {
            for (std::size_t attempt = 0; attempt < 2000; ++attempt) {
                auto snapshot = service.Query(id);
                REQUIRE(snapshot.has_value());
                if (snapshot->state == Editor::ProjectCreationOperationState::Succeeded ||
                    snapshot->state == Editor::ProjectCreationOperationState::Failed)
                    return *snapshot;
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
            }
            FAIL("Typed sample project creation did not reach a terminal state.");
            return {};
        }

        std::vector<SceneSource::SceneObjectSnapshot> Objects() {
            std::vector<SceneSource::SceneObjectSnapshot> objects{{.id = {1}, .name = "XR Tracking Origin"},
                                                                  {.id = {2},
                                                                   .name = "Semantic Movement Cube",
                                                                   .primitiveMesh = Runtime::PrimitiveMeshDescriptor{}},
                                                                  {.id = {3}, .name = "Sample Light"}};
            const auto type = Gameplay::BehaviorTypeId::Parse("game.xr_simulator.movement");
            REQUIRE(type.HasValue());
            objects[1].components.behaviors.push_back({.instanceId = {1}, .typeId = type.Value(), .schemaVersion = 1, .enabled = true});
            objects[2].components.light = Runtime::LightComponent{.kind = Runtime::LightKind::Directional};
            return objects;
        }
    }  // namespace

    /** @copydoc MaterializeXRSimulatorSample */
    void MaterializeXRSimulatorSample(const std::filesystem::path &root) {
        REQUIRE_FALSE(std::filesystem::exists(root));
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
        EngineDataBus bus;
        Editor::ProjectCreationService service{jobs, bus, std::make_shared<SampleRandom>(), std::make_shared<NativeDurableFileSystem>()};
        const Editor::ProjectCreationRequest request{.templateId = "3d-starter",
                                                     .projectName = "XRSimulator",
                                                     .projectRoot = root,
                                                     .projectVersion = "0.1.0",
                                                     .defaultScene = "Assets/Scenes/main.horo",
                                                     .renderBackend = "opengl",
                                                     .physicsEnabled = false,
                                                     .targetFrameRate = 60,
                                                     .buildProfile = "desktop-debug",
                                                     .assetCompression = "lz4",
                                                     .textureCompression = "bc7",
                                                     .targetPlatform = "host",
                                                     .compilerFamily = "default",
                                                     .minimumCxxStandard = 20,
                                                     .initializeGit = false,
                                                     .restorePackages = false,
                                                     .includeStarterContent = true,
                                                     .generateCMakeProject = false};
        const auto started = service.StartCreate(request);
        REQUIRE(started.HasValue());
        const auto completed = Wait(service, started.Value().id);
        INFO((completed.error ? completed.error->message : std::string{}));
        REQUIRE(completed.state == Editor::ProjectCreationOperationState::Succeeded);
        const auto objects = Objects();
        Write(root / "Assets/Scenes/main.horo", SceneSource::EncodeSceneSource({objects, {}}));
        Write(root / "Assets/Scenes/main.horo.horo",
              R"({"schemaVersion":1,"assetId":"17122007-0100-4000-8000-000000000001","assetType":"core.scene"})");
        Write(root / "Assets/Scripts/SimulatorMovement.horo_script", R"(return horo.behavior {
    type_id = "game.xr_simulator.movement",
    display_name = "Simulator Semantic Movement",
    category = "Simulator",
    schema_version = 1,
    fields = {},
    on_fixed_update = function(ctx, dt)
        local x, y, z = ctx.transform.position()
        local move_x, move_y, down = ctx.input.action("gameplay.move")
        if down then ctx.transform.set_position(x + move_x, y, z + move_y) end
    end
}
)");
        Write(root / "Assets/Scripts/SimulatorMovement.horo_script.meta",
              R"({"schemaVersion":1,"runtime":"lua","behaviorTypeId":"game.xr_simulator.movement"})");
    }
}  // namespace Horo::Tests
