#include "Horo/PCG/PCGErrors.h"
#include "Horo/PCGScene/SceneOutputAdapter.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::PCG {
    namespace {
        template <typename T> T Id(std::uint64_t value) {
            return T::Create(value).Value();
        }

        PCGGenerationDigest Digest(std::uint8_t value) {
            PCGGenerationDigest d;
            d.bytes.fill(value);
            return d;
        }

        struct Fixture {
            ExecutionId execution{{Id<GraphId>(1), Id<GraphRevision>(1)}, Id<ExecutionValue>(1)};
            PCGCapabilitySet capabilities = PCGCapabilitySet::Create(std::array{PCGCapability::SceneOutput}).Value();
            PCGGenerationTargetReceipt receipt{Id<GenerationValidationReceiptId>(1), Id<GenerationTargetOwnerId>(1), 1, 1, capabilities};
            PCGGeneratedOutputProvenance provenance{Id<GenerationLogicalOutputId>(1),
                                                    {execution, Id<NodeId>(1), Id<PinId>(1), Id<SourceSampleId>(1), 0},
                                                    Id<GenerationLineageId>(1),
                                                    Id<GeneratedSetId>(1),
                                                    1,
                                                    1,
                                                    receipt.owner,
                                                    Id<GenerationCellId>(1)};
            PCGSceneOutputIntent intent{provenance.logicalOutput, {.id = {10}, .name = "generated"}, {}};
            PCGGenerationPlanCandidate candidate{CurrentPCGGenerationPlanVersion,
                                                 Id<GenerationPlanId>(1),
                                                 execution,
                                                 8,
                                                 provenance.lineage,
                                                 provenance.set,
                                                 1,
                                                 provenance.cell,
                                                 receipt.owner,
                                                 capabilities,
                                                 receipt,
                                                 {},
                                                 {}};
            PCGSceneOutputContext context{{PCGGenerationPlanAdmission::Accepting, receipt, {}, {}}, {}, {}};

            Fixture() {
                SetContent();
            }

            void SetContent() {
                auto digest = FingerprintPCGSceneOutput(intent.desired, 1024 * 1024);
                REQUIRE(digest.HasValue());
                candidate.outputs = {{PCGOutputDeltaKind::Create, provenance, 0, {}, digest.Value(), {}, {1, 1, 1, 1}}};
            }

            PCGGenerationPlan Plan() const {
                auto plan = CreatePCGGenerationPlan(candidate, context.generation);
                REQUIRE(plan.HasValue());
                return plan.Value();
            }

            Result<PCGSceneOutputBatch> Translate() const {
                return TranslatePCGSceneOutputs(Plan(), std::span{&intent, 1}, context);
            }
        };

        template <typename T> void Failure(const Result<T> &result, const ErrorCodeDescriptor &code) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().domain.Value() == code.domain.Value());
            CHECK(result.ErrorValue().code.Value() == code.code.Value());
        }
    }  // namespace

    TEST_CASE("PCG scene translation is detached deterministic and owns typed component values", "[pcg][scene-adapter]") {
        Fixture f;
        f.intent.desired.localTransform.translation = {1, 2, 3};
        f.intent.desired.components.light = Runtime::LightComponent{};
        f.SetContent();
        auto a = f.Translate();
        auto b = f.Translate();
        REQUIRE(a.HasValue());
        REQUIRE(b.HasValue());
        CHECK(a.Value().Commands()[0].object == b.Value().Commands()[0].object);
        CHECK(a.Value().Commands()[0].delta == b.Value().Commands()[0].delta);
        CHECK(a.Value().Plan().Target() == f.receipt);
        CHECK(a.Value().ChargedBytes() > sizeof(PCGSceneOutputCommand));
        f.intent.desired.name = "edited after staging";
        CHECK(a.Value().Commands()[0].desired->name == "generated");
        CHECK(a.Value().Commands()[0].desired->components.light.has_value());
        CHECK(a.Value().Commands()[0].desired->localTransform.translation == Math::Vec3{1, 2, 3});
    }

    TEST_CASE("PCG static mesh receipt binds exact asset dependency and payload", "[pcg][scene-adapter]") {
        Fixture f;
        std::array<std::uint8_t, 16> bytes{};
        bytes[0] = 1;
        const auto asset = Assets::AssetId::FromBytes(bytes);
        const PCGGenerationDependency dependency{Id<GenerationDependencyId>(3), 2, Digest(4)};
        f.intent.desired.meshAsset = asset;
        f.intent.mesh = PCGSceneMeshReceipt{asset, dependency};
        f.SetContent();
        f.candidate.dependencies = {dependency};
        f.candidate.outputs[0].dependencies = {dependency.id};
        auto result = f.Translate();
        REQUIRE(result.HasValue());
        CHECK(result.Value().Commands()[0].desired->meshAsset == asset);
        CHECK(result.Value().Commands()[0].mesh->dependency == dependency);
        f.intent.mesh->dependency.revision++;
        Failure(f.Translate(), PCGErrors::GenerationPlanInvalid);
        f.intent.mesh.reset();
        Failure(f.Translate(), PCGErrors::GenerationPlanInvalid);
    }

    TEST_CASE("PCG scene rejects stale receipts and closed lifecycle before translation", "[pcg][scene-adapter]") {
        Fixture f;
        auto plan = f.Plan();
        f.context.generation.currentTarget.ownerGeneration++;
        Failure(TranslatePCGSceneOutputs(plan, std::span{&f.intent, 1}, f.context), PCGErrors::GenerationPlanStale);
        for (auto admission : {PCGGenerationPlanAdmission::CancellationRequested, PCGGenerationPlanAdmission::ShuttingDown}) {
            f.context.generation.admission = admission;
            Failure(TranslatePCGSceneOutputs(plan, std::span{&f.intent, 1}, f.context), PCGErrors::GenerationPlanLifecycleUnavailable);
        }
    }

    TEST_CASE("PCG scene rejects altered payload and ambiguous intents", "[pcg][scene-adapter]") {
        Fixture f;
        auto plan = f.Plan();
        f.intent.desired.name = "different";
        Failure(TranslatePCGSceneOutputs(plan, std::span{&f.intent, 1}, f.context), PCGErrors::GenerationPlanStale);
        Failure(TranslatePCGSceneOutputs(plan, {}, f.context), PCGErrors::GenerationPlanInvalid);
        const std::array duplicate{f.intent, f.intent};
        Failure(TranslatePCGSceneOutputs(plan, duplicate, f.context), PCGErrors::GenerationPlanCapacityExceeded);
    }

    TEST_CASE("PCG scene protects authored identities and bounded retained storage", "[pcg][scene-adapter]") {
        Fixture f;
        const std::array occupied{f.intent.desired.id};
        f.context.occupiedObjects = occupied;
        Failure(f.Translate(), PCGErrors::GenerationOwnershipMismatch);
        f.context.occupiedObjects = {};
        f.context.maximumRetainedBytes = 1;
        Failure(f.Translate(), PCGErrors::GenerationPlanCapacityExceeded);
        f.context.maximumRetainedBytes = 16 * 1024 * 1024;
        f.context.maximumPayloadBytes = 1;
        Failure(f.Translate(), PCGErrors::GenerationPlanCapacityExceeded);
    }

    TEST_CASE("PCG scene fingerprint rejects malformed transform and mesh combinations", "[pcg][scene-adapter]") {
        Fixture f;
        f.intent.desired.localTransform.translation.x = std::numeric_limits<float>::infinity();
        CHECK(FingerprintPCGSceneOutput(f.intent.desired, 1024 * 1024).HasError());
        f.intent.desired.localTransform = {};
        f.intent.desired.parent = f.intent.desired.id;
        Failure(FingerprintPCGSceneOutput(f.intent.desired, 1024 * 1024), PCGErrors::GenerationPlanInvalid);
        f.intent.desired.parent.reset();
        f.intent.desired.meshAsset = Assets::AssetId{};
        Failure(FingerprintPCGSceneOutput(f.intent.desired, 1024 * 1024), PCGErrors::GenerationPlanInvalid);
    }

    TEST_CASE("PCG scene validates hierarchy and sorted complete occupied snapshot", "[pcg][scene-adapter]") {
        Fixture f;
        f.intent.desired.parent = SceneSource::SceneObjectId{30};
        f.SetContent();
        Failure(f.Translate(), PCGErrors::GenerationPlanInvalid);
        const std::array occupied{SceneSource::SceneObjectId{30}};
        f.context.occupiedObjects = occupied;
        REQUIRE(f.Translate().HasValue());
        const std::array unordered{SceneSource::SceneObjectId{30}, SceneSource::SceneObjectId{20}};
        f.context.occupiedObjects = unordered;
        Failure(f.Translate(), PCGErrors::GenerationPlanInvalid);
    }

    TEST_CASE("PCG scene malformed UTF8 and excess component payload produce typed failures", "[pcg][scene-adapter]") {
        Fixture f;
        f.intent.desired.name = std::string(1, static_cast<char>(0xFF));
        Failure(FingerprintPCGSceneOutput(f.intent.desired, 1024), PCGErrors::GenerationPlanInvalid);
        f.intent.desired.name = "valid";
        f.intent.desired.components.colliders.resize(129);
        Failure(FingerprintPCGSceneOutput(f.intent.desired, 1024), PCGErrors::GenerationPlanCapacityExceeded);
    }

    TEST_CASE("PCG scene staged hierarchy rejects a cycle and preserves canonical output ordering", "[pcg][scene-adapter]") {
        Fixture f;
        auto second = f.intent;
        second.logical = Id<GenerationLogicalOutputId>(2);
        second.desired.id = {20};
        second.desired.parent = f.intent.desired.id;
        f.intent.desired.parent = second.desired.id;
        f.SetContent();
        auto delta = f.candidate.outputs[0];
        delta.provenance.logicalOutput = second.logical;
        delta.desiredContent = FingerprintPCGSceneOutput(second.desired, 1024 * 1024).Value();
        f.candidate.outputs.insert(f.candidate.outputs.begin(), delta);
        std::array intents{second, f.intent};
        Failure(TranslatePCGSceneOutputs(f.Plan(), intents, f.context), PCGErrors::GenerationPlanInvalid);
        f.intent.desired.parent.reset();
        f.candidate.outputs[1].desiredContent = FingerprintPCGSceneOutput(f.intent.desired, 1024 * 1024).Value();
        intents[1] = f.intent;
        auto valid = TranslatePCGSceneOutputs(f.Plan(), intents, f.context);
        REQUIRE(valid.HasValue());
        CHECK(valid.Value().Commands()[0].delta.provenance.logicalOutput == f.intent.logical);
        CHECK(valid.Value().Commands()[1].desired->parent == f.intent.desired.id);
    }

    TEST_CASE("PCG scene update and removal preserve exact stable mapping and retained batches", "[pcg][scene-adapter]") {
        Fixture f;
        auto old = f.Translate();
        REQUIRE(old.HasValue());
        const PCGOwnedGeneratedOutput owned{f.provenance, f.candidate.outputs[0].desiredContent};
        const std::array ownedValues{owned};
        const std::array mapping{PCGSceneOutputMapping{f.intent.logical, f.intent.desired.id}};
        const std::array occupied{f.intent.desired.id};
        f.context.generation.currentOutputs = ownedValues;
        f.context.currentMappings = mapping;
        f.context.occupiedObjects = occupied;
        f.intent.desired.name = "replacement";
        f.SetContent();
        f.candidate.setRevision = 2;
        f.candidate.outputs[0].provenance.setRevision = 2;
        f.candidate.outputs[0].kind = PCGOutputDeltaKind::Update;
        f.candidate.outputs[0].expectedSetRevision = 1;
        f.candidate.outputs[0].priorContent = owned.content;
        auto updated = f.Translate();
        REQUIRE(updated.HasValue());
        CHECK(updated.Value().Commands()[0].object == old.Value().Commands()[0].object);
        CHECK(old.Value().Commands()[0].desired->name == "generated");
        f.candidate.outputs[0].kind = PCGOutputDeltaKind::Remove;
        f.candidate.outputs[0].desiredContent = {};
        auto removed = TranslatePCGSceneOutputs(f.Plan(), {}, f.context);
        REQUIRE(removed.HasValue());
        CHECK_FALSE(removed.Value().Commands()[0].desired.has_value());
        f.context.currentMappings = {};
        Failure(TranslatePCGSceneOutputs(f.Plan(), {}, f.context), PCGErrors::GenerationOwnershipMismatch);
    }
}  // namespace Horo::PCG
