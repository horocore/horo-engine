#include "editor/screens/workspace/panels/viewport/navigation/NavigationOverlayLayer.h"
#include "editor_ui/HeadlessEditorGuiFixture.h"
#include "navigation/NavigationSceneSourceFixture.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Editor {
    namespace {
        namespace N = Navigation;
        using Horo::Tests::NavigationSources::Asset;
        using Horo::Tests::NavigationSources::Id;

        /** @brief Copies real authored surface, modifier and direction evidence; input storage ends at return. */
        std::shared_ptr<const NavigationViewportSnapshot> Authored(const std::size_t profiles = 1) {
            auto object = Horo::Tests::NavigationSources::Object();
            auto &surface = *object.components.navigationSurface;
            surface.bakeScope = Runtime::NavigationBakeScope::LocalBounds;
            surface.localBounds = Runtime::NavigationLocalBounds{.center = {3, 0, 3}, .halfExtents = {3, 1, 3}};
            surface.profiles.clear();
            for (std::size_t i = 1; i <= profiles; ++i)
                surface.profiles.push_back(Id<N::NavigationAgentProfileId>(i));
            object.components.navigationModifier =
                Runtime::NavigationModifierComponent{.id = Id<N::NavigationModifierId>(9),
                                                     .surface = surface.id,
                                                     .volume = Runtime::NavigationCylinderVolume{.center = {3, 0, 3}}};
            object.components.navigationLink =
                Runtime::NavigationLinkComponent{.id = Id<N::NavigationLinkId>(10),
                                                 .start = {.surface = surface.id, .localPosition = {2, 0, 2}},
                                                 .end = {.surface = surface.id, .localPosition = {3, 0, 2}},
                                                 .direction = Runtime::NavigationLinkDirection::Bidirectional,
                                                 .profiles = {surface.profiles.front()}};
            const std::array scenes{Application::NavigationSceneBakeSource{.asset = Asset(2),
                                                                           .path = ProjectPath::Parse("Assets/overlay scene.horo").Value(),
                                                                           .document = {.objects = {object}}}};
            auto snapshot = NavigationViewportSnapshot::Capture(scenes, Asset(1));
            REQUIRE(snapshot.HasValue());
            return std::move(snapshot).Value();
        }

        /** @brief Uses the production neutral tile validator, with no provider or renderer-native memory. */
        std::shared_ptr<const NavigationViewportSnapshot> Cooked(const NavigationViewportSnapshot &authored) {
            Sha256Digest digest{};
            digest.bytes.fill(1);
            N::NavigationPreparedTile input{.tile = {.key = {.profile = Id<N::NavigationAgentProfileId>(1),
                                                             .surface = Id<N::SurfaceId>(10),
                                                             .scope = authored.Scenes().front().scope},
                                                     .bounds = {{0, -1, 0}, {6, 1, 6}},
                                                     .tileSizeMeters = 6},
                                            .borderSizeCells = 5,
                                            .dependencyKey = digest};
            N::NavigationTileBuildResult topology{.state = N::NavigationTileBuildState::Built,
                                                  .bounds = input.tile.bounds,
                                                  .vertices = {{0, 0, 0}, {6, 0, 0}, {0, 0, 6}},
                                                  .polygons = {{.vertexIndices = {0, 3}, .area = Id<N::NavigationAreaId>(1)}},
                                                  .polygonVertexIndices = {0, 1, 2},
                                                  .offMeshLinks = {{.start = {1, 0, 1},
                                                                    .end = {2, 0, 2},
                                                                    .radiusMeters = 0.25F,
                                                                    .area = Id<N::NavigationAreaId>(1),
                                                                    .bidirectional = true}},
                                                  .provenance = {{.producer = Id<N::NavigationSourceProducerId>(1),
                                                                  .contribution = Id<N::NavigationSourceContributionId>(1),
                                                                  .revision = Id<N::NavigationSourceRevision>(1),
                                                                  .sourceDigest = digest,
                                                                  .polygons = {0, 1}}}};
            auto tile = N::NavigationCookedTile::Create(input, std::move(topology));
            REQUIRE(tile.HasValue());
            auto snapshot = NavigationViewportSnapshot::WithCooked(authored, {.inputFingerprint = digest, .tiles = {tile.Value()}});
            REQUIRE(snapshot.HasValue());
            return std::move(snapshot).Value();
        }

        /** @brief Measures actual CPU draw output; AA line tessellation is fixed so a missing draw cap is observable. */
        int Frame(Tests::EditorGuiContextFixture &gui, NavigationOverlayLayer &layer, const NavigationViewportAccess &access,
                  const std::filesystem::path &scene, const Math::ClipDepthRange depth) {
            const std::filesystem::path root{"overlay project"};
            const EditorViewportCamera camera{.position = {3, 6, 9}, .target = {3, 0, 3}};
            gui.imgui.BeginFrame();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({640, 480});
            ImGui::Begin("NavigationOverlayRender", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
            ImGui::GetWindowDrawList()->Flags &= ~ImDrawListFlags_AntiAliasedLines;
            layer.Draw(access, {.projectRoot = root,
                                .scenePath = scene,
                                .camera = camera,
                                .origin = {0, 0},
                                .size = {640, 480},
                                .depthRange = depth,
                                .gui = gui.context});
            ImGui::End();
            gui.imgui.EndFrame();
            return ImGui::GetDrawData()->TotalVtxCount;
        }
    }  // namespace

    TEST_CASE("Navigation viewport renders consumed geometry and suppresses hidden and foreign-Scene queries",
              "[unit][editor][navigation][render]") {
        Tests::EditorGuiContextFixture gui{{640, 480}};
        std::size_t queries{};
        const auto snapshot = Cooked(*Authored());
        NavigationViewportAccess access{.query = [&](const std::filesystem::path &) {
            ++queries;
            return snapshot;
        }};
        NavigationOverlayLayer hidden;
        NavigationOverlayLayer visible{true};
        const std::filesystem::path scene{"Assets/overlay scene.horo"};
        for (const auto depth : {Math::ClipDepthRange::NegativeOneToOne, Math::ClipDepthRange::ZeroToOne}) {
            queries = 0;
            static_cast<void>(Frame(gui, hidden, access, scene, depth));
            const int baseline = Frame(gui, hidden, access, scene, depth);
            CHECK(queries == 0);
            CHECK(Frame(gui, visible, access, scene, depth) > baseline);
            CHECK(queries == 1);
            CHECK(Frame(gui, visible, access, "Assets/other.horo", depth) == baseline);
            CHECK(queries == 2);
        }
    }

    TEST_CASE("Navigation viewport bounds actual large geometry submission independently from capture",
              "[unit][editor][navigation][render][budget]") {
        Tests::EditorGuiContextFixture gui{{640, 480}};
        const auto snapshot = Authored(64);
        REQUIRE(snapshot->Scenes().front().primitives.size() > 1024);
        NavigationViewportAccess access{.query = [&](const std::filesystem::path &) {
            return snapshot;
        }};
        NavigationOverlayLayer hidden;
        NavigationOverlayLayer visible{true};
        const auto &scene = snapshot->Scenes().front().path;
        static_cast<void>(Frame(gui, hidden, access, scene, Math::ClipDepthRange::ZeroToOne));
        const int baseline = Frame(gui, hidden, access, scene, Math::ClipDepthRange::ZeroToOne);
        const int rendered = Frame(gui, visible, access, scene, Math::ClipDepthRange::ZeroToOne);
        CHECK(rendered > baseline);
        CHECK(rendered <= baseline + 1024 * 4);
    }

    TEST_CASE("Navigation overlay options prepare once and render the snapshot's shared filter controls",
              "[unit][editor][navigation][render]") {
        Tests::EditorGuiContextFixture gui{{640, 480}};
        const auto snapshot = Authored();
        std::size_t preparations{};
        NavigationViewportAccess access{.prepare = [&](const std::filesystem::path &) {
            ++preparations;
            return Result<void>::Success();
        }, .query = [&](const std::filesystem::path &) {
            return snapshot;
        }};
        NavigationOverlayLayer visible{true};
        const auto &scene = snapshot->Scenes().front().path;
        const auto draw = [&] {
            return Frame(gui, visible, access, scene, Math::ClipDepthRange::ZeroToOne);
        };
        static_cast<void>(draw());
        ImGui::GetIO().AddMousePosEvent(40, 120);
        static_cast<void>(draw());
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        static_cast<void>(draw());
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        static_cast<void>(draw());
        CHECK(preparations == 1);
        static_cast<void>(draw());
        CHECK(preparations == 1);
    }
}  // namespace Horo::Editor
