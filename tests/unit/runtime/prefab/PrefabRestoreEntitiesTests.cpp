#include "AllocationProbe.h"
#include "GameplayStructuralUnitObserver.h"
#include "Horo/Prefab/PrefabRestoreEntities.h"
#include "Horo/Runtime/Save/SaveRestoreTransaction.h"
#include "PrefabTemplateProviderTestSupport.h"
#include "SceneAllocationQualification.h"

namespace Horo::Prefab {
    namespace {
        using namespace TestSupport;
        using namespace Runtime;

        template <typename Identity> Identity Stable(std::uint8_t value) {
            SaveIdentityDetail::Bytes bytes{};
            bytes.back() = value;
            return Identity::FromBytes(bytes).Value();
        }

        /** @brief Uses the actual source resolver/cooker for genuine root/member provenance.
         * Adds a declared test interface to that cooked output because authoring binding declarations are not yet source fields.
         */
        std::vector<std::uint8_t> CookedWithInterface(Fixture &fixture) {
            const auto behavior = Gameplay::BehaviorComponent{{1}, Gameplay::BehaviorTypeId::Parse("game.tests.restore").Value()};
            PrefabObjectNode root{.localId = {0}, .name = "root", .behaviors = {behavior}};
            PrefabObjectNode child{.localId = {7}, .parentLocalId = LocalObjectId{0}, .name = "child"};
            auto document = PrefabDocument::Create({.projectVersion = Application::ParseHoroVersion("1.2.3").Value(),
                                                    .assetId = Test::Asset(),
                                                    .objects = {root, child},
                                                    .referencedAssets = {Test::Asset(2)}},
                                                   Profile());
            REQUIRE(document.HasValue());
            const auto sourceBytes = document.Value().SerializeCanonical().Value();
            PrefabSourceRevision revision{document.Value().Data().projectVersion,
                                          ComputeSha256(std::as_bytes(std::span{sourceBytes.data(), sourceBytes.size()}))};
            auto sources = BuildPrefabSourceResolverSnapshot(fixture.registry.Snapshot(), {{document.Value(), revision}}, Profile());
            REQUIRE(sources.HasValue());
            const PrefabTemplateCookResource dependency{Test::Asset(2), fixture.dependency};
            auto cooked = CookPrefabTemplate(sources.Value(), fixture.registry.Snapshot(), Test::Asset(), std::span{&dependency, 1},
                                             Target(), Profile());
            REQUIRE(cooked.HasValue());
            REQUIRE(cooked.Value().Data().rootSourceRevision == revision);
            auto data = cooked.Value().Data();
            data.bindings = {{PrefabPropertyId::Create(11).Value(), {}, true}};
            data.references = {{{{0}, 0}, PrefabPropertyId::Create(4).Value(), CookedPrefabBindingSlot{0}}};
            auto withInterface = CookedPrefab::Create(std::move(data), Profile());
            REQUIRE(withInterface.HasValue());
            std::vector<std::uint8_t> bytes;
            for (const auto byte : withInterface.Value().Bytes())
                bytes.push_back(std::to_integer<std::uint8_t>(byte));
            return Envelope(Test::Asset(), "core.prefab", std::move(bytes));
        }

        PrefabPersistentInstance Recipe(const PrefabTemplateLease &lease, std::uint8_t instance, std::uint8_t firstEntity,
                                        std::uint64_t behavior) {
            PrefabPersistentInstance result;
            result.identity = Stable<SavePrefabInstanceId>(instance);
            result.creator = "editor.play";
            result.source = {lease.Template()->Data().assetId, lease.Artifact().Digest(), *lease.Target()};
            result.rootSourceRevision = *lease.Template()->Data().rootSourceRevision;
            result.creationLineage = {result.source.asset};
            result.overrides = {result.source.asset, result.rootSourceRevision, {}, {}, {}, {}};
            const auto definition = SaveAssetId::FromBytes(result.source.asset.Bytes()).Value();
            for (const auto &source : lease.Template()->Data().entities) {
                PrefabPersistentMember member{{Stable<PersistentEntityId>(firstEntity++),
                                               {1},
                                               SpawnedPersistentEntityProvenance{definition, result.identity},
                                               PersistentEntityDisposition::Live},
                                              source.provenance,
                                              {}};
                for (const auto &value : source.members)
                    if (const auto *original = std::get_if<Gameplay::BehaviorComponent>(&value))
                        member.behaviors.push_back({original->typeId, original->instanceId, {behavior++}, original->schemaVersion});
                result.members.push_back(std::move(member));
            }
            return result;
        }

