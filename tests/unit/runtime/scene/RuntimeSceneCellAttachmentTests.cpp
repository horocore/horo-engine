#include "../world_streaming/StreamingCellCandidateTestSupport.h"
#include "Horo/Assets/AssetCook.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Runtime/Scene/RuntimeSceneCellPayload.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Runtime {
    namespace {
        namespace W = WorldStreaming;
        using W::CandidateTestSupport::Cell;
        using W::TestSupport::Asset;
        using W::TestSupport::IdentityFrom;
        using W::TestSupport::World;

        struct Counts final {
            int authoritiesReleased{};
            int prepared{};
            int published{};
            int rolledBack{};
            int notified{};
            bool reject{};
            bool invalidateFence{};
        };

        class Authority final : public SceneCellPayloadAuthority {
        public:
            Authority(SceneCellPayloadIdentity identity, W::StreamingFence fence, Counts &counts)
                : identity(identity), fence(fence), counts(counts) {}

            ~Authority() override {
                ++counts.authoritiesReleased;
            }

            Result<void> ValidatePublication(const SceneCellPayloadIdentity &value, const W::StreamingFence &attempt) const override {
                if (!ready || value != identity || attempt != fence)
                    return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Stale));
                return Result<void>::Success();
            }

            SceneCellPayloadIdentity identity;
            W::StreamingFence fence;
            Counts &counts;
            bool ready{true};
        };

        class Candidate final : public SceneStructuralCandidate {
        public:
            explicit Candidate(Counts &counts) : counts(counts) {}

            ~Candidate() override {
                if (!published)
                    ++counts.rolledBack;
            }

            Result<void> ValidatePublication() const override {
                return counts.reject ? Result<void>::Failure(MakeError(SceneCellPayloadErrors::Unsupported)) : Result<void>::Success();
            }

            void Publish() noexcept override {
                published = true;
                ++counts.published;
            }

            Result<void> AfterPublication() override {
                ++counts.notified;
                return Result<void>::Success();
            }

            Counts &counts;
            bool published{};
        };

        class Participant final : public SceneStructuralParticipant {
        public:
            Participant(Counts &counts, Authority *authority = nullptr) : counts(counts), authority(authority) {}

            SceneStructuralOwner Owner() const noexcept override {
                return SceneStructuralOwner::Gameplay;
            }

            Result<std::unique_ptr<SceneStructuralCandidate>> Prepare(RuntimeSceneView active, std::span<const RuntimeEntityView> created,
                                                                      std::span<const EntityRef> destroyed) override {
                REQUIRE(active.IsCurrent());
                for (const auto &entity : created) {
                    REQUIRE(entity.entity.runtime == active.RuntimeId());
                    const auto prior = active.Find(*entity.authoredObject);
                    REQUIRE((!prior || std::ranges::find(destroyed, *prior) != destroyed.end()));
                }
                for (const auto entity : destroyed)
                    REQUIRE(active.Get(entity).HasValue());
                ++counts.prepared;
                if (counts.invalidateFence && authority)
                    authority->ready = false;
                std::unique_ptr<SceneStructuralCandidate> result = std::make_unique<Candidate>(counts);
                return Result<std::unique_ptr<SceneStructuralCandidate>>::Success(std::move(result));
            }

            Counts &counts;
            Authority *authority{};
        };

        RuntimeSceneCellPayload Payload(const int cell = 0, const std::uint64_t revision = 1,
                                        const std::span<const SceneAssetDependency> dependencies = {}, const bool empty = false) {
            const auto manifest = W::CandidateTestSupport::Manifest();
            const SceneCellPayloadIdentity identity{World(), Cell(cell), {static_cast<std::uint64_t>(77 + cell)}, {revision}};
            // Child precedes parent deliberately: attachment must resolve complete hierarchy before publication.
            const auto first = static_cast<std::uint64_t>(10 + cell * 10);
            const std::array entities{RuntimeEntityDefinition{.object = {first + 1}, .parent = SceneObjectId{first}},
                                      RuntimeEntityDefinition{.object = {first}}};
            auto result = CookRuntimeSceneCellPayload(manifest.Descriptor(),
                                                      {identity,
                                                       empty ? std::span<const RuntimeEntityDefinition>{}
                                                             : std::span<const RuntimeEntityDefinition>{entities},
                                                       dependencies},
                                                      identity, {32, 32, 1024 * 1024});
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        void Commit(RuntimeSceneService &service) {
            REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, FrameContext{1, {}, 0, 0, {}, false, {}}).HasValue());
        }

        void Activate(RuntimeSceneService &service) {
            SceneDefinitionBuilder builder{{1}, {1}};
            builder.Add({.object = {100}});
            REQUIRE(service.QueuePreparation(std::move(builder).Build().Value()).HasValue());
            Commit(service);
            REQUIRE(service.ActiveScene().has_value());
        }

        SceneCellAttachmentRequest Request(const RuntimeSceneService &service, const RuntimeSceneCellPayload &payload) {
            return {service.ActiveScene()->RuntimeId(),
                    {World(), IdentityFrom<W::PartitionEpoch>(1), payload.Identity().cell, IdentityFrom<W::StreamingGeneration>(1)},
                    {},
                    {4, 32, 32},
                    {},
                    {},
                    {}};
        }

        std::shared_ptr<Authority> Owner(const RuntimeSceneCellPayload &payload, const SceneCellAttachmentRequest &request,
                                         Counts &counts) {
            return std::make_shared<Authority>(payload.Identity(), request.fence, counts);
        }

        void Attach(RuntimeSceneService &service, const RuntimeSceneCellPayload &payload, const SceneCellAttachmentRequest &request,
                    const std::shared_ptr<Authority> &authority) {
            REQUIRE(QueueRuntimeSceneCellAttachment(service, payload, request, {}, authority).HasValue());
            Commit(service);
            REQUIRE_FALSE(service.TakeOperationError().has_value());
        }
    }  // namespace

    TEST_CASE("Cells attach to one canonical runtime and preserve resident handles", "[scene][cell_attachment]") {
        Counts counts;
        RuntimeSceneService service;
        Activate(service);
        const auto root = *service.ActiveScene()->Find({100});
        const auto runtime = service.ActiveScene()->RuntimeId();
        const auto first = Payload();
        const auto firstRequest = Request(service, first);
        Attach(service, first, firstRequest, Owner(first, firstRequest, counts));
        const auto resident = *service.ActiveScene()->Find({10});
        const auto child = service.ActiveScene()->Get(*service.ActiveScene()->Find({11})).Value();
        REQUIRE(child.parent == resident);
        const auto viewBeforeSecond = *service.ActiveScene();
        const auto second = Payload(1);
        const auto request = Request(service, second);
        REQUIRE(QueueRuntimeSceneCellAttachment(service, second, request, {}, Owner(second, request, counts)).HasValue());
        REQUIRE(viewBeforeSecond.IsCurrent());
        REQUIRE_FALSE(service.ActiveScene()->Find({20}).has_value());
        REQUIRE(service.OnPhase(RuntimePhase::VariableUpdate, FrameContext{1, {}, 0, 0, {}, false, {}}).HasValue());
        REQUIRE_FALSE(service.ActiveScene()->Find({20}).has_value());
        Commit(service);
        REQUIRE_FALSE(viewBeforeSecond.IsCurrent());
        REQUIRE(service.ActiveScene()->RuntimeId() == runtime);
        REQUIRE(service.ActiveScene()->DefinitionId() == SceneDefinitionId{1});
        REQUIRE(service.ActiveScene()->Find({100}) == root);
        REQUIRE(service.ActiveScene()->Find({10}) == resident);
        REQUIRE(service.ActiveScene()->Get(resident).HasValue());
        REQUIRE(service.ActiveScene()->BaselineCount() == 2);
        REQUIRE(service.ActiveScene()->FindBaseline(first.Identity().scene)->entities.size() == 2);
        REQUIRE(counts.authoritiesReleased == 2);
    }

    TEST_CASE("Cell replacement and eviction retire only exact ownership", "[scene][cell_attachment][lifecycle]") {
        Counts counts;
        RuntimeSceneService service;
        Activate(service);
        const auto first = Payload();
        const auto second = Payload(1);
        const auto initial = Request(service, first);
        const auto otherRequest = Request(service, second);
        Attach(service, first, initial, Owner(first, initial, counts));
        Attach(service, second, otherRequest, Owner(second, otherRequest, counts));
        const auto old = *service.ActiveScene()->Find({10});
        const auto unaffected = *service.ActiveScene()->Find({20});
        const auto replacement = Payload(0, 2);
        auto request = Request(service, replacement);
        request.expectedRevision = {1};
        Attach(service, replacement, request, Owner(replacement, request, counts));
        REQUIRE(service.ActiveScene()->Get(old).HasError());
        REQUIRE(service.ActiveScene()->Find({20}) == unaffected);
        REQUIRE(service.ActiveScene()->FindBaseline(first.Identity().scene)->revision == SceneDefinitionRevision{2});
        REQUIRE(QueueRuntimeSceneCellDetachment(service, first.Identity(), initial, Owner(first, initial, counts)).HasValue());
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.baseline.stale");
        REQUIRE(service.ActiveScene()->BaselineCount() == 2);
        REQUIRE(QueueRuntimeSceneCellDetachment(service, replacement.Identity(), request, Owner(replacement, request, counts)).HasValue());
        Commit(service);
        REQUIRE_FALSE(service.TakeOperationError().has_value());
        REQUIRE_FALSE(service.ActiveScene()->Find({10}).has_value());
        REQUIRE(service.ActiveScene()->Find({20}) == unaffected);
        REQUIRE(service.ActiveScene()->BaselineCount() == 1);
    }

    TEST_CASE("Cell cancellation and stale provider barrier preserve canonical storage", "[scene][cell_attachment][rollback]") {
        Counts counts;
        RuntimeSceneService service;
        Activate(service);
        const auto payload = Payload();
        auto request = Request(service, payload);
        CancellationSource cancellation;
        request.cancellation = cancellation.Token();
        auto authority = Owner(payload, request, counts);
        auto view = *service.ActiveScene();
        REQUIRE(QueueRuntimeSceneCellAttachment(service, payload, request, {}, authority).HasValue());
        authority.reset();
        cancellation.RequestCancellation();
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.cell_payload.cancelled");
        REQUIRE(view.IsCurrent());
        REQUIRE(service.ActiveScene()->BaselineCount() == 0);
        REQUIRE(counts.authoritiesReleased == 1);
        request.cancellation = {};
        authority = Owner(payload, request, counts);
        REQUIRE(QueueRuntimeSceneCellAttachment(service, payload, request, {}, authority).HasValue());
        authority->ready = false;
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.cell_payload.stale");
        REQUIRE(view.IsCurrent());
    }

    TEST_CASE("Cell explicit cancellation is fenced and allows a replacement request", "[scene][cell_attachment][cancel]") {
        Counts counts;
        RuntimeSceneService service;
        Activate(service);
        const auto payload = Payload();
        const auto request = Request(service, payload);
        REQUIRE(QueueRuntimeSceneCellAttachment(service, payload, request, {}, Owner(payload, request, counts)).HasValue());
        auto wrongIdentity = payload.Identity();
        wrongIdentity.scene = {999};
        REQUIRE(CancelRuntimeSceneCellOperation(service, wrongIdentity, request).HasError());
        auto wrongRequest = request;
        wrongRequest.runtime = {999};
        REQUIRE(CancelRuntimeSceneCellOperation(service, payload.Identity(), wrongRequest).HasError());
        wrongRequest = request;
        wrongRequest.fence.generation = IdentityFrom<W::StreamingGeneration>(2);
        REQUIRE(CancelRuntimeSceneCellOperation(service, payload.Identity(), wrongRequest).HasError());
        wrongIdentity = payload.Identity();
        wrongIdentity.revision = {2};
        REQUIRE(CancelRuntimeSceneCellOperation(service, wrongIdentity, request).HasError());
        REQUIRE(CancelRuntimeSceneCellOperation(service, payload.Identity(), request).HasValue());
        REQUIRE(counts.authoritiesReleased == 1);
        Attach(service, payload, request, Owner(payload, request, counts));
        REQUIRE(CancelRuntimeSceneCellOperation(service, payload.Identity(), request).HasError());
        REQUIRE(service.ActiveScene()->BaselineCount() == 1);
    }

    TEST_CASE("Cell aggregate capacity includes empty cells and resident entities", "[scene][cell_attachment][capacity]") {
        Counts counts;
        RuntimeSceneService service;
        Activate(service);
        const auto first = Payload();
        const auto request = Request(service, first);
        Attach(service, first, request, Owner(first, request, counts));
        const auto second = Payload(1);
        auto secondRequest = Request(service, second);
        secondRequest.limits.maximumEntities = 3;
        const auto oldView = *service.ActiveScene();
        REQUIRE(QueueRuntimeSceneCellAttachment(service, second, secondRequest, {}, Owner(second, secondRequest, counts)).HasValue());
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.baseline.capacity_exceeded");
        REQUIRE(oldView.IsCurrent());
        const auto empty = Payload(1, 1, {}, true);
        auto emptyRequest = Request(service, empty);
        emptyRequest.limits.maximumAttachments = 1;
        REQUIRE(QueueRuntimeSceneCellAttachment(service, empty, emptyRequest, {}, Owner(empty, emptyRequest, counts)).HasValue());
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.baseline.capacity_exceeded");
        emptyRequest.limits.maximumAttachments = 2;
        Attach(service, empty, emptyRequest, Owner(empty, emptyRequest, counts));
        REQUIRE(service.ActiveScene()->FindBaseline(empty.Identity().scene)->entities.empty());
        REQUIRE(service.ActiveScene()->BaselineCount() == 2);
    }

    TEST_CASE("Cell required owners and final publication predicate reject atomically", "[scene][cell_attachment][provider]") {
        Counts counts;
        RuntimeSceneService service;
        const auto payload = Payload();
        REQUIRE(service.AddStructuralParticipant(std::make_unique<Participant>(counts)).HasValue());
        Activate(service);
        const auto request = Request(service, payload);
        auto authority = Owner(payload, request, counts);
        counts.reject = true;
        const auto before = *service.ActiveScene();
        REQUIRE(QueueRuntimeSceneCellAttachment(service, payload, request, {}, authority).HasValue());
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.cell_payload.unsupported");
        REQUIRE(counts.prepared == 1);
        REQUIRE(counts.rolledBack == 1);
        REQUIRE(counts.published == 0);
        REQUIRE(before.IsCurrent());
        counts.reject = false;
        Attach(service, payload, request, authority);
        REQUIRE(counts.published == 1);
        REQUIRE(counts.notified == 1);
    }

    TEST_CASE("Cell fence is sampled again after fallible provider preparation", "[scene][cell_attachment][provider][stale]") {
        Counts counts;
        RuntimeSceneService service;
        const auto payload = Payload();
        SceneCellAttachmentRequest request{{1},
                                           {World(), IdentityFrom<W::PartitionEpoch>(1), Cell(), IdentityFrom<W::StreamingGeneration>(1)},
                                           {},
                                           {4, 32, 32}};
        auto authority = Owner(payload, request, counts);
        REQUIRE(service.AddStructuralParticipant(std::make_unique<Participant>(counts, authority.get())).HasValue());
        Activate(service);
        request.runtime = service.ActiveScene()->RuntimeId();
        counts.invalidateFence = true;
        const auto before = *service.ActiveScene();
        REQUIRE(QueueRuntimeSceneCellAttachment(service, payload, request, {}, authority).HasValue());
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.cell_payload.stale");
        REQUIRE(counts.published == 0);
        REQUIRE(counts.rolledBack == 1);
        REQUIRE(before.IsCurrent());
    }

    TEST_CASE("Cell entities cannot escape their baseline retirement owner", "[scene][cell_attachment][ownership]") {
        Counts counts;
        RuntimeSceneService service;
        Activate(service);
        const auto payload = Payload();
        const auto request = Request(service, payload);
        Attach(service, payload, request, Owner(payload, request, counts));
        const auto child = *service.ActiveScene()->Find({11});
        SceneCommandBuffer commands;
        commands.Destroy(child);
        REQUIRE(service.QueueStructuralCommands(std::move(commands)).HasValue());
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.baseline.invalid");
        REQUIRE(service.ActiveScene()->Get(child).HasValue());
        SceneCommandBuffer external;
        static_cast<void>(external.Create({.parent = child, .authoredObject = SceneObjectId{999}}));
        REQUIRE(service.QueueStructuralCommands(std::move(external)).HasValue());
        Commit(service);
        REQUIRE(QueueRuntimeSceneCellDetachment(service, payload.Identity(), request, Owner(payload, request, counts)).HasValue());
        const auto before = *service.ActiveScene();
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.baseline.invalid");
        REQUIRE(before.IsCurrent());
        REQUIRE(service.ActiveScene()->Get(child).HasValue());
    }

    TEST_CASE("Cell pending and resident ownership retires on world replacement and shutdown", "[scene][cell_attachment][shutdown]") {
        Counts counts;
        RuntimeSceneService service;
        Activate(service);
        const auto payload = Payload();
        const auto request = Request(service, payload);
        Attach(service, payload, request, Owner(payload, request, counts));
        const auto old = *service.ActiveScene()->Find({10});
        Activate(service);
        REQUIRE(service.ActiveScene()->Get(old).HasError());
        REQUIRE(service.ActiveScene()->BaselineCount() == 0);
        auto nextRequest = Request(service, payload);
        REQUIRE(QueueRuntimeSceneCellAttachment(service, payload, nextRequest, {}, Owner(payload, nextRequest, counts)).HasValue());
        service.Shutdown();
        service.Shutdown();
        REQUIRE(counts.authoritiesReleased == 2);
        REQUIRE_FALSE(service.ActiveScene().has_value());
        REQUIRE(QueueRuntimeSceneCellAttachment(service, payload, nextRequest, {}, Owner(payload, nextRequest, counts)).HasError());
    }

    TEST_CASE("Cell invalid runtime identity limits and duplicate attachment preserve residents", "[scene][cell_attachment][invalid]") {
        Counts counts;
        RuntimeSceneService service;
        Activate(service);
        const auto payload = Payload();
        auto request = Request(service, payload);
        auto authority = Owner(payload, request, counts);
        const auto before = *service.ActiveScene();
        request.runtime = {999};
        REQUIRE(QueueRuntimeSceneCellAttachment(service, payload, request, {}, authority).ErrorValue().code.Value() ==
                "scene.cell_payload.stale");
        request = Request(service, payload);
        request.fence.epoch = IdentityFrom<W::PartitionEpoch>(2);
        REQUIRE(QueueRuntimeSceneCellAttachment(service, payload, request, {}, authority).ErrorValue().code.Value() ==
                "scene.cell_payload.stale");
        request = Request(service, payload);
        request.limits.maximumAttachments = 0;
        REQUIRE(QueueRuntimeSceneCellAttachment(service, payload, request, {}, authority).HasError());
        request = Request(service, payload);
        REQUIRE(QueueRuntimeSceneCellAttachment(service, payload, request, {}, {}).HasError());
        REQUIRE(before.IsCurrent());
        Attach(service, payload, request, authority);
        REQUIRE(QueueRuntimeSceneCellAttachment(service, payload, request, {}, authority).HasValue());
        const auto published = *service.ActiveScene();
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.baseline.stale");
        REQUIRE(published.IsCurrent());
        REQUIRE(service.ActiveScene()->BaselineCount() == 1);
    }

    TEST_CASE("Cell required Physics admission rejects a missing composed owner", "[scene][cell_attachment][unsupported]") {
        Counts counts;
        RuntimeSceneService service;
        Activate(service);
        const auto manifest = W::CandidateTestSupport::Manifest();
        const SceneCellPayloadIdentity identity{World(), Cell(), {77}, {1}};
        RuntimeEntityDefinition entity{.object = {10}};
        entity.components.rigidBody =
            RigidBodyComponent{.id = {10}, .body = {100}, .motion = AuthoredPhysicsMotionType::Static, .mass = AuthoredPhysicsNoMass{}};
        // A valid authored body needs an explicit collider before owner admission can be exercised.
        entity.components.colliders.push_back({.id = {11},
                                               .collider = {1},
                                               .body = {{10}, {100}},
                                               .collisionProfile = Physics::CollisionProfileId::FromBytes({1}),
                                               .materials = {{Physics::PhysicsMaterialSlotId::FromValue(1), Asset(1)}}});
        const std::array entities{entity};
        auto cooked = CookRuntimeSceneCellPayload(manifest.Descriptor(), {identity, entities}, identity, {8, 8, 1024 * 1024});
        INFO((cooked.HasError() ? cooked.ErrorValue().message : ""));
        REQUIRE(cooked.HasValue());
        const auto payload = std::move(cooked).Value();
        const auto request = Request(service, payload);
        const auto before = *service.ActiveScene();
        REQUIRE(QueueRuntimeSceneCellAttachment(service, payload, request, {}, Owner(payload, request, counts)).HasValue());
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.baseline.unsupported");
        REQUIRE(before.IsCurrent());
        REQUIRE(service.ActiveScene()->BaselineCount() == 0);
        REQUIRE_FALSE(service.ActiveScene()->Find({10}).has_value());
    }

    TEST_CASE("Cell resident identity rejects foreign partition cell and mounted epoch owners", "[scene][cell_attachment][identity]") {
        Counts counts;
        RuntimeSceneService service;
        Activate(service);
        const auto payload = Payload();
        const auto request = Request(service, payload);
        Attach(service, payload, request, Owner(payload, request, counts));
        const auto resident = *service.ActiveScene()->Find({10});
        REQUIRE(FindRuntimeSceneCellAttachment(*service.ActiveScene(), payload.Identity(), request.fence.epoch).has_value());
        for (int variant = 0; variant < 3; ++variant) {
            auto identity = payload.Identity();
            auto foreign = request;
            if (variant == 0) {
                identity.partition = World(2);
                foreign.fence.partition = identity.partition;
            } else if (variant == 1) {
                identity.cell = Cell(1);
                foreign.fence.cell = identity.cell;
            } else {
                foreign.fence.epoch = IdentityFrom<W::PartitionEpoch>(2);
            }
            auto validForeign = std::make_shared<Authority>(identity, foreign.fence, counts);
            REQUIRE(validForeign->ValidatePublication(identity, foreign.fence).HasValue());
            REQUIRE_FALSE(FindRuntimeSceneCellAttachment(*service.ActiveScene(), identity, foreign.fence.epoch).has_value());
            REQUIRE(QueueRuntimeSceneCellDetachment(service, identity, foreign, validForeign).HasValue());
            const auto before = *service.ActiveScene();
            Commit(service);
            REQUIRE(service.TakeOperationError()->code.Value() == "scene.baseline.stale");
            REQUIRE(before.IsCurrent());
            REQUIRE(service.ActiveScene()->Find({10}) == resident);
            REQUIRE(service.ActiveScene()->BaselineCount() == 1);
            identity.revision = {2};
            const auto manifest = W::CandidateTestSupport::Manifest();
            const auto &source = manifest.Descriptor();
            auto partition = W::WorldPartitionDescriptor::Create({}, identity.partition, source.Bounds(), source.Grid(), source.Layers(),
                                                                 source.Cells(), {4, 8, 1024})
                                 .Value();
            const std::array entities{RuntimeEntityDefinition{.object = {10}}};
            auto replacement = CookRuntimeSceneCellPayload(partition, {identity, entities}, identity, {8, 8, 1024 * 1024}).Value();
            foreign.expectedRevision = {1};
            validForeign = std::make_shared<Authority>(identity, foreign.fence, counts);
            REQUIRE(QueueRuntimeSceneCellAttachment(service, replacement, foreign, {}, validForeign).HasValue());
            Commit(service);
            REQUIRE(service.TakeOperationError()->code.Value() == "scene.baseline.stale");
            REQUIRE(before.IsCurrent());
            REQUIRE(service.ActiveScene()->Find({10}) == resident);
        }
    }

}  // namespace Horo::Runtime
