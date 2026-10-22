#include "Horo/Editor/Localization/ILocalizationService.h"
#include "PCGAuthoringInspector.h"
#include "PCGDocumentTestSupport.h"
#include "PCGWorkspace.h"

#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <limits>
#include <map>
#include <memory>

namespace Horo::Editor {
    namespace {
        class LongLabels final : public ILocalizationService {
        public:
            const std::string &Get(std::string_view, std::string_view key) const override {
                return labels.try_emplace(std::string{key}, value + " " + std::string{key}).first->second;
            }

            std::string value{"Uzun yerelleştirilmiş şema özellikleri ve belirlenimci varsayılan değerler"};
            mutable std::map<std::string, std::string> labels;
        };

        /** @brief Owns the real headless ImGui context even if a regression assertion aborts a scenario. */
        struct GuiFixture final {
            std::unique_ptr<ImGuiContext, void (*)(ImGuiContext *)> context{ImGui::CreateContext(), &ImGui::DestroyContext};
            Theme::Fonts fonts;

            GuiFixture() {
                auto &io = ImGui::GetIO();
                io.IniFilename = nullptr;
                io.DisplaySize = {1200, 900};
                io.DeltaTime = 1.0F / 60.0F;
                auto *font = io.Fonts->AddFontDefault();
                REQUIRE(io.Fonts->Build());
                fonts = {.sans = font, .sansCompact = font, .sansEmphasis = font, .icon = font};
            }

            ~GuiFixture() {
                Theme::SetThemePreset(Theme::Preset::HoroDark);
            }
        };

        /** @brief Captures a real jitter schema for repeated numeric-buffer and replacement assertions. */
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

        /** @brief Rejects non-finite control/canvas geometry in real generated ImGui draw data. */
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

        /** @brief Adds every exact exposed-value alternative to one unavailable but losslessly retained node. */
        void AddExposedDefaults(PCG::PCGGraphSourceData &data, std::uint64_t &nextPin) {
            using namespace PCGDocumentTestSupport;
            const std::array<PCG::PCGGraphValue, 7> values{true,
                                                           std::numeric_limits<std::int64_t>::min(),
                                                           std::numeric_limits<std::uint64_t>::max(),
                                                           0.25,
                                                           Math::Vec2{0.25F, 0.5F},
                                                           Math::Vec3{0.25F, 0.5F, 0.75F},
                                                           Math::Vec4{0.25F, 0.5F, 0.75F, 1.0F}};
            constexpr std::array types{PCG::PCGPinType::Boolean, PCG::PCGPinType::SignedInteger, PCG::PCGPinType::UnsignedInteger,
                                       PCG::PCGPinType::Scalar,  PCG::PCGPinType::Vector2,       PCG::PCGPinType::Vector3,
                                       PCG::PCGPinType::Vector4};
            PCG::PCGGraphNode node{.id = Id<PCG::NodeId>(data.nodes.size() + 1),
                                   .type = Id<PCG::NodeTypeId>(1),
                                   .version = {1, 0},
                                   .payload = {1, 2}};
            for (std::size_t index = 0; index < values.size(); ++index) {
                const auto pin = Id<PCG::PinId>(++nextPin);
                node.pins.push_back({pin, PCG::PCGPinDirection::Input, types[index], PCG::PCGPinCardinality::Single, values[index]});
                data.exposedInputs.push_back(
                    {Id<PCG::ExposedInputId>(index + 1), "graph.value_" + std::to_string(index + 1), node.id, pin, values[index]});
            }
            data.nodes.push_back(std::move(node));
        }

