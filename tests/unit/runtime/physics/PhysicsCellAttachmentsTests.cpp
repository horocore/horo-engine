#include "../world_streaming/StreamingCellCandidateTestSupport.h"
#include "Horo/Physics/PhysicsCellAttachments.h"
#include "Horo/Physics/PhysicsTriangleMeshCook.h"
#include "PhysicsSceneActivationTestSupport.h"

namespace Horo::Physics {
    namespace {
        namespace W = WorldStreaming;
        using W::TestSupport::IdentityFrom;

        struct AttachmentFixture final {
            PhysicsShapeCookTargetDigest target;
            PhysicsTriangleMeshCookResult cooked;
            std::unique_ptr<Assets::AssetPayloadCache> bytes;
            Assets::AssetPayloadLease lease;
            PhysicsCookedShapeCache shapes;
            std::unique_ptr<PhysicsRuntime> runtime;
            PhysicsSceneActivationAuthority authority;
            PhysicsSceneActivationParticipant native;

            AttachmentFixture()
                : target{W::CandidateTestSupport::Hash()}, cooked(Cook()), bytes(Assets::AssetPayloadCache::Create(8, 65536).Value()),
                  lease(bytes->Admit(std::as_bytes(std::span{cooked.payload})).Value()),
                  shapes(PhysicsCookedShapeCache::Create(target).Value()), runtime(CreateRuntime()),
                  native(*runtime, authority, Settings()) {}

            static std::unique_ptr<PhysicsRuntime> CreateRuntime() {
#if HORO_TEST_PHYSICS_NATIVE
                auto result = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
#else
                auto result = PhysicsRuntime::Create(PhysicsRuntimeMode::Null);
#endif
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }

            static PhysicsTriangleMeshCookResult Cook() {
                const std::array<Math::Vec3, 3> vertices{{{0, 0, 0}, {1, 0, 0}, {0, 0, 1}}};
                const std::array slots{PhysicsMaterialSlotId::FromValue(7)};
                const std::array<PhysicsTriangleMeshSourceTriangle, 1> triangles{
                    {{{0, 1, 2}, PhysicsShapeSubresourceId::FromValue(101), slots[0]}}};
                auto result = CookPhysicsTriangleMesh({.asset = Asset(7),
                                                       .subresource = PhysicsShapeSubresourceId::FromValue(3),
                                                       .vertices = vertices,
                                                       .triangles = triangles,
                                                       .materialSlots = slots,
                                                       .target = {W::CandidateTestSupport::Hash()}});
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }

            W::CellAttachmentReference Reference(const bool required = true) const {
                return {W::StreamingCellProvider::PhysicsMesh,
                        cooked.descriptor.asset,
                        IdentityFrom<W::CellAttachmentSubresource>(3),
                        IdentityFrom<W::CellAttachmentRevision>(1),
                        3,
                        required ? W::StreamingCellPayloadRequirement::Required : W::StreamingCellPayloadRequirement::Optional,
                        lease.Digest(),
                        lease.Bytes().size()};
            }

            W::CellAttachmentManifest Manifest(const bool required = true, const std::uint64_t revision = 1) const {
                const auto world = W::CandidateTestSupport::Manifest();
                auto rows = W::CandidateTestSupport::Payloads();
                rows[1].provider = W::StreamingCellProvider::PhysicsMesh;
                const auto candidate =
                    W::PrepareStreamingCellCandidate(world, W::CandidateTestSupport::Context(), W::CandidateTestSupport::Header(rows))
                        .Value();
                const std::array references{Reference(required)};
                return W::CellAttachmentManifest::Create(candidate, IdentityFrom<W::CellAttachmentRevision>(revision), references,
                                                         {8, 65536})
                    .Value();
            }

            Runtime::SceneCellAttachmentContext Admission(const std::uint64_t revision = 1) const {
                return {Runtime::SceneDefinitionId{19},
                        Runtime::SceneDefinitionRevision{1},
                        W::CandidateTestSupport::Operation(),
                        IdentityFrom<W::CellAttachmentRevision>(revision),
                        8,
                        8};
            }

