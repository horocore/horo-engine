#include "../../mcp/McpAuthorizationTestSupport.h"
#include "Horo/Cli/NavigationCommands.h"
#include "Horo/Mcp/McpSession.h"
#include "Horo/Mcp/NavigationTools.h"
#include "navigation/NavigationBakeApplicationFixture.h"

namespace Horo::Application {
    namespace {
        using NavigationApplicationTestSupport::Session;

        /** @brief Fixture lifetime exceeds adapter lifetime; production roots return an owning project lease. */
        std::shared_ptr<NavigationBakeApplication> Borrow(const Session &session) {
            return {session.application.get(), [](NavigationBakeApplication *) {
                // Session exclusively owns the application and outlives this borrowed adapter lease.
            }};
        }

        /** @brief Seals explicit invocation configuration for the real dispatcher. */
        ConfigurationSnapshot Configuration() {
            ConfigurationSchema schema;
            REQUIRE(schema.Seal().HasValue());
            auto resolved = ConfigurationResolver::Resolve(schema, {}, 1);
            REQUIRE(resolved.HasValue());
            return std::move(resolved).Value();
        }

        /** @brief Registers the actual adapter against its descriptor and exact capability. */
        Cli::CliDispatcher Dispatcher(const Session &session, unsigned &acquisitions) {
            const std::array descriptors{Cli::DescribeNavigationBakeCommand()};
            auto registry = Cli::CliCommandRegistry::Create(descriptors, {.activeHost = Cli::CliHostKind::HoroEngine,
                                                                          .supportedContractVersion = {1, 0, 0},
                                                                          .grantedCapabilities = {{"horo.navigation.bake"}}});
            REQUIRE(registry.HasValue());
            std::vector<Cli::CliCommandAdapterRegistration> adapters;
            adapters.push_back(Cli::MakeNavigationBakeCommandAdapter(
                [&session, &acquisitions](const Cli::CliCommandRequest &, Cli::CliExecutionContext &) {
                ++acquisitions;
                return Result<std::shared_ptr<NavigationBakeApplication>>::Success(Borrow(session));
            }));
            auto dispatcher = Cli::CliDispatcher::Create(std::move(registry).Value(), std::move(adapters),
                                                         {.activeHost = Cli::CliHostKind::HoroEngine,
                                                          .maximumSideEffects = Cli::CliSideEffectPolicy::WritesFiles,
                                                          .grantedCapabilities = {{"horo.navigation.bake"}}});
            REQUIRE(dispatcher.HasValue());
            return std::move(dispatcher).Value();
        }
    }  // namespace

    TEST_CASE("Navigation CLI uses canonical parser dispatcher and application for CI validation", "[navigation][application][cli]") {
        Session session;
        unsigned acquisitions{};
        auto dispatcher = Dispatcher(session, acquisitions);
        const std::array<std::string_view, 5> arguments{"navigation", "bake", "--definition", "00000000-0000-0000-0000-000000000001",
                                                        "--validate-only"};
        const auto parsed = Cli::CliOptionParser::Parse(arguments, dispatcher.Registry(), {});
        REQUIRE(parsed.HasValue());
        const auto configuration = Configuration();
        const auto terminal = dispatcher.Dispatch(parsed.Value(), {.invocation = {1}, .configuration = &configuration});
        REQUIRE(terminal.Outcome().HasValue());
        CHECK(acquisitions == 1);
        CHECK(terminal.Correlation().operation.has_value());
        const auto &fields = terminal.Outcome().Value().fields;
        const auto published = std::ranges::find(fields, "published", &Cli::CliResultField::name);
        REQUIRE(published != fields.end());
        CHECK_FALSE(std::get<bool>(published->value));
        CHECK(session.builder->builds.load() == 0);
        CHECK_FALSE(std::filesystem::exists(session.service.targetRoot));
    }