        /** @brief Uses production creation recipes and the canonical source validator for every built-in. */
        PCGDocumentSnapshot FullSchemaSnapshot(const PCGEditorCatalog &catalog) {
            using namespace PCGDocumentTestSupport;
            auto data = Graph();
            data.nodes.clear();
            data.edges.clear();
            data.exposedInputs.clear();
            std::uint64_t nextPin{};
            for (const auto &entry : catalog.palette) {
                auto node = entry.prototype;
                node.id = Id<PCG::NodeId>(data.nodes.size() + 1);
                for (auto &pin : node.pins)
                    pin.id = Id<PCG::PinId>(++nextPin);
                data.nodes.push_back(std::move(node));
            }
            AddExposedDefaults(data, nextPin);
            auto graph = PCG::PCGGraphAsset::Create(std::move(data), {.unknownNodePolicy = PCG::PCGUnknownNodePolicy::PreserveInert,
                                                                      .supportedNodeTypes = catalog.document.types});
            REQUIRE(graph.HasValue());
            return {.asset = Asset(),
                    .identity = Identity(),
                    .session = {Asset(9), 1},
                    .revision = 1,
                    .catalogRevision = catalog.document.revision,
                    .source = std::make_shared<const PCG::PCGGraphAsset>(std::move(graph).Value())};
        }

        /** @brief Renders the actual graph surface with the same schema and inspector arguments as its registered presenter. */
        void DrawFullSchemaFrame(const PCGWorkspaceDocument &document, const PCGEditorCatalog &catalog, PCGAuthoringInspectorState &state,
                                 const ILocalizationService &labels, const Theme::Fonts &fonts, float width) {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({width, 800});
            ImGui::Begin("All PCG schemas", nullptr, ImGuiWindowFlags_NoSavedSettings);
            CHECK_FALSE(DrawPCGGraphEditor(document, catalog.palette, labels, fonts, &catalog.schemas, &state));
            ImGui::End();
            ImGui::Render();
        }

        /** @brief Records shared-control positions while leaving real button hit testing and state transitions intact. */
        class ActionLabels final : public ILocalizationService {
        public:
            const std::string &Get(std::string_view, std::string_view key) const override {
                positions[std::string{key}] = ImGui::GetCursorScreenPos();
                return labels.try_emplace(std::string{key}, key.substr(key.rfind('.') + 1)).first->second;
            }

            mutable std::map<std::string, std::string, std::less<>> labels;
            mutable std::map<std::string, ImVec2, std::less<>> positions;
        };

        /** @brief Drives the real inspector through mouse press/release frames without changing source authority. */
        struct InteractionContext final {
            const PCGDocumentSnapshot &snapshot;
            const PCG::PCGNodeCatalogSnapshot &catalog;
            const Theme::Fonts &fonts;
            ActionLabels labels;
            PCGAuthoringInspectorState state;

            std::optional<PCGDocumentCommand> Frame() {
                std::array<PCG::NodeId, 1> selected{};
                if (snapshot.source && !snapshot.source->Data().nodes.empty())
                    selected[0] = snapshot.source->Data().nodes.front().id;
                ImGui::NewFrame();
                ImGui::SetNextWindowPos({0, 0});
                ImGui::SetNextWindowSize({1000, 1600});
                ImGui::Begin("PCG interaction", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
                auto command = DrawPCGAuthoringInspector(state, snapshot, catalog, selected, true, labels, fonts);
                ImGui::End();
                ImGui::Render();
                return command;
            }

            std::optional<PCGDocumentCommand> Click(std::string_view action) {
                CHECK_FALSE(Frame());
                CHECK_FALSE(Frame());
                const auto point = labels.positions.at("workspace.pcg.authoring." + std::string{action});
                auto &io = ImGui::GetIO();
                REQUIRE(point.y + 8 < io.DisplaySize.y);
                io.AddMousePosEvent(point.x + 8, point.y + 8);
                CHECK_FALSE(Frame());
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                auto pressed = Frame();
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                auto released = Frame();
                return pressed ? std::move(pressed) : std::move(released);
            }
        };

