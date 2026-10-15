#include "Horo/PCGScene/SceneBake.h"
#include "Horo/Scene/SceneSource.h"
#include "PCGDocumentTestSupport.h"

#include <array>
#include <limits>
#include <thread>

namespace Horo::PCG {
    namespace {
        namespace Doc = Editor::PCGDocumentTestSupport;

        template <typename T> T Id(const std::uint64_t value) {
            return T::Create(value).Value();
        }

        Editor::PCGGraphDocument OpenGraph() {
            auto opened = Editor::PCGGraphDocument::Open(Doc::Asset(), Doc::Identity(), {Doc::Asset(9), 1}, Doc::Capture(), Doc::Catalog());
            REQUIRE(opened.HasValue());
            return std::move(opened).Value();
        }

        Editor::SceneTransactionContext Fence(const Editor::SceneTransactionSnapshot &v) {
            return {v.session, v.revision, v.state, true, {}};
        }

        std::string Bytes(const Editor::SceneTransactionSnapshot &v) {
            return SceneSource::EncodeSceneSource({v.objects, v.prefabInstances});
        }

        struct BakeFixture final {
            Editor::PCGGraphDocument graph{OpenGraph()};
            Editor::SceneDocumentTransactions target{
                std::move(Editor::SceneDocumentTransactions::Create({Doc::Asset(2), 1}, {{{1}, {}, "hand authored"}})).Value()};
            PCGSceneBakeBinding binding{Id<GenerationTargetOwnerId>(7),
                                        1,
                                        PCGCapabilitySet::Create(std::array{PCGCapability::SceneOutput, PCGCapability::OfflineBake})
                                            .Value(),
                                        Id<GenerationDependencyId>(99),
                                        {Id<GenerationLineageId>(1), Id<GeneratedSetId>(1), Id<GenerationCellId>(1)}};
            Editor::PCGDocumentSnapshot graphCapture{graph.Snapshot().Value()};

            PCGSceneBakeContext Context() const {
                const auto now = target.Snapshot();
                REQUIRE(now.HasValue());
                return {binding, Fence(now.Value()), {}, {}};
            }

            PCGGeneratedOutputProvenance Provenance(const ExecutionId execution, const std::uint64_t revision) const {
                return {Id<GenerationLogicalOutputId>(1),
                        {execution, Id<NodeId>(1), Id<PinId>(2), Id<SourceSampleId>(1), 0},
                        binding.scope.lineage,
                        binding.scope.set,
                        revision,
                        1,
                        binding.owner,
                        binding.scope.cell};
            }

            PCGSceneOutputBatch Batch(const PCGOutputDeltaKind kind = PCGOutputDeltaKind::Create, std::string name = "generated",
                                      const std::uint64_t revision = 1) const {
                const auto capture = CapturePCGSceneBakeTarget(target, binding);
                REQUIRE(capture.HasValue());
                const auto &view = capture.Value();
                const ExecutionId execution{graphCapture.source->Data().generation, Id<ExecutionValue>(revision)};
                const auto provenance = Provenance(execution, revision);
                const auto input = CapturePCGSceneGraphInput(graphCapture, binding.graphSource);
                REQUIRE(input.HasValue());
                PCGSceneOutputIntent intent{provenance.logicalOutput, {.id = {10}, .name = std::move(name)}, {}};
                PCGOutputDelta delta{kind,
                                     provenance,
                                     kind == PCGOutputDeltaKind::Create ? 0 : revision - 1,
                                     {},
                                     {},
                                     {input.Value().id},
                                     {1, 65536, 65536, 65536}};
                if (kind != PCGOutputDeltaKind::Create) {
                    REQUIRE(view.generated.size() == 1);
                    delta.priorContent = view.generated[0].content;
                }
                if (kind != PCGOutputDeltaKind::Remove)
                    delta.desiredContent = FingerprintPCGSceneOutput(intent.desired, 1024 * 1024).Value();
                PCGGenerationPlanCandidate candidate{.plan = Id<GenerationPlanId>(revision),
                                                     .execution = execution,
                                                     .seed = graphCapture.source->Data().deterministicSeed,
                                                     .lineage = binding.scope.lineage,
                                                     .set = binding.scope.set,
                                                     .setRevision = revision,
                                                     .cell = binding.scope.cell,
                                                     .targetOwner = binding.owner,
                                                     .requiredCapabilities = binding.capabilities,
                                                     .validation = view.receipt,
                                                     .dependencies = {input.Value()},
                                                     .outputs = {delta}};
                const PCGGenerationPlanContext planContext{PCGGenerationPlanAdmission::Accepting, view.receipt, view.generated, {}};
                const auto plan = CreatePCGGenerationPlan(std::move(candidate), planContext);
                REQUIRE(plan.HasValue());
                const auto intents = kind == PCGOutputDeltaKind::Remove ? std::span<const PCGSceneOutputIntent>{}
                                                                        : std::span<const PCGSceneOutputIntent>{&intent, 1};
                const auto translated = TranslatePCGSceneOutputs(plan.Value(), intents, {planContext, view.mappings, view.occupied});
                REQUIRE(translated.HasValue());
                return translated.Value();
            }

