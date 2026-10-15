#include "../mcp/McpAuthorizationTestSupport.h"
#include "Horo/Cli/PackageCommands.h"
#include "Horo/Editor/PackageModalSession.h"
#include "Horo/Mcp/McpErrors.h"
#include "Horo/Mcp/PackageTools.h"
#include "PackageApplicationServiceTestSupport.h"

#include <algorithm>
#include <type_traits>

namespace {
    using namespace Horo;
    using namespace Horo::Application;
    using namespace Horo::Tests::PackageApplicationService;

    static_assert(std::is_invocable_v<decltype(&Editor::PackageModalSession::Submit), Editor::PackageModalSession &,
                                      Editor::PackageModalGeneration, PackageOperationKind>);
    static_assert(!std::is_invocable_v<decltype(&Editor::PackageModalSession::Submit), const Editor::PackageModalSession &,
                                       Editor::PackageModalGeneration, PackageOperationKind>);
    static_assert(std::is_invocable_v<decltype(&Editor::PackageModalSession::Activate), Editor::PackageModalSession &,
                                      Editor::PackageModalGeneration, std::span<const Packages::EnabledPackageExtension>>);
    static_assert(!std::is_invocable_v<decltype(&Editor::PackageModalSession::Activate), const Editor::PackageModalSession &,
                                       Editor::PackageModalGeneration, std::span<const Packages::EnabledPackageExtension>>);

    // Fixture outlives every adapter; production roots issue owning alias leases instead.
    std::shared_ptr<PackageApplication> Borrow(Fixture &fixture) {
        return {fixture.application.get(), [](PackageApplication *) {
        }};
    }

    ConfigurationSnapshot Configuration() {
        ConfigurationSchema schema;
        REQUIRE(schema.Seal().HasValue());
        auto resolved = ConfigurationResolver::Resolve(schema, {}, 1);
        REQUIRE(resolved.HasValue());
        return std::move(resolved).Value();
    }

    Cli::CliDispatcher Dispatcher(Fixture &fixture, std::function<Result<Packages::PackageActivationBoundary>()> boundary = [] {
        return Result<Packages::PackageActivationBoundary>::Success(Packages::PackageActivationBoundary::Quiescent);
    }) {
        const auto descriptors = Cli::DescribePackageCommands();
        auto registry = Cli::CliCommandRegistry::Create(descriptors, {.activeHost = Cli::CliHostKind::HoroEngine,
                                                                      .supportedContractVersion = {1, 0, 0},
                                                                      .grantedCapabilities = {{"horo.package.manage"}}});
        REQUIRE(registry.HasValue());
        auto adapters = Cli::MakePackageCommandAdapters([&fixture](const Cli::CliCommandRequest &, Cli::CliExecutionContext &) {
            return Result<std::shared_ptr<PackageApplication>>::Success(Borrow(fixture));
        }, std::move(boundary));
        auto dispatcher = Cli::CliDispatcher::Create(std::move(registry).Value(), std::move(adapters),
                                                     {.activeHost = Cli::CliHostKind::HoroEngine,
                                                      .maximumSideEffects = Cli::CliSideEffectPolicy::WritesFiles,
                                                      .grantedCapabilities = {{"horo.package.manage"}}});
        REQUIRE(dispatcher.HasValue());
        return std::move(dispatcher).Value();
    }

    TEST_CASE("Every package CLI adapter preserves the canonical registry option schema", "[packages][application][cli]") {
        const auto descriptors = Cli::DescribePackageCommands();
        REQUIRE(descriptors.size() == 4);
        for (const auto &descriptor : descriptors)
            CHECK(std::ranges::is_sorted(descriptor.options, {}, &Cli::CliOptionDescriptor::name));
        const auto activation = std::ranges::find_if(descriptors, [](const auto &descriptor) {
            return descriptor.path == Cli::CommandPath{{"package", "activate"}};
        });
        REQUIRE(activation != descriptors.end());
        REQUIRE(activation->options.size() == 3);
        CHECK(activation->options.front().name == "enable");
        Fixture fixture;
        auto dispatcher = Dispatcher(fixture);
        CHECK(dispatcher.Registry().Find({{"package", "activate"}}) != nullptr);
        CHECK(fixture.source.calls == 0);
    }

