#include "Horo/Extensions/BackendOperationRegistry.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <format>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Extensions::Tests {
    namespace {
        [[nodiscard]] ApplicationCapabilityProviderIdentity ProviderIdentity(const std::string_view id = "provider",
                                                                             const std::uint64_t generation = 1) {
            return {.moduleId = "com.example.backend", .providerId = std::string{id}, .generation = generation};
        }

        [[nodiscard]] BackendOperationProviderDescriptor Provider(
            const ApplicationCapabilityProviderIdentity &identity = ProviderIdentity(), const std::size_t maximumOperations = 8,
            const std::size_t maximumDiagnostics = 8, const std::size_t maximumDiagnosticBytes = 256,
            const std::size_t maximumResultBytes = 128) {
            return {.provider = identity,
                    .operationTypes = {{{"compile"}, {"compile.result"}}, {{"inspect"}, {"inspect.result"}}},
                    .maximumOperations = maximumOperations,
                    .maximumDiagnostics = maximumDiagnostics,
                    .maximumDiagnosticBytes = maximumDiagnosticBytes,
                    .maximumResultBytes = maximumResultBytes};
        }

        [[nodiscard]] BackendOperationDescriptor OperationDescriptor() {
            return {.initialPhase = {"queued"}};
        }

        [[nodiscard]] BackendOperationRegistration RegisterProvider(BackendOperationRegistry &registry,
                                                                    BackendOperationProviderDescriptor descriptor = Provider()) {
            auto result = registry.Register(std::move(descriptor));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        [[nodiscard]] BackendOperationController BeginOperation(BackendOperationRegistry &registry,
                                                                const ApplicationCapabilityProviderIdentity &provider = ProviderIdentity(),
                                                                const BackendOperationTypeId &type = {"compile"},
                                                                BackendOperationDescriptor descriptor = OperationDescriptor()) {
            auto result = registry.Begin(provider, type, std::move(descriptor));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        void RequireErrorCode(const auto &result, const std::string_view code) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == code);
        }

        [[nodiscard]] bool IsTerminal(const BackendOperationSnapshot &snapshot) {
            using enum BackendOperationState;
            return snapshot.state == Completed || snapshot.state == Failed || snapshot.state == Cancelled;
        }
    }  // namespace

    TEST_CASE("Backend operation registry validates provider contracts and stable registration errors", "[Extensions][BackendOperations]") {
        BackendOperationRegistry registry;
        auto malformed = Provider();
        malformed.provider.generation = 0;
        RequireErrorCode(registry.Register(std::move(malformed)), "backend_operation_registry_invalid");

        auto emptyTypes = Provider();
        emptyTypes.operationTypes.clear();
        RequireErrorCode(registry.Register(std::move(emptyTypes)), "backend_operation_registry_invalid");

        auto duplicateResults = Provider();
        duplicateResults.operationTypes[1].result = duplicateResults.operationTypes[0].result;
        RequireErrorCode(registry.Register(std::move(duplicateResults)), "backend_operation_registry_invalid");

        auto firstResult = registry.Register(Provider());
        REQUIRE(firstResult.HasValue());
        auto first = std::move(firstResult).Value();
        RequireErrorCode(registry.Register(Provider()), "backend_operation_registry_duplicate");
        auto disjointOperations = Provider();
        disjointOperations.operationTypes = {{{"thumbnail"}, {"thumbnail.result"}}};
        RequireErrorCode(registry.Register(std::move(disjointOperations)), "backend_operation_registry_duplicate");
        CHECK(first.IsRegistered());

        registry.BeginShutdown();
        CHECK(registry.IsShutdown());
        RequireErrorCode(registry.Register(Provider(ProviderIdentity("replacement", 2))), "backend_operation_registry_shutdown");
        first.Reset();
    }

    TEST_CASE("Backend operation registry enforces provider and active operation bounds", "[Extensions][BackendOperations]") {
        BackendOperationRegistry normalizedRegistry({.maximumOperations = 0});
        auto normalizedRegistration = normalizedRegistry.Register(Provider());
        REQUIRE(normalizedRegistration.HasValue());
        auto normalizedOperation = normalizedRegistry.Begin(ProviderIdentity(), {"compile"}, OperationDescriptor());
        REQUIRE(normalizedOperation.HasValue());
        auto normalizedController = std::move(normalizedOperation).Value();
        CHECK(normalizedController.Complete() == BackendOperationTransitionResult::Applied);

        BackendOperationRegistry registry({.maximumOperations = 2});
        auto registration = registry.Register(Provider(ProviderIdentity(), 2));
        REQUIRE(registration.HasValue());
        auto firstResult = registry.Begin(ProviderIdentity(), {"compile"}, OperationDescriptor());
        auto secondResult = registry.Begin(ProviderIdentity(), {"inspect"}, OperationDescriptor());
        REQUIRE(firstResult.HasValue());
        REQUIRE(secondResult.HasValue());
        auto first = std::move(firstResult).Value();
        auto second = std::move(secondResult).Value();
        RequireErrorCode(registry.Begin(ProviderIdentity(), {"compile"}, OperationDescriptor()),
                         "backend_operation_registry_capacity_exceeded");
        CHECK(first.Complete() == BackendOperationTransitionResult::Applied);
        auto replacement = registry.Begin(ProviderIdentity(), {"compile"}, OperationDescriptor());
        REQUIRE(replacement.HasValue());
    }

    TEST_CASE("Backend operation registry enforces its provider publication bound", "[Extensions][BackendOperations]") {
        BackendOperationRegistry registry;
        std::vector<BackendOperationRegistration> registrations;
        registrations.reserve(BackendOperationRegistry::MaximumProviders);
        for (std::size_t index = 0; index < BackendOperationRegistry::MaximumProviders; ++index) {
            auto result = registry.Register(Provider(ProviderIdentity(std::format("provider-{}", index), index + 1)));
            REQUIRE(result.HasValue());
            registrations.push_back(std::move(result).Value());
        }
        RequireErrorCode(registry.Register(Provider(ProviderIdentity("overflow", 999))), "backend_operation_registry_capacity_exceeded");
    }

    TEST_CASE("Backend operation IDs and snapshots retain exact provider and operation contracts", "[Extensions][BackendOperations]") {
        BackendOperationRegistry registry;
        auto registration = RegisterProvider(registry);
        auto controller = BeginOperation(registry);
        const auto handle = controller.Handle();
        const auto snapshot = handle.Snapshot();
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->operation.IsValid());
        CHECK(snapshot->operation == handle.Id());
        CHECK(snapshot->provider == ProviderIdentity());
        CHECK(snapshot->type == BackendOperationTypeId{"compile"});
        CHECK(snapshot->result == BackendOperationResultId{"compile.result"});
        CHECK(snapshot->state == BackendOperationState::Queued);
        CHECK(snapshot->revision == 1);
    }

    TEST_CASE("Backend operation progress is monotonic within a phase and resets on phase change", "[Extensions][BackendOperations]") {
        BackendOperationRegistry registry;
        auto registration = RegisterProvider(registry);
        auto controller = BeginOperation(registry);
        const auto handle = controller.Handle();

        CHECK(controller.PublishProgress({"decode"}, {.completedUnits = 4, .totalUnits = 10}) == BackendOperationTransitionResult::Applied);
        CHECK(controller.PublishProgress({"decode"}, {.completedUnits = 3, .totalUnits = 10}) ==
              BackendOperationTransitionResult::InvalidTransition);
        CHECK(controller.PublishProgress({"decode"}, {.completedUnits = 4, .totalUnits = 9}) == BackendOperationTransitionResult::Applied);
        CHECK(controller.PublishProgress({"upload"}, {.completedUnits = 1, .totalUnits = 2}) == BackendOperationTransitionResult::Applied);
        const auto snapshot = handle.Snapshot();
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state == BackendOperationState::Running);
        CHECK(snapshot->phase == BackendOperationPhaseId{"upload"});
        CHECK(snapshot->progress.completedUnits == 1);
        CHECK(snapshot->progress.totalUnits == 2);
        CHECK(snapshot->revision == 4);
    }

    TEST_CASE("Backend operation progress compares uint64 work ratios exactly at the maximum bound", "[Extensions][BackendOperations]") {
        BackendOperationRegistry registry;
        auto registration = RegisterProvider(registry);
        auto controller = BeginOperation(registry);
        constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
        CHECK(controller.PublishProgress({"maximum"}, {.completedUnits = maximum - 1, .totalUnits = maximum}) ==
              BackendOperationTransitionResult::Applied);
        CHECK(controller.PublishProgress({"maximum"}, {.completedUnits = maximum - 2, .totalUnits = maximum - 1}) ==
              BackendOperationTransitionResult::InvalidTransition);
        CHECK(controller.PublishProgress({"maximum"}, {.completedUnits = maximum, .totalUnits = maximum}) ==
              BackendOperationTransitionResult::Applied);
    }

    TEST_CASE("Backend operation diagnostics and typed result payloads are host bounded", "[Extensions][BackendOperations]") {
        BackendOperationRegistry registry;
        auto registration = RegisterProvider(registry, Provider(ProviderIdentity(), 8, 1, 32, 2));
        auto controller = BeginOperation(registry);
        auto validDiagnostic = Diagnostic{.code = DiagnosticCode{"backend.compile"},
                                          .severity = DiagnosticSeverity::Warning,
                                          .message = "ok",
                                          .location = {.source = "shader.h", .line = 3, .column = 4},
                                          .path = "compile"};
        REQUIRE(controller.AddDiagnostic(validDiagnostic).HasValue());
        RequireErrorCode(controller.AddDiagnostic(validDiagnostic), "backend_operation_payload_invalid");

        auto invalidResult = BackendOperationResultPayload{.id = {"other.result"}, .bytes = {std::byte{0}}};
        CHECK(controller.Complete(std::move(invalidResult)) == BackendOperationTransitionResult::InvalidTransition);
        auto oversizedResult = BackendOperationResultPayload{.id = {"compile.result"}, .bytes = std::vector<std::byte>(3, std::byte{0})};
        CHECK(controller.Complete(std::move(oversizedResult)) == BackendOperationTransitionResult::InvalidTransition);
        auto validResult = BackendOperationResultPayload{.id = {"compile.result"}, .bytes = std::vector<std::byte>(2, std::byte{0x2a})};
        CHECK(controller.Complete(std::move(validResult)) == BackendOperationTransitionResult::Applied);
        const auto snapshot = controller.Handle().Snapshot();
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state == BackendOperationState::Completed);
        CHECK_FALSE(snapshot->terminalError.has_value());
        REQUIRE(snapshot->resultPayload.has_value());
        CHECK(snapshot->resultPayload->id == BackendOperationResultId{"compile.result"});
        CHECK(snapshot->diagnostics.size() == 1);
    }

    TEST_CASE("Backend operation diagnostics enforce the aggregate byte bound", "[Extensions][BackendOperations]") {
        BackendOperationRegistry registry;
        auto registration = RegisterProvider(registry, Provider(ProviderIdentity(), 8, 4, 10, 128));
        auto controller = BeginOperation(registry);
        const Diagnostic diagnostic{.code = DiagnosticCode{"a.b"},
                                    .severity = DiagnosticSeverity::Note,
                                    .message = "x",
                                    .location = {.source = "s"},
                                    .path = {}};
        REQUIRE(controller.AddDiagnostic(diagnostic).HasValue());
        REQUIRE(controller.AddDiagnostic(diagnostic).HasValue());
        RequireErrorCode(controller.AddDiagnostic(diagnostic), "backend_operation_payload_invalid");
    }

    TEST_CASE("Backend operation cancellation observes caller and parent tokens atomically", "[Extensions][BackendOperations]") {
        BackendOperationRegistry registry;
        auto registration = RegisterProvider(registry);

        auto caller = BeginOperation(registry);
        const auto callerHandle = caller.Handle();
        CHECK(callerHandle.RequestCancellation() == BackendOperationCancellationRequestResult::Requested);
        CHECK(caller.ObserveCancellation() == BackendOperationCancellationObservation::Cancelled);
        CHECK(callerHandle.Snapshot()->cancellationReason == BackendOperationCancellationReason::Caller);
        CHECK(caller.Complete() == BackendOperationTransitionResult::AlreadyTerminal);

        CancellationSource parent;
        auto parentController = BeginOperation(registry, ProviderIdentity(), {"compile"}, {.parentCancellation = parent.Token()});
        parent.RequestCancellation();
        CHECK(parentController.Cancellation().IsCancellationRequested());
        CHECK(parentController.ObserveCancellation() == BackendOperationCancellationObservation::Cancelled);
        CHECK(parentController.Handle().Snapshot()->cancellationReason == BackendOperationCancellationReason::Parent);
    }

    TEST_CASE("Backend operation caller cancellation source survives provider teardown", "[Extensions][BackendOperations]") {
        {
            BackendOperationRegistry registry;
            auto registration = RegisterProvider(registry);
            auto controller = BeginOperation(registry);
            const auto handle = controller.Handle();
            CHECK(handle.RequestCancellation() == BackendOperationCancellationRequestResult::Requested);
            registration.Reset();
            const auto snapshot = handle.Snapshot();
            REQUIRE(snapshot.has_value());
            CHECK(snapshot->state == BackendOperationState::Cancelled);
            CHECK(snapshot->cancellationReason == BackendOperationCancellationReason::Caller);
        }
    }

    TEST_CASE("Backend operation teardown publishes its cancellation source", "[Extensions][BackendOperations]") {
        {
            BackendOperationRegistry registry;
            auto registration = RegisterProvider(registry);
            CancellationSource parent;
            auto controller = BeginOperation(registry, ProviderIdentity(), {"compile"}, {.parentCancellation = parent.Token()});
            const auto handle = controller.Handle();
            parent.RequestCancellation();
            registry.BeginShutdown();
            const auto snapshot = handle.Snapshot();
            REQUIRE(snapshot.has_value());
            CHECK(snapshot->state == BackendOperationState::Cancelled);
            CHECK(snapshot->cancellationReason == BackendOperationCancellationReason::Parent);
        }

        {
            BackendOperationRegistry registry;
            auto registration = RegisterProvider(registry);
            auto controller = BeginOperation(registry);
            const auto handle = controller.Handle();
            registration.Reset();
            CHECK(handle.RequestCancellation() == BackendOperationCancellationRequestResult::AlreadyTerminal);
            CHECK(handle.Snapshot()->cancellationReason == BackendOperationCancellationReason::Provider);
        }

        {
            BackendOperationRegistry registry;
            auto registration = RegisterProvider(registry);
            auto controller = BeginOperation(registry);
            const auto handle = controller.Handle();
            registry.BeginShutdown();
            CHECK(handle.RequestCancellation() == BackendOperationCancellationRequestResult::AlreadyTerminal);
            CHECK(handle.Snapshot()->cancellationReason == BackendOperationCancellationReason::Shutdown);
        }
    }

    TEST_CASE("Backend operation cancellation wins the next producer transition", "[Extensions][BackendOperations]") {
        BackendOperationRegistry registry;
        auto registration = RegisterProvider(registry);
        auto controller = BeginOperation(registry);
        const auto handle = controller.Handle();
        CHECK(handle.RequestCancellation() == BackendOperationCancellationRequestResult::Requested);
        const auto diagnostic = Diagnostic{.code = DiagnosticCode{"backend.compile"}, .message = "cancelled"};
        const auto result = controller.AddDiagnostic(diagnostic);
        RequireErrorCode(result, "backend_operation_cancelled");
        CHECK(handle.Snapshot()->state == BackendOperationState::Cancelled);
        CHECK(handle.Snapshot()->cancellationReason == BackendOperationCancellationReason::Caller);
    }

    TEST_CASE("Provider reset and registry shutdown cancel active operations and invalidate admission", "[Extensions][BackendOperations]") {
        BackendOperationRegistry registry;
        auto registration = RegisterProvider(registry);
        auto controller = BeginOperation(registry);
        const auto handle = controller.Handle();
        registration.Reset();
        CHECK_FALSE(registration.IsRegistered());
        CHECK(controller.Cancellation().IsCancellationRequested());
        const auto snapshot = handle.Snapshot();
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state == BackendOperationState::Cancelled);
        CHECK(snapshot->cancellationReason == BackendOperationCancellationReason::Provider);
        CHECK(controller.PublishProgress({"late"}, {1, 1}) == BackendOperationTransitionResult::AlreadyTerminal);

        auto secondRegistration = RegisterProvider(registry, Provider(ProviderIdentity("second", 2)));
        auto second = BeginOperation(registry, ProviderIdentity("second", 2));
        const auto secondHandle = second.Handle();
        registry.BeginShutdown();
        CHECK(second.Cancellation().IsCancellationRequested());
        CHECK(secondHandle.Snapshot()->state == BackendOperationState::Cancelled);
        CHECK(secondHandle.Snapshot()->cancellationReason == BackendOperationCancellationReason::Shutdown);
        RequireErrorCode(registry.Begin(ProviderIdentity("second", 2), {"compile"}, OperationDescriptor()),
                         "backend_operation_registry_shutdown");
    }

    TEST_CASE("Backend operation producer abandonment is terminal and stale transitions are harmless", "[Extensions][BackendOperations]") {
        BackendOperationRegistry registry;
        auto registration = RegisterProvider(registry);
        BackendOperationHandle handle;
        {
            auto controller = BeginOperation(registry);
            handle = controller.Handle();
            CHECK(controller.PublishProgress({"decode"}, {1, 2}) == BackendOperationTransitionResult::Applied);
        }
        const auto snapshot = handle.Snapshot();
        REQUIRE(snapshot.has_value());
        CHECK(IsTerminal(*snapshot));
        CHECK(snapshot->state == BackendOperationState::Failed);
        REQUIRE(snapshot->terminalError.has_value());
        CHECK(snapshot->terminalError->code.Value() == "backend_operation_abandoned");
    }

    TEST_CASE("Backend operation abandonment honors pending caller and parent cancellation", "[Extensions][BackendOperations]") {
        BackendOperationRegistry registry;
        auto registration = RegisterProvider(registry);

        BackendOperationHandle callerHandle;
        {
            auto controller = BeginOperation(registry);
            callerHandle = controller.Handle();
            CHECK(callerHandle.RequestCancellation() == BackendOperationCancellationRequestResult::Requested);
        }
        REQUIRE(callerHandle.Snapshot().has_value());
        CHECK(callerHandle.Snapshot()->state == BackendOperationState::Cancelled);
        CHECK(callerHandle.Snapshot()->cancellationReason == BackendOperationCancellationReason::Caller);

        CancellationSource parent;
        BackendOperationHandle parentHandle;
        {
            auto controller = BeginOperation(registry, ProviderIdentity(), {"compile"}, {.parentCancellation = parent.Token()});
            parentHandle = controller.Handle();
            parent.RequestCancellation();
        }
        REQUIRE(parentHandle.Snapshot().has_value());
        CHECK(parentHandle.Snapshot()->state == BackendOperationState::Cancelled);
        CHECK(parentHandle.Snapshot()->cancellationReason == BackendOperationCancellationReason::Parent);
    }

    TEST_CASE("Backend operation teardown races producer completion, failure, observation, and abandonment",
              "[Extensions][BackendOperations]") {
        for (std::size_t iteration = 0; iteration < 32; ++iteration) {
            BackendOperationRegistry registry({.maximumOperations = 1});
            auto registration = RegisterProvider(registry);
            auto controller = BeginOperation(registry);
            const auto handle = controller.Handle();
            std::atomic_bool start{};
            auto produce = [controller = std::move(controller), &start, iteration]() mutable {
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();
                switch (iteration % 3U) {
                    case 0:
                        static_cast<void>(controller.Complete());
                        break;
                    case 1:
                        static_cast<void>(controller.Fail(MakeError(ExtensionErrors::BackendOperationPayloadInvalid)));
                        break;
                    default:
                        static_cast<void>(controller.ObserveCancellation());
                        break;
                }
            };
            auto tearDown = [&registration, &registry, &start, iteration] {
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();
                if ((iteration % 2U) == 0U)
                    registration.Reset();
                else
                    registry.BeginShutdown();
            };
            std::thread producer(std::move(produce));   // NOSONAR -- Apple libc++ lacks std::jthread; joined below.
            std::thread teardown(std::move(tearDown));  // NOSONAR -- Apple libc++ lacks std::jthread; joined below.
            start.store(true, std::memory_order_release);
            producer.join();
            teardown.join();
            const auto snapshot = handle.Snapshot();
            REQUIRE(snapshot.has_value());
            CHECK(snapshot->IsTerminal());
            CHECK(snapshot->revision >= 2);
        }
    }

    TEST_CASE("Backend operation completion and cancellation races publish exactly one terminal state", "[Extensions][BackendOperations]") {
        BackendOperationRegistry registry({.maximumOperations = 1});
        auto registration = RegisterProvider(registry);
        for (std::size_t iteration = 0; iteration < 64; ++iteration) {
            auto controller = BeginOperation(registry);
            const auto handle = controller.Handle();
            std::thread complete([&controller] {  // NOSONAR -- The supported Apple standard library lacks std::jthread.
                static_cast<void>(controller.Complete());
            });
            std::thread cancel([handle] {  // NOSONAR -- The supported Apple standard library lacks std::jthread.
                static_cast<void>(handle.RequestCancellation());
            });
            complete.join();
            cancel.join();
            const auto snapshot = handle.Snapshot();
            REQUIRE(snapshot.has_value());
            CHECK(IsTerminal(*snapshot));
            CHECK((snapshot->state == BackendOperationState::Completed || snapshot->state == BackendOperationState::Cancelled));
            CHECK(snapshot->revision >= 2);
        }
    }

    TEST_CASE("Backend operation error descriptors are stable", "[Extensions][BackendOperations]") {
        const auto &first = ExtensionErrors::BackendOperationRegistryInvalid;
        const auto &second = ExtensionErrors::BackendOperationRegistryInvalid;
        CHECK(first.domain.Value() == "horo.extensions");
        CHECK(first.code.Value() == second.code.Value());
        CHECK(first.summary == second.summary);
        CHECK_FALSE(first.retryable);
        CHECK_FALSE(first.userActionable);
    }
}  // namespace Horo::Extensions::Tests