        /** @brief Includes the actual typed Scene admission reason in positive-case diagnostics. */
        void RequireQueued(Result<void> queued) {
            INFO((queued.HasError() ? queued.ErrorValue().code.Value() + ": " + queued.ErrorValue().message
                                    : "Actual private Scene queued"));
            REQUIRE(queued.HasValue());
        }

        struct Observation final {
            std::vector<PersistentEntityRuntimeBinding> bindings;
            bool prepared{};
            bool published{};
            bool rolledBack{};
            bool failOwners{};
            SceneRestoreStructuralLimits limits{16'384, 16'384, 65'536, 65'536, 16'384 * 16, 64U << 20U};
        };

        /** @brief Exercises the service-issued real private candidate path; this fixture does not claim SaveSession coverage. */
        class Aggregate final : public SceneAggregateRestore {
        public:
            Aggregate(std::shared_ptr<PrefabRestoreEntities> adapter, std::vector<PersistentEntityRecord> records,
                      std::shared_ptr<Observation> observation)
                : adapter_(std::move(adapter)), records_(std::move(records)), observation_(std::move(observation)) {}

            Result<void> ValidateRestorePartitions(std::span<const SceneRestoreGroupPartition> partitions) const override {
                return ValidateSceneRestorePartitions(Stable<SaveWorldId>(1), records_, partitions, 16'384);
            }

            SceneRestoreStructuralLimits StructuralLimits() const noexcept override {
                return observation_->limits;
            }

            Result<void> PrepareScene(RuntimeScene &) override {
                return Result<void>::Failure(MakeError(SaveErrors::RestoreContextInvalid));
            }

            Result<void> PrepareScene(RuntimeScene &scene, SceneRestoreCandidateCommands &commands) override {
                auto bindings = adapter_->Prepare(scene, records_, 16'384, commands);
                if (bindings.HasError())
                    return Result<void>::Failure(bindings.ErrorValue());
                observation_->bindings = std::move(bindings).Value();
                observation_->prepared = true;
                return Result<void>::Success();
            }

            Result<bool> PrepareOwners() override {
                if (observation_->failOwners)
                    return Result<bool>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
                return Result<bool>::Success(true);
            }

            Result<void> ValidatePublication(const RuntimeSceneView *) const override {
                return adapter_->ValidatePublication();
            }

            Result<void> Commit(IStagedRestoreAggregatePublication &transfer) override {
                adapter_->PublishPrepared();
                transfer.PublishPrepared();
                observation_->published = true;
                return Result<void>::Success();
            }

            void Rollback() noexcept override {
                if (observation_->published)
                    return;
                adapter_->Cancel();
                observation_->rolledBack = true;
            }

        private:
            std::shared_ptr<PrefabRestoreEntities> adapter_;
            std::vector<PersistentEntityRecord> records_;
            std::shared_ptr<Observation> observation_;
        };

        struct RestoreFixture final {
            std::shared_ptr<Tests::GameplayStructuralUnitEvents> structural{std::make_shared<Tests::GameplayStructuralUnitEvents>()};
            Fixture fixture;
            PrefabTemplateLease lease;
            PrefabPersistentState saved;
            std::vector<PersistentEntityRecord> records;

            explicit RestoreFixture(bool composeOwner = true)
                : fixture({}, composeOwner ? std::make_unique<Tests::GameplayStructuralUnitObserver>(structural) : nullptr) {
                fixture.root = CookedWithInterface(fixture);
                fixture.bytes.Memory().Insert(Test::Asset(), fixture.root);
                lease = fixture.Ready();
                saved = {Stable<SaveWorldId>(1), {Recipe(lease, 2, 10, 100), Recipe(lease, 3, 20, 200)}};
                // Cross-instance references are cyclic but the hierarchy is acyclic.
                saved.instances[0].bindings = {
                    {PrefabPropertyId::Create(11).Value(), {{saved.world, saved.instances[1].members[1].record.identity}, {1}}}};
                saved.instances[1].bindings = {
                    {PrefabPropertyId::Create(11).Value(), {{saved.world, saved.instances[0].members[1].record.identity}, {1}}}};
                saved.instances[0].parent =
                    PrefabPersistentEntityReference{{saved.world, saved.instances[1].members[1].record.identity}, {1}};
                for (const auto &instance : saved.instances)
                    for (const auto &member : instance.members)
                        records.push_back(member.record);
            }

            Result<std::shared_ptr<PrefabRestoreEntities>> Adapter() {
                return PrefabRestoreEntities::Create(saved, {lease, lease}, fixture.provider, nullptr, nullptr, {}, {});
            }

            Result<void> Queue(std::shared_ptr<PrefabRestoreEntities> adapter, std::shared_ptr<Observation> observation) {
                SceneDefinitionBuilder builder{{7}, {1}};
                return fixture.scenes.QueuePreparationWithRestore(std::move(builder).Build().Value(),
                                                                  std::make_unique<Aggregate>(std::move(adapter), records,
                                                                                              std::move(observation)));
            }
        };
    }  // namespace

