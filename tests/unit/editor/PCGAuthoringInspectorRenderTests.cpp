#include "Horo/Editor/Localization/ILocalizationService.h"
#include "PCGAuthoringInspector.h"
#include "PCGDocumentTestSupport.h"

#include <cmath>
#include <imgui.h>
#include <limits>

namespace Horo::Editor {
    namespace {
        class LongLabels final : public ILocalizationService {
        public:
            const std::string &Get(std::string_view, std::string_view) const override {
                return value;
            }

            std::string value{"Uzun yerelleştirilmiş şema özellikleri ve belirlenimci varsayılan değerler"};
        };

        PCGDocumentSnapshot JitterSnapshot(const PCG::PCGNodeCatalogSnapshot &catalog) {
            using namespace PCGDocumentTestSupport;
            const auto &descriptor = catalog.Nodes().front();
            auto data = Graph();
            data.nodes.clear();
            data.edges.clear();
            data.exposedInputs.clear();
            PCG::PCGGraphNode node{.id = Id<PCG::NodeId>(1), .type = descriptor.type, .version = descriptor.version};
            for (std::size_t i = 0; i < descriptor.pinCount; ++i) {
                const auto &pin = descriptor.pins[i];
                node.pins.push_back({Id<PCG::PinId>(i + 1), pin.direction, pin.type, pin.cardinality, {}});
            }
            node.payload = PCG::EncodePCGPointGenerationSettings(PCG::PCGJitterGenerationSettings{Id<PCG::SpatialElementId>(1)}).Value();
            data.nodes.push_back(node);
            const std::array types{PCG::PCGNodeTypeSupport{descriptor.type, descriptor.version, descriptor.version}};
            auto graph = PCG::PCGGraphAsset::Create(data, {.supportedNodeTypes = types});
            REQUIRE(graph.HasValue());
            return PCGDocumentSnapshot{.asset = Asset(),
                                       .identity = Identity(),
                                       .session = {Asset(9), 1},
                                       .revision = 1,
                                       .catalogRevision = 1,
                                       .source = std::make_shared<const PCG::PCGGraphAsset>(std::move(graph).Value())};
        }

        void RequireFiniteVertices() {
            const auto *draw = ImGui::GetDrawData();
            REQUIRE(draw);
            CHECK(draw->TotalVtxCount > 0);
            for (int list = 0; list < draw->CmdListsCount; ++list)
                for (const auto &vertex : draw->CmdLists[list]->VtxBuffer) {
                    CHECK(std::isfinite(vertex.pos.x));
                    CHECK(std::isfinite(vertex.pos.y));
                }
        }

        /** @brief Shared drawing harness keeps each behavior assertion scenario bounded and independent. */
        struct RenderContext final {
            const PCGDocumentSnapshot &snapshot;
            const PCG::PCGNodeCatalogSnapshot &catalog;
            const Theme::Fonts &fonts;
            const ILocalizationService &labels;

            void Draw(PCGAuthoringInspectorState &state, const PCGDocumentSnapshot &current, bool canEdit, float width = 240) const {
                const std::array selected{current.source->Data().nodes.front().id};
                ImGui::NewFrame();
                ImGui::SetNextWindowPos({0, 0});
                ImGui::SetNextWindowSize({width, 800});
                ImGui::Begin("PCG inspector", nullptr, ImGuiWindowFlags_NoSavedSettings);
                CHECK_FALSE(DrawPCGAuthoringInspector(state, current, catalog, selected, canEdit, labels, fonts));
                ImGui::End();
                ImGui::Render();
            }

            void CheckBufferedNumbers(PCGAuthoringInspectorState &state, bool canEdit) const {
                state.text["node.1.grid"] = "0";
                for (int frame = 0; frame < 2; ++frame) {
                    Draw(state, snapshot, canEdit);
                    CHECK(state.invalid.contains("node.1.grid.identity"));
                    CHECK(std::get<PCG::PCGJitterGenerationSettings>(*state.draft->properties[0].settings).grid.Value() == 1);
                }
                state.text["node.1.grid"] = "2";
                state.text["graph.seed"] = "18446744073709551615";
                Draw(state, snapshot, canEdit);
                CHECK(state.invalid.empty());
                CHECK(std::get<PCG::PCGJitterGenerationSettings>(*state.draft->properties[0].settings).grid.Value() == 2);
                CHECK(state.draft->values.deterministicSeed == std::numeric_limits<std::uint64_t>::max());
            }

            void CheckReplacement(PCGAuthoringInspectorState &state, bool canEdit) const {
                auto replaced = snapshot;
                ++replaced.revision;
                state.draft->values.deterministicSeed = 42;
                Draw(state, replaced, canEdit);
                CHECK(state.draft->values.deterministicSeed == snapshot.source->Data().deterministicSeed);
            }
        };
    }  // namespace

    TEST_CASE("PCG schema inspector renders narrow localized and disabled controls in both themes", "[editor][pcg][render]") {
        using namespace PCGDocumentTestSupport;
        ImGui::CreateContext();
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {1200, 900};
        io.DeltaTime = 1.0F / 60.0F;
        auto *font = io.Fonts->AddFontDefault();
        REQUIRE(io.Fonts->Build());
        const Theme::Fonts fonts{.sans = font, .sansCompact = font, .sansEmphasis = font, .icon = font};
        const std::array grants{PCG::PCGCapability::Validation};
        auto owner = PCG::PCGNodeCatalog::Create({PCG::PCGHostProfile::Interactive, PCG::PCGCapabilitySet::Create(grants).Value()}).Value();
        REQUIRE(owner.Register(PCG::PCGCpuNodeKind::GridJitter).HasValue());
        const auto catalog = owner.Snapshot().Value();
        const auto snapshot = JitterSnapshot(catalog);
        const auto &data = snapshot.source->Data();
        LongLabels labels;
        const RenderContext context{snapshot, catalog, fonts, labels};
        for (const auto preset : {Theme::Preset::HoroDark, Theme::Preset::Light}) {
            Theme::SetThemePreset(preset);
            Theme::ApplyCurrentTheme();
            for (const float width : {240.0F, 900.0F})
                for (const bool canEdit : {true, false}) {
                    PCGAuthoringInspectorState state;
                    for (int frame = 0; frame < 2; ++frame)
                        context.Draw(state, snapshot, canEdit, width);
                    REQUIRE(state.draft);
                    CHECK(state.draft->values == data);
                    RequireFiniteVertices();
                    CHECK(snapshot.source->Data() == data);
                    context.CheckBufferedNumbers(state, canEdit);
                    context.CheckReplacement(state, canEdit);
                }
        }
        Theme::SetThemePreset(Theme::Preset::HoroDark);
        ImGui::DestroyContext();
    }
}  // namespace Horo::Editor
