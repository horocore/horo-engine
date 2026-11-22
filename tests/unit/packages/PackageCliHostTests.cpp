#include "Horo/Cli/CliErrors.h"
#include "Horo/Packages/PackageRequest.h"
#include "PackageApplicationServiceTestSupport.h"
#include "PackageCliHost.h"

#include <fstream>
#include <iterator>

namespace {
    using namespace Horo;
    using namespace Horo::Cli;
    using namespace Horo::Application;
    using namespace Horo::Application::Internal;

    class InvocationProbe final : public ICliCommandAdapter {
    public:
        explicit InvocationProbe(std::function<void(CliExecutionContext &)> callback)
            : descriptor_(DescribePackageCommands().front()), callback_(std::move(callback)) {
            descriptor_.timeout = {.defaultMilliseconds = 5'000, .maximumMilliseconds = 5'000};
        }

        const CliCommandDescriptor &GetDescriptor() const noexcept override {
            return descriptor_;
        }

        Result<CliCommandResult> Execute(const CliCommandRequest &, CliExecutionContext &context) override {
            callback_(context);
            return Result<CliCommandResult>::Success({});
        }

    private:
        CliCommandDescriptor descriptor_;
        std::function<void(CliExecutionContext &)> callback_;
    };

    // The dispatcher creates the real invocation authority; the probe never fabricates a context.
    void InInvocation(std::function<void(CliExecutionContext &)> callback, CancellationToken cancellation = {},
                      const std::uint64_t timeout = 5'000) {
        bool executed = false;
        auto adapter = std::make_unique<InvocationProbe>([&](CliExecutionContext &context) {
            executed = true;
            callback(context);
        });
        const auto descriptor = adapter->GetDescriptor();
        auto registry = CliCommandRegistry::Create(std::span{&descriptor, 1}, {.activeHost = CliHostKind::HoroEngine,
                                                                               .supportedContractVersion = {1, 0, 0},
                                                                               .grantedCapabilities = {{"horo.package.manage"}}});
        REQUIRE(registry.HasValue());
        std::vector<CliCommandAdapterRegistration> registrations;
        registrations.push_back({std::move(adapter), {{"horo.package.manage"}}});
        auto dispatcher = CliDispatcher::Create(std::move(registry).Value(), std::move(registrations),
                                                {.activeHost = CliHostKind::HoroEngine,
                                                 .maximumSideEffects = CliSideEffectPolicy::WritesFiles,
                                                 .grantedCapabilities = {{"horo.package.manage"}}});
        REQUIRE(dispatcher.HasValue());
        ConfigurationSchema schema;
        REQUIRE(schema.Seal().HasValue());
        auto configuration = ConfigurationResolver::Resolve(schema, {}, 1);
        REQUIRE(configuration.HasValue());
        auto admitted = std::move(dispatcher).Value();
        static_cast<void>(admitted.Dispatch({.command = descriptor.path}, {.invocation = {1},
                                                                           .configuration = &configuration.Value(),
                                                                           .cancellation = std::move(cancellation),
                                                                           .timeoutMilliseconds = timeout}));
        REQUIRE(executed);
    }

    struct NativeProject final {
        Tests::Packages::TemporaryDirectory project{"cli package project ü"}, storage{"cli package storage ü"};
        Tests::PackageApplicationService::AssetGraph graph;

        NativeProject() {
            const nlohmann::json intent{{"sources", {{"fixture.source", {{"kind", "public-registry"}, {"registry", "official"}}}}},
                                        {"dependencies",
                                         {{"com.horo.application-fixture", {{"source", "fixture.source"}, {"version", "1.0.0"}}}}}};
            auto request = Packages::ValidatedPackageRequest::Parse(intent.dump());
            REQUIRE(request.HasValue());
            Packages::PackageResolutionPlan plan{.packages = {{graph.package, graph.version, graph.source, graph.archive.Digest(), {}}}};
            Packages::PackageLockArtifact artifact{.package = graph.package,
                                                   .version = graph.version,
                                                   .source = graph.source,
                                                   .artifactDigest = graph.archive.Digest(),
                                                   .manifestDigest = graph.archive.PackageManifestDigest(),
                                                   .fileManifestDigest = graph.archive.Manifest().Digest(),
                                                   .packageFormatVersion = 1,
                                                   .platforms = {},
                                                   .contributions = {"assets"}};
            auto lock = Packages::ValidatedPackageLockfileV1::Generate(plan, std::span{&graph.package, 1U}, request.Value().Digest(),
                                                                       std::span{&artifact, 1U});
            REQUIRE(lock.HasValue());
            std::filesystem::create_directories(project.Path() / ".horo");
            std::filesystem::create_directories(storage.Path() / "temporary");
            Write("packages.json", intent.dump());
            Write("packages.lock", lock.Value().SerializeCanonical());
        }