    TEST_CASE("Private prefab reconstruction preserves cyclic forward bindings and parent ordering in one real Scene group",
              "[prefab][save][restore][references]") {
        RestoreFixture fixture;
        const auto original = fixture.fixture.scenes.ActiveScene()->RuntimeId();
        auto adapter = fixture.Adapter();
        REQUIRE(adapter.HasValue());
        auto observed = std::make_shared<Observation>();
        RequireQueued(fixture.Queue(adapter.Value(), observed));
        REQUIRE(observed->prepared);
        CHECK(fixture.structural->prepared == 0);
        CHECK(fixture.structural->published == 0);
        CHECK_FALSE(observed->published);
        CHECK(fixture.fixture.scenes.ActiveScene()->RuntimeId() == original);
        fixture.fixture.Commit();
        REQUIRE(observed->published);
        const auto active = fixture.fixture.scenes.ActiveScene().value();
        CHECK(active.RuntimeId() != original);
        REQUIRE(observed->bindings.size() == 4);
        const auto instances = adapter.Value()->PreparedInstances();
        REQUIRE(instances.size() == 2);
        const auto first = active.Get(instances[0].members[0]);
        const auto second = active.Get(instances[1].members[0]);
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        CHECK(first.Value().parent == instances[1].members[1]);
        CHECK(first.Value().components->behaviors[0].instanceId.value == 100);
        CHECK(second.Value().components->behaviors[0].instanceId.value == 200);
        REQUIRE(first.Value().groupReferences.size() == 1);
        REQUIRE(second.Value().groupReferences.size() == 1);
        REQUIRE(std::holds_alternative<RuntimeGroupExternalReference>(first.Value().groupReferences[0].target));
        REQUIRE(std::holds_alternative<RuntimeGroupExternalReference>(second.Value().groupReferences[0].target));
        CHECK_FALSE(std::get<RuntimeGroupExternalReference>(first.Value().groupReferences[0].target).component);
        CHECK(std::get<RuntimeGroupExternalReference>(first.Value().groupReferences[0].target).entity == instances[1].members[1]);
        CHECK(std::get<RuntimeGroupExternalReference>(second.Value().groupReferences[0].target).entity == instances[0].members[1]);
    }

