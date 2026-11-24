#include "../runtime/prefab/PrefabLayerOverrideTestSupport.h"
#include "Horo/Scene/SceneSource.h"
#include "NativePublicationFiles.h"
#include "PrefabDocumentSessionTestSupport.h"
#include "editor/screens/workspace/prefab/assets/PrefabApplyTransaction.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <functional>

namespace Horo::Editor {
    namespace {
        /** @brief Injects failure at the real tracked replacement boundary with native receipts intact. */
        class ApplyFiles final : public TestSupport::NativePublicationFiles {
        public:
            std::size_t replacements{};
            bool failScene{};
            bool failAfterSceneRename{};
            bool failRollback{};
            std::size_t failReplacement{};
            std::size_t journalReplacements{};
            std::size_t failJournalReplacement{};
            std::size_t failAfterJournalRename{};
            std::function<void(const std::filesystem::path &)> afterPublication;
            std::function<void(const std::filesystem::path &, std::span<const std::byte>)> afterPrivateWrite;

            Result<void> WritePrivateDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes, bool &created,
                                             const std::filesystem::perms mode) override {
                auto written = native.WritePrivateDurable(path, bytes, created, mode);
                if (written.HasValue() && afterPrivateWrite && path.filename() == "payload.tmp")
                    afterPrivateWrite(path, bytes);
                return written;
            }