    TEST_CASE("Navigation CLI rejects malformed duplicate and unscoped selections before project acquisition",
              "[navigation][application][cli]") {
        Session session;
        unsigned acquisitions{};
        auto dispatcher = Dispatcher(session, acquisitions);
        const auto configuration = Configuration();
        for (const auto text : {"10", "00000000-0000-0000-0000-000000000002:0", "00000000-0000-0000-0000-000000000002:010"}) {
            const std::array<std::string_view, 6> arguments{"navigation", "bake", "--definition", "00000000-0000-0000-0000-000000000001",
                                                            "--surface",  text};
            const auto parsed = Cli::CliOptionParser::Parse(arguments, dispatcher.Registry(), {});
            REQUIRE(parsed.HasValue());
            CHECK(dispatcher.Dispatch(parsed.Value(), {.invocation = {1}, .configuration = &configuration}).Outcome().HasError());
        }
        for (const auto definition : {"invalid", "00000000-0000-0000-0000-000000000000", "00000000-0000-0000-0000-00000000000A"}) {
            const std::array<std::string_view, 4> arguments{"navigation", "bake", "--definition", definition};
            const auto invalid = Cli::CliOptionParser::Parse(arguments, dispatcher.Registry(), {});
            REQUIRE(invalid.HasValue());
            CHECK(dispatcher.Dispatch(invalid.Value(), {.invocation = {1}, .configuration = &configuration}).Outcome().HasError());
        }
        constexpr std::string_view surface = "00000000-0000-0000-0000-000000000002:10";
        const std::array<std::string_view, 8> duplicate{"navigation", "bake",  "--definition", "00000000-0000-0000-0000-000000000001",
                                                        "--surface",  surface, "--surface",    surface};
        const auto parsed = Cli::CliOptionParser::Parse(duplicate, dispatcher.Registry(), {});
        REQUIRE(parsed.HasValue());
        CHECK(dispatcher.Dispatch(parsed.Value(), {.invocation = {1}, .configuration = &configuration}).Outcome().HasError());
        CHECK(acquisitions == 0);
    }

    TEST_CASE("Navigation MCP observes the shared operation and rejects replaced project authority", "[navigation][application][mcp]") {
        Session session;
        std::uint64_t generation{7};
        auto tools = Mcp::MakeNavigationTools(Borrow(session), "project-one", generation, [&generation] {
            return generation;
        });
        REQUIRE(tools.HasValue());
        REQUIRE(tools.Value().size() == 3);
        auto context = Mcp::Test::Context({.clientIdentity = "test-client",
                                           .capabilities = {"horo.navigation.bake"},
                                           .projectIdentity = "project-one",
                                           .authorizationRevision = 3});
        const auto started = tools.Value()[0].adapter->Invoke({{"validateOnly", true}}, context);
        REQUIRE(started.HasValue());
        const auto id = std::stoull(started.Value().at("operationId").get<std::string>());
        REQUIRE(session.Finish(id).state == OperationState::Succeeded);
        const auto observed = tools.Value()[1].adapter->Invoke({{"operationId", std::to_string(id)}}, context);
        REQUIRE(observed.HasValue());
        CHECK(observed.Value().at("validateOnly") == true);
        CHECK(observed.Value().at("published") == false);
        ++generation;
        CHECK(tools.Value()[0].adapter->Invoke(nlohmann::json::object(), context).HasError());
        context.projectIdentity = "another-project";
        CHECK(tools.Value()[2].adapter->Invoke({{"operationId", std::to_string(id)}}, context).HasError());
        CHECK(session.builder->builds.load() == 0);
    }