    TEST_CASE("Private materialization still rejects missing composed Gameplay authority before any publication",
              "[prefab][save][restore][owner][rollback]") {
        RestoreFixture fixture{false};
        auto adapter = fixture.Adapter();
        REQUIRE(adapter.HasValue());
        const auto original = fixture.fixture.scenes.ActiveScene()->RuntimeId();
        auto observation = std::make_shared<Observation>();
        auto queued = fixture.Queue(adapter.Value(), observation);
        REQUIRE(queued.HasError());
        INFO(queued.ErrorValue().code.Value() << ": " << queued.ErrorValue().message);
        CHECK(observation->rolledBack);
        CHECK_FALSE(observation->published);
        CHECK(fixture.fixture.scenes.ActiveScene()->RuntimeId() == original);
        CHECK(fixture.fixture.scenes.ActiveScene()->SlotCount() == 0);
    }

    TEST_CASE("Private prefab reconstruction rejects malformed maps and hierarchy cycles without changing the active Scene",
              "[prefab][save][restore][rollback]") {
        RestoreFixture fixture;
        const auto original = fixture.fixture.scenes.ActiveScene()->RuntimeId();
        SECTION("stale binding generation") {
            fixture.saved.instances[0].bindings[0].target.generation = {9};
        }
        SECTION("parent cycle") {
            fixture.saved.instances[1].parent =
                PrefabPersistentEntityReference{{fixture.saved.world, fixture.saved.instances[0].members[1].record.identity}, {1}};
        }
        SECTION("sole aggregate record mismatch") {
            fixture.records.front().generation = {9};
        }
        SECTION("removed member provenance") {
            fixture.saved.instances[0].members[0].source.sourceDigest = {};
        }
        SECTION("behavior schema mismatch") {
            fixture.saved.instances[0].members[0].behaviors[0].schemaVersion = 99;
        }
        auto adapter = fixture.Adapter();
        if (adapter.HasValue()) {
            auto observation = std::make_shared<Observation>();
            CHECK(fixture.Queue(adapter.Value(), observation).HasError());
            CHECK(observation->rolledBack);
        }
        CHECK(fixture.fixture.scenes.ActiveScene()->RuntimeId() == original);
        CHECK(fixture.fixture.scenes.ActiveScene()->SlotCount() == 0);
    }

    TEST_CASE("Private prefab group is discarded if a later owner fails or restore cancellation wins",
              "[prefab][save][restore][lifecycle]") {
        RestoreFixture fixture;
        const auto original = fixture.fixture.scenes.ActiveScene()->RuntimeId();
        auto adapter = fixture.Adapter();
        REQUIRE(adapter.HasValue());
        auto observation = std::make_shared<Observation>();
        RequireQueued(fixture.Queue(adapter.Value(), observation));
        REQUIRE(observation->prepared);
        SECTION("later owner failure") {
            observation->failOwners = true;
        }
        SECTION("explicit unpublished cancellation") {
            adapter.Value()->Cancel();
        }
        fixture.fixture.Commit();
        CHECK_FALSE(observation->published);
        CHECK(observation->rolledBack);
        CHECK(fixture.fixture.scenes.ActiveScene()->RuntimeId() == original);
        CHECK(fixture.fixture.scenes.TakeOperationError().has_value());
        CHECK(adapter.Value()->PreparedInstances().empty());
    }
}  // namespace Horo::Prefab

namespace Horo::Prefab {
    TEST_CASE("Every actual dynamic prefab adapter preparation allocation fails without changing installed live state",
              "[prefab][save][restore][allocation]") {
        Horo::Tests::RequireSupportedSceneAllocationInjection();
        RestoreFixture fixture;
        const auto original = fixture.fixture.scenes.ActiveScene()->RuntimeId();
        Tests::AllocationProbe::Measurement measured;
        auto saved = fixture.saved;
        std::vector<PrefabTemplateLease> leases{fixture.lease, fixture.lease};
        std::optional<Result<std::shared_ptr<PrefabRestoreEntities>>> success;
        {
            Tests::AllocationProbe::ScopedMeasurement measurement;
            success.emplace(
                PrefabRestoreEntities::Create(std::move(saved), std::move(leases), fixture.fixture.provider, nullptr, nullptr, {}, {}));
            measured = measurement.Snapshot();
        }
        REQUIRE(success->HasValue());
        REQUIRE(measured.requests > 0);
        success.reset();
        for (std::size_t failureIndex = 0; failureIndex < measured.requests; ++failureIndex) {
            INFO(failureIndex);
            auto input = fixture.saved;
            std::vector<PrefabTemplateLease> pinned{fixture.lease, fixture.lease};
            std::optional<Result<std::shared_ptr<PrefabRestoreEntities>>> result;
            {
                Tests::AllocationProbe::ScopedFailure failure{failureIndex};
                result.emplace(
                    PrefabRestoreEntities::Create(std::move(input), std::move(pinned), fixture.fixture.provider, nullptr, nullptr, {}, {}));
            }
            REQUIRE(result.has_value());
            CHECK(result->HasError());
            CHECK(fixture.fixture.scenes.ActiveScene()->RuntimeId() == original);
            CHECK(fixture.fixture.scenes.ActiveScene()->SlotCount() == 0);
        }
    }