        void Write(const char *name, const std::string &text) {
            std::ofstream output{project.Path() / ".horo" / name, std::ios::binary};
            output << text;
            REQUIRE(output.good());
        }

        static CliParsedOption PathOption(const char *name, const std::filesystem::path &path) {
            const auto utf8 = path.generic_u8string();
            return {name, {{CliPathValue{std::string{utf8.begin(), utf8.end()}}, CliValueSource::Invocation, {}}}};
        }

        CliCommandRequest Request() const {
            return {.command = {{"package", "restore"}},
                    .options = {PathOption("project", std::filesystem::canonical(project.Path())),
                                PathOption("package-cache-root", std::filesystem::canonical(storage.Path()) / "cache"),
                                PathOption("package-temporary-root", std::filesystem::canonical(storage.Path()) / "temporary")}};
        }
    };

    TEST_CASE("Headless package update preview reads candidate documents without repairing or modifying the project",
              "[packages][application][cli][update]") {
        NativeProject fixture;
        const auto root = std::filesystem::canonical(fixture.project.Path());
        const auto metadata = root / ".horo";
        const auto read = [](const std::filesystem::path &path) {
            const std::ifstream input{path, std::ios::binary};
            REQUIRE(input.good());
            return std::string{std::istreambuf_iterator<char>{input.rdbuf()}, {}};
        };
        const auto originalRequest = read(metadata / "packages.json");
        const auto originalLock = read(metadata / "packages.lock");
        auto candidate = nlohmann::json::parse(originalRequest);
        candidate["dependencies"] = nlohmann::json::object();
        const auto intent = Packages::ValidatedPackageRequest::Parse(candidate.dump());
        REQUIRE(intent.HasValue());
        const auto lock = Packages::ValidatedPackageLockfileV1::Generate({}, {}, intent.Value().Digest(), {});
        REQUIRE(lock.HasValue());
        fixture.Write("candidate.json", candidate.dump());
        fixture.Write("candidate.lock", lock.Value().SerializeCanonical());
        auto registration = MakePackageUpdatePreviewCommandAdapter();
        const auto descriptor = registration.adapter->GetDescriptor();
        CHECK(descriptor.sideEffects == CliSideEffectPolicy::ReadsState);
        const CliDispatchPolicy policy{.activeHost = CliHostKind::HoroEngine,
                                       .maximumSideEffects = CliSideEffectPolicy::ReadsState,
                                       .grantedCapabilities = registration.capabilities};
        auto registry = CliCommandRegistry::Create(std::span{&descriptor, 1}, {.activeHost = policy.activeHost,
                                                                               .supportedContractVersion = {1, 0, 0},
                                                                               .grantedCapabilities = policy.grantedCapabilities});
        REQUIRE(registry.HasValue());
        std::vector<CliCommandAdapterRegistration> adapters;
        adapters.push_back(std::move(registration));
        auto dispatcher = CliDispatcher::Create(std::move(registry).Value(), std::move(adapters), policy);
        REQUIRE(dispatcher.HasValue());
        auto admitted = std::move(dispatcher).Value();
        ConfigurationSchema schema;
        REQUIRE(schema.Seal().HasValue());
        const auto configuration = ConfigurationResolver::Resolve(schema, {}, 1);
        REQUIRE(configuration.HasValue());
        const CliCommandRequest command{.command = descriptor.path,
                                        .options = {NativeProject::PathOption("project", root),
                                                    NativeProject::PathOption("candidate-request", metadata / "candidate.json"),
                                                    NativeProject::PathOption("candidate-lock", metadata / "candidate.lock")}};
        const auto execute = [&] {
            return admitted.Dispatch(command, {.invocation = {1}, .configuration = &configuration.Value()});
        };
        SECTION("the preview reports the exact removal") {
            const auto result = execute();
            REQUIRE(result.Outcome().HasValue());
            const auto &fields = result.Outcome().Value().fields;
            REQUIRE(fields.size() == 4);
            CHECK(std::get<bool>(fields[0].value));
            CHECK(std::get<std::int64_t>(fields[1].value) == 1);
            CHECK(std::get<std::int64_t>(fields[2].value) == -1);
            CHECK(std::get<std::string>(fields[3].value).starts_with("com.horo.application-fixture [1.0.0 fixture.source "));
            CHECK(std::get<std::string>(fields[3].value).ends_with("] -> [absent]"));
        }
        SECTION("a pending publication is left intact") {
            std::filesystem::create_directory(metadata / "local");
            fixture.Write("local/package-metadata.pending", "pending publication");
            CHECK(execute().Outcome().HasError());
            CHECK(read(metadata / "local/package-metadata.pending") == "pending publication");
        }
        CHECK(read(metadata / "packages.json") == originalRequest);
        CHECK(read(metadata / "packages.lock") == originalLock);
    }

