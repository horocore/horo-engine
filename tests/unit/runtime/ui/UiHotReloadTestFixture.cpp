#include "UiHotReloadTestFixture.h"

#include "Horo/Foundation/Sha256.h"

#include <array>

namespace Horo::Runtime::Ui::ReloadTests {
    namespace {
        Assets::AssetId RootAsset() {
            SerializedUiId bytes{};
            bytes.back() = 7;
            return Assets::AssetId::FromBytes(bytes);
        }

        Assets::AssetTypeId Type(std::string_view value) {
            return Assets::AssetTypeId::Parse(value).Value();
        }

        /** @brief Uses real encode/provider/async-load/decode contracts rather than manufacturing a terminal closure. */
        UiRuntimeAssetLoadResult Load(std::uint64_t version, bool secondCanvas) {
            UiDocumentBuilder builder{Stable<UiDocumentId>(1), Revision<UiDocumentRevision>(version)};
            const std::uint8_t count = secondCanvas ? 2 : 1;
            for (std::uint8_t i = 0; i < count; ++i) {
                REQUIRE(builder.AddCanvas({Stable<UiCanvasId>(2 + i), Stable<UiElementId>(10 + i * 2)}).HasValue());
                REQUIRE(builder.AddElement({Stable<UiElementId>(10 + i * 2), {}, Type("core.panel"), {}, {}}).HasValue());
                REQUIRE(
                    builder.AddElement({Stable<UiElementId>(11 + i * 2), Stable<UiElementId>(10 + i * 2), Type("core.text_input"), {}, {}})
                        .HasValue());
            }
            REQUIRE(builder.AddRoute({Stable<UiRouteId>(9), UiPresentationBand::Screen, 1, false}).HasValue());
            auto document = std::move(builder).Build();
            REQUIRE(document.HasValue());
            const auto cooked = CookedUiDocument::Cook(document.Value());
            REQUIRE(cooked.HasValue());
            const auto type = Type("runtime.ui.document");
            Assets::AssetCookArtifact artifact;
            artifact.id = RootAsset();
            artifact.type = type;
            artifact.target = AssetCookTargetId::Parse("linux-x64").Value();
            artifact.payload.assign(cooked.Value().Payload().begin(), cooked.Value().Payload().end());
            artifact.payloadDigest = ComputeSha256(std::as_bytes(std::span{artifact.payload}));
            Assets::AssetRegistry registry;
            REQUIRE(registry
                        .Publish({{RootAsset(), type, ProjectPath::Parse("assets/ui.horoasset").Value(),
                                   ProjectPath::Parse("assets/ui.horoasset.horo").Value()}})
                        .status == Assets::AssetRegistryBuildStatus::Complete);
            Assets::MemoryAssetProvider provider;
            provider.Insert(RootAsset(), Assets::EncodeCookedArtifact(artifact).Value());
            JobSystem jobs{JobSystemConfig{1, 8}};
            Assets::AssetLoadService assets{jobs, provider};
            UiRuntimeAssetLoadService loader{registry, assets};
            auto handle =
                loader.LoadAsync(registry.Snapshot(),
                                 {{RootAsset(), Stable<UiDocumentId>(1), Stable<UiCanvasId>(2), Revision<UiDocumentRevision>(version)},
                                  type,
                                  artifact.target});
            REQUIRE(handle.HasValue());
            auto loading = std::move(handle).Value();
            REQUIRE(loading.Wait().HasValue());
            return std::move(loading.TakeResult()).Value();
        }

        /** @brief Deterministic real layout with version-dependent overflow bounds for scroll clamp coverage. */
        class Evaluator final : public UiLayoutEvaluator {
        public:
            explicit Evaluator(std::int32_t extent) : extent_(extent) {}

            Result<void> ResolveChildConstraints(const UiLayoutChildConstraintRequest &request,
                                                 std::span<UiLayoutConstraints> output) const override {
                for (auto &constraint : output)
                    constraint = request.constraints;
                return Result<void>::Success();
            }