            Result<PCGSceneBakeResult> Bake(const PCGSceneOutputBatch &batch) {
                return BakePCGSceneOutputs(graph, graphCapture, target, batch, Context());
            }
        };
    }  // namespace

    TEST_CASE("Offline PCG bake commits real authored generation and one exact undo redo history entry", "[pcg][bake][history]") {
        BakeFixture f;
        REQUIRE(f.graph.Execute(Doc::Edit(f.graphCapture, 12)).HasValue());
        f.graphCapture = f.graph.Snapshot().Value();
        const auto graphBefore = f.graphCapture;
        const auto before = f.target.Snapshot().Value();
        const auto batch = f.Batch();
        const auto baked = f.Bake(batch);
        REQUIRE(baked.HasValue());
        CHECK(baked.Value().target.committed);
        CHECK(baked.Value().fromUnsaved);
        CHECK(baked.Value().graphRevision == graphBefore.revision);
        const auto after = f.target.Snapshot().Value();
        REQUIRE(after.objects.size() == 2);
        CHECK(after.objects[0].name == "hand authored");
        CHECK(after.objects[1].generated.has_value());
        CHECK(after.revision == before.revision + 1);
        CHECK(after.dirty);
        const auto persisted = SceneSource::DecodeSceneSource(Bytes(after));
        REQUIRE(persisted.HasValue());
        CHECK(persisted.Value().objects[1].generated == after.objects[1].generated);
        REQUIRE(f.target.Replay(Fence(after), false).HasValue());
        const auto undone = f.target.Snapshot().Value();
        CHECK(Bytes(undone) == Bytes(before));
        CHECK_FALSE(undone.dirty);
        CHECK(f.target.Replay(Fence(undone), false).HasError());
        REQUIRE(f.target.Replay(Fence(undone), true).HasValue());
        CHECK(Bytes(f.target.Snapshot().Value()) == Bytes(after));
        CHECK(f.graph.Snapshot().Value().state == graphBefore.state);
        CHECK(f.graph.Snapshot().Value().dirty);
    }

    TEST_CASE("Offline PCG exact repeated bake is idempotent and does not add history", "[pcg][bake][idempotent]") {
        BakeFixture f;
        const auto batch = f.Batch();
        REQUIRE(f.Bake(batch).HasValue());
        const auto before = f.target.Snapshot().Value();
        const auto repeated = f.Bake(batch);
        REQUIRE(repeated.HasValue());
        CHECK_FALSE(repeated.Value().target.committed);
        CHECK(f.target.Snapshot().Value().revision == before.revision);
        REQUIRE(f.target.Replay(Fence(before), false).HasValue());
        const auto undone = f.target.Snapshot().Value();
        CHECK(undone.objects.size() == 1);
        CHECK(f.target.Replay(Fence(undone), false).HasError());
    }