    TEST_CASE("Concrete package CLI host rejects malformed typed paths before constructing a project", "[packages][application][cli]") {
        NativeProject fixture;
        auto request = fixture.Request();
        SECTION("missing project") {
            request.options.erase(request.options.begin());
        }
        SECTION("missing cache") {
            request.options.erase(request.options.begin() + 1);
        }
        SECTION("missing temporary") {
            request.options.pop_back();
        }
        SECTION("wrong typed value") {
            request.options.front().values.front().value = std::string{"not a typed path"};
        }
        SECTION("multiple values") {
            request.options.front().values.push_back(request.options.front().values.front());
        }
        SECTION("relative") {
            request.options.front().values.front().value = CliPathValue{"relative/project"};
        }
        SECTION("not normalized") {
            request.options.front().values.front().value =
                CliPathValue{std::filesystem::canonical(fixture.project.Path()).string() + "/../project"};
        }
        PackageCliHost host;
        InInvocation([&](CliExecutionContext &context) {
            auto result = host.Bind(request, context);
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == CliErrors::ParseFailed.code.Value());
        });
        CHECK(host.Project() == nullptr);
        REQUIRE(host.Boundary().HasError());
    }

    TEST_CASE("Concrete package CLI binding retains its exact project and rejects path rebinding", "[packages][application][cli]") {
        NativeProject fixture;
        const auto request = fixture.Request();
        PackageCliHost host;
        InInvocation([&](CliExecutionContext &context) {
            auto first = host.Bind(request, context);
            REQUIRE(first.HasValue());
            REQUIRE(host.Project() != nullptr);
            const auto generation = host.Project()->Generation();
            REQUIRE(generation > 0);
            auto repeated = host.Bind(request, context);
            REQUIRE(repeated.HasValue());
            CHECK(first.Value() == repeated.Value());
            CHECK(host.Boundary().Value() == Packages::PackageActivationBoundary::Quiescent);
            for (std::size_t index = 0; index < request.options.size(); ++index) {
                auto different = request;
                std::get<CliPathValue>(different.options[index].values.front().value).value += "/another";
                auto rejected = host.Bind(different, context);
                REQUIRE(rejected.HasError());
                CHECK(rejected.ErrorValue().code.Value() == CliErrors::ExecutionContextInvalid.code.Value());
                CHECK(host.Project()->Generation() == generation);
            }
        });
    }

    TEST_CASE("Concrete package CLI host observes admitted cancellation and deadline before metadata admission",
              "[packages][application][cli]") {
        NativeProject fixture;
        CancellationSource stop;
        PackageCliHost host;
        SECTION("cooperative cancellation") {
            InInvocation([&](CliExecutionContext &context) {
                stop.RequestCancellation();
                auto result = host.Bind(fixture.Request(), context);
                REQUIRE(result.HasError());
                CHECK(result.ErrorValue().code.Value() == CliErrors::ExecutionCancelled.code.Value());
            }, stop.Token());
        }
        SECTION("finite deadline") {
            InInvocation([&](CliExecutionContext &context) {
                const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds{1};
                while (!context.IsStopRequested() && std::chrono::steady_clock::now() < limit)
                    std::this_thread::yield();
                REQUIRE(context.IsStopRequested());
                auto result = host.Bind(fixture.Request(), context);
                REQUIRE(result.HasError());
                CHECK(result.ErrorValue().code.Value() == CliErrors::ExecutionTimedOut.code.Value());
            }, {}, 1);
        }
        REQUIRE(host.Project() != nullptr);
        CHECK(host.Project()->Generation() == 0);
    }

    TEST_CASE("Concrete MCP package binding exposes safe generation identity and retains closed application leases",
              "[packages][application][cli][mcp]") {
        NativeProject fixture;
        std::shared_ptr<PackageApplication> retained;
        {
            PackageCliHost host;
            InInvocation([&](CliExecutionContext &context) {
                auto bound = host.BindMcp(fixture.Request(), context);
                REQUIRE(bound.HasValue());
                auto binding = std::move(bound).Value();
                retained = binding.application;
                CHECK(binding.projectIdentity.starts_with("horo.project."));
                CHECK(binding.projectIdentity.find(fixture.project.Path().string()) == std::string::npos);
                REQUIRE(binding.generation > 0);
                CHECK(binding.currentGeneration() == binding.generation);
                REQUIRE(binding.pump().HasValue());
                auto again = host.BindMcp(fixture.Request(), context);
                REQUIRE(again.HasValue());
                CHECK(again.Value().projectIdentity == binding.projectIdentity);
                REQUIRE(host.Project()->Retire(std::filesystem::canonical(fixture.project.Path())).HasValue());
                CHECK(binding.currentGeneration() > binding.generation);
                CHECK(retained->Restore().HasError());
            });
        }
        REQUIRE(retained);
        CHECK(retained->Install().HasError());
        REQUIRE(retained->Close().HasValue());
    }
}  // namespace