    TEST_CASE("Every actual private Scene prefab reconstruction allocation preserves the last published world",
              "[prefab][save][restore][allocation][rollback]") {
        Horo::Tests::RequireSupportedSceneAllocationInjection();
        RestoreFixture fixture;
        const auto original = fixture.fixture.scenes.ActiveScene()->RuntimeId();
        const auto queue = [&](std::optional<std::size_t> failure, Tests::AllocationProbe::Measurement *measured) {
            auto adapter = fixture.Adapter();
            REQUIRE(adapter.HasValue());
            auto observation = std::make_shared<Observation>();
            observation->failOwners = true;  // Always discard the successful measurement candidate through actual owner failure.
            std::unique_ptr<SceneAggregateRestore> aggregate = std::make_unique<Aggregate>(adapter.Value(), fixture.records, observation);
            SceneDefinitionBuilder builder{{7}, {1}};
            auto definition = std::move(builder).Build().Value();
            std::optional<Result<void>> result;
            if (failure) {
                Tests::AllocationProbe::ScopedFailure injected{*failure};
                result.emplace(fixture.fixture.scenes.QueuePreparationWithRestore(std::move(definition), std::move(aggregate)));
            } else {
                Tests::AllocationProbe::ScopedMeasurement measurement;
                result.emplace(fixture.fixture.scenes.QueuePreparationWithRestore(std::move(definition), std::move(aggregate)));
                *measured = measurement.Snapshot();
            }
            if (!failure) {
                INFO((result->HasError() ? result->ErrorValue().code.Value() + ": " + result->ErrorValue().message
                                         : "Actual measured private Scene queued"));
                REQUIRE(result->HasValue());
            }
            if (result->HasValue())
                fixture.fixture.Commit();
            CHECK(fixture.fixture.scenes.ActiveScene()->RuntimeId() == original);
            CHECK(fixture.fixture.scenes.ActiveScene()->SlotCount() == 0);
            CHECK(observation->rolledBack);
            CHECK_FALSE(observation->published);
            CHECK(adapter.Value()->PreparedInstances().empty());
            return result->HasError();
        };
        Tests::AllocationProbe::Measurement measured;
        CHECK_FALSE(queue({}, &measured));
        REQUIRE(measured.requests > 0);
        for (std::size_t failureIndex = 0; failureIndex < measured.requests; ++failureIndex) {
            INFO(failureIndex);
            CHECK(queue(failureIndex, nullptr));
        }
    }
}  // namespace Horo::Prefab

namespace Horo::Prefab {
    namespace {
        template <typename Identity> Identity StableWord(std::uint16_t value) {
            SaveIdentityDetail::Bytes bytes{};
            bytes[14] = static_cast<std::uint8_t>(value >> 8U);
            bytes[15] = static_cast<std::uint8_t>(value);
            return Identity::FromBytes(bytes).Value();
        }