            Result<UiLayoutMeasurement> Measure(const UiLayoutMeasureRequest &request) const override {
                return Result<UiLayoutMeasurement>::Success({request.constraints.maximum, false, false});
            }

            Result<UiLayoutArrangement> Arrange(const UiLayoutArrangeRequest &request, std::span<UiLogicalRect> children) const override {
                for (auto &child : children)
                    child = {{0, 0}, {extent_, extent_}};
                const UiLogicalRect overflow{request.assignedContent.origin, {extent_, extent_}};
                return Result<UiLayoutArrangement>::Success({request.assignedContent, request.assignedContent, request.assignedContent,
                                                             request.assignedContent, overflow, request.assignedContent, NoUiBaseline});
            }

        private:
            std::int32_t extent_;
        };

        UiReloadCanvas MakeCanvas(UiElementSlotAllocator &allocator, const CookedUiDocument &document, std::size_t index,
                                  std::uint64_t version) {
            const auto &authored = document.Canvases()[index];
            const UiCanvasInstanceId canvas{Owner(), static_cast<std::uint32_t>(index + 2), 1};
            const auto child = Stable<UiElementId>(static_cast<std::uint8_t>(11 + index * 2));
            const std::array elements{UiElementDescriptor{authored.rootElement, {}}, UiElementDescriptor{child, authored.rootElement}};
            auto tree = UiElementTree::Create(allocator,
                                              {Instance(),
                                               canvas,
                                               document.Id(),
                                               document.SourceRevision(),
                                               Revision<UiRuntimeTreeRevision>(version),
                                               {8, 8, 8}},
                                              elements);
            REQUIRE(tree.HasValue());
            return {authored.id, std::move(tree).Value()};
        }

        void InputOwners(UiReloadCanvas &canvas, const CookedUiDocument &document, std::size_t index, std::uint64_t version,
                         std::uint16_t textLimit, std::uint32_t modalHighWater, std::uint32_t routeHighWater) {
            const auto root = canvas.tree.Root().Value();
            const auto child = Stable<UiElementId>(static_cast<std::uint8_t>(11 + index * 2));
            const auto handle = canvas.tree.Find(child).Value();
            const UiActionOwnerContext owner{Instance(),
                                             canvas.tree.Canvas(),
                                             document.Id(),
                                             document.SourceRevision(),
                                             canvas.tree.Revision(),
                                             Revision<UiInteractionRevision>(version)};
            UiFocusGraphDescriptor focus{{owner.instance,
                                          owner.canvas,
                                          owner.document,
                                          owner.documentRevision,
                                          owner.treeRevision,
                                          owner.interaction,
                                          {UiFocusPlayerId{Owner(), 3, 1}, {Owner(), 4, 1}}},
                                         child,
                                         UiFocusRecoveryPolicy::AncestorThenDefaultThenFirst,
                                         8,
                                         4,
                                         4};
            focus.previousModalIncarnation = modalHighWater;
            const std::array
                nodes{UiFocusNodeDescriptor{root.handle, root.id, {}, {}, UiFocusBringIntoViewPolicy::Nearest, false, true, true},
                      UiFocusNodeDescriptor{handle, child, root.id, {}, UiFocusBringIntoViewPolicy::Nearest, true, true, true}};
            canvas.focus.emplace(std::move(UiFocusGraph::Create(focus, nodes)).Value());
            canvas.controls.push_back(
                {child, std::move(UiControlStateMachine::Create(
                                      UiTextInputControlDescriptor{{owner, handle, Stable<UiActionId>(8), {}, true, true, {}},
                                                                   Text("base"),
                                                                   textLimit,
                                                                   true}))
                            .Value()});
            canvas.routes.emplace(
                std::move(UiScreenStack::Create({Owner(), {Owner(), root.handle.slot, 1}, document.Routes(), 4, routeHighWater})).Value());
            canvas.captures.emplace(std::move(UiPointerCaptureStore::Create({Owner(), 8})).Value());
        }