            Result<void> AtomicReplaceTracked(const std::filesystem::path &prepared, const std::filesystem::path &destination,
                                              AtomicFileReplacementReceipt &receipt) override {
                const bool journal = destination.filename() == "prefab-apply-pending.bin";
                if (journal) {
                    if (++journalReplacements == failJournalReplacement)
                        return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::ReadFailed));
                } else if (++replacements == failReplacement || (failScene && replacements == 2) || (failRollback && replacements == 3)) {
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::ReadFailed));
                }
                auto replaced = native.AtomicReplaceTracked(prepared, destination, receipt);
                if (replaced.HasValue() && afterPublication)
                    afterPublication(destination);
                const bool unconfirmed =
                    journal ? journalReplacements == failAfterJournalRename : failAfterSceneRename && replacements == 2;
                if (replaced.HasValue() && unconfirmed)
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

        TEST_CASE("Prefab apply promotes atomic singleton replacement in each owning topology layer", "[editor][prefab][apply][native]") {
            using namespace Prefab::LayerTest;
            Graph graph;
            const auto parent = Asset();
            const auto rootAsset = Id(8);
            const auto instance = Take(Prefab::PrefabInstanceId::Create(1));
            Gameplay::ComponentDescriptor descriptor{Type(),
                                                     1,
                                                     "Values",
                                                     "Tests",
                                                     {{Gameplay::ComponentPropertyId::Parse("speed").Value(), "Speed",
                                                       Gameplay::ComponentPropertyKind::Number, true}},
                                                     {}};
            const auto valueSchemas = Schemas();
            const auto captured = valueSchemas.Properties();
            std::vector<Prefab::PrefabValuePropertySchema> properties{captured.begin(), captured.end()};
            properties.front().required = true;
            graph.schemas = std::make_shared<const Prefab::PrefabValueSchemaSnapshot>(
                Take(Prefab::PrefabValueSchemaSnapshot::Create(std::move(properties), Limits(), {{std::move(descriptor), 1, true, true}})));
            auto object = Object();
            bool nested{};
            SECTION("variant writes a remove precondition derived from its immediate parent") {
                graph.AddVariant(8, parent.Bytes().back(), Revision(1), graph.Assign(parent, Property(), 4.0));
            }
            SECTION("a repeated nested asset replaces only the selected occurrence") {
                nested = true;
                const auto existing = graph.Assign(parent, Property(), 4.0);
                graph.AddNested(8, parent.Bytes().back(), Revision(1));
                auto owner = graph.sources.back().document.Data();
                auto &placements = owner.composition->nestedPlacements;
                auto sibling = placements.front();
                sibling.placementLocalId = {9};
                placements.front().valueOverrides = existing;
                placements.push_back(std::move(sibling));
                graph.sources.back().document = Take(Prefab::PrefabDocument::Create(std::move(owner), Limits()));
                object = NestedProperty().Object();
            }
            const auto sources = graph.Snapshot();
            const auto baseline = Take(sources.Resolve(rootAsset, instance, Limits()));
            const auto occurrence = Take(Prefab::PrefabComponentInstanceId::Create(1));
            Prefab::RawComponentPayload added{Take(Prefab::PrefabComponentInstanceId::Create(2)), {.typeId = Type()}};
            const std::string_view payload{R"({"speed":7.0})"};
            const auto bytes = std::as_bytes(std::span{payload});
            added.component.payload.assign(bytes.begin(), bytes.end());
            const auto sceneEdit = Take(
                Prefab::PreparePrefabComponentEdit(baseline, {}, Prefab::PrefabReplaceComponentEdit{{object, Type(), occurrence}, added},
                                                   graph.schemas.get(), Limits()));
            const auto removal = [](const Prefab::PrefabValueOverrideSet &set) -> const Prefab::PrefabRemoveComponentOverride & {
                const auto record = std::ranges::find_if(set.componentRecords, [](const auto &operation) {
                    return std::holds_alternative<Prefab::PrefabRemoveComponentOverride>(operation);
                });
                REQUIRE(record != set.componentRecords.end());
                return std::get<Prefab::PrefabRemoveComponentOverride>(*record);
            };
            const SceneDocumentSnapshot
                scene{{4}, {6}, {}, {{instance, Take(Prefab::PrefabAssetReference::Create(rootAsset)), {}, {}, sceneEdit.rebased}}};
            const auto applied =
                Take(PreparePrefabApply(scene, {instance, scene.revision, baseline.Revision(), graph.schemas->Revision(), object, 1},
                                        graph.registry.Snapshot(), sources, graph.schemas, Limits()));
            CHECK_FALSE(applied.placements.front().valueOverrides);
            const auto &composition = *applied.source.Data().composition;
            const auto &owned = nested ? composition.nestedPlacements.front().valueOverrides : composition.variantOverrides;
            REQUIRE(owned);
            CHECK(owned->records.empty());
            CHECK(owned->componentRecords.size() == 2);
            CHECK(removal(*owned).target.object == Object());
            CHECK(removal(*owned).target.instance == occurrence);
            CHECK(removal(*owned).expectedSourceComponent != removal(sceneEdit.rebased).expectedSourceComponent);
            if (nested)
                CHECK_FALSE(composition.nestedPlacements.back().valueOverrides);
            CHECK(Speed(Take(sources.Resolve(parent, instance, Limits())), Object()) == 2.0);
        }

        TEST_CASE("Prefab apply folds a selected property into its owning component addition", "[editor][prefab][apply][native]") {
            using namespace Prefab::LayerTest;
            Graph graph;
            const auto parent = Asset();
            const auto owner = Id(8);
            const auto instance = Take(Prefab::PrefabInstanceId::Create(1));
            const auto speedName = Gameplay::ComponentPropertyId::Parse("speed").Value();
            const auto weightName = Gameplay::ComponentPropertyId::Parse("weight").Value();
            const auto speedId = Take(Prefab::PrefabPropertyId::Create(7));
            const auto weightId = Take(Prefab::PrefabPropertyId::Create(8));
            const auto occurrence = Take(Prefab::PrefabComponentInstanceId::Create(2));
            Gameplay::ComponentDescriptor descriptor{Type(),
                                                     1,
                                                     "Values",
                                                     "Tests",
                                                     {{speedName, "Speed", Gameplay::ComponentPropertyKind::Number, true},
                                                      {weightName, "Weight", Gameplay::ComponentPropertyKind::Number, false}},
                                                     {}};
            const std::vector<Prefab::PrefabValuePropertySchema> properties{{Type(), speedId, speedName, 1,
                                                                             Gameplay::ComponentPropertyKind::Number, 1.0, 0.0, 10.0, 4096,
                                                                             true},
                                                                            {Type(), weightId, weightName, 1,
                                                                             Gameplay::ComponentPropertyKind::Number, 1.0, 0.0, 10.0}};
            graph.schemas = std::make_shared<const Prefab::PrefabValueSchemaSnapshot>(
                Take(Prefab::PrefabValueSchemaSnapshot::Create(properties, Limits(), {{std::move(descriptor), 2, true, true}})));
            const auto parentBaseline = graph.Resolve(parent);
            Prefab::RawComponentPayload added{occurrence, {.typeId = Type()}};
            const std::string_view payload{R"({"speed":4.0,"weight":3.0})"};
            const auto bytes = std::as_bytes(std::span{payload});
            added.component.payload.assign(bytes.begin(), bytes.end());
            const auto addition =
                Take(Prefab::PreparePrefabComponentEdit(parentBaseline, {}, Prefab::PrefabAddComponentOverride{Object(), added},
                                                        graph.schemas.get(), Limits()))
                    .rebased;
            auto object = Object();
            bool nested{};
            SECTION("variant retains its original addition and the parent bytes") {
                graph.AddVariant(8, parent.Bytes().back(), Revision(1), addition);
            }
            SECTION("nested scope folds only the selected occurrence's addition") {
                nested = true;
                graph.AddNested(8, parent.Bytes().back(), Revision(1));
                auto data = graph.sources.back().document.Data();
                auto &placements = data.composition->nestedPlacements;
                placements.front().valueOverrides = addition;
                auto sibling = placements.front();
                sibling.placementLocalId = {9};
                placements.push_back(std::move(sibling));
                graph.sources.back().document = Take(Prefab::PrefabDocument::Create(std::move(data), Limits()));
                object = NestedProperty().Object();
            }
            const auto sources = graph.Snapshot();
            const auto baseline = Take(sources.Resolve(owner, instance, Limits()));
            const auto speed = Take(Prefab::PrefabPropertyAddress::Create(object, Type(), occurrence, speedId));
            const auto weight = Take(Prefab::PrefabPropertyAddress::Create(object, Type(), occurrence, weightId));
            const auto assign = [&](const auto &current, const auto &target, const double value) {
                return Take(Prefab::PreparePrefabValueEdit(baseline, current, {Prefab::PrefabValueEditKind::Assign, target, value},
                                                           graph.schemas.get(), Limits()))
                    .rebased;
            };
            const auto selected = assign(assign(std::nullopt, speed, 8.0), weight, 9.0);
            const SceneDocumentSnapshot scene{{4},
                                              {6},
                                              {},
                                              {{instance, Take(Prefab::PrefabAssetReference::Create(owner)), {}, {}, selected}}};
            const auto promote = [&](const ScenePrefabRevertTarget &scope) {
                return Take(PreparePrefabApply(scene, {instance, scene.revision, baseline.Revision(), graph.schemas->Revision(), scope, 1},
                                               graph.registry.Snapshot(), sources, graph.schemas, Limits()));
            };
            const auto verify = [&](const PreparedPrefabApply &candidate) -> nlohmann::json {
                const auto &composition = *candidate.source.Data().composition;
                const auto &owned = nested ? composition.nestedPlacements.front().valueOverrides : composition.variantOverrides;
                REQUIRE(owned);
                REQUIRE(owned->componentRecords.size() == 1);
                CHECK(owned->records.empty());
                const auto &folded = std::get<Prefab::PrefabAddComponentOverride>(owned->componentRecords.front());
                CHECK(folded.object == Object());
                CHECK(folded.payload.component.typeId == Type());
                CHECK(folded.payload.instance == occurrence);
                if (nested)
                    CHECK(composition.nestedPlacements.back().valueOverrides == addition);
                const auto &stored = folded.payload.component.payload;
                return nlohmann::json::parse(std::string(reinterpret_cast<const char *>(stored.data()), stored.size()));
            };
            const auto one = promote(Prefab::PrefabValueOverrideTarget{speed});
            REQUIRE(one.placements.front().valueOverrides);
            REQUIRE(one.placements.front().valueOverrides->records.size() == 1);
            CHECK(one.placements.front().valueOverrides->records.front().target == Prefab::PrefabValueOverrideTarget{weight});
            CHECK(verify(one) == nlohmann::json{{"speed", 8.0}, {"weight", 3.0}});

            const auto both = promote(object);
            CHECK_FALSE(both.placements.front().valueOverrides);
            CHECK(verify(both) == nlohmann::json{{"speed", 8.0}, {"weight", 9.0}});
            CHECK(parentBaseline.Objects().front().object.components.size() == 1);
        }

        TEST_CASE("Prefab apply compensates each owned write before reporting failure or recovery", "[editor][prefab][apply][native]") {
            ApplyFixture fixture;
            const auto source = fixture.source.document->Snapshot();
            const auto scene = fixture.document.Snapshot();
            std::optional<Assets::AssetSourceIdentity> external;
            SECTION("journal preparation fails before any canonical write") {
                fixture.files.failJournalReplacement = 1;
            }
            SECTION("committed disposition cannot be published") {
                fixture.files.failJournalReplacement = 2;
            }
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
            if (fixture.files.failJournalReplacement == 1)
                CHECK(fixture.files.replacements == 0);
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

        TEST_CASE("Prefab apply compensation survives restart and reconciles only the exact pair", "[editor][prefab][apply][native]") {
            ApplyFixture fixture;
            fixture.files.failScene = true;
            fixture.files.failRollback = true;
            const auto outcome = fixture.Execute();
            REQUIRE_FALSE(outcome.committed);
            REQUIRE(outcome.recovery[0]);
            REQUIRE_FALSE(outcome.recoveryPersistenceError);
            const auto &root = fixture.source.project.directory.path;
            const auto &sourcePath = fixture.source.project.source;
            const auto installed = fixture.Read(sourcePath);
            REQUIRE(installed.identity != fixture.sourceBefore.identity);
            const auto inspected = PrefabApplyTransaction::InspectPendingRecovery(root);
            REQUIRE(inspected.HasValue());
            REQUIRE(inspected.Value());
            CHECK(fixture.Read(sourcePath).identity == installed.identity);
            CHECK(fixture.Read(fixture.scenePath).identity == fixture.sceneBefore.identity);

            NativeDurableFileSystem restartedFiles;
            const ProjectMutationCoordinator restartedMutations{restartedFiles};
            const auto resolve = [&](const PrefabApplyRecoveryCandidate &candidate) {
                return PrefabApplyTransaction::ResolvePendingRecovery(root, candidate, restartedMutations, restartedFiles);
            };
            const auto expected = *inspected.Value();
            REQUIRE(expected.action == PrefabApplyRecoveryAction::RestoreOriginals);
            SECTION("unresolved paired apply is fenced and a confirmed restore removes the record") {
                const auto attempt = fixture.Execute();
                REQUIRE(attempt.error);
                CHECK(attempt.error->code.Value() == Assets::AssetSourceErrors::DurabilityUnconfirmed.code.Value());
                REQUIRE(resolve(expected).HasValue());
                CHECK(fixture.Read(sourcePath).bytes == fixture.sourceBefore.bytes);
                CHECK(fixture.Read(fixture.scenePath).bytes == fixture.sceneBefore.bytes);
                const auto cleared = PrefabApplyTransaction::InspectPendingRecovery(root);
                REQUIRE(cleared.HasValue());
                CHECK_FALSE(cleared.Value());
            }
            SECTION("an external Scene edit prevents even the otherwise owned Prefab rollback") {
                std::ofstream(fixture.scenePath, std::ios::binary | std::ios::app) << '\n';
                const auto external = fixture.Read(fixture.scenePath);
                const auto recovered = resolve(expected);
                REQUIRE(recovered.HasError());
                CHECK(recovered.ErrorValue().code.Value() == Assets::AssetSourceErrors::Conflict.code.Value());
                CHECK(fixture.Read(fixture.scenePath).identity == external.identity);
                CHECK(fixture.Read(sourcePath).identity == installed.identity);
                const auto pending = PrefabApplyTransaction::InspectPendingRecovery(root);
                REQUIRE(pending.HasValue());
                REQUIRE(pending.Value());
                CHECK(pending.Value()->capsule == expected.capsule);
            }
            SECTION("old or corrupted capsule identity cannot authorize a retry") {
                auto stale = expected;
                ++stale.capsule.bytes;
                REQUIRE(resolve(stale).HasError());
                auto record = root / ".horo/local/recovery/prefab-apply-pending.bin";
                std::ofstream(record, std::ios::binary | std::ios::app) << '\n';
                CHECK(PrefabApplyTransaction::InspectPendingRecovery(root).HasError());
                REQUIRE(resolve(expected).HasError());
                CHECK(fixture.Read(sourcePath).identity == installed.identity);
                CHECK(fixture.Read(fixture.scenePath).identity == fixture.sceneBefore.identity);
            }
            SECTION("already restored exact bytes can finish a prior retry") {
                std::ofstream output(sourcePath, std::ios::binary | std::ios::trunc);
                output.write(reinterpret_cast<const char *>(fixture.sourceBefore.bytes.data()),
                             static_cast<std::streamsize>(fixture.sourceBefore.bytes.size()));
                output.close();
                CHECK(resolve(expected).HasError());
                const auto refreshed = PrefabApplyTransaction::InspectPendingRecovery(root);
                REQUIRE(refreshed.HasValue());
                REQUIRE(refreshed.Value());
                REQUIRE(resolve(*refreshed.Value()).HasValue());
                CHECK(fixture.Read(sourcePath).bytes == fixture.sourceBefore.bytes);
                const auto cleared = PrefabApplyTransaction::InspectPendingRecovery(root);
                REQUIRE(cleared.HasValue());
                CHECK_FALSE(cleared.Value());
            }
        }

        TEST_CASE("Prefab apply keeps its recovery record after an interrupted two-source retry", "[editor][prefab][apply][native]") {
            ApplyFixture fixture;
            fixture.files.failAfterSceneRename = true;
            fixture.files.failRollback = true;
            fixture.files.failReplacement = 4;
            const auto outcome = fixture.Execute();
            REQUIRE_FALSE(outcome.committed);
            REQUIRE(outcome.recovery[0]);
            REQUIRE(outcome.recovery[1]);
            REQUIRE_FALSE(outcome.recoveryPersistenceError);
            const auto &root = fixture.source.project.directory.path;
            auto inspected = PrefabApplyTransaction::InspectPendingRecovery(root);
            REQUIRE(inspected.HasValue());
            REQUIRE(inspected.Value());
            const auto expected = *inspected.Value();
            REQUIRE(expected.action == PrefabApplyRecoveryAction::RestoreOriginals);

            ApplyFiles firstRetry;
            firstRetry.failReplacement = 2;
            const ProjectMutationCoordinator firstMutations{firstRetry};
            const auto failed = PrefabApplyTransaction::ResolvePendingRecovery(root, expected, firstMutations, firstRetry);
            REQUIRE(failed.HasError());
            CHECK(fixture.Read(fixture.scenePath).identity == fixture.sceneBefore.identity);
            CHECK(fixture.Read(fixture.source.project.source).identity == outcome.recovery[0]->installed);
            inspected = PrefabApplyTransaction::InspectPendingRecovery(root);
            REQUIRE(inspected.HasValue());
            REQUIRE(inspected.Value());
            CHECK(inspected.Value()->capsule == expected.capsule);

            NativeDurableFileSystem restartedFiles;
            const ProjectMutationCoordinator restartedMutations{restartedFiles};
            CHECK(PrefabApplyTransaction::ResolvePendingRecovery(root, expected, restartedMutations, restartedFiles).HasError());
            REQUIRE(
                PrefabApplyTransaction::ResolvePendingRecovery(root, *inspected.Value(), restartedMutations, restartedFiles).HasValue());
            CHECK(fixture.Read(fixture.scenePath).bytes == fixture.sceneBefore.bytes);
            CHECK(fixture.Read(fixture.source.project.source).bytes == fixture.sourceBefore.bytes);
            const auto cleared = PrefabApplyTransaction::InspectPendingRecovery(root);
            REQUIRE(cleared.HasValue());
            CHECK_FALSE(cleared.Value());
        }

        TEST_CASE("Prefab apply retains a committed decision with unconfirmed journal durability", "[editor][prefab][apply][native]") {
            ApplyFixture fixture;
            const auto &root = fixture.source.project.directory.path;
            fixture.files.failAfterJournalRename = 2;
            const auto outcome = fixture.Execute();
            REQUIRE(outcome.committed);
            REQUIRE(outcome.pendingRecovery);
            REQUIRE(outcome.recoveryPersistenceError);
            CHECK(fixture.files.replacements == 2);
            const auto publishedPrefab = fixture.Read(fixture.source.project.source).identity;
            const auto publishedScene = fixture.Read(fixture.scenePath).identity;
            auto review = PrefabApplyTransaction::InspectPendingRecovery(root);
            REQUIRE(review.HasValue());
            REQUIRE(review.Value());
            const auto candidate = *review.Value();
            REQUIRE(candidate.action == PrefabApplyRecoveryAction::KeepPublished);

            NativeDurableFileSystem restartedFiles;
            const ProjectMutationCoordinator restartedMutations{restartedFiles};
            SECTION("a fresh owner can keep the exact already-published pair") {
                REQUIRE(PrefabApplyTransaction::ResolvePendingRecovery(root, candidate, restartedMutations, restartedFiles).HasValue());
                CHECK(fixture.Read(fixture.source.project.source).identity == publishedPrefab);
                CHECK(fixture.Read(fixture.scenePath).identity == publishedScene);
            }
            SECTION("a committed decision cannot be reclassified after a source is externally restored") {
                std::ofstream original(fixture.source.project.source, std::ios::binary | std::ios::trunc);
                original.write(reinterpret_cast<const char *>(fixture.sourceBefore.bytes.data()),
                               static_cast<std::streamsize>(fixture.sourceBefore.bytes.size()));
                original.close();
                CHECK(PrefabApplyTransaction::ResolvePendingRecovery(root, candidate, restartedMutations, restartedFiles).HasError());
                review = PrefabApplyTransaction::InspectPendingRecovery(root);
                REQUIRE(review.HasValue());
                REQUIRE(review.Value());
                CHECK(review.Value()->capsule == candidate.capsule);
                CHECK(review.Value()->action == PrefabApplyRecoveryAction::KeepPublished);
                REQUIRE(
                    PrefabApplyTransaction::ResolvePendingRecovery(root, *review.Value(), restartedMutations, restartedFiles).HasError());
                CHECK(fixture.Read(fixture.source.project.source).identity == fixture.sourceBefore.identity);
                CHECK(fixture.Read(fixture.scenePath).identity == publishedScene);
            }
        }

        TEST_CASE("Prefab apply restart distinguishes exact durable publication boundaries", "[editor][prefab][apply][native]") {
            ApplyFixture fixture;
            const auto &live = fixture.source.project.directory.path;
            const auto prefabName = fixture.source.project.source.lexically_relative(live);
            const auto sceneName = fixture.scenePath.lexically_relative(live);
            const std::filesystem::path journalName = ".horo/local/recovery/prefab-apply-pending.bin";
            const std::array files{prefabName, sceneName, journalName};
            const std::array order{journalName, prefabName, sceneName, journalName};
            std::array<PrefabHierarchyTestSupport::ProjectDirectory, 4> restarts;
            std::size_t checkpoint{};
            fixture.files.afterPublication = [&](const std::filesystem::path &path) {
                REQUIRE(checkpoint < restarts.size());
                REQUIRE(path.lexically_relative(live) == order[checkpoint]);
                const auto &restartRoot = restarts[checkpoint++].path;
                for (const auto &file : files) {
                    const auto destination = restartRoot / file;
                    std::filesystem::create_directories(destination.parent_path());
                    std::filesystem::copy_file(live / file, destination);
                }
            };
            const auto outcome = fixture.Execute();
            REQUIRE(outcome.committed);
            CHECK_FALSE(outcome.pendingRecovery);
            REQUIRE(checkpoint == restarts.size());
            const auto finalPrefab = fixture.Read(live / prefabName).identity;
            const auto finalScene = fixture.Read(live / sceneName).identity;
            REQUIRE(finalPrefab != fixture.sourceBefore.identity);
            REQUIRE(finalScene != fixture.sceneBefore.identity);

            for (std::size_t index = 0; index < restarts.size(); ++index) {
                CAPTURE(index);
                const auto &root = restarts[index].path;
                ApplyFiles restartedFiles;
                const ProjectMutationCoordinator restartedMutations{restartedFiles};
                const auto inspected = PrefabApplyTransaction::InspectPendingRecovery(root);
                REQUIRE(inspected.HasValue());
                REQUIRE(inspected.Value());
                const auto &candidate = *inspected.Value();
                const bool publishedPair = index >= 2;
                CHECK(candidate.prefab.planned == finalPrefab);
                CHECK(candidate.prefab.original == fixture.sourceBefore.identity);
                CHECK(candidate.prefab.observed == (index >= 1 ? finalPrefab : fixture.sourceBefore.identity));
                CHECK(candidate.scene.planned == finalScene);
                CHECK(candidate.scene.original == fixture.sceneBefore.identity);
                CHECK(candidate.scene.observed == (publishedPair ? finalScene : fixture.sceneBefore.identity));
                CHECK(candidate.action ==
                      (publishedPair ? PrefabApplyRecoveryAction::KeepPublished : PrefabApplyRecoveryAction::RestoreOriginals));

                REQUIRE(PrefabApplyTransaction::ResolvePendingRecovery(root, candidate, restartedMutations, restartedFiles).HasValue());
                CHECK(restartedFiles.replacements == (index == 1 ? 1 : 0));
                const auto prefab = Assets::CaptureAssetSource(root, root / prefabName, Prefab::PrefabHardLimits::SourceDocumentBytes);
                const auto scene = Assets::CaptureAssetSource(root, root / sceneName, Prefab::PrefabHardLimits::SourceDocumentBytes);
                REQUIRE(prefab.HasValue());
                REQUIRE(scene.HasValue());
                CHECK(prefab.Value().identity == (publishedPair ? finalPrefab : fixture.sourceBefore.identity));
                CHECK(scene.Value().identity == (publishedPair ? finalScene : fixture.sceneBefore.identity));
                CHECK_FALSE(std::filesystem::exists(root / journalName));
            }
            CHECK_FALSE(std::filesystem::exists(live / journalName));
        }

        TEST_CASE("Prefab apply restarts through incomplete private staging without deleting the recorded bytes",
                  "[editor][prefab][apply][native]") {
            ApplyFixture live;
            std::array<std::unique_ptr<ApplyFixture>, 4> restarts;
            for (auto &restart : restarts)
                restart = std::make_unique<ApplyFixture>();
            std::array<std::filesystem::path, 4> preservedStages;
            constexpr auto journalPath = ".horo/local/recovery/prefab-apply-pending.bin";
            const auto &liveRoot = live.source.project.directory.path;
            std::size_t cut{};
            live.files.afterPrivateWrite = [&](const std::filesystem::path &stage, const std::span<const std::byte> bytes) {
                REQUIRE(cut < restarts.size());
                auto &restart = *restarts[cut];
                const auto &root = restart.source.project.directory.path;
                const auto candidate = root / stage.lexically_relative(liveRoot);
                preservedStages[cut++] = candidate;
                REQUIRE(bytes.size() > 1);
                std::filesystem::create_directories(candidate.parent_path());
                for (const auto &entry : std::filesystem::directory_iterator(stage.parent_path())) {
                    if (entry.path() != stage)
                        std::filesystem::copy_file(entry.path(), candidate.parent_path() / entry.path().filename());
                }
                REQUIRE(restart.files.native.WriteDurable(candidate, bytes.first(1)).HasValue());
                std::filesystem::copy_file(live.source.project.source, restart.source.project.source,
                                           std::filesystem::copy_options::overwrite_existing);
                std::filesystem::copy_file(live.scenePath, restart.scenePath, std::filesystem::copy_options::overwrite_existing);
                if (std::filesystem::exists(liveRoot / journalPath)) {
                    std::filesystem::create_directories((root / journalPath).parent_path());
                    std::filesystem::copy_file(liveRoot / journalPath, root / journalPath);
                }
            };
            REQUIRE(live.Execute().committed);
            REQUIRE(cut == restarts.size());

            for (std::size_t index = 0; index < restarts.size(); ++index) {
                CAPTURE(index);
                auto &restart = *restarts[index];
                const auto &root = restart.source.project.directory.path;
                auto inspected = PrefabApplyTransaction::InspectPendingRecovery(root);
                REQUIRE(inspected.HasValue());
                if (index == 0) {
                    // The interrupted journal bootstrap never occupied the canonical recovery slot.
                    REQUIRE_FALSE(inspected.Value());
                } else {
                    REQUIRE(inspected.Value());
                    const auto expected =
                        index == 3 ? PrefabApplyRecoveryAction::KeepPublished : PrefabApplyRecoveryAction::RestoreOriginals;
                    REQUIRE(inspected.Value()->action == expected);
                    REQUIRE(PrefabApplyTransaction::ResolvePendingRecovery(root, *inspected.Value(), restart.mutations, restart.files)
                                .HasValue());
                    CHECK_FALSE(std::filesystem::exists(root / journalPath));
                }
                if (index != 3) {
                    CHECK(restart.Read(restart.source.project.source).identity == restart.sourceBefore.identity);
                    CHECK(restart.Read(restart.scenePath).identity == restart.sceneBefore.identity);
                    REQUIRE(restart.Execute().committed);
                }
                CHECK(restart.Read(restart.source.project.source).identity == live.Read(live.source.project.source).identity);
                CHECK(restart.Read(restart.scenePath).identity == live.Read(live.scenePath).identity);
                CHECK(std::filesystem::file_size(preservedStages[index]) == 1);
            }
        }
    }  // namespace
}  // namespace Horo::Editor