    TEST_CASE("Offline PCG replacement and removal replay exact prior generated values", "[pcg][bake][replacement]") {
        BakeFixture f;
        REQUIRE(f.Bake(f.Batch()).HasValue());
        const auto original = f.target.Snapshot().Value();
        REQUIRE(f.target.MarkSaved(Fence(original)).HasValue());
        REQUIRE(f.Bake(f.Batch(PCGOutputDeltaKind::Update, "replacement", 2)).HasValue());
        const auto replaced = f.target.Snapshot().Value();
        CHECK(replaced.objects[1].name == "replacement");
        REQUIRE(f.target.Replay(Fence(replaced), false).HasValue());
        CHECK(Bytes(f.target.Snapshot().Value()) == Bytes(original));
        CHECK_FALSE(f.target.Snapshot().Value().dirty);
        REQUIRE(f.target.Replay(Fence(f.target.Snapshot().Value()), true).HasValue());
        REQUIRE(f.Bake(f.Batch(PCGOutputDeltaKind::Remove, {}, 3)).HasValue());
        const auto removed = f.target.Snapshot().Value();
        CHECK(removed.objects.size() == 1);
        REQUIRE(f.target.Replay(Fence(removed), false).HasValue());
        CHECK(Bytes(f.target.Snapshot().Value()) == Bytes(replaced));
    }

    TEST_CASE("Offline PCG stale cancelled unauthorized and over budget bakes publish nothing", "[pcg][bake][failure]") {
        BakeFixture f;
        const auto batch = f.Batch();
        const auto before = f.target.Snapshot().Value();
        auto context = f.Context();
        CancellationSource cancellation;
        SECTION("cancelled") {
            cancellation.RequestCancellation();
            context.target.cancellation = cancellation.Token();
        }
        SECTION("unauthorized") {
            context.target.canEdit = false;
        }
        SECTION("stale target") {
            ++context.target.revision;
        }
        SECTION("bounded reservation") {
            context.limits.maximumPreparedBytes = 1;
        }
        SECTION("graph edited after evaluation") {
            REQUIRE(f.graph.Execute(Doc::Edit(f.graphCapture, 99)).HasValue());
        }
        SECTION("graph closed") {
            REQUIRE(f.graph.Close().HasValue());
        }
        CHECK(BakePCGSceneOutputs(f.graph, f.graphCapture, f.target, batch, context).HasError());
        const auto after = f.target.Snapshot().Value();
        CHECK(Bytes(after) == Bytes(before));
        CHECK(after.revision == before.revision);
        CHECK(after.state == before.state);
        CHECK_FALSE(after.dirty);
        CHECK(f.target.Replay(Fence(after), false).HasError());
    }

    TEST_CASE("Offline PCG late full hierarchy failure preserves generated and hand authored content", "[pcg][bake][rollback]") {
        BakeFixture f;
        REQUIRE(f.Bake(f.Batch()).HasValue());
        const auto owned = f.target.Snapshot().Value();
        const std::array addChild{Editor::SceneObjectReplacement{{2},
                                                                 {},
                                                                 {},
                                                                 SceneSource::SceneObjectSnapshot{.id = {2},
                                                                                                  .parent = SceneSource::SceneObjectId{10},
                                                                                                  .name = "authored child"},
                                                                 {}}};
        REQUIRE(f.target.Execute(Fence(owned), addChild).HasValue());
        const auto before = f.target.Snapshot().Value();
        const auto removal = f.Batch(PCGOutputDeltaKind::Remove, {}, 2);
        CHECK(f.Bake(removal).HasError());
        const auto after = f.target.Snapshot().Value();
        CHECK(Bytes(after) == Bytes(before));
        CHECK(after.revision == before.revision);
        CHECK(after.state == before.state);
        CHECK(after.dirty == before.dirty);
        REQUIRE(f.target.Replay(Fence(after), false).HasValue());
        CHECK(f.target.Snapshot().Value().objects.size() == 2);
    }