        /** @brief Starts with the production density-filter recipe that owns a typed scalar input pin. */
        PCGDocumentSnapshot ScalarInputSnapshot(const PCGEditorCatalog &catalog) {
            auto snapshot = FullSchemaSnapshot(catalog);
            auto data = snapshot.source->Data();
            const auto type = PCG::PCGCpuNodeType(PCG::PCGCpuNodeKind::DensityFilter).Value();
            std::erase_if(data.nodes, [type](const auto &node) {
                return node.type != type;
            });
            data.exposedInputs.clear();
            auto graph = PCG::PCGGraphAsset::Create(std::move(data), {.supportedNodeTypes = catalog.document.types});
            REQUIRE(graph.HasValue());
            snapshot.source = std::make_shared<const PCG::PCGGraphAsset>(std::move(graph).Value());
            return snapshot;
        }

        /** @brief Finds the editable scalar input without depending on descriptor ordering. */
        const PCG::PCGGraphPin &ScalarPin(const PCGGraphAuthoringDraft &draft) {
            const auto &pins = draft.values.nodes.front().pins;
            const auto found = std::ranges::find(pins, PCG::PCGPinType::Scalar, &PCG::PCGGraphPin::type);
            REQUIRE(found != pins.end());
            return *found;
        }

        /** @brief Confirms the rendered form owns every closed settings alternative without repairing unsupported source. */
        void CheckFullSchemaDraft(const PCGAuthoringInspectorState &state, const PCGDocumentSnapshot &snapshot) {
            REQUIRE(state.draft);
            CHECK(state.invalid.empty());
            CHECK(state.draft->values == snapshot.source->Data());
            std::array<bool, std::variant_size_v<PCG::PCGNodeAuthoringSettings>> alternatives{};
            std::size_t supported{}, unsupported{};
            for (const auto &property : state.draft->properties) {
                if (property.settings) {
                    alternatives[property.settings->index()] = true;
                    ++supported;
                } else {
                    CHECK(property.diagnostic.has_value());
                    ++unsupported;
                }
            }
            CHECK(supported == 17);
            CHECK(unsupported == 1);
            CHECK(std::ranges::all_of(alternatives, [](bool covered) {
                return covered;
            }));
            CHECK(state.text.at("input.2.default") == "-9223372036854775808");
            CHECK(state.text.at("input.3.default") == "18446744073709551615");
        }
    }  // namespace

    TEST_CASE("PCG schema inspector renders narrow localized and disabled controls in both themes", "[editor][pcg][render]") {
        using namespace PCGDocumentTestSupport;
        GuiFixture gui;
        const auto &fonts = gui.fonts;
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
    }

    TEST_CASE("PCG inspector explicit actions coalesce defaults bindings and seed without mutating source", "[editor][pcg][render]") {
        GuiFixture gui;
        ImGui::GetIO().DisplaySize = {1200, 1800};
        const auto created = CreatePCGEditorCatalog();
        REQUIRE(created.HasValue());
        const auto &catalog = created.Value();
        const auto snapshot = ScalarInputSnapshot(catalog);
        InteractionContext context{snapshot, catalog.schemas, gui.fonts};
        CHECK_FALSE(context.Click("add_default"));
        REQUIRE(context.state.draft);
        REQUIRE(ScalarPin(*context.state.draft).defaultValue);
        CHECK(std::get<double>(*ScalarPin(*context.state.draft).defaultValue) == 0.0);
        CHECK_FALSE(context.Click("expose_input"));
        REQUIRE(context.state.draft->values.exposedInputs.size() == 1);
        const auto &input = context.state.draft->values.exposedInputs.front();
        CHECK(input.pin == ScalarPin(*context.state.draft).id);
        CHECK(input.defaultValue == *ScalarPin(*context.state.draft).defaultValue);
        context.state.text["graph.seed"] = "18446744073709551615";
        const auto command = context.Click("apply");
        REQUIRE(command);
        CHECK(command->operations.size() == 3);
        CHECK(context.state.draft->values.deterministicSeed == std::numeric_limits<std::uint64_t>::max());
        CHECK(snapshot.source->Data().exposedInputs.empty());
        CHECK(std::ranges::none_of(snapshot.source->Data().nodes.front().pins, [](const auto &pin) {
            return pin.defaultValue.has_value();
        }));
        CHECK_FALSE(snapshot.dirty);
    }