            Runtime::SceneCellAttachmentProvider Provider(const bool required = true) {
                const std::array references{PhysicsCellAttachment{Reference(required), cooked.descriptor}};
                auto result = MakePhysicsCellAttachmentProvider(IdentityFrom<W::StreamingRuntimeServiceId>(1),
                                                                IdentityFrom<W::StreamingRuntimeServiceRevision>(1), 3, shapes, native,
                                                                references, 8);
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }

            std::unique_ptr<Runtime::SceneCellAttachmentParticipant> Participant(const bool required = true) {
                auto result = Runtime::SceneCellAttachmentParticipant::Create(Admission(), Manifest(required), {Provider(required)},
                                                                              {{cooked.descriptor.asset, lease}});
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }
        };

        void Commit(Runtime::RuntimeSceneService &service) {
            REQUIRE(service.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Context({})).HasValue());
        }
    }  // namespace

    TEST_CASE("Streamed Physics attachments admit real cooked leases and aggregate native Scene publication",
              "[physics][attachment][production][publication]") {
        AttachmentFixture fixture;
        auto participant = fixture.Participant();
        auto *owner = participant.get();
        Runtime::RuntimeSceneService service;
        REQUIRE(service.AddActivationParticipant(std::move(participant)).HasValue());
        REQUIRE(service.Startup({}).HasValue());
        REQUIRE(service.QueuePreparation(Definition()).HasValue());
        REQUIRE_FALSE(service.ActiveScene());
        Commit(service);
        REQUIRE_FALSE(service.TakeOperationError());
        REQUIRE(service.ActiveScene());
        REQUIRE(owner->ActiveStatus().size() == 1);
        REQUIRE(owner->ActiveStatus()[0].available);
        REQUIRE(fixture.shapes.Stats().residentShapes == 1);
        fixture.bytes->Shutdown();
        REQUIRE(fixture.shapes.Shutdown().HasValue());
        REQUIRE(service.ActiveScene()->Find({1}));
        REQUIRE(service.QueueUnload().HasValue());
        Commit(service);
        REQUIRE(owner->ActiveStatus().empty());
        service.Shutdown();
    }

    TEST_CASE("Required streamed attachment failure preserves active Scene and native causes",
              "[physics][attachment][production][failure]") {
        AttachmentFixture fixture;
        auto participant = fixture.Participant();
        auto *owner = participant.get();
        Runtime::RuntimeSceneService service;
        REQUIRE(service.AddActivationParticipant(std::move(participant)).HasValue());
        REQUIRE(service.Startup({}).HasValue());
        REQUIRE(service.QueuePreparation(Definition()).HasValue());
        Commit(service);
        const auto original = service.ActiveScene()->Find({1}).value();
        REQUIRE(service.QueuePreparation(Definition()).HasValue());
        REQUIRE(fixture.authority.AdvanceOriginGeneration().HasValue());
        Commit(service);
        REQUIRE(service.TakeOperationError().has_value());
        REQUIRE(service.ActiveScene()->Get(original).HasValue());
        REQUIRE(owner->ActiveStatus()[0].available);
        REQUIRE(fixture.shapes.Shutdown().HasValue());
        REQUIRE(service.QueuePreparation(Definition()).HasError());
        REQUIRE(service.ActiveScene()->Get(original).HasValue());
        service.Shutdown();
    }

    TEST_CASE("Optional attachment absence and native invalidation remain observable without blocking Scene",
              "[physics][attachment][production][optional]") {
        AttachmentFixture fixture;
        auto absent = Runtime::SceneCellAttachmentParticipant::Create(fixture.Admission(), fixture.Manifest(false), {}, {});
        REQUIRE(absent.HasValue());
        auto scene = RequireScene(Definition());
        auto candidate = absent.Value()->Prepare(Definition(), scene->View());
        REQUIRE(candidate.HasValue());
        candidate.Value()->Publish();
        REQUIRE_FALSE(absent.Value()->ActiveStatus()[0].available);
        REQUIRE(absent.Value()->ActiveStatus()[0].cause->code.Value() == W::CellAttachmentErrors::Unsupported.code.Value());
        candidate.Value()->Shutdown();
        auto participant = fixture.Participant(false);
        auto prepared = participant->Prepare(Definition(), scene->View());
        REQUIRE(prepared.HasValue());
        REQUIRE(fixture.authority.AdvanceOriginGeneration().HasValue());
        REQUIRE(prepared.Value()->ValidatePublication().HasValue());
        prepared.Value()->Publish();
        REQUIRE_FALSE(participant->ActiveStatus()[0].available);
        REQUIRE(participant->ActiveStatus()[0].cause.has_value());
        prepared.Value()->Shutdown();
    }

    TEST_CASE("Attachment admission rejects integrity capacity duplicates and unsupported required providers before native work",
              "[physics][attachment][production][admission]") {
        AttachmentFixture fixture;
        REQUIRE(
            Runtime::SceneCellAttachmentParticipant::Create(fixture.Admission(), fixture.Manifest(), {}, {}).ErrorValue().code.Value() ==
            W::CellAttachmentErrors::Unsupported.code.Value());
        auto wrong = fixture.bytes->Admit(std::as_bytes(std::span{"wrong", 5})).Value();
        REQUIRE(Runtime::SceneCellAttachmentParticipant::Create(fixture.Admission(), fixture.Manifest(), {fixture.Provider()},
                                                                {{fixture.cooked.descriptor.asset, wrong}})
                    .ErrorValue()
                    .code.Value() == W::CellAttachmentErrors::Stale.code.Value());
        auto context = fixture.Admission();
        context.maximumProviders = 1;
        REQUIRE(Runtime::SceneCellAttachmentParticipant::Create(context, fixture.Manifest(), {fixture.Provider(), fixture.Provider()},
                                                                {{fixture.cooked.descriptor.asset, fixture.lease}})
                    .ErrorValue()
                    .code.Value() == W::CellAttachmentErrors::CapacityExceeded.code.Value());
        REQUIRE(Runtime::SceneCellAttachmentParticipant::Create(fixture.Admission(), fixture.Manifest(),
                                                                {fixture.Provider(), fixture.Provider()},
                                                                {{fixture.cooked.descriptor.asset, fixture.lease}})
                    .ErrorValue()
                    .code.Value() == W::CellAttachmentErrors::Invalid.code.Value());
        REQUIRE(fixture.shapes.Stats().residentShapes == 0);
    }

    TEST_CASE("Attachment cancellation replacement and shutdown fence unpublished real Physics resources",
              "[physics][attachment][production][lifecycle]") {
        AttachmentFixture fixture;
        auto scene = RequireScene(Definition());
        auto participant = fixture.Participant();
        auto prepared = participant->Prepare(Definition(), scene->View());
        REQUIRE(prepared.HasValue());
        REQUIRE(
            participant
                ->Replace(fixture.Admission(), fixture.Manifest(), {fixture.Provider()}, {{fixture.cooked.descriptor.asset, fixture.lease}})
                .HasError());
        REQUIRE(prepared.Value()->ValidatePublication().HasValue());
        REQUIRE(participant
                    ->Replace(fixture.Admission(2), fixture.Manifest(true, 2), {fixture.Provider()},
                              {{fixture.cooked.descriptor.asset, fixture.lease}})
                    .HasValue());
        REQUIRE(prepared.Value()->ValidatePublication().HasError());
        prepared.Value()->Shutdown();
        auto fresh = participant->Prepare(Definition(), scene->View());
        REQUIRE(fresh.HasValue());
        participant->RequestCancellation();
        REQUIRE(fresh.Value()->ValidatePublication().HasError());
        fresh.Value()->Shutdown();
        participant->Shutdown();
        participant->Shutdown();
        REQUIRE(participant->Prepare(Definition(), scene->View()).HasError());
    }
}  // namespace Horo::Physics