    TEST_CASE("Offline PCG refuses adoption or unrelated ownership instead of broad cleanup", "[pcg][bake][ownership]") {
        BakeFixture f;
        const auto original = f.Batch();
        REQUIRE(f.Bake(original).HasValue());
        const auto before = f.target.Snapshot().Value();
        auto adopted = before.objects[1];
        adopted.generated.reset();
        adopted.name = "adopted authored object";
        const std::array changes{Editor::SceneObjectReplacement{adopted.id,
                                                                FingerprintPCGSceneOutput(before.objects[1], 1024 * 1024).Value(),
                                                                before.objects[1].generated,
                                                                adopted,
                                                                {65536, 65536, 65536}}};
        REQUIRE(f.target.Execute(Fence(before), changes).HasValue());
        const auto after = f.target.Snapshot().Value();
        CHECK(f.Bake(original).HasError());
        CHECK(Bytes(f.target.Snapshot().Value()) == Bytes(after));
        CHECK(after.objects[1].name == "adopted authored object");
    }

    TEST_CASE("Scene aggregate malformed late values and stale replay fences retain exact state", "[scene][bake][boundary]") {
        BakeFixture f;
        const auto before = f.target.Snapshot().Value();
        SceneSource::SceneObjectSnapshot first{.id = {2}, .name = "valid"};
        SceneSource::SceneObjectSnapshot bad{.id = {3}, .name = "invalid"};
        bad.localTransform.translation.x = std::numeric_limits<float>::infinity();
        const std::array changes{Editor::SceneObjectReplacement{first.id, {}, {}, first, {}},
                                 Editor::SceneObjectReplacement{bad.id, {}, {}, bad, {}}};
        CHECK(f.target.Execute(Fence(before), changes).HasError());
        CHECK(Bytes(f.target.Snapshot().Value()) == Bytes(before));
        CHECK_FALSE(f.target.Snapshot().Value().dirty);
        REQUIRE(f.Bake(f.Batch()).HasValue());
        CHECK(f.target.Replay(Fence(before), false).HasError());
        CHECK(f.target.Snapshot().Value().objects.size() == 2);
    }

    TEST_CASE("Scene generated ownership codec rejects malformed evidence and preserves legacy bytes", "[scene][bake][codec]") {
        BakeFixture f;
        const auto legacy = Bytes(f.target.Snapshot().Value());
        CHECK(legacy.find("generated") == std::string::npos);
        REQUIRE(f.Bake(f.Batch()).HasValue());
        auto source = f.target.Snapshot().Value();
        source.objects[1].generated->ownershipGeneration = 0;
        CHECK(SceneSource::DecodeSceneSource(Bytes(source)).HasError());
        source = f.target.Snapshot().Value();
        source.objects[1].generated->content = {};
        CHECK(SceneSource::DecodeSceneSource(Bytes(source)).HasError());
    }

    TEST_CASE("Offline PCG lifecycle closes admission while historical undo remains graph independent", "[pcg][bake][lifecycle]") {
        BakeFixture f;
        const auto batch = f.Batch();
        const auto context = f.Context();
        SECTION("target shutdown") {
            f.target.Shutdown();
            CHECK(BakePCGSceneOutputs(f.graph, f.graphCapture, f.target, batch, context).HasError());
        }
        SECTION("capability generation changed") {
            auto stale = context;
            ++stale.binding.capabilityGeneration;
            CHECK(BakePCGSceneOutputs(f.graph, f.graphCapture, f.target, batch, stale).HasError());
            CHECK_FALSE(f.target.Snapshot().Value().dirty);
        }
        SECTION("foreign owner thread") {
            std::optional<Result<PCGSceneBakeResult>> result;
            std::thread other([&] {
                result = BakePCGSceneOutputs(f.graph, f.graphCapture, f.target, batch, context);
            });
            other.join();
            REQUIRE(result.has_value());
            CHECK(result->HasError());
            CHECK_FALSE(f.target.Snapshot().Value().dirty);
        }
        SECTION("graph owner is closed after successful bake") {
            REQUIRE(f.Bake(batch).HasValue());
            const auto after = f.target.Snapshot().Value();
            REQUIRE(f.graph.Close().HasValue());
            REQUIRE(f.target.Replay(Fence(after), false).HasValue());
            REQUIRE(f.target.Replay(Fence(f.target.Snapshot().Value()), true).HasValue());
            CHECK(Bytes(f.target.Snapshot().Value()) == Bytes(after));
        }
    }