    TEST_CASE("PCG inspector removes pending buffers rejects no-op apply and cancels a rejected gesture", "[editor][pcg][render]") {
        GuiFixture gui;
        ImGui::GetIO().DisplaySize = {1200, 1800};
        const auto created = CreatePCGEditorCatalog();
        REQUIRE(created.HasValue());
        const auto &catalog = created.Value();
        const auto snapshot = ScalarInputSnapshot(catalog);
        InteractionContext context{snapshot, catalog.schemas, gui.fonts};
        CHECK_FALSE(context.Click("add_default"));
        CHECK_FALSE(context.Click("expose_input"));
        CHECK_FALSE(context.Frame());
        context.state.text["input.1.default"] = "invalid";
        CHECK_FALSE(context.Frame());
        CHECK(context.state.invalid.contains("input.1.default"));
        CHECK_FALSE(context.Click("remove_input"));
        REQUIRE(context.state.draft);
        CHECK(context.state.draft->values.exposedInputs.empty());
        CHECK_FALSE(context.state.text.contains("input.1.default"));
        CHECK(context.state.invalid.empty());
        CHECK_FALSE(context.Click("remove_default"));
        CHECK_FALSE(ScalarPin(*context.state.draft).defaultValue);
        CHECK(context.state.draft->values == snapshot.source->Data());
        CHECK_FALSE(context.Click("apply"));
        REQUIRE(context.state.error);
        CHECK_FALSE(context.Frame());
        CHECK_FALSE(context.Click("cancel"));
        CHECK_FALSE(context.state.draft);
        CHECK_FALSE(context.state.error);
        CHECK(snapshot.source->Data().exposedInputs.empty());
    }

    TEST_CASE("PCG inspector missing source reports capture failure without allocating a gesture", "[editor][pcg][render]") {
        GuiFixture gui;
        const auto created = CreatePCGEditorCatalog();
        REQUIRE(created.HasValue());
        const PCGDocumentSnapshot missing;
        InteractionContext context{missing, created.Value().schemas, gui.fonts};
        CHECK_FALSE(context.Frame());
        CHECK_FALSE(context.state.draft);
        REQUIRE(context.state.error);
        CHECK(context.state.error->code.Value() == PCGDocumentError(PCGDocumentCommandFailureReason::Invalid).code.Value());
        RequireFiniteVertices();
    }

    TEST_CASE("PCG graph surface renders all catalog schemas and exposed value types without source mutation",
              "[editor][pcg][render][schema]") {
        GuiFixture gui;
        LongLabels labels;
        const auto created = CreatePCGEditorCatalog();
        REQUIRE(created.HasValue());
        const auto &catalog = created.Value();
        const auto snapshot = FullSchemaSnapshot(catalog);
        PCGWorkspaceDocument document;
        RefreshPCGCanvas(document, snapshot);
        for (const auto &node : snapshot.source->Data().nodes)
            document.canvas.selection.push_back(node.id);
        for (const auto preset : {Theme::Preset::HoroDark, Theme::Preset::Light}) {
            Theme::SetThemePreset(preset);
            Theme::ApplyCurrentTheme();
            for (const float width : {240.0F, 1100.0F}) {
                PCGAuthoringInspectorState state;
                for (int frame = 0; frame < 2; ++frame)
                    DrawFullSchemaFrame(document, catalog, state, labels, gui.fonts, width);
                RequireFiniteVertices();
                CheckFullSchemaDraft(state, snapshot);
            }
        }
        CHECK(snapshot.source->UnknownNodeCount() == 1);
        CHECK_FALSE(snapshot.dirty);
    }
}  // namespace Horo::Editor
