#include "../../helpers/editor_ui/HeadlessEditorGuiFixture.h"
#include "SceneDocumentTestSupport.h"
#include "editor/renderer/EditorGuiRenderer.h"
#include "editor/screens/workspace/ContentBrowserModel.h"
#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserCards.h"
#include "editor/screens/workspace/prefab/PrefabAssetPreview.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace Horo::Editor {
    namespace {
        /** @brief Consumes actual produced RGBA bytes while recording selected-renderer resource ownership. */
        class CardBridge final : public IEditorGuiRenderer {
        public:
            Result<void> Initialize() override {
                return Result<void>::Success();
            }

            Result<void> BeginFrame() override {
                return Result<void>::Success();
            }

            Result<void> RenderDrawData() override {
                return Result<void>::Success();
            }

            Result<std::uintptr_t> CreateTexture(const EditorRgba8ImageView &image) override {
                REQUIRE(image.IsValid());
                REQUIRE(std::ranges::any_of(image.pixels, [](auto byte) {
                    return byte != 0;
                }));
                return Result<std::uintptr_t>::Success(++uploads);
            }

            void DestroyTexture(std::uintptr_t) noexcept override {
                ++destroyed;
            }

            void Shutdown() noexcept override {}

            std::uintptr_t uploads{};
            std::size_t destroyed{};
        };

        /** @brief Exercises canonical source resolution and rasterization before the actual card consumer. */
        Assets::AssetPreviewImage Thumbnail() {
            using namespace SceneDocumentTestSupport;
            const Prefab::RawComponentPayload
                box{.instance = Prefab::PrefabComponentInstanceId::Create(1).Value(),
                    .component = {.typeId = Gameplay::ComponentTypeId::Parse("game.horo.primitive_mesh").Value(),
                                  .schemaVersion = 1,
                                  .encoding = Gameplay::ComponentPayloadEncoding::CanonicalJson,
                                  .payload = Bytes(R"({"id":"primitive.mesh.box","version":1,"parameters":{"size":[1,2,3]}})")}};
            const auto asset = PrefabAsset().Asset();
            auto resolver = ScenePrefabResolver(asset, {{.localId = {}, .name = "Box", .components = {box}}});
            auto candidate = resolver.Resolve(asset, Prefab::PrefabInstanceId::Create(1).Value(), ScenePrefabLimits());
            REQUIRE(candidate.HasValue());
            auto image = RenderPrefabAssetPreview(candidate.Value(), 128, 128);
            REQUIRE(image.HasValue());
            return std::move(image).Value();
        }
    }  // namespace

    TEST_CASE("Prefab card consumer uploads canonical thumbnails once and retires textures across presentation states",
              "[editor][prefab][assets][render]") {
        Tests::EditorGuiContextFixture fixture;
        CardBridge bridge;
        AssetBrowserCardRenderer cards;
        cards.Attach(&bridge);
        ContentBrowserEntry entry{.absolutePath = "/project/assets/prefabs/box.prefab",
                                  .displayName = "Uzun yerelleştirilmiş prefab adı with persistent source identity",
                                  .assetType = "core.prefab",
                                  .registered = true,
                                  .registeredType = Assets::AssetTypeId::Parse("core.prefab").Value(),
                                  .previewState = ContentBrowserPreviewState::Ready,
                                  .previewImage = Thumbnail()};
        for (const auto preset : {Theme::Preset::HoroDark, Theme::Preset::Light}) {
            Theme::SetThemePreset(preset);
            for (const float width : {128.0F, 280.0F})
                for (int state = 0; state < 5; ++state) {
                    fixture.imgui.BeginFrame();
                    ImGui::SetNextWindowSize({width + 30, 250});
                    ImGui::Begin("Prefab thumbnail consumer", nullptr, ImGuiWindowFlags_NoSavedSettings);
                    const ImVec2 origin = ImGui::GetCursorScreenPos();
                    cards.Draw({.drawList = ImGui::GetWindowDrawList(),
                                .font = ImGui::GetFont(),
                                .iconFont = ImGui::GetFont(),
                                .fontSize = Theme::TextPx::Label(),
                                .cardMin = origin,
                                .cardWidth = width,
                                .cardHeight = 180,
                                .previewWidth = width,
                                .previewHeight = 128,
                                .secondaryText = "Prefab — Geçiş gerekli",
                                .listView = state == 4,
                                .hovered = state == 1,
                                .selected = state == 2,
                                .dimmed = state == 3},
                               entry);
                    ImGui::Dummy({width, 180});
                    ImGui::End();
                    fixture.imgui.EndFrame();
                    const auto *draw = ImGui::GetDrawData();
                    REQUIRE(draw != nullptr);
                    REQUIRE(draw->TotalVtxCount > 0);
                    for (int list = 0; list < draw->CmdListsCount; ++list)
                        for (const auto &vertex : draw->CmdLists[list]->VtxBuffer)
                            CHECK((std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y)));
                }
        }
        CHECK(bridge.uploads == 1);
        cards.RetainVisible({}, {});
        CHECK(bridge.destroyed == 1);
        cards.Detach();
        cards.Detach();
        CHECK(bridge.destroyed == 1);
        Theme::SetThemePreset(Theme::Preset::HoroDark);
    }
}  // namespace Horo::Editor