        PrefabPersistentState ManyRecipes(const PrefabTemplateLease &lease) {
            PrefabPersistentState saved{Stable<SaveWorldId>(1), {}};
            for (std::uint16_t index = 0; index < 150; ++index) {
                auto recipe = Recipe(lease, static_cast<std::uint8_t>(index + 1), 1, 10'000 + index);
                recipe.members[0].record.identity = StableWord<PersistentEntityId>(1000 + index * 2);
                recipe.members[1].record.identity = StableWord<PersistentEntityId>(1001 + index * 2);
                if (index % 2)
                    recipe.creationLineage.insert(recipe.creationLineage.begin(), Test::Asset(3));
                saved.instances.push_back(std::move(recipe));
            }
            for (std::size_t index = 0; index < saved.instances.size(); ++index) {
                const auto &target = saved.instances[(index + 1) % saved.instances.size()].members[1].record;
                saved.instances[index].bindings = {
                    {PrefabPropertyId::Create(11).Value(), {{saved.world, target.identity}, target.generation}}};
            }
            return saved;
        }

        std::vector<PersistentEntityRecord> RecipeRecords(const PrefabPersistentState &saved) {
            std::vector<PersistentEntityRecord> records;
            for (const auto &recipe : saved.instances)
                for (const auto &member : recipe.members)
                    records.push_back(member.record);
            return records;
        }

        Result<void> QueueRecipes(TestSupport::Fixture &fixture, const PrefabPersistentState &saved, const PrefabTemplateLease &lease,
                                  std::shared_ptr<Observation> observation) {
            auto adapter = PrefabRestoreEntities::Create(saved, std::vector<PrefabTemplateLease>(saved.instances.size(), lease),
                                                         fixture.provider, nullptr, nullptr, {}, {});
            REQUIRE(adapter.HasValue());
            SceneDefinitionBuilder builder{{7}, {1}};
            return fixture.scenes.QueuePreparationWithRestore(std::move(builder).Build().Value(),
                                                              std::make_unique<Aggregate>(adapter.Value(), RecipeRecords(saved),
                                                                                          std::move(observation)));
        }
    }  // namespace

    TEST_CASE("Private restore admits more than256 members with independent canonically shared lineage and cyclic bindings",
              "[prefab][save][restore][aggregate-bounds]") {
        RestoreFixture fixture;
        const auto saved = ManyRecipes(fixture.lease);
        auto observation = std::make_shared<Observation>();
        RequireQueued(QueueRecipes(fixture.fixture, saved, fixture.lease, observation));
        fixture.fixture.Commit();
        REQUIRE(observation->published);
        REQUIRE(observation->bindings.size() == 300);
        const auto scene = *fixture.fixture.scenes.ActiveScene();
        const auto identities = PersistentEntityIdentityMap::Create(scene.RuntimeId(), RecipeRecords(saved), observation->bindings).Value();
        const auto first = scene.Get(identities.Resolve(saved.instances[0].members[0].record.identity, {1}).Value()).Value();
        const auto second = scene.Get(identities.Resolve(saved.instances[1].members[0].record.identity, {1}).Value()).Value();
        const auto third = scene.Get(identities.Resolve(saved.instances[2].members[0].record.identity, {1}).Value()).Value();
        CHECK(std::ranges::equal(first.groupSpawnLineage, saved.instances[0].creationLineage));
        CHECK(std::ranges::equal(second.groupSpawnLineage, saved.instances[1].creationLineage));
        CHECK(first.groupSpawnLineage.data() == third.groupSpawnLineage.data());
        REQUIRE(std::holds_alternative<RuntimeGroupExternalReference>(first.groupReferences[0].target));
        CHECK(std::get<RuntimeGroupExternalReference>(first.groupReferences[0].target).entity ==
              identities.Resolve(saved.instances[1].members[1].record.identity, {1}).Value());
        SceneCommandBuffer ordinary;
        CHECK(ordinary
                  .CreateGroup(std::vector<RuntimeEntityGroupEntry>(300), {},
                               {scene.RuntimeId(), fixture.fixture.registry.Snapshot().Revision(), {}})
                  .HasError());
    }