    TEST_CASE("Package modal reopening reconnects to actual retained work and rejects stale controls", "[packages][application][editor]") {
        Fixture fixture;
        Editor::PackageModalSession modal{Borrow(fixture)};
        auto first = modal.Open();
        REQUIRE(first.HasValue());
        REQUIRE(fixture.source.calls == 0);
        auto operation = modal.Submit(first.Value(), PackageOperationKind::Install);
        REQUIRE(operation.HasValue());
        REQUIRE(modal.Close(first.Value()).HasValue());
        auto second = modal.Open();
        REQUIRE(second.HasValue());
        REQUIRE(second.Value() != first.Value());
        REQUIRE(modal.Operation());
        CHECK(modal.Operation()->id == operation.Value());
        CHECK(modal.Cancel(first.Value()).HasError());
        CHECK(WaitForTerminal(fixture, operation.Value()).outcome == PackageOperationOutcome::Succeeded);
        REQUIRE(modal.Refresh(second.Value()).HasValue());
        REQUIRE(modal.Plan());
        CHECK(modal.Plan()->packages.front().verification == PackagePlanVerification::ArchiveVerified);
        REQUIRE(modal.Close(second.Value()).HasValue());
    }

    TEST_CASE("Package CLI dispatcher uses the shared install authority and preserves policy causes", "[packages][application][cli]") {
        Fixture fixture;
        auto dispatcher = Dispatcher(fixture);
        auto configuration = Configuration();
        Cli::CliCommandRequest request{.command = {{"package", "install"}}};
        auto terminal = dispatcher.Dispatch(request, {.invocation = {1}, .configuration = &configuration});
        REQUIRE(terminal.Outcome().HasValue());
        CHECK(fixture.source.calls == 1);
        REQUIRE(fixture.application->Current().Value());
        CHECK(fixture.application->Current().Value()->installedRevision.has_value());
        fixture.admissionAllowed = false;
        auto rejected = dispatcher.Dispatch(request, {.invocation = {2}, .configuration = &configuration});
        REQUIRE(rejected.Outcome().HasError());
        CHECK(rejected.Outcome().ErrorValue().code.Value() == "fixture.metadata.stale");
        CHECK(fixture.source.calls == 1);
    }

    TEST_CASE("Package MCP admission rejects old project generations before forwarding mutation", "[packages][application][mcp]") {
        Fixture fixture;
        std::uint64_t generation = 7;
        auto tools = Mcp::MakePackageTools(Borrow(fixture), "project-one", generation, [&generation] {
            return generation;
        });
        REQUIRE(tools.HasValue());
        REQUIRE(tools.Value().size() == 6);
        auto context = Mcp::Test::Context({.clientIdentity = "test-client",
                                           .capabilities = {"horo.package.manage"},
                                           .projectIdentity = "project-one",
                                           .authorizationRevision = 3});
        auto started = tools.Value()[1].adapter->Invoke(nlohmann::json::object(), context);
        REQUIRE(started.HasValue());
        REQUIRE(fixture.application->Current().Value());
        auto operation = fixture.application->Current().Value()->id;
        ++generation;
        CHECK(tools.Value()[0].adapter->Invoke(nlohmann::json::object(), context).HasError());
        context.projectIdentity = "another-project";
        CHECK(tools.Value()[5].adapter->Invoke({{"operationId", std::to_string(operation.value)}}, context).HasError());
        CHECK(WaitForTerminal(fixture, operation).outcome == PackageOperationOutcome::Succeeded);
    }

    TEST_CASE("Package modal cancellation addresses only its actual operation", "[packages][application][editor]") {
        Fixture fixture;
        fixture.source.block = true;
        Editor::PackageModalSession modal{Borrow(fixture)};
        auto opening = modal.Open();
        REQUIRE(opening.HasValue());
        auto operation = modal.Submit(opening.Value(), PackageOperationKind::Restore);
        REQUIRE(operation.HasValue());
        REQUIRE(fixture.source.entered.try_acquire_for(std::chrono::seconds{2}));
        REQUIRE(modal.Refresh(opening.Value()).HasValue());
        REQUIRE(modal.Cancel(opening.Value()).HasValue());
        fixture.source.Unblock();
        CHECK(WaitForTerminal(fixture, operation.Value()).outcome == PackageOperationOutcome::Cancelled);
        REQUIRE(modal.Close(opening.Value()).HasValue());
    }