    TEST_CASE("Navigation MCP observation never advances capture into bake admission", "[navigation][application][mcp]") {
        Session session;
        auto tools = Mcp::MakeNavigationTools(Borrow(session), "project-one", 1, [] {
            return 1U;
        });
        REQUIRE(tools.HasValue());
        auto context = Mcp::Test::Context({.clientIdentity = "test-client",
                                           .capabilities = {"horo.navigation.bake"},
                                           .projectIdentity = "project-one",
                                           .authorizationRevision = 3});
        const auto started = tools.Value()[0].adapter->Invoke(nlohmann::json::object(), context);
        REQUIRE(started.HasValue());
        const auto id = std::stoull(started.Value().at("operationId").get<std::string>());
        bool captureDrained{};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline) {
            if (const auto jobs = session.jobs.SnapshotIfChanged(0);
                jobs.has_value() && !jobs->jobs.empty() && std::ranges::all_of(jobs->jobs, [](const JobSnapshot &job) {
                return job.terminalResult.has_value();
            })) {
                captureDrained = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        REQUIRE(captureDrained);
        for (unsigned observation = 0; observation < 3; ++observation) {
            const auto observed = tools.Value()[1].adapter->Invoke({{"operationId", std::to_string(id)}}, context);
            REQUIRE(observed.HasValue());
            CHECK(observed.Value().at("phase") == "capture");
            const auto snapshot = session.application->Query(id);
            REQUIRE(snapshot.HasValue());
            CHECK_FALSE(snapshot.Value().bakeOperation.has_value());
        }
        CHECK(session.builder->builds.load() == 0);
        CHECK_FALSE(std::filesystem::exists(session.service.targetRoot));
        CHECK(session.Finish(id).state == OperationState::Succeeded);
        CHECK(session.builder->builds.load() > 0);
    }

    TEST_CASE("Navigation MCP cancels accepted capture without publication", "[navigation][application][mcp]") {
        Session session;
        auto tools = Mcp::MakeNavigationTools(Borrow(session), "project-one", 1, [] {
            return 1U;
        });
        REQUIRE(tools.HasValue());
        auto context = Mcp::Test::Context({.clientIdentity = "test-client",
                                           .capabilities = {"horo.navigation.bake"},
                                           .projectIdentity = "project-one",
                                           .authorizationRevision = 3});
        const auto started = tools.Value()[0].adapter->Invoke(nlohmann::json::object(), context);
        REQUIRE(started.HasValue());
        const auto id = std::stoull(started.Value().at("operationId").get<std::string>());
        REQUIRE(tools.Value()[2].adapter->Invoke({{"operationId", std::to_string(id)}}, context).HasValue());
        const auto terminal = session.Finish(id);
        CHECK(terminal.state == OperationState::Cancelled);
        CHECK_FALSE(terminal.publication);
        CHECK(session.builder->builds.load() == 0);
        CHECK_FALSE(std::filesystem::exists(session.service.targetRoot));
    }

    TEST_CASE("Navigation MCP rejects invalid exact selections and expired requests before capture", "[navigation][application][mcp]") {
        Session session;
        auto tools = Mcp::MakeNavigationTools(Borrow(session), "project-one", 1, [] {
            return 1U;
        });
        REQUIRE(tools.HasValue());
        auto context = Mcp::Test::Context({.clientIdentity = "test-client",
                                           .capabilities = {"horo.navigation.bake"},
                                           .projectIdentity = "project-one",
                                           .authorizationRevision = 3});
        const nlohmann::json surface{{"scene", "00000000-0000-0000-0000-000000000002"}, {"surface", "10"}};
        auto invalid = surface;
        invalid["surface"] = "010";
        auto zeroScene = surface;
        zeroScene["scene"] = "00000000-0000-0000-0000-000000000000";
        for (const auto &selection : {nlohmann::json::array(), nlohmann::json::array({surface, surface}), nlohmann::json::array({invalid}),
                                      nlohmann::json::array({zeroScene})})
            CHECK(tools.Value()[0].adapter->Invoke({{"surfaces", selection}}, context).HasError());
        for (const auto identity : {"0", "01", "-1", "18446744073709551616"})
            CHECK(tools.Value()[1].adapter->Invoke({{"operationId", identity}}, context).HasError());
        context.deadline = std::chrono::steady_clock::now();
        CHECK(tools.Value()[0].adapter->Invoke(nlohmann::json::object(), context).HasError());
        CHECK_FALSE(session.application->Query(1).HasValue());
        CHECK(session.builder->builds.load() == 0);
    }

    TEST_CASE("Navigation MCP preserves a valid selected surface through shared validation", "[navigation][application][mcp]") {
        Session session;
        REQUIRE(session.Execute(Navigation::NavigationBakeExecutionMode::BuildAndPublish).state == OperationState::Succeeded);
        const auto builds = session.builder->builds.load();
        auto tools = Mcp::MakeNavigationTools(Borrow(session), "project-one", 1, [] {
            return 1U;
        });
        REQUIRE(tools.HasValue());
        auto context = Mcp::Test::Context({.clientIdentity = "test-client",
                                           .capabilities = {"horo.navigation.bake"},
                                           .projectIdentity = "project-one",
                                           .authorizationRevision = 3});
        const nlohmann::json selected{{"scene", "00000000-0000-0000-0000-000000000002"}, {"surface", "10"}};
        const auto started =
            tools.Value()[0].adapter->Invoke({{"validateOnly", true}, {"surfaces", nlohmann::json::array({selected})}}, context);
        REQUIRE(started.HasValue());
        const auto terminal = session.Finish(std::stoull(started.Value().at("operationId").get<std::string>()));
        INFO("Validation diagnostic: " << (terminal.diagnostic ? terminal.diagnostic->message : std::string{}));
        CHECK(terminal.state == OperationState::Succeeded);
        CHECK_FALSE(terminal.publication);
        CHECK(session.builder->builds.load() == builds);
    }

    TEST_CASE("Navigation MCP construction rejects incomplete host ownership", "[navigation][application][mcp]") {
        Session session;
        const auto generation = [] {
            return 1U;
        };
        CHECK(Mcp::MakeNavigationTools({}, "project-one", 1, generation).HasError());
        CHECK(Mcp::MakeNavigationTools(Borrow(session), "", 1, generation).HasError());
        CHECK(Mcp::MakeNavigationTools(Borrow(session), "project-one", 0, generation).HasError());
        CHECK(Mcp::MakeNavigationTools(Borrow(session), "project-one", 1, {}).HasError());
        CHECK_FALSE(session.application->Query(1).HasValue());
    }

    TEST_CASE("Navigation MCP registry rejects missing capability and malformed operation input", "[navigation][application][mcp]") {
        Session session;
        auto tools = Mcp::MakeNavigationTools(Borrow(session), "project-one", 1, [] {
            return 1U;
        });
        REQUIRE(tools.HasValue());
        Mcp::McpToolRegistry registry{Mcp::Test::Authorization()};
        const std::vector<std::string> capabilities{"horo.navigation.bake"};
        REQUIRE(registry.Publish(std::move(tools).Value(), capabilities).HasValue());
        const auto snapshot = registry.Read();
        auto context = Mcp::Test::Context({.clientIdentity = "test-client", .projectIdentity = "project-one", .authorizationRevision = 3});
        CHECK(snapshot->Invoke({"navigation.operation"}, {{"operationId", "1"}}, context).HasError());
        context.capabilities = capabilities;
        for (const auto &input :
             {nlohmann::json::object(), nlohmann::json{{"operationId", 1}}, nlohmann::json{{"operationId", "1"}, {"extra", true}}})
            CHECK(snapshot->Invoke({"navigation.operation"}, input, context).HasError());
        CHECK(session.builder->builds.load() == 0);
        CHECK_FALSE(session.application->Query(1).HasValue());
    }

    TEST_CASE("Navigation MCP executes only an exact host-approved registry mutation and refuses replay",
              "[navigation][application][mcp]") {
        Session session;
        auto policy = std::make_shared<Mcp::McpAuthorization>(std::make_shared<Mcp::Test::Entropy>());
        REQUIRE(policy->SetTrust("project-one", 3, true).HasValue());
        auto tools = Mcp::MakeNavigationTools(Borrow(session), "project-one", 1, [] {
            return 1U;
        });
        REQUIRE(tools.HasValue());
        Mcp::McpToolRegistry registry{policy};
        const std::vector<std::string> capabilities{"horo.navigation.bake"};
        const auto published = registry.Publish(std::move(tools).Value(), capabilities);
        REQUIRE(published.HasValue());
        const auto admission = Mcp::Test::Authenticate({.clientIdentity = "local-client",
                                                        .capabilities = capabilities,
                                                        .projectIdentity = "project-one",
                                                        .authorizationRevision = 3,
                                                        .registryRevision = published.Value()},
                                                       policy);
        Mcp::McpRequestContext context{.session = {1, 1},
                                       .clientIdentity = admission.clientIdentity,
                                       .capabilities = admission.capabilities,
                                       .projectIdentity = admission.projectIdentity,
                                       .authorizationRevision = admission.authorizationRevision,
                                       .registryRevision = admission.registryRevision,
                                       .deadline = std::chrono::steady_clock::now() + std::chrono::minutes{1},
                                       .authority = admission.authority,
                                       .authorization = policy,
                                       .requestIdentity = 7};
        const nlohmann::json arguments{{"validateOnly", true}};
        const auto snapshot = registry.Read();
        CHECK(snapshot->Invoke({"navigation.bake"}, arguments, context).HasError());
        CHECK_FALSE(session.application->Query(1).HasValue());
        const Mcp::McpRequest request{.id = 7, .method = "tools/call", .params = {{"name", "navigation.bake"}, {"arguments", arguments}}};
        const auto challenge = policy->Challenge(context, request, std::chrono::minutes{1});
        REQUIRE(challenge.HasValue());
        REQUIRE(policy->Decide(challenge.Value(), true).HasValue());
        const auto started = snapshot->Invoke({"navigation.bake"}, arguments, context);
        REQUIRE(started.HasValue());
        const auto id = std::stoull(started.Value().at("operationId").get<std::string>());
        CHECK(session.Finish(id).state == OperationState::Succeeded);
        CHECK(snapshot->Invoke({"navigation.bake"}, arguments, context).HasError());
        CHECK_FALSE(session.application->Query(id + 1).HasValue());
        CHECK(session.builder->builds.load() == 0);
        CHECK_FALSE(std::filesystem::exists(session.service.targetRoot));
    }
}  // namespace Horo::Application
