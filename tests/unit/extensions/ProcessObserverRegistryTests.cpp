#include "Horo/Extensions/ProcessObserverRegistry.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Extensions::Tests {
    namespace {
        constexpr auto HostStarted = ProcessObserverEventKind::HostStarted;
        constexpr auto OperationStarted = ProcessObserverEventKind::OperationStarted;
        constexpr auto OperationFinished = ProcessObserverEventKind::OperationFinished;
        constexpr auto DiagnosticRaised = ProcessObserverEventKind::DiagnosticRaised;

        [[nodiscard]] ExtensionAdmissionPolicy Policy(const bool approved = true) {
            return {.revision = 7,
                    .knownPermissions = {{"process.observe"}, {"process.execute"}},
                    .approvedPermissions =
                        approved ? std::vector<ExtensionPermissionId>{{"process.observe"}} : std::vector<ExtensionPermissionId>{},
                    .availableCapabilities = {{"horo.process.observe"}, {"horo.process.execute"}}};
        }

        [[nodiscard]] ExtensionAdmissionRequest Request(const bool declarePermission = true) {
            return {.extensionId = "com.example.observer",
                    .moduleId = "com.example.observer.backend",
                    .activationGeneration = 3,
                    .capabilities = {{.capability = {"horo.process.observe"},
                                      .requiredPermissions = declarePermission ? std::vector<ExtensionPermissionId>{{"process.observe"}}
                                                                               : std::vector<ExtensionPermissionId>{}}}};
        }

        [[nodiscard]] ExtensionCapabilityAdmission Admission() {
            auto admitted = ExtensionCapabilityAdmission::Evaluate(Request(), Policy());
            REQUIRE(admitted.HasValue());
            return std::move(admitted).Value();
        }

        [[nodiscard]] ExtensionCapabilityHandle Authority(const ExtensionCapabilityAdmission &admission) {
            auto granted = admission.Grant({"horo.process.observe"});
            REQUIRE(granted.HasValue());
            return std::move(granted).Value();
        }

        [[nodiscard]] ProcessObserverDescriptor Descriptor(std::string id = "com.example.observer.first",
                                                           std::vector<ProcessObserverEventKind> kinds = {HostStarted}) {
            return {.observerId = std::move(id),
                    .extensionId = "com.example.observer",
                    .moduleId = "com.example.observer.backend",
                    .activationGeneration = 3,
                    .allowedEvents = std::move(kinds)};
        }

        [[nodiscard]] std::shared_ptr<void> CodeLease() {
            return std::make_shared<int>(42);
        }

        void RequireError(const auto &result, const std::string_view code) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == code);
        }
    }  // namespace

    TEST_CASE("Process observation requires its own declared and approved permission", "[unit][extensions][process-observer]") {
        RequireError(ExtensionCapabilityAdmission::Evaluate(Request(false), Policy()), "capability_admission_invalid");
        RequireError(ExtensionCapabilityAdmission::Evaluate(Request(), Policy(false)), "permission_denied");

        auto executeRequest = Request();
        executeRequest.capabilities = {{.capability = {"horo.process.execute"}, .requiredPermissions = {{"process.execute"}}}};
        auto executePolicy = Policy();
        executePolicy.approvedPermissions = {{"process.execute"}};
        auto executeAdmission = ExtensionCapabilityAdmission::Evaluate(executeRequest, executePolicy);
        REQUIRE(executeAdmission.HasValue());
        auto executeHandle = executeAdmission.Value().Grant({"horo.process.execute"});
        REQUIRE(executeHandle.HasValue());
        ProcessObserverRegistry registry{{HostStarted}};
        RequireError(registry.Register(Descriptor(), executeHandle.Value(),
                                       [](const auto &) {
            return Result<void>::Success();
        }, CodeLease()),
                     "permission_denied");
    }

    TEST_CASE("Process observer dispatch is filtered, ordered, and content-free", "[unit][extensions][process-observer]") {
        auto admission = Admission();
        ProcessObserverRegistry registry{{HostStarted, OperationStarted, OperationFinished, DiagnosticRaised}};
        std::vector<std::string> order;
        auto second = registry.Register(Descriptor("com.example.observer.z", {OperationFinished}), Authority(admission),
                                        [&order](const ProcessObserverEvent &event) {
            CHECK(event.operationId == 19);
            CHECK(event.outcome == ProcessObserverOutcome::Succeeded);
            order.emplace_back("z");
            return Result<void>::Success();
        }, CodeLease());
        auto first = registry.Register(Descriptor("com.example.observer.a", {OperationFinished}), Authority(admission),
                                       [&order](const ProcessObserverEvent &) {
            order.emplace_back("a");
            return Result<void>::Success();
        }, CodeLease());
        REQUIRE(second.HasValue());
        REQUIRE(first.HasValue());
        REQUIRE(registry.Dispatch({.kind = HostStarted, .revision = 1}).HasValue());
        CHECK(order.empty());
        REQUIRE(
            registry.Dispatch({.kind = OperationFinished, .revision = 2, .operationId = 19, .outcome = ProcessObserverOutcome::Succeeded})
                .HasValue());
        CHECK(order == std::vector<std::string>{"a", "z"});
    }

    TEST_CASE("Process observer rejects malformed payload and unapproved event kinds", "[unit][extensions][process-observer]") {
        auto admission = Admission();
        ProcessObserverRegistry registry{{HostStarted, OperationFinished}};
        auto callback = [](const ProcessObserverEvent &) {
            return Result<void>::Success();
        };
        RequireError(registry.Register(Descriptor("com.example.observer.bad", {DiagnosticRaised}), Authority(admission), callback,
                                       CodeLease()),
                     "process_observer_invalid");
        RequireError(registry.Register(Descriptor("com.example.observer.bad", {HostStarted, HostStarted}), Authority(admission), callback,
                                       CodeLease()),
                     "process_observer_invalid");
        RequireError(registry.Register(Descriptor(), Authority(admission), callback, {}), "process_observer_invalid");
        RequireError(registry.Dispatch({.kind = HostStarted}), "process_observer_invalid");
        RequireError(registry.Dispatch({.kind = HostStarted, .revision = 1, .operationId = 4}), "process_observer_invalid");
        RequireError(registry.Dispatch({.kind = OperationFinished, .revision = 1, .operationId = 4}), "process_observer_invalid");
        RequireError(registry.Dispatch({.kind = DiagnosticRaised, .revision = 1, .diagnostic = ProcessObserverDiagnostic::ProviderFailed}),
                     "process_observer_invalid");
        ProcessObserverRegistry invalidPolicy{{HostStarted, HostStarted}};
        RequireError(invalidPolicy.Register(Descriptor(), Authority(admission), callback, CodeLease()), "process_observer_invalid");
    }

    TEST_CASE("Process observer registration and revocation close future dispatch", "[unit][extensions][process-observer]") {
        auto admission = Admission();
        ProcessObserverRegistry registry{{HostStarted}};
        std::size_t calls{};
        auto callback = [&calls](const ProcessObserverEvent &) {
            ++calls;
            return Result<void>::Success();
        };
        auto registered = registry.Register(Descriptor(), Authority(admission), callback, CodeLease());
        REQUIRE(registered.HasValue());
        auto owned = std::move(registered).Value();
        auto revokedAuthority = Authority(admission);
        RequireError(registry.Register(Descriptor(), Authority(admission), callback, CodeLease()), "process_observer_duplicate");
        REQUIRE(registry.Dispatch({.kind = HostStarted, .revision = 1}).HasValue());
        CHECK(calls == 1);
        admission.Revoke();
        REQUIRE(registry.Dispatch({.kind = HostStarted, .revision = 2}).HasValue());
        CHECK(calls == 1);
        owned.Reset();
        CHECK_FALSE(owned.IsRegistered());
        registry.BeginShutdown();
        registry.BeginShutdown();
        RequireError(registry.Dispatch({.kind = HostStarted, .revision = 3}), "process_observer_shutdown");
        RequireError(registry.Register(Descriptor(), revokedAuthority, callback, CodeLease()), "capability_revoked");
    }

    TEST_CASE("Process observer failure is isolated and callback code stays leased", "[unit][extensions][process-observer]") {
        auto admission = Admission();
        ProcessObserverRegistry registry{{HostStarted}};
        std::weak_ptr<void> weakCode;
        auto code = CodeLease();
        weakCode = code;
        std::size_t succeedingCalls{};
        auto failing = registry.Register(Descriptor("com.example.observer.a"), Authority(admission),
                                         [&weakCode](const ProcessObserverEvent &) -> Result<void> {
            CHECK_FALSE(weakCode.expired());
            throw 1;
        }, code);
        auto succeeding =
            registry.Register(Descriptor("com.example.observer.z"), Authority(admission), [&succeedingCalls](const ProcessObserverEvent &) {
            ++succeedingCalls;
            return Result<void>::Success();
        }, CodeLease());
        REQUIRE(failing.HasValue());
        REQUIRE(succeeding.HasValue());
        auto failingRegistration = std::move(failing).Value();
        code.reset();
        RequireError(registry.Dispatch({.kind = HostStarted, .revision = 1}), "process_observer_callback_failed");
        CHECK(succeedingCalls == 1);
        CHECK_FALSE(failingRegistration.IsRegistered());
        failingRegistration.Reset();
        CHECK(weakCode.expired());
        REQUIRE(registry.Dispatch({.kind = HostStarted, .revision = 2}).HasValue());
        CHECK(succeedingCalls == 2);
    }

    TEST_CASE("Process observer dispatch stays on its owner thread and rejects recursion", "[unit][extensions][process-observer]") {
        auto admission = Admission();
        ProcessObserverRegistry registry{{HostStarted}};
        auto registered = registry.Register(Descriptor(), Authority(admission), [&registry](const ProcessObserverEvent &) {
            RequireError(registry.Dispatch({.kind = HostStarted, .revision = 2}), "process_observer_reentrant");
            return Result<void>::Success();
        }, CodeLease());
        REQUIRE(registered.HasValue());
        auto otherThread = std::async(std::launch::async, [&registry] {
            return registry.Dispatch({.kind = HostStarted, .revision = 1});
        });
        RequireError(otherThread.get(), "process_observer_thread_violation");
        REQUIRE(registry.Dispatch({.kind = HostStarted, .revision = 1}).HasValue());
    }
}  // namespace Horo::Extensions::Tests