    TEST_CASE("Package CLI restore disable and explicit selection preserve canonical terminal outcomes", "[packages][application][cli]") {
        Fixture fixture;
        auto dispatcher = Dispatcher(fixture);
        auto configuration = Configuration();
        auto restored = dispatcher.Dispatch({.command = {{"package", "restore"}}}, {.invocation = {1}, .configuration = &configuration});
        REQUIRE(restored.Outcome().HasValue());
        REQUIRE(restored.Correlation().operation);
        CHECK(fixture.application->Current().Value()->kind == PackageOperationKind::Restore);
        CHECK(fixture.application->Current().Value()->outcome == PackageOperationOutcome::Succeeded);
        auto disabled = dispatcher.Dispatch({.command = {{"package", "disable"}}}, {.invocation = {2}, .configuration = &configuration});
        REQUIRE(disabled.Outcome().HasValue());
        CHECK(fixture.application->Current().Value()->kind == PackageOperationKind::Disable);
        CHECK_FALSE(fixture.application->Current().Value()->activeGeneration);
        CHECK(fixture.source.calls == 1);

        Cli::CliCommandRequest activation{.command = {{"package", "activate"}}};
        SECTION("missing explicit selection") {}
        SECTION("selection is not a string") {
            activation.options = {{"enable", {{true}}}};
        }
        SECTION("missing descriptor separator") {
            activation.options = {{"enable", {{std::string{"com.horo.application-fixture"}}}}};
        }
        SECTION("invalid package") {
            activation.options = {{"enable", {{std::string{"INVALID:extension.json"}}}}};
        }
        SECTION("escaping descriptor") {
            activation.options = {{"enable", {{std::string{"com.horo.application-fixture:../outside"}}}}};
        }
        SECTION("oversized selection") {
            activation.options = {{"enable", {{std::string(1282, 'x')}}}};
        }
        SECTION("too many selections") {
            activation.options = {
                {"enable", std::vector<Cli::CliResolvedValue>(65, {std::string{"com.horo.application-fixture:extension.json"}})}};
        }
        SECTION("unverified declared descriptor") {
            activation.options = {{"enable", {{std::string{"com.horo.application-fixture:extensions/native/extension.json"}}}}};
        }
        const auto before = fixture.lifecycle->Active();
        auto rejected = dispatcher.Dispatch(activation, {.invocation = {3}, .configuration = &configuration});
        REQUIRE(rejected.Outcome().HasError());
        CHECK(fixture.lifecycle->Active() == before);
        CHECK(fixture.source.calls == 1);
    }

    TEST_CASE("Package CLI safe-point failure retains its typed cause and acknowledges admitted work", "[packages][application][cli]") {
        Fixture fixture;
        unsigned observations{};
        auto dispatcher = Dispatcher(fixture, [&] {
            ++observations;
            if (observations > 1)
                return Result<Packages::PackageActivationBoundary>::Failure(
                    Error{ErrorCode{"fixture.boundary.revoked"}, ErrorDomainId{"fixture.boundary"}});
            return Result<Packages::PackageActivationBoundary>::Success(Packages::PackageActivationBoundary::Quiescent);
        });
        auto configuration = Configuration();
        auto result = dispatcher.Dispatch({.command = {{"package", "restore"}}}, {.invocation = {1}, .configuration = &configuration});
        REQUIRE(result.Outcome().HasError());
        CHECK(result.Outcome().ErrorValue().code.Value() == "fixture.boundary.revoked");
        REQUIRE(result.Correlation().operation);
        REQUIRE(fixture.application->Current().Value());
        CHECK(fixture.application->Current().Value()->outcome != PackageOperationOutcome::Running);
        CHECK(fixture.source.calls.load() <= 1);
    }

    Mcp::McpRequestContext PackageContext() {
        return Mcp::Test::Context({.clientIdentity = "test-client",
                                   .capabilities = {"horo.package.manage"},
                                   .projectIdentity = "project-one",
                                   .authorizationRevision = 3});
    }