    TEST_CASE("Scene generation ownership rejects duplicate logical records before target publication", "[scene][bake][ownership]") {
        BakeFixture f;
        REQUIRE(f.Bake(f.Batch()).HasValue());
        auto snapshot = f.target.Snapshot().Value();
        auto duplicate = snapshot.objects[1];
        duplicate.id = {20};
        snapshot.objects.push_back(std::move(duplicate));
        CHECK(Editor::SceneDocumentTransactions::Create({Doc::Asset(3), 1}, snapshot.objects).HasError());
        CHECK(f.target.Snapshot().Value().objects.size() == 2);
    }

    TEST_CASE("Scene Transaction Rejects A Duplicate After A No Op", "[unit][pcg][scene][bake]") {
        BakeFixture fixture;
        const auto before = fixture.target.Snapshot().Value();
        const auto &object = before.objects.front();
        const auto content = Editor::FingerprintSceneObjectContent(object, 1024 * 1024).Value();
        auto desired = object;
        desired.name = "conflicting second submission";
        const std::array changes{Editor::SceneObjectReplacement{object.id, content, {}, object, {}},
                                 Editor::SceneObjectReplacement{object.id, content, {}, desired, {}}};
        const auto rejected = fixture.target.Execute(Fence(before), changes);
        REQUIRE(rejected.HasError());
        REQUIRE(rejected.ErrorValue().code.Value() == Editor::SceneTransactionError(Editor::SceneTransactionFailure::Invalid).code.Value());
        const auto after = fixture.target.Snapshot().Value();
        REQUIRE(Bytes(after) == Bytes(before));
        REQUIRE(after.revision == before.revision);
        REQUIRE(after.state == before.state);
        REQUIRE(after.dirty == before.dirty);
        REQUIRE(fixture.target.Replay(Fence(after), false).HasError());
    }

    TEST_CASE("Scene Mixed Replacements Preserve Authored Ordering Through Replay", "[unit][pcg][scene][bake]") {
        std::vector<SceneSource::SceneObjectSnapshot> baseline{{.id = {1}, .name = "A"},
                                                               {.id = {2}, .name = "B"},
                                                               {.id = {3}, .name = "C"}};
        auto opened = Editor::SceneDocumentTransactions::Create({Doc::Asset(2), 1}, baseline);
        REQUIRE(opened.HasValue());
        auto target = std::move(opened).Value();
        const auto before = target.Snapshot().Value();
        auto updated = baseline[1];
        updated.name = "updated B";
        const std::array
            changes{Editor::SceneObjectReplacement{{1}, Editor::FingerprintSceneObjectContent(baseline[0], 65536).Value(), {}, {}, {}},
                    Editor::SceneObjectReplacement{{2}, Editor::FingerprintSceneObjectContent(baseline[1], 65536).Value(), {}, updated, {}},
                    Editor::SceneObjectReplacement{{4}, {}, {}, SceneSource::SceneObjectSnapshot{.id = {4}, .name = "D"}, {}}};
        REQUIRE(target.Execute(Fence(before), changes).HasValue());
        const auto after = target.Snapshot().Value();
        REQUIRE(after.objects.size() == 3);
        REQUIRE(after.objects[0].id.value == 2);
        REQUIRE(after.objects[1].id.value == 3);
        REQUIRE(after.objects[2].id.value == 4);
        REQUIRE(target.Replay(Fence(after), false).HasValue());
        const auto undone = target.Snapshot().Value();
        REQUIRE(Bytes(undone) == Bytes(before));
        REQUIRE(target.Replay(Fence(undone), true).HasValue());
        REQUIRE(Bytes(target.Snapshot().Value()) == Bytes(after));
    }
}  // namespace Horo::PCG
