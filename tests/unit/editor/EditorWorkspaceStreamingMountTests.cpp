#include "../runtime/world_streaming/WorldArtifactCodecTestSupport.h"
#include "Horo/Platform/NativeProjectReadFiles.h"
#include "OwnedTestDirectory.h"
#include "WorldCompositionSourceTestSupport.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <catch2/catch_test_macros.hpp>
#include <fstream>

namespace Horo::Editor {
    namespace {
        namespace WS = WorldStreaming;

        struct NativeProject {
            Horo::Tests::OwnedTestDirectory directory{"streaming preview ü space"};
            WS::ArtifactCodecTestSupport::Fixture artifact;
            WorldStreamingPreviewRuntimePolicy policy = MakeWorldStreamingPreviewRuntimePolicy().Value();
            WorldStreamingPreviewHost bridge;
            Runtime::RuntimeSceneService scene;
            std::unique_ptr<EditorWorkspaceController> controller;

            NativeProject() {
                const auto owner = WS::TestSupport::Asset(9);
                {
                    std::ofstream file{directory.Path() / "source.horoworld"};
                    file << WorldCompositionTests::WorldSource();
                }
                const auto encoded = artifact.Encode();
                {
                    std::ofstream file{directory.Path() / "world.index", std::ios::binary};
                    file.write(reinterpret_cast<const char *>(encoded.bytes.data()), static_cast<std::streamsize>(encoded.bytes.size()));
                }
                const auto files = Platform::CreateNativeProjectReadFiles(directory.Path(), owner.ToString(), 1).Value();
                controller = std::make_unique<EditorWorkspaceController>(directory.Path(), scene, Assets::AssetRegistrySnapshot{},
                                                                         EditorWorkspaceDependencies{.world = {.streamingPolicy = &policy,
                                                                                                               .streamingPreview = &bridge,
                                                                                                               .files = files},
                                                                                                     .documentSessionOwner = owner});
                REQUIRE(controller->OpenWorldCompositionSource("source.horoworld").HasValue());
                const auto root = controller->WorldDocuments().Snapshot().Value();
                PartitionedWorldSource source{.shape = WS::StreamingSourceBox{Math::WorldCoordinate64::FromMillimeters(-1000, 0, 0),
                                                                              Math::WorldCoordinate64::FromMillimeters(999, 999, 999)},
                                              .target = WS::StreamingDesiredResidency::Activated,
                                              .priority = WS::StreamingSourcePriority::Create(1).Value()};
                EditorWorkspaceViewCommandData edit;
                edit.command = EditorWorkspaceViewCommand::WorldSourceTool;
                edit.worldSource = WorldSourceToolCommand{.action = WorldSourceToolAction::Create,
                                                          .edit = {.expected = root->fence, .value = source},
                                                          .globalOrigin = {},
                                                          .viewRevision = controller->ViewModel().worldViewport.viewRevision};
                controller->ProcessCommand(edit);
            }

            void Mount() {
                const auto fence = controller->ViewModel().worldViewport.snapshot->fence;
                REQUIRE(controller->MountWorldStreamingPreview("world.index", fence).HasValue());
                for (unsigned frame = 0; frame < 256 && controller->ViewModel().worldStreamingPreparing; ++frame)
                    controller->RefreshWorldDocumentProjection();
                REQUIRE_FALSE(controller->ViewModel().worldStreamingPreparing);
                REQUIRE_FALSE(controller->ViewModel().worldStreamingMountError);
                REQUIRE(controller->ViewModel().worldStreamingPreview);
            }
        };

        TEST_CASE("Native project source-to-index mount reaches the registered viewport projection without fake residency",
                  "[editor][streaming_preview][native_mount]") {
            NativeProject project;
            project.Mount();
            const auto geometry = project.controller->ViewModel().worldStreamingPreview;
            REQUIRE(geometry->boxes.size() == 2);
            CHECK(geometry->preview->Cells().front().desired->residency == WS::StreamingDesiredResidency::Activated);
            CHECK(geometry->preview->Cells().front().attempts.empty());
            CHECK(project.controller->ViewModel().worldStreamingIndexPath == "world.index");
            project.controller->StopWorldStreamingPreview();
            project.controller->RefreshWorldDocumentProjection();
            CHECK_FALSE(project.controller->ViewModel().worldStreamingPreview);
            CHECK(geometry->preview->Cells().size() == 2);
        }

        TEST_CASE("Native mount replacement unbinds the old epoch and failed index capture preserves the current mount",
                  "[editor][streaming_preview][native_mount]") {
            NativeProject project;
            project.Mount();
            const auto first = project.controller->ViewModel().worldStreamingPreview;
            const auto originalOwner = first->preview->Fence().registry.owner;
            const auto fence = project.controller->ViewModel().worldViewport.snapshot->fence;
            CHECK(project.controller->MountWorldStreamingPreview("../world.index", fence).HasError());
            CHECK(project.controller->ViewModel().worldStreamingPreview == first);
            {
                std::ofstream corrupt{project.directory.Path() / "bad.index", std::ios::binary};
                corrupt << "bad index";
            }
            CHECK(project.controller->MountWorldStreamingPreview("bad.index", fence).HasError());
            CHECK(project.controller->ViewModel().worldStreamingPreview == first);
            const auto unsupported =
                WS::EncodeWorldIndexArtifact(project.artifact.manifest, "World", WS::WorldIndexCoordinateSystem::RightHandedZUp, {},
                                             project.policy.artifact);
            REQUIRE(unsupported.HasValue());
            {
                std::ofstream file{project.directory.Path() / "z-up.index", std::ios::binary};
                file.write(reinterpret_cast<const char *>(unsupported.Value().bytes.data()),
                           static_cast<std::streamsize>(unsupported.Value().bytes.size()));
            }
            const auto rejected = project.controller->MountWorldStreamingPreview("z-up.index", fence);
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == WorldDocumentErrors::Unsupported.code.Value());
            CHECK(project.controller->ViewModel().worldStreamingPreview == first);
            project.Mount();
            const auto second = project.controller->ViewModel().worldStreamingPreview;
            CHECK(second->preview->Fence().registry.owner.epoch.Value() > originalOwner.epoch.Value());
            CHECK(project.bridge.UnbindRuntime(originalOwner).HasError());
            project.controller->DetachWorldSourceOwner();
            CHECK_FALSE(project.controller->ViewModel().worldStreamingPreview);
            CHECK(project.bridge.Refresh({project.controller->WorldDocuments().Snapshot().Value(), {}, 1}).HasError());
        }
    }  // namespace
}  // namespace Horo::Editor
