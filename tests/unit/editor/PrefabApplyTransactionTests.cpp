#include "../runtime/prefab/PrefabLayerOverrideTestSupport.h"
#include "Horo/Scene/SceneSource.h"
#include "NativePublicationFiles.h"
#include "PrefabDocumentSessionTestSupport.h"
#include "editor/screens/workspace/prefab/PrefabApplyTransaction.h"

#include <algorithm>
#include <array>
#include <fstream>

namespace Horo::Editor {
    namespace {
        /** @brief Injects failure at the real tracked replacement boundary with native receipts intact. */
        class ApplyFiles final : public TestSupport::NativePublicationFiles {
        public:
            std::size_t replacements{};
            bool failScene{};
            bool failAfterSceneRename{};
            bool failRollback{};

            Result<void> WritePrivateDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes, bool &created,
                                             const std::filesystem::perms mode) override {
                return native.WritePrivateDurable(path, bytes, created, mode);
            }

            Result<void> AtomicReplaceTracked(const std::filesystem::path &prepared, const std::filesystem::path &destination,
                                              AtomicFileReplacementReceipt &receipt) override {
                ++replacements;
                if ((failScene && replacements == 2) || (failRollback && replacements == 3))
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::ReadFailed));
                auto replaced = native.AtomicReplaceTracked(prepared, destination, receipt);
                if (replaced.HasValue() && failAfterSceneRename && replacements == 2)
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed));
                return replaced;
            }
        };

        /** @brief Owns genuine documents, files, source captures and both authoring histories. */
        struct ApplyFixture final {
            PrefabDocumentSessionTestSupport::Fixture source;
            ApplyFiles files;
            ProjectMutationCoordinator mutations{files};
            SceneDocument document;
            EditorHistory history;
            SceneDocumentTransactions scene;
            PrefabHierarchySources sources{Capture()};
            std::filesystem::path scenePath{source.project.directory.path / "assets/scenes/main.horo"};
            Prefab::PrefabInstanceId peer{Prefab::PrefabInstanceId::Create(2).Value()};
            Prefab::PrefabObjectAddress root{Prefab::PrefabObjectAddress::Create({}, {0}).Value()};
            Math::Transform desired{{1, 2, 3}};
            Assets::AssetSourceCapture sourceBefore;
            Assets::AssetSourceCapture sceneBefore;

            ApplyFixture() {
                std::vector<ScenePrefabInstance> placements;
                const auto asset = Prefab::PrefabAssetReference::Create(source.project.asset).Value();
                for (const auto instance : {source.project.instance, peer}) {
                    const auto baseline = sources.resolver.Resolve(source.project.asset, instance, source.limits);
                    REQUIRE(baseline.HasValue());
                    const auto transform = instance == peer ? Math::Transform{{4, 5, 6}} : desired;
                    auto delta =
                        Prefab::PreparePrefabValueEdit(baseline.Value(), {}, {Prefab::PrefabValueEditKind::Assign, root, transform},
                                                       nullptr, source.limits);
                    REQUIRE(delta.HasValue());
                    placements.push_back({instance, asset, {}, {}, std::move(delta).Value().rebased});
                }
                REQUIRE(document.LoadSaved({}, std::move(placements)).HasValue());
                scene = std::move(SceneDocumentTransactions::Bind(document, history, {source.project.asset, 1})).Value();
                std::filesystem::create_directories(scenePath.parent_path());
                std::ofstream(scenePath, std::ios::binary)
                    << SceneSource::EncodeSceneSource({document.Objects(), document.PrefabInstances()});
                sourceBefore = Read(source.project.source);
                sceneBefore = Read(scenePath);
            }

            PrefabHierarchySources Capture() {
                auto captured = source.Capture();
                return std::move(captured.sources);
            }

            Assets::AssetSourceCapture Read(const std::filesystem::path &path) const {
                auto captured =
                    Assets::CaptureAssetSource(source.project.directory.path, path, Prefab::PrefabHardLimits::SourceDocumentBytes);
                REQUIRE(captured.HasValue());
                return std::move(captured).Value();
            }

            PrefabApplyOutcome Execute() {
                const auto snapshot = scene.Snapshot();
                REQUIRE(snapshot.HasValue());
                auto baseline = sources.resolver.Resolve(source.project.asset, source.project.instance, source.limits);
                REQUIRE(baseline.HasValue());
                const auto revision = document.Revision();
                return PrefabApplyTransaction::Execute(scene, *source.document,
                                                       {{snapshot.Value().session, revision.value, document.State().value, true},
                                                        {source.project.instance,
                                                         revision,
                                                         baseline.Value().Revision(),
                                                         {},
                                                         Prefab::PrefabValueOverrideTarget{root},
                                                         document.ReplacementEpoch()},
                                                        source.document->Snapshot().fence,
                                                        scenePath,
                                                        sceneBefore.identity},
                                                       source.project.registry.Snapshot(), sources, {}, source.limits, mutations, files);
            }
        };

        TEST_CASE("Prefab apply publishes the concrete source and all affected active Scene overrides together",
                  "[editor][prefab][apply][native]") {
            ApplyFixture fixture;
            const auto applied = fixture.Execute();
            INFO((applied.error ? applied.error->code.Value() : std::string_view{}));
            INFO((applied.error ? applied.error->message : std::string{}));
            REQUIRE(applied.committed);
            CHECK(applied.committed->revision == 1);
            CHECK_FALSE(applied.error);
            CHECK_FALSE(fixture.document.IsDirty());
            REQUIRE(fixture.document.PrefabInstances().size() == 2);
            CHECK_FALSE(fixture.document.PrefabInstances()[0].valueOverrides);
            const auto &peer = fixture.document.PrefabInstances()[1].valueOverrides;
            REQUIRE(peer);
            REQUIRE(peer->conflicts.size() == 1);
            CHECK(peer->conflicts.front().original.value == Prefab::PrefabOverrideValue{Math::Transform{{4, 5, 6}}});
            const auto accepted = fixture.source.document->Snapshot();
            CHECK_FALSE(accepted.dirty);
            CHECK(accepted.source->Data().objects.front().localTransform == fixture.desired);
            CHECK(fixture.Read(fixture.source.project.source).identity == accepted.acceptedSource);
            const auto savedScene = fixture.Read(fixture.scenePath);
            CHECK(savedScene.identity == applied.sceneIdentity);
            const std::string_view text{reinterpret_cast<const char *>(savedScene.bytes.data()), savedScene.bytes.size()};
            const auto decoded = SceneSource::DecodeSceneSource(text);
            REQUIRE(decoded.HasValue());
            CHECK(std::ranges::equal(decoded.Value().prefabInstances, fixture.document.PrefabInstances()));
        }

        TEST_CASE("Prefab apply replays both durable owner histories together", "[editor][prefab][apply][native]") {
            ApplyFixture fixture;
            const auto applied = fixture.Execute();
            REQUIRE(applied.committed);
            REQUIRE(applied.group);
            const auto initialSource = fixture.Read(fixture.source.project.source);
            const auto replay = [&](const bool redo, const PrefabApplyHistoryGroup group, const Assets::AssetSourceIdentity identity) {
                const auto snapshot = fixture.scene.Snapshot().Value();
                const SceneTransactionContext context{snapshot.session, snapshot.revision, snapshot.state, true};
                REQUIRE(PrefabApplyTransaction::NextSceneGroup(fixture.scene, context, redo).Value() == applied.group);
                REQUIRE(fixture.source.document->NextApplyGroup(redo) == applied.group);
                return PrefabApplyTransaction::Replay(fixture.scene, *fixture.source.document,
                                                      {context, fixture.source.document->Snapshot().fence, fixture.scenePath, identity,
                                                       group, redo},
                                                      fixture.mutations, fixture.files);
            };
            auto otherGroup = *applied.group;
            ++otherGroup.sceneOpening;
            const auto rejected = replay(false, otherGroup, applied.sceneIdentity);
            REQUIRE(rejected.error);
            CHECK_FALSE(rejected.committed);
            CHECK(fixture.files.replacements == 2);

            const auto undone = replay(false, *applied.group, applied.sceneIdentity);
            INFO((undone.error ? undone.error->message : std::string{}));
            REQUIRE(undone.committed);
            CHECK_FALSE(undone.error);
            CHECK_FALSE(fixture.document.IsDirty());
            CHECK_FALSE(fixture.source.document->Snapshot().dirty);
            CHECK(fixture.Read(fixture.scenePath).bytes == fixture.sceneBefore.bytes);
            CHECK(fixture.Read(fixture.source.project.source).bytes == fixture.sourceBefore.bytes);

            const auto redone = replay(true, *applied.group, undone.sceneIdentity);
            INFO((redone.error ? redone.error->message : std::string{}));
            REQUIRE(redone.committed);
            CHECK_FALSE(redone.error);
            CHECK_FALSE(fixture.document.IsDirty());
            CHECK_FALSE(fixture.source.document->Snapshot().dirty);
            CHECK(fixture.Read(fixture.scenePath).identity == applied.sceneIdentity);
            CHECK(fixture.Read(fixture.source.project.source).bytes == initialSource.bytes);
        }

        TEST_CASE("Prefab apply retires both history boundaries when one owner evicts its entry", "[editor][prefab][apply][native]") {
            ApplyFixture fixture;
            const auto applied = fixture.Execute();
            REQUIRE(applied.committed);
            REQUIRE(applied.group);
            const auto reconcile = [&] {
                const auto snapshot = fixture.scene.Snapshot().Value();
                const SceneTransactionContext context{snapshot.session, snapshot.revision, snapshot.state, true};
                PrefabApplyTransaction::ReconcileHistory(fixture.scene, context, std::span{&fixture.source.document, 1});
            };

            SECTION("Source history eviction retains the newer Scene transitions") {
                SceneDocumentCommandExecutor sceneCommands{fixture.document, fixture.history};
                REQUIRE(sceneCommands.Execute(CreateSceneObjectCommand{.name = "Later Scene object"}).HasValue());
                for (unsigned index = 0; index < 128; ++index) {
                    REQUIRE(fixture.source.document
                                ->Execute(fixture.source.document->Snapshot().fence, RenamePrefabObject{{0}, index % 2 == 0 ? "A" : "B"})
                                .HasValue());
                }
                REQUIRE_FALSE(fixture.source.document->HasApplyHistory());
                reconcile();
                REQUIRE(sceneCommands.Undo().HasValue());
                CHECK_FALSE(fixture.history.CanUndo());
                CHECK(fixture.history.CanRedo());
                CHECK(fixture.source.document->Snapshot().canUndo);
                CHECK(fixture.source.document->Snapshot().dirty);
            }

            SECTION("Scene history eviction releases the unmatched source transition") {
                SceneDocumentCommandExecutor sceneCommands{fixture.document, fixture.history};
                const auto created = sceneCommands.Execute(CreateSceneObjectCommand{.name = "Later Scene object"});
                REQUIRE(created.HasValue());
                for (unsigned index = 0; index < 256; ++index) {
                    REQUIRE(sceneCommands.Execute(RenameSceneObjectCommand{created.Value().object, index % 2 == 0 ? "A" : "B"}).HasValue());
                }
                reconcile();
                CHECK_FALSE(fixture.source.document->HasApplyHistory());
                CHECK_FALSE(fixture.source.document->Snapshot().canUndo);
                CHECK(fixture.history.CanUndo());
                CHECK(fixture.source.document->Snapshot().source->Data().objects.front().localTransform == fixture.desired);
            }
            SECTION("a new Scene edit discards the entire paired Source redo range") {
                const auto snapshot = fixture.scene.Snapshot().Value();
                const SceneTransactionContext context{snapshot.session, snapshot.revision, snapshot.state, true};
                const auto undone = PrefabApplyTransaction::Replay(fixture.scene, *fixture.source.document,
                                                                   {context, fixture.source.document->Snapshot().fence, fixture.scenePath,
                                                                    applied.sceneIdentity, *applied.group, false},
                                                                   fixture.mutations, fixture.files);
                REQUIRE(undone.committed);
                REQUIRE(fixture.source.document->Snapshot().canRedo);
                SceneDocumentCommandExecutor sceneCommands{fixture.document, fixture.history};
                REQUIRE(sceneCommands.Execute(CreateSceneObjectCommand{.name = "New Scene branch"}).HasValue());
                reconcile();
                CHECK_FALSE(fixture.source.document->HasApplyHistory());
                CHECK_FALSE(fixture.source.document->Snapshot().canRedo);
                CHECK(fixture.history.CanUndo());
            }
        }

        TEST_CASE("Prefab apply promotes values into the owning variant or exact repeated occurrence", "[editor][prefab][apply][native]") {
            using namespace Prefab::LayerTest;
            Graph graph;
            const auto parent = Asset();
            const auto owner = Id(8);
            const auto instance = Take(Prefab::PrefabInstanceId::Create(1));
            Prefab::PrefabObjectAddress object;
            auto property = Property();
            bool nested{};
            SECTION("a variant of a variant writes only its own layer") {
                graph.AddVariant(7, parent.Bytes().back(), Revision(1), graph.Assign(parent, property, 4.0));
                graph.AddParent(8, 7, Revision(7));
            }
            SECTION("two occurrences of the same asset retain their independent values and mounting") {
                nested = true;
                graph.AddNested(8, parent.Bytes().back(), Revision(1));
                auto data = graph.sources.back().document.Data();
                auto &placements = data.composition->nestedPlacements;
                placements.front().localRootTransform = Math::Transform{{10, 0, 0}};
                auto sibling = placements.front();
                sibling.placementLocalId = {9};
                placements.push_back(std::move(sibling));
                graph.sources.back().document = Take(Prefab::PrefabDocument::Create(std::move(data), Limits()));
                property = NestedProperty();
                object = property.Object();
            }
            const auto sources = graph.Snapshot();
            const auto baseline = Take(sources.Resolve(owner, instance, Limits()));
            const auto edit = [&](const auto &existing, auto target, auto value) {
                return Take(Prefab::PreparePrefabValueEdit(baseline, existing, {Prefab::PrefabValueEditKind::Assign, target, value},
                                                           graph.schemas.get(), Limits()))
                    .rebased;
            };
            const auto transform = Math::Transform{{1, 2, 3}};
            const auto values = edit(edit(std::nullopt, property, 9.0), object, transform);
            const SceneDocumentSnapshot scene{{4},
                                              {6},
                                              {},
                                              {{instance, Take(Prefab::PrefabAssetReference::Create(owner)), {}, {}, values}}};
            const auto oneValue = Take(PreparePrefabApply(scene,
                                                          {instance, scene.revision, baseline.Revision(), graph.schemas->Revision(),
                                                           Prefab::PrefabValueOverrideTarget{property}, 1},
                                                          graph.registry.Snapshot(), sources, graph.schemas, Limits()));
            REQUIRE(oneValue.placements.front().valueOverrides);
            const auto &remaining = oneValue.placements.front().valueOverrides->records;
            REQUIRE(remaining.size() == 1);
            CHECK(remaining.front().target == Prefab::PrefabValueOverrideTarget{object});
            const auto result =
                Take(PreparePrefabApply(scene, {instance, scene.revision, baseline.Revision(), graph.schemas->Revision(), object, 1},
                                        graph.registry.Snapshot(), sources, graph.schemas, Limits()));
            REQUIRE_FALSE(result.placements.front().valueOverrides);
            const auto &composition = *result.source.Data().composition;
            const auto &owned = nested ? composition.nestedPlacements.front().valueOverrides : composition.variantOverrides;
            REQUIRE(owned);
            REQUIRE(owned->records.size() == 2);
            CHECK(owned->sourcePrefab == (nested ? parent : Id(7)));
            CHECK(owned->authoredAgainst == (nested ? Revision(1) : Revision(7)));
            CHECK(std::ranges::all_of(owned->records, [](const auto &entry) {
                if (const auto *target = std::get_if<Prefab::PrefabPropertyAddress>(&entry.target))
                    return *target == Property();
                return std::get<Prefab::PrefabObjectAddress>(entry.target) == Prefab::PrefabObjectAddress{};
            }));
            if (nested) {
                REQUIRE(composition.nestedPlacements.size() == 2);
                CHECK_FALSE(composition.nestedPlacements.back().valueOverrides);
                CHECK(composition.nestedPlacements.front().localRootTransform == Math::Transform{{10, 0, 0}});
            }
            CHECK(Speed(Take(sources.Resolve(nested ? parent : Id(7), instance, Limits())), {}) == (nested ? 2.0 : 4.0));
        }

        TEST_CASE("Prefab apply compensates each owned write before reporting failure or recovery", "[editor][prefab][apply][native]") {
            ApplyFixture fixture;
            const auto source = fixture.source.document->Snapshot();
            const auto scene = fixture.document.Snapshot();
            std::optional<Assets::AssetSourceIdentity> external;
            SECTION("scene write fails before replacement") {
                fixture.files.failScene = true;
            }
            SECTION("scene write fails after replacement") {
                fixture.files.failAfterSceneRename = true;
            }
            SECTION("source compensation cannot write") {
                fixture.files.failScene = true;
                fixture.files.failRollback = true;
            }
            SECTION("an external source edit invalidates the captured transaction") {
                fixture.source.project.WriteSource("External edit");
                external = fixture.Read(fixture.source.project.source).identity;
            }
            const auto outcome = fixture.Execute();
            INFO((outcome.error ? outcome.error->code.Value() : std::string_view{}));
            INFO((outcome.error ? outcome.error->message : std::string{}));
            CHECK_FALSE(outcome.committed);
            REQUIRE(outcome.error);
            CHECK(fixture.document.Revision() == scene.revision);
            CHECK(std::ranges::equal(fixture.document.PrefabInstances(), scene.prefabInstances));
            CHECK(fixture.Read(fixture.scenePath).identity == fixture.sceneBefore.identity);
            const auto currentSource = fixture.source.document->Snapshot();
            CHECK(currentSource.fence.revision == source.fence.revision);
            CHECK(currentSource.source == source.source);
            const auto diskSource = fixture.Read(fixture.source.project.source);
            CHECK_FALSE(outcome.recovery[1]);
            if (fixture.files.failRollback) {
                REQUIRE(outcome.recovery[0]);
                CHECK(outcome.recovery[0]->original.bytes == fixture.sourceBefore.bytes);
                CHECK(outcome.recovery[0]->installed == diskSource.identity);
            } else if (external) {
                CHECK(fixture.files.replacements == 0);
                CHECK_FALSE(outcome.recovery[0]);
                CHECK(diskSource.identity == *external);
            } else {
                CHECK_FALSE(outcome.recovery[0]);
                CHECK(diskSource.identity == fixture.sourceBefore.identity);
            }
        }
    }  // namespace
}  // namespace Horo::Editor
