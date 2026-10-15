#include "../mcp/McpAuthorizationTestSupport.h"
#include "Horo/Cli/PackageCommands.h"
#include "Horo/Editor/PackageModalSession.h"
#include "Horo/Mcp/PackageTools.h"
#include "PackageApplicationServiceTestSupport.h"

#include <algorithm>

namespace {
    using namespace Horo;
    using namespace Horo::Application;
    using namespace Horo::Tests::PackageApplicationService;

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

    Cli::CliDispatcher Dispatcher(Fixture &fixture) {
        const auto descriptors = Cli::DescribePackageCommands();
        auto registry = Cli::CliCommandRegistry::Create(descriptors, {.activeHost = Cli::CliHostKind::HoroEngine,
                                                                      .supportedContractVersion = {1, 0, 0},
                                                                      .grantedCapabilities = {{"horo.package.manage"}}});
        REQUIRE(registry.HasValue());
        auto adapters = Cli::MakePackageCommandAdapters([&fixture](const Cli::CliCommandRequest &, Cli::CliExecutionContext &) {
            return Result<std::shared_ptr<PackageApplication>>::Success(Borrow(fixture));
        }, [] {
            return Result<Packages::PackageActivationBoundary>::Success(Packages::PackageActivationBoundary::Quiescent);
        });
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
}  // namespace