    TEST_CASE("Package MCP registry enforces capability and input schema before application observation", "[packages][application][mcp]") {
        Fixture fixture;
        auto tools = Mcp::MakePackageTools(Borrow(fixture), "project-one", 1, [] {
            return 1U;
        });
        REQUIRE(tools.HasValue());
        Mcp::McpToolRegistry registry{Mcp::Test::Authorization()};
        const std::vector<std::string> capabilities{"horo.package.manage"};
        REQUIRE(registry.Publish(std::move(tools).Value(), capabilities).HasValue());
        const auto snapshot = registry.Read();
        auto denied = Mcp::Test::Context({.clientIdentity = "test-client", .projectIdentity = "project-one", .authorizationRevision = 3});
        auto noGrant = snapshot->Invoke({"package.operation"}, {{"operationId", "1"}}, denied);
        REQUIRE(noGrant.HasError());
        CHECK(noGrant.ErrorValue().code.Value() == Mcp::McpErrors::ToolCapabilityUnavailable.code.Value());
        auto admitted = PackageContext();
        for (const auto &invalid :
             {nlohmann::json::object(), nlohmann::json{{"operationId", 1}}, nlohmann::json{{"operationId", "1"}, {"extra", true}}}) {
            auto rejected = snapshot->Invoke({"package.operation"}, invalid, admitted);
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == Mcp::McpErrors::ToolInputInvalid.code.Value());
        }
        CHECK_FALSE(fixture.application->Current().Value());
        CHECK(fixture.source.calls == 0);
        const auto disabled = fixture.application->Disable();
        REQUIRE(disabled.HasValue());
        REQUIRE(WaitForTerminal(fixture, disabled.Value()).outcome == PackageOperationOutcome::Succeeded);
        auto observed = snapshot->Invoke({"package.operation"}, {{"operationId", std::to_string(disabled.Value().value)}}, admitted);
        REQUIRE(observed.HasValue());
        CHECK(observed.Value()["kind"] == static_cast<unsigned>(PackageOperationKind::Disable));
        CHECK(observed.Value()["outcome"] == static_cast<unsigned>(PackageOperationOutcome::Succeeded));
        CHECK(fixture.source.calls == 0);
    }

    TEST_CASE("Package MCP metadata construction rejects unavailable host bindings without starting work", "[packages][application][mcp]") {
        Fixture fixture;
        const auto current = [] {
            return 1U;
        };
        CHECK(Mcp::MakePackageTools({}, "project-one", 1, current).HasError());
        CHECK(Mcp::MakePackageTools(Borrow(fixture), "", 1, current).HasError());
        CHECK(Mcp::MakePackageTools(Borrow(fixture), std::string(257, 'x'), 1, current).HasError());
        CHECK(Mcp::MakePackageTools(Borrow(fixture), std::string(1, static_cast<char>(0xff)), 1, current).HasError());
        CHECK(Mcp::MakePackageTools(Borrow(fixture), "project-one", 0, current).HasError());
        CHECK(Mcp::MakePackageTools(Borrow(fixture), "project-one", 1, {}).HasError());
        CHECK(Mcp::MakePackageTools(Borrow(fixture), "project-one", 1, current, Mcp::McpOwnerContext::Build).HasError());
        CHECK_FALSE(fixture.application->Current().Value());
        CHECK(fixture.source.calls == 0);
        CHECK(fixture.trust.calls == 0);
    }

    TEST_CASE("Package MCP tools encode real terminal state and reject noncanonical operation identities", "[packages][application][mcp]") {
        Fixture fixture;
        auto tools = Mcp::MakePackageTools(Borrow(fixture), "project-one", 1, [] {
            return 1U;
        });
        REQUIRE(tools.HasValue());
        auto context = PackageContext();
        auto restore = tools.Value()[0].adapter->Invoke(nlohmann::json::object(), context);
        REQUIRE(restore.HasValue());
        const auto operation = fixture.application->Current().Value()->id;
        REQUIRE(WaitForTerminal(fixture, operation).outcome == PackageOperationOutcome::Succeeded);
        auto terminal = tools.Value()[4].adapter->Invoke({{"operationId", std::to_string(operation.value)}}, context);
        REQUIRE(terminal.HasValue());
        CHECK(terminal.Value()["operationId"] == std::to_string(operation.value));
        CHECK(terminal.Value()["outcome"] == static_cast<unsigned>(PackageOperationOutcome::Succeeded));
        CHECK(terminal.Value()["completedPackages"] == 1);
        CHECK(terminal.Value()["diagnostic"].empty());
        for (const char *invalid : {"0", "01", "-1", "1x", "18446744073709551616"}) {
            auto rejected = tools.Value()[4].adapter->Invoke({{"operationId", invalid}}, context);
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == Mcp::McpErrors::ToolInputInvalid.code.Value());
        }
        CHECK(tools.Value()[4].adapter->Invoke({{"operationId", "99999"}}, context).HasError());
        CHECK(fixture.source.calls == 1);
        REQUIRE(fixture.application->Close().HasValue());
        auto closed = tools.Value()[4].adapter->Invoke({{"operationId", std::to_string(operation.value)}}, context);
        REQUIRE(closed.HasError());
        CHECK(closed.ErrorValue().code.Value() == "packages.application.closed");
    }

    TEST_CASE("Package MCP explicit selections preserve the active graph and disable reports canonical success",
              "[packages][application][mcp]") {
        Fixture fixture;
        auto tools = Mcp::MakePackageTools(Borrow(fixture), "project-one", 1, [] {
            return 1U;
        });
        REQUIRE(tools.HasValue());
        auto context = PackageContext();
        const auto installed = fixture.application->Install();
        REQUIRE(installed.HasValue());
        REQUIRE(WaitForTerminal(fixture, installed.Value()).outcome == PackageOperationOutcome::Succeeded);
        const auto before = fixture.lifecycle->Active();
        for (const auto &selection : {nlohmann::json{{"package", "INVALID"}, {"descriptor", "extension.json"}},
                                      nlohmann::json{{"package", "com.horo.application-fixture"}, {"descriptor", "../outside"}}}) {
            CHECK(tools.Value()[2].adapter->Invoke({{"enabled", nlohmann::json::array({selection})}}, context).HasError());
            CHECK(fixture.lifecycle->Active() == before);
        }
        auto selected = tools.Value()[2].adapter->Invoke({{"enabled",
                                                           {{{"package", "com.horo.application-fixture"},
                                                             {"descriptor", "extensions/native/extension.json"}}}}},
                                                         context);
        REQUIRE(selected.HasValue());
        const auto activation = fixture.application->Current().Value()->id;
        const auto failed = WaitForTerminal(fixture, activation);
        CHECK(failed.outcome == PackageOperationOutcome::Failed);
        REQUIRE(failed.diagnostic);
        CHECK(fixture.lifecycle->Active() == before);
        auto diagnostic = tools.Value()[4].adapter->Invoke({{"operationId", std::to_string(activation.value)}}, context);
        REQUIRE(diagnostic.HasValue());
        CHECK_FALSE(diagnostic.Value()["diagnostic"].empty());
        auto disabled = tools.Value()[3].adapter->Invoke(nlohmann::json::object(), context);
        REQUIRE(disabled.HasValue());
        const auto operation = fixture.application->Current().Value()->id;
        CHECK(WaitForTerminal(fixture, operation).outcome == PackageOperationOutcome::Succeeded);
        CHECK(fixture.source.calls == 1);
    }

    TEST_CASE("Package MCP cancellation retains running work through source acknowledgment", "[packages][application][mcp]") {
        Fixture fixture;
        fixture.source.block = true;
        auto tools = Mcp::MakePackageTools(Borrow(fixture), "project-one", 1, [] {
            return 1U;
        });
        REQUIRE(tools.HasValue());
        auto context = PackageContext();
        REQUIRE(tools.Value()[0].adapter->Invoke(nlohmann::json::object(), context).HasValue());
        const auto operation = fixture.application->Current().Value()->id;
        REQUIRE(fixture.source.entered.try_acquire_for(std::chrono::seconds{2}));
        REQUIRE(tools.Value()[5].adapter->Invoke({{"operationId", std::to_string(operation.value)}}, context).HasValue());
        CHECK(fixture.application->Query(operation).Value().outcome == PackageOperationOutcome::Running);
        fixture.source.Unblock();
        CHECK(WaitForTerminal(fixture, operation).outcome == PackageOperationOutcome::Cancelled);
        CancellationSource stop;
        stop.RequestCancellation();
        context.cancellation = stop.Token();
        auto rejected = tools.Value()[1].adapter->Invoke(nlohmann::json::object(), context);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == Mcp::McpErrors::RequestCancelled.code.Value());
        CHECK(fixture.source.calls == 1);
    }
}  // namespace
