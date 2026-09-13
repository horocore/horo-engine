#include "RenderResourceRegistry.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <type_traits>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Render::Detail;

    static_assert(!std::is_same_v<RenderBufferHandle, RenderTextureHandle>);
    static_assert(!std::is_convertible_v<RenderBufferHandle, RenderTextureHandle>);

    [[nodiscard]] RenderResourceIdentity Identity(const ResourceReservation &reservation) {
        return reservation.identity;
    }

    struct ReleasedBackendResources {
        std::array<std::uint64_t, 4> instances{};
        std::size_t count{0};
    };

    TEST_CASE("Resource registry publishes a pending generation and records completion", "[unit][runtime][renderer][resource]") {
        const auto owner = AcquireRenderResourceOwnerId();
        REQUIRE(owner.HasValue());
        RenderResourceRegistry registry{owner.Value(), {}};

        auto reserved = registry.Reserve(RenderResourceClass::Texture);
        REQUIRE(reserved.HasValue());
        REQUIRE(reserved.Value().identity.owner == owner.Value());
        REQUIRE(reserved.Value().identity.slot != 0);
        REQUIRE(reserved.Value().identity.generation != 0);
        REQUIRE(reserved.Value().operation.IsValid());
        REQUIRE(registry.State(RenderResourceClass::Texture, Identity(reserved.Value())).Value() == RenderResourceState::Pending);

        const Result<void> pending = registry.OperationResult(reserved.Value().operation);
        REQUIRE(pending.HasError());
        REQUIRE(pending.ErrorValue().code.Value() == "render.frontend.resource.operation_pending");

        REQUIRE(registry.Publish(RenderResourceClass::Texture, Identity(reserved.Value()), 17).HasValue());
        REQUIRE(registry.State(RenderResourceClass::Texture, Identity(reserved.Value())).Value() == RenderResourceState::Ready);
        REQUIRE(registry.BackendInstance(RenderResourceClass::Texture, Identity(reserved.Value())).Value() == 17);
        REQUIRE(registry.OperationResult(reserved.Value().operation).HasValue());
    }

    TEST_CASE("Resource registry rejects foreign wrong-type and stale identities", "[unit][runtime][renderer][resource]") {
        const auto firstOwner = AcquireRenderResourceOwnerId();
        const auto secondOwner = AcquireRenderResourceOwnerId();
        REQUIRE(firstOwner.HasValue());
        REQUIRE(secondOwner.HasValue());
        REQUIRE(firstOwner.Value() != secondOwner.Value());
        RenderResourceRegistry registry{firstOwner.Value(), {}};
        auto reserved = registry.Reserve(RenderResourceClass::Buffer);
        REQUIRE(reserved.HasValue());
        REQUIRE(registry.Publish(RenderResourceClass::Buffer, Identity(reserved.Value()), 1).HasValue());

        RenderResourceIdentity foreign = Identity(reserved.Value());
        foreign.owner = secondOwner.Value();
        const auto wrongOwner = registry.State(RenderResourceClass::Buffer, foreign);
        REQUIRE(wrongOwner.HasError());
        REQUIRE(wrongOwner.ErrorValue().code.Value() == "render.frontend.resource.wrong_owner");

        const auto wrongType = registry.State(RenderResourceClass::Texture, Identity(reserved.Value()));
        REQUIRE(wrongType.HasError());
        REQUIRE(wrongType.ErrorValue().code.Value() == "render.frontend.resource.wrong_type");

        REQUIRE(registry.Release(RenderResourceClass::Buffer, Identity(reserved.Value())).HasValue());
        REQUIRE(registry.DrainRetirements() == 1);
        const auto stale = registry.State(RenderResourceClass::Buffer, Identity(reserved.Value()));
        REQUIRE(stale.HasError());
        REQUIRE(stale.ErrorValue().code.Value() == "render.frontend.resource.stale");

        auto replacement = registry.Reserve(RenderResourceClass::Buffer);
        REQUIRE(replacement.HasValue());
        REQUIRE(replacement.Value().identity.slot == reserved.Value().identity.slot);
        REQUIRE(replacement.Value().identity.generation == reserved.Value().identity.generation + 1);
    }

    TEST_CASE("Resource registry enforces pending queue and slot bounds", "[unit][runtime][renderer][resource]") {
        const auto owner = AcquireRenderResourceOwnerId();
        REQUIRE(owner.HasValue());
        RenderResourceRegistry registry{owner.Value(),
                                        {.maximumSlots = 2,
                                         .maximumPendingRequests = 1,
                                         .retirementDrainBudget = 1,
                                         .maximumOperationResults = 2}};

        auto first = registry.Reserve(RenderResourceClass::Buffer);
        REQUIRE(first.HasValue());
        const auto queueFull = registry.Reserve(RenderResourceClass::Texture);
        REQUIRE(queueFull.HasError());
        REQUIRE(queueFull.ErrorValue().code.Value() == "render.frontend.resource.queue_full");
        REQUIRE(registry.Publish(RenderResourceClass::Buffer, Identity(first.Value()), 1).HasValue());

        auto second = registry.Reserve(RenderResourceClass::Texture);
        REQUIRE(second.HasValue());
        REQUIRE(registry.Publish(RenderResourceClass::Texture, Identity(second.Value()), 2).HasValue());
        const auto capacity = registry.Reserve(RenderResourceClass::Sampler);
        REQUIRE(capacity.HasError());
        REQUIRE(capacity.ErrorValue().code.Value() == "render.frontend.resource.capacity_exhausted");
    }

    TEST_CASE("Resource registry limits reject every invalid bound", "[unit][runtime][renderer][resource]") {
        REQUIRE(RenderResourceRegistryLimits{}.IsValid());
        REQUIRE_FALSE(RenderResourceRegistryLimits{.maximumSlots = 0}.IsValid());
        REQUIRE_FALSE(RenderResourceRegistryLimits{.maximumPendingRequests = 0}.IsValid());
        REQUIRE_FALSE(RenderResourceRegistryLimits{.retirementDrainBudget = 0}.IsValid());
        REQUIRE_FALSE(RenderResourceRegistryLimits{.maximumPendingRequests = 2, .maximumOperationResults = 1}.IsValid());
    }

    TEST_CASE("Resource registry reports malformed identities", "[unit][runtime][renderer][resource]") {
        const auto owner = AcquireRenderResourceOwnerId();
        REQUIRE(owner.HasValue());
        RenderResourceRegistry registry{owner.Value(), {}};
        REQUIRE(registry.Owner() == owner.Value());

        const std::array malformed{
            RenderResourceIdentity{},
            RenderResourceIdentity{.owner = owner.Value(), .slot = 0, .generation = 1},
            RenderResourceIdentity{.owner = owner.Value(), .slot = 1, .generation = 0},
        };
        for (const RenderResourceIdentity identity : malformed) {
            const auto state = registry.State(RenderResourceClass::Buffer, identity);
            REQUIRE(state.HasError());
            REQUIRE(state.ErrorValue().code.Value() == "render.frontend.resource.handle_malformed");
        }

        const auto outOfRange =
            registry.State(RenderResourceClass::Buffer, RenderResourceIdentity{.owner = owner.Value(), .slot = 1, .generation = 1});
        REQUIRE(outOfRange.HasError());
        REQUIRE(outOfRange.ErrorValue().code.Value() == "render.frontend.resource.slot_out_of_range");

        auto reserved = registry.Reserve(RenderResourceClass::Buffer);
        REQUIRE(reserved.HasValue());
        const RenderResourceIdentity identity = Identity(reserved.Value());
        REQUIRE(identity != RenderResourceIdentity{.owner = {}, .slot = identity.slot, .generation = identity.generation});
        REQUIRE(identity != RenderResourceIdentity{.owner = identity.owner, .slot = identity.slot + 1, .generation = identity.generation});
        REQUIRE(identity != RenderResourceIdentity{.owner = identity.owner, .slot = identity.slot, .generation = identity.generation + 1});
    }

    TEST_CASE("Resource registry rejects state-incompatible operations", "[unit][runtime][renderer][resource]") {
        const auto owner = AcquireRenderResourceOwnerId();
        REQUIRE(owner.HasValue());
        RenderResourceRegistry registry{owner.Value(), {}};
        auto reserved = registry.Reserve(RenderResourceClass::Buffer);
        REQUIRE(reserved.HasValue());
        const RenderResourceIdentity identity = Identity(reserved.Value());

        const auto pendingBackend = registry.BackendInstance(RenderResourceClass::Buffer, identity);
        REQUIRE(pendingBackend.HasError());
        REQUIRE(pendingBackend.ErrorValue().code.Value() == "render.frontend.resource.not_ready");
        REQUIRE(registry.AddSubmissionPin(RenderResourceClass::Buffer, identity).HasError());
        REQUIRE(registry.ReleaseSubmissionPin(RenderResourceClass::Buffer, identity).HasError());
        REQUIRE(registry.Release(RenderResourceClass::Buffer, identity).HasError());

        const auto invalidBackend = registry.Publish(RenderResourceClass::Buffer, identity, 0);
        REQUIRE(invalidBackend.HasError());
        REQUIRE(invalidBackend.ErrorValue().code.Value() == "render.frontend.resource.backend_instance_invalid");
        REQUIRE(registry.Publish(RenderResourceClass::Buffer, identity, 11).HasValue());
        REQUIRE(registry.Publish(RenderResourceClass::Buffer, identity, 12).HasError());

        const Error lateFailure{ErrorCode{"render.test.late_failure"},
                                ErrorDomainId{"render.test"},
                                ErrorSeverity::Error,
                                "Injected late failure.",
                                {}};
        REQUIRE(registry.Fail(RenderResourceClass::Buffer, identity, lateFailure).HasError());
        REQUIRE(registry.ReleaseSubmissionPin(RenderResourceClass::Buffer, identity).HasError());
        REQUIRE(registry.AddSubmissionPin(RenderResourceClass::Buffer, identity).HasValue());
        REQUIRE(registry.Release(RenderResourceClass::Buffer, identity).HasValue());
        const auto duplicateRelease = registry.Release(RenderResourceClass::Buffer, identity);
        REQUIRE(duplicateRelease.HasError());
        REQUIRE(duplicateRelease.ErrorValue().code.Value() == "render.frontend.resource.already_retiring");
        REQUIRE(registry.AddSubmissionPin(RenderResourceClass::Buffer, identity).HasError());
        REQUIRE(registry.BackendInstance(RenderResourceClass::Buffer, identity).HasError());
        REQUIRE(registry.DrainRetirements() == 0);
        REQUIRE(registry.ReleaseSubmissionPin(RenderResourceClass::Buffer, identity).HasValue());
        REQUIRE(registry.DrainRetirements() == 1);
    }

    TEST_CASE("Resource registry validates dependency generations and operation identities", "[unit][runtime][renderer][resource]") {
        const auto owner = AcquireRenderResourceOwnerId();
        const auto foreignOwner = AcquireRenderResourceOwnerId();
        REQUIRE(owner.HasValue());
        REQUIRE(foreignOwner.HasValue());
        RenderResourceRegistry registry{owner.Value(), {}};

        auto ready = registry.Reserve(RenderResourceClass::Buffer);
        auto pending = registry.Reserve(RenderResourceClass::Texture);
        REQUIRE(ready.HasValue());
        REQUIRE(pending.HasValue());
        REQUIRE(registry.Publish(RenderResourceClass::Buffer, Identity(ready.Value()), 1).HasValue());

        const std::array pendingDependency{Identity(pending.Value())};
        const auto notReady = registry.Reserve(RenderResourceClass::Mesh, pendingDependency);
        REQUIRE(notReady.HasError());
        REQUIRE(notReady.ErrorValue().code.Value() == "render.frontend.resource.dependency_not_ready");

        const std::array duplicateDependencies{Identity(ready.Value()), Identity(ready.Value())};
        const auto duplicate = registry.Reserve(RenderResourceClass::Mesh, duplicateDependencies);
        REQUIRE(duplicate.HasError());
        REQUIRE(duplicate.ErrorValue().code.Value() == "render.frontend.resource.handle_malformed");

        RenderResourceIdentity foreign = Identity(ready.Value());
        foreign.owner = foreignOwner.Value();
        const std::array foreignDependency{foreign};
        REQUIRE(registry.Reserve(RenderResourceClass::Mesh, foreignDependency).HasError());

        REQUIRE(registry.Release(RenderResourceClass::Buffer, Identity(ready.Value())).HasValue());
        REQUIRE(registry.DrainRetirements() == 1);
        const std::array staleDependency{Identity(ready.Value())};
        REQUIRE(registry.Reserve(RenderResourceClass::Mesh, staleDependency).HasError());

        const auto invalidOperation = registry.OperationResult({});
        REQUIRE(invalidOperation.HasError());
        REQUIRE(invalidOperation.ErrorValue().code.Value() == "render.frontend.resource.operation_unknown");
        const auto unknownOperation = registry.OperationResult(ResourceOperationId{999'999});
        REQUIRE(unknownOperation.HasError());
        REQUIRE(unknownOperation.ErrorValue().code.Value() == "render.frontend.resource.operation_unknown");
    }

    TEST_CASE("Resource registry preserves incomplete results when its result store is full", "[unit][runtime][renderer][resource]") {
        const auto owner = AcquireRenderResourceOwnerId();
        REQUIRE(owner.HasValue());
        RenderResourceRegistry registry{owner.Value(),
                                        {.maximumSlots = 3,
                                         .maximumPendingRequests = 2,
                                         .retirementDrainBudget = 1,
                                         .maximumOperationResults = 2}};

        auto first = registry.Reserve(RenderResourceClass::Buffer);
        auto second = registry.Reserve(RenderResourceClass::Texture);
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        REQUIRE(registry.Publish(RenderResourceClass::Texture, Identity(second.Value()), 2).HasValue());

        const auto resultStoreFull = registry.Reserve(RenderResourceClass::Sampler);
        REQUIRE(resultStoreFull.HasError());
        REQUIRE(resultStoreFull.ErrorValue().code.Value() == "render.frontend.resource.queue_full");
        REQUIRE(registry.OperationResult(first.Value().operation).HasError());
        REQUIRE(registry.OperationResult(second.Value().operation).HasValue());
    }

    TEST_CASE("Resource registry bounds retained operation results", "[unit][runtime][renderer][resource]") {
        const auto owner = AcquireRenderResourceOwnerId();
        REQUIRE(owner.HasValue());
        RenderResourceRegistry registry{owner.Value(),
                                        {.maximumSlots = 1,
                                         .maximumPendingRequests = 1,
                                         .retirementDrainBudget = 1,
                                         .maximumOperationResults = 1}};

        auto first = registry.Reserve(RenderResourceClass::Sampler);
        REQUIRE(first.HasValue());
        REQUIRE(registry.Publish(RenderResourceClass::Sampler, Identity(first.Value()), 1).HasValue());
        REQUIRE(registry.Release(RenderResourceClass::Sampler, Identity(first.Value())).HasValue());
        REQUIRE(registry.DrainRetirements() == 1);

        auto second = registry.Reserve(RenderResourceClass::Sampler);
        REQUIRE(second.HasValue());
        const auto evicted = registry.OperationResult(first.Value().operation);
        REQUIRE(evicted.HasError());
        REQUIRE(evicted.ErrorValue().code.Value() == "render.frontend.resource.operation_unknown");
    }

    TEST_CASE("Pinned dependencies and submissions defer retirement", "[unit][runtime][renderer][resource]") {
        const auto owner = AcquireRenderResourceOwnerId();
        REQUIRE(owner.HasValue());
        ReleasedBackendResources released;
        RenderResourceRegistry registry{owner.Value(),
                                        {.maximumSlots = 8,
                                         .maximumPendingRequests = 8,
                                         .retirementDrainBudget = 1,
                                         .maximumOperationResults = 8},
                                        [&released](const RenderResourceClass, const std::uint64_t backendInstance,
                                                    const std::optional<RenderMemoryAllocationId>) noexcept {
            released.instances[released.count++] = backendInstance;
        }};

        auto buffer = registry.Reserve(RenderResourceClass::Buffer);
        REQUIRE(buffer.HasValue());
        REQUIRE(registry.Publish(RenderResourceClass::Buffer, Identity(buffer.Value()), 1).HasValue());
        const std::array dependencies{Identity(buffer.Value())};
        auto mesh = registry.Reserve(RenderResourceClass::Mesh, dependencies);
        REQUIRE(mesh.HasValue());
        REQUIRE(registry.Publish(RenderResourceClass::Mesh, Identity(mesh.Value()), 2).HasValue());

        REQUIRE(registry.AddSubmissionPin(RenderResourceClass::Mesh, Identity(mesh.Value())).HasValue());
        REQUIRE(registry.Release(RenderResourceClass::Buffer, Identity(buffer.Value())).HasValue());
        REQUIRE(registry.Release(RenderResourceClass::Mesh, Identity(mesh.Value())).HasValue());
        REQUIRE(registry.DrainRetirements() == 0);
        REQUIRE(released.count == 0);
        REQUIRE(registry.ReleaseSubmissionPin(RenderResourceClass::Mesh, Identity(mesh.Value())).HasValue());
        REQUIRE(registry.DrainRetirements() == 1);
        REQUIRE(released.count == 1);
        REQUIRE(released.instances[0] == 2);
        REQUIRE(registry.DrainRetirements() == 1);
        REQUIRE(released.count == 2);
        REQUIRE(released.instances[1] == 1);
    }

    TEST_CASE("Failure and shutdown preserve terminal operation results", "[unit][runtime][renderer][resource]") {
        const auto owner = AcquireRenderResourceOwnerId();
        REQUIRE(owner.HasValue());
        RenderResourceRegistry registry{owner.Value(), {}};

        auto failed = registry.Reserve(RenderResourceClass::ShaderModule);
        REQUIRE(failed.HasValue());
        const Error backendError{ErrorCode{"render.test.create_failed"},
                                 ErrorDomainId{"render.test"},
                                 ErrorSeverity::Error,
                                 "Injected backend creation failure.",
                                 {}};
        REQUIRE(registry.Fail(RenderResourceClass::ShaderModule, Identity(failed.Value()), backendError).HasValue());
        const auto failure = registry.OperationResult(failed.Value().operation);
        REQUIRE(failure.HasError());
        REQUIRE(failure.ErrorValue().code.Value() == "render.test.create_failed");
        REQUIRE(registry.DrainRetirements() == 1);

        auto cancelled = registry.Reserve(RenderResourceClass::Pipeline);
        REQUIRE(cancelled.HasValue());
        registry.Shutdown();
        const auto cancellation = registry.OperationResult(cancelled.Value().operation);
        REQUIRE(cancellation.HasError());
        REQUIRE(cancellation.ErrorValue().code.Value() == "render.frontend.resource.registry_stopped");
        const auto stopped = registry.Reserve(RenderResourceClass::Buffer);
        REQUIRE(stopped.HasError());
        REQUIRE(stopped.ErrorValue().code.Value() == "render.frontend.resource.registry_stopped");
        registry.Shutdown();
    }
}  // namespace