        void Geometry(UiReloadCanvas &canvas, std::uint64_t version, std::uint32_t concurrentSnapshots) {
            canvas.layoutEngine.emplace(
                std::move(UiLayoutEngine::Create({Instance(), canvas.tree.Canvas(), canvas.tree.SourceDocument(), 8, 8, concurrentSnapshots,
                                                  Revision<UiInteractionRevision>(version)}))
                    .Value());
            Evaluator evaluator{version == 1 ? 300 : 150};
            const UiLayoutSourceRevisions sources{canvas.tree.SourceDocumentRevision(),   canvas.tree.Revision(),
                                                  Revision<UiLayoutContentRevision>(1),   Revision<UiLayoutStyleRevision>(1),
                                                  Revision<UiLayoutIntrinsicRevision>(1), Revision<UiLayoutCanvasRevision>(1),
                                                  Revision<UiLayoutPolicyRevision>(1)};
            canvas.layout.emplace(
                std::move(canvas.layoutEngine->Update(canvas.tree, {sources, {{0, 0}, {100, 100}}, {{0, 0}, {100, 100}}, &evaluator}))
                    .Value());
            canvas.clipping.emplace(std::move(UiLayoutClipEngine::Create({Instance(), canvas.tree.Canvas(), canvas.tree.SourceDocument(), 8,
                                                                          8, 8, concurrentSnapshots}))
                                        .Value());
            const auto root = canvas.tree.Root().Value().handle;
            for (const auto &record : canvas.layout->Records())
                canvas.clipPolicies.push_back(
                    {record.element, record.element == root ? UiLayoutOverflowPolicy::Scroll : UiLayoutOverflowPolicy::Visible, {}});
            canvas.clipped.emplace(std::move(canvas.clipping->Update(canvas.tree, *canvas.layout, {canvas.clipPolicies, {}})).Value());
            canvas.presentations.push_back(UiPresentedInteractionState::Create({Owner(), 10, 1}, canvas.tree.Canvas()).Value());
        }
    }  // namespace

    UiReloadGeneration Generation(UiElementSlotAllocator &allocator, std::uint64_t version, std::uint16_t textLimit, bool secondCanvas,
                                  std::uint32_t modalHighWater, std::uint32_t routeHighWater, std::uint32_t concurrentSnapshots) {
        auto loaded = Load(version, secondCanvas);
        std::vector<UiReloadCanvas> canvases;
        for (std::size_t i = 0; i < loaded.document.Canvases().size(); ++i) {
            auto canvas = MakeCanvas(allocator, loaded.document, i, version);
            InputOwners(canvas, loaded.document, i, version, textLimit, modalHighWater, routeHighWater);
            Geometry(canvas, version, concurrentSnapshots);
            canvases.push_back(std::move(canvas));
        }
        auto generation = UiReloadGeneration::Create(std::move(loaded), Instance(), std::move(canvases));
        REQUIRE(generation.HasValue());
        return std::move(generation).Value();
    }

    UiReloadCanvas &Canvas(UiHotReload &publisher) {
        return *publisher.Current()->Canvas(Stable<UiCanvasId>(2));
    }

    UiControlInput Input(const UiControlStateMachine &control, UiControlInputKind kind, std::uint64_t sequence, std::string_view text) {
        return {{control.Owner(), control.Element()}, kind,      UiControlActivationSource::Keyboard, sequence, 0,
                UiControlAdjustment::Count,           Text(text)};
    }

    void Draft(UiReloadCanvas &canvas, std::string_view text, std::uint64_t firstSequence) {
        auto &control = canvas.controls.front().control;
        REQUIRE(control.Handle(Input(control, UiControlInputKind::FocusGained, firstSequence)).HasValue());
        REQUIRE(control.Handle(Input(control, UiControlInputKind::TextInput, firstSequence + 1, text)).HasValue());
    }
}  // namespace Horo::Runtime::Ui::ReloadTests
