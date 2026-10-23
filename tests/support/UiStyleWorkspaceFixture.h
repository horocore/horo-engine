#pragma once
#include "EditorUiStylePreparation.h"
#include "Horo/Application/UiStyleWorkspace.h"
#include "Horo/Runtime/Ui/UiDocumentSerialization.h"
#include "Horo/Runtime/Ui/UiElementCatalog.h"
#include "UiInspectorDraft.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>
#include <thread>

namespace Horo::UiStyleTest {
    using namespace Runtime::Ui;
    using namespace Editor;

    /** @brief Real on-disk tracked assets and native factory/services, destroyed after their async capability drains. */
    struct Project final {
        std::filesystem::path root;
        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations{files};
        Assets::AssetRegistry assets;
        JobSystem jobs{JobSystemConfig{.workerCount = 2, .maxQueuedJobs = 32}};
        Assets::AssetId styleAsset = Assets::AssetId::Parse("13a2bc7e-d668-4def-9c4e-f2267172e846").Value();
        Assets::AssetId workspace = Assets::AssetId::Parse("02f55810-e86a-434a-9d31-b51c91967eb4").Value();
        UiStyleAssetDocument style{UiStyleContentRevision::Create(1).Value(),
                                   {.id = RuntimeStyleAssetId::Create(styleAsset.Bytes()).Value()},
                                   {},
                                   {}};
        UiInspectorResourcesFactory factory;
        std::unique_ptr<UiInspectorResources> resources;

        Project() {
            root = std::filesystem::temp_directory_path() /
                   ("horo-ui-style-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(root / "assets");
            root = std::filesystem::canonical(root);
            style.style.assignments = {{CoreUiStylePropertyId(UiCoreStyleProperty::Width),
                                        UiStyleValueSource::Literal(UiStyleDimension{640}), false},
                                       {CoreUiStylePropertyId(UiCoreStyleProperty::Fill),
                                        UiStyleValueSource::Literal(UiStyleColor{0.2F, 0.3F, 0.4F, 1}), false}};
            // A later same-layer write to a sealed property is the actual transactional rejection.
            style.style.states = {{UiVisualStateMask{UiVisualState::Disabled},
                                   {},
                                   UiStateLayer::Availability,
                                   {{CoreUiStylePropertyId(UiCoreStyleProperty::Fill),
                                     UiStyleValueSource::Literal(UiStyleColor{0.5F, 0.5F, 0.5F, 1}), true}}},
                                  {UiVisualStateMask{UiVisualState::Disabled},
                                   {},
                                   UiStateLayer::Availability,
                                   {{CoreUiStylePropertyId(UiCoreStyleProperty::Fill),
                                     UiStyleValueSource::Literal(UiStyleColor{0.8F, 0.8F, 0.8F, 1}), false}}}};
            WriteStyle();
            Rebuild();
            factory = MakeEditorUiInspectorResourcesFactory(jobs, assets, mutations, files);
            auto created = factory(root, workspace);
            REQUIRE(created.HasValue());
            resources = std::move(created).Value();
        }

        ~Project() {
            resources.reset();
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        std::filesystem::path StylePath() const {
            return root / "assets" / std::filesystem::path{u8"tema_日本 style.uistyle"};
        }

        void WriteStyle() const {
            auto bytes = SerializeUiStyleAsset(style);
            REQUIRE(bytes.HasValue());
            std::ofstream(StylePath()) << bytes.Value();
            std::ofstream(std::filesystem::path{StylePath()}.concat(".horo"))
                << nlohmann::json{{"schemaVersion", 1}, {"assetId", styleAsset.ToString()}, {"assetType", UiStyleAssetTypeName}};
        }

        void Rebuild() {
            auto rebuilt = Assets::RebuildAssetRegistry(assets, root, Assets::AssetRegistryOpenMode::ReadOnly);
            REQUIRE(rebuilt.HasValue());
            REQUIRE(rebuilt.Value().status == Assets::AssetRegistryBuildStatus::Complete);
        }

        void Prepare() {
            REQUIRE(resources->Refresh().HasValue());
            REQUIRE(resources->SelectTheme(style.style.id).HasValue());
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
            while (resources->Capture().pending && std::chrono::steady_clock::now() < deadline) {
                const auto polled = resources->Refresh();
                INFO((polled.HasError() ? polled.ErrorValue().message : ""));
                REQUIRE(polled.HasValue());
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            REQUIRE_FALSE(resources->Capture().pending);
            REQUIRE(resources->Capture().StyleGeneration().IsValid());
        }

        template <typename T> T Identity(const std::uint8_t last) const {
            auto bytes = workspace.Bytes();
            bytes.back() = last;
            return T::Create(bytes).Value();
        }

        std::filesystem::path WriteCanvas() const {
            UiDocumentBuilder builder{Identity<UiDocumentId>(11), UiDocumentRevision::Create(1).Value()};
            REQUIRE(builder
                        .AddCanvas(
                            {.id = Identity<UiCanvasId>(12), .rootElement = Identity<UiElementId>(13), .referenceResolution = {800, 600}})
                        .HasValue());
            REQUIRE(builder.AddElement(CreateUiElement(UiElementKind::Canvas, Identity<UiElementId>(13), {}).Value()).HasValue());
            const auto source = SerializeUiDocument(std::move(builder).Build().Value());
            REQUIRE(source.HasValue());
            std::ofstream(root / "Hud.uicanvas")
                << nlohmann::json{{"format", "horo.ui_canvas"}, {"formatVersion", 2}, {"document", nlohmann::json::parse(source.Value())}};
            return root / "Hud.uicanvas";
        }
    };

    inline UiInspectorContext Context(const UiCanvasDocument &document, const UiInspectorResourceSnapshot &resources) {
        return {.document = document.Identity().instance,
                .state = document.CurrentState(),
                .revision = document.Document().Revision(),
                .element = document.Selection(),
                .selectionRevision = 1,
                .styles = resources.StyleGeneration(),
                .assets = resources.assets.Revision(),
                .resourcePublication = resources.publication};
    }
}  // namespace Horo::UiStyleTest