    TEST_CASE("Private restore respects exact lower bundle and live host resource budgets before changing the active Scene",
              "[prefab][save][restore][aggregate-bounds][rollback]") {
        RuntimeSceneAssetLimits hostLimits;
        auto observation = std::make_shared<Observation>();
        SECTION("lower actual host dependency count") {
            hostLimits.maximumDependencies = 1;
        }
        SECTION("lower actual host resource bytes") {
            hostLimits.maximumResidentBytes = 1;
        }
        SECTION("lower actual admitted entity count") {
            observation->limits.maximumEntities = 299;
        }
        SECTION("lower actual admitted interface count") {
            observation->limits.maximumReferences = 149;
        }
        SECTION("lower actual admitted lineage work") {
            observation->limits.maximumLineageEntries = 449;
        }
        TestSupport::Fixture fixture{{}, std::make_unique<Tests::GameplayStructuralUnitObserver>(), hostLimits};
        fixture.root = CookedWithInterface(fixture);
        fixture.bytes.Memory().Insert(Test::Asset(), fixture.root);
        const auto lease = fixture.Ready();
        const auto original = fixture.scenes.ActiveScene()->RuntimeId();
        CHECK(QueueRecipes(fixture, ManyRecipes(lease), lease, observation).HasError());
        CHECK_FALSE(observation->published);
        CHECK(fixture.scenes.ActiveScene()->RuntimeId() == original);
        CHECK(fixture.scenes.ActiveScene()->SlotCount() == 0);
    }
}  // namespace Horo::Prefab

namespace Horo::Prefab {
    namespace {
        std::vector<SceneRestoreGroupPartition> DeclaredPartitions(const PrefabPersistentState &saved) {
            std::vector<SceneRestoreGroupPartition> partitions;
            std::size_t slot{};
            for (const auto &recipe : saved.instances) {
                SceneRestoreGroupPartition partition{saved.world, recipe.identity, slot, {}};
                for (const auto &member : recipe.members)
                    partition.members.push_back({slot++, member.record.identity, member.record.generation.value});
                partitions.push_back(std::move(partition));
            }
            return partitions;
        }
    }  // namespace

    TEST_CASE("Scene restore partition inventory rejects empty duplicated forged mixed and cross-occurrence declarations",
              "[prefab][save][restore][partitions][malformed]") {
        RestoreFixture fixture;
        auto partitions = DeclaredPartitions(fixture.saved);
        SECTION("empty member set") {
            partitions[0].members.clear();
        }
        SECTION("duplicate occurrence token") {
            partitions[1].occurrence = partitions[0].occurrence;
        }
        SECTION("foreign world") {
            partitions[0].world = Stable<SaveWorldId>(99);
        }
        SECTION("unknown persistent member") {
            partitions[0].members[0].identity = Stable<PersistentEntityId>(99);
        }
        SECTION("stale persistent incarnation") {
            ++partitions[0].members[0].generation;
        }
        SECTION("cross-recipe member") {
            std::swap(partitions[0].members[0], partitions[1].members[0]);
        }
        SECTION("mixed duplicated member") {
            partitions[1].members[0] = partitions[0].members[0];
        }
        SECTION("duplicate dense slot") {
            partitions[1].members[0].entity = partitions[0].members[0].entity;
        }
        SECTION("missing live member") {
            partitions[0].members.pop_back();
        }
        SECTION("forged root") {
            partitions[0].root = 16'384;
        }
        CHECK(ValidateSceneRestorePartitions(fixture.saved.world, fixture.records, partitions, 16'384).HasError());
        CHECK(fixture.fixture.scenes.ActiveScene()->SlotCount() == 0);
    }
}  // namespace Horo::Prefab
