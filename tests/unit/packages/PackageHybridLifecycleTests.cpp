#include "../extensions/ScriptInvocationTestSupport.h"
#include "PackageLifecycleTestSupport.h"

using namespace Horo::Packages::Tests::Support;

namespace {
    /** @brief Builds a genuinely mixed archive with native importer, editor activity and script provider binaries. */
    std::shared_ptr<PackageRestoreGraph> HybridGraph(const bool incompatibleGui = false) {
        const auto suffix = std::filesystem::path{HORO_PACKAGE_NATIVE_FIXTURE}.extension().string();
        nlohmann::json descriptor{{"id", "fixture.package"},
                                  {"version", "1.0.0"},
                                  {"modules",
                                   {{{"id", "fixture.backend"},
                                     {"version", "1.0.0"},
                                     {"kind", "native"},
                                     {"roles", {"backend-capability"}},
                                     {"entry", "module" + suffix}},
                                    {{"id", "fixture.script"},
                                     {"version", "1.0.0"},
                                     {"kind", "native"},
                                     {"roles", {"script-provider"}},
                                     {"entry", "script" + suffix},
                                     {"dependencies", {"fixture.backend"}},
                                     {"requiredCapabilities", {"com.example.service"}}},
                                    {{"id", "fixture.module"},
                                     {"version", "1.0.0"},
                                     {"kind", "native"},
                                     {"roles", {"editor-presentation"}},
                                     {"entry", "gui" + suffix},
                                     {"abi", {{"major", incompatibleGui ? 99 : 1}, {"minimumMinor", 4}}},
                                     {"requiredCapabilities", {"editor.activity"}}}}},
                                  {"contributions",
                                   {{{"type", "asset.importer"}, {"id", "fixture.importer"}, {"module", "fixture.backend"}},
                                    {{"type", "editor.activity_item"}, {"id", "fixture.activity"}, {"module", "fixture.module"}},
                                    {{"type", "editor.panel"}, {"id", "fixture.drawer"}, {"module", "fixture.module"}}}}};
        return Graph(
            HORO_PACKAGE_NATIVE_FIXTURE, true, true, "fixture.package", std::move(descriptor),
            {{"extensions/native/script" + suffix, FileBytes(HORO_PACKAGE_EMPTY_FIXTURE)},
             {"extensions/native/gui" + suffix, FileBytes(HORO_EDITOR_ACTIVITY_FIXTURE)},
             {"extensions/native/icons/tool.svg",
              R"(<svg xmlns="http://www.w3.org/2000/svg" width="48" height="48"><path fill="#ffffff" d="M4 4 H44 V44 H4 Z"/></svg>)"}});
    }

    struct HybridFixture final {
        Fixture base;
        JobSystem jobs;
        Extensions::ScriptCapabilityScope project{Extensions::ScriptCapabilityScopeKind::Project};
        Extensions::ScriptCapabilityScope runtime{Extensions::ScriptCapabilityScopeKind::Runtime};
        Extensions::ScriptCapabilityScope scene{Extensions::ScriptCapabilityScopeKind::Scene};
        Extensions::ScriptCapabilityScope operation{Extensions::ScriptCapabilityScopeKind::Operation};
        std::shared_ptr<Extensions::EditorActivityHost> editor{std::make_shared<Extensions::EditorActivityHost>(jobs)};
        std::shared_ptr<Extensions::ScriptInvocationRegistry> scripts{std::make_shared<Extensions::ScriptInvocationRegistry>()};
        std::shared_ptr<PackageModuleRetirementQueue> retirement{std::make_shared<PackageModuleRetirementQueue>(4)};
        unsigned nativeLoads{};
        unsigned failNativeAt{};
        std::shared_ptr<NativePackageModuleHost> native;
        std::shared_ptr<ScriptPackageModuleHost> script;
        std::vector<EnabledPackageModule>
            enabled{{Parse<HoroPackageId>("fixture.package"), Parse<PackagePath>("extensions/native/extension.json"), "fixture.backend",
                     Extensions::ExtensionModuleRole::BackendCapability},
                    {Parse<HoroPackageId>("fixture.package"), Parse<PackagePath>("extensions/native/extension.json"), "fixture.script",
                     Extensions::ExtensionModuleRole::ScriptProvider},
                    {Parse<HoroPackageId>("fixture.package"), Parse<PackagePath>("extensions/native/extension.json"), "fixture.module",
                     Extensions::ExtensionModuleRole::EditorPresentation, PackageModuleRequirement::Optional}};

        explicit HybridFixture(const bool headless = false) {
            base.service.reset();
            base.trust.capabilities = {{"editor.activity"}, {"com.example.service"}};
            PackageLifecycleConfiguration configuration{.temporaryRoot = std::filesystem::canonical(base.temporary.Path()),
                                                        .profile = headless ? Extensions::ExtensionHostProfile::Headless
                                                                            : Extensions::ExtensionHostProfile::Interactive,
                                                        .capabilities = {"editor.activity", "com.example.service"},
                                                        .artifactGate = ::Horo::Tests::CreateAcceptingArtifactGate()};
            configuration.libraryLoader = [this](const std::string &path) -> Result<std::unique_ptr<Platform::DynamicLibrary>> {
                if (++nativeLoads == failNativeAt)
                    return Result<std::unique_ptr<Platform::DynamicLibrary>>::Failure(MakeError(PackageLifecycleErrors::StorageFailed));
                return Platform::LoadDynamicLibrary(path);
            };
            if (!headless)
                configuration.editorActivityHost = editor;
            native = std::make_shared<NativePackageModuleHost>(configuration);
            script =
                std::make_shared<ScriptPackageModuleHost>(std::vector<PackageScriptRegistryRealm>{{Parse<HoroPackageId>("fixture.package"),
                                                                                                   "fixture.script", scripts}},
                                                          native);
            configuration.moduleHosts = {native, script};
            configuration.moduleRetirements = retirement;
            auto created = PackageLifecycleService::Create(base.install, base.trust, std::move(configuration));
            REQUIRE(created.HasValue());
            base.service = std::move(created).Value();
        }

        ~HybridFixture() {
            base.service.reset();
            CHECK(retirement->Drain() == 0);
            CHECK(retirement->Close().HasValue());
        }

        Result<void> Activate() {
            return base.service->ActivateModules(enabled, PackageActivationBoundary::Quiescent, base.cancellation.Token());
        }
    };
}  // namespace

TEST_CASE("Hybrid package activates actual native editor and script owners independently", "[packages][extensions][hybrid]") {
    HybridFixture fixture;
    fixture.base.Install(HybridGraph());
    REQUIRE(fixture.Activate().HasValue());
    const auto snapshot = fixture.base.service->Active();
    REQUIRE(snapshot->modules.size() == 3);
    for (const auto &report : snapshot->modules)
        CHECK(report.outcome == PackageModuleOutcome::Active);
    fixture.editor->Update();
    CHECK(fixture.editor->Prepared().size() == 1);
    CHECK(fixture.native->Importers(fixture.enabled[0].package, "fixture.backend", snapshot->generation));
    const auto provider = fixture.script->Provider(fixture.enabled[0].package, "fixture.script", snapshot->generation);
    CHECK(provider.IsUsable());
    fixture.base.service->Shutdown();
    CHECK_FALSE(provider.IsUsable());
    fixture.editor->Update();
    CHECK(fixture.editor->Prepared().empty());
    CHECK(fixture.retirement->Drain() == 0);
}

TEST_CASE("Optional incompatible GUI does not poison verified backend and script activation", "[packages][extensions][hybrid]") {
    for (const bool headless : {false, true}) {
        HybridFixture fixture(headless);
        fixture.base.Install(HybridGraph(true));
        REQUIRE(fixture.Activate().HasValue());
        const auto snapshot = fixture.base.service->Active();
        REQUIRE(snapshot->modules.size() == 3);
        CHECK(snapshot->modules[0].outcome == PackageModuleOutcome::Active);
        CHECK(snapshot->modules[1].outcome == PackageModuleOutcome::Active);
        CHECK(snapshot->modules[2].outcome != PackageModuleOutcome::Active);
        CHECK(snapshot->modules[2].diagnostic.has_value());
        CHECK(snapshot->install == fixture.base.install.InstalledRecord());
        CHECK(fixture.script->Provider(fixture.enabled[0].package, "fixture.script", snapshot->generation).IsUsable());
    }
}

namespace {
    struct RetainedRoleWork final {
        bool busy{true};
        unsigned activationCalls{};
        unsigned failAt{};
        std::vector<std::string> finalized;
        std::vector<std::string> destroyed;
    };

    /** @brief Models an actual externally retained callback while checking the coordinator's concrete owner lifetime. */
    class RetainedRoleReceipt final : public IPackageModuleActivation {
    public:
        RetainedRoleReceipt(VerifiedPackageModule module, std::shared_ptr<RetainedRoleWork> work)
            : module_(std::move(module)), work_(std::move(work)) {}

        ~RetainedRoleReceipt() override {
            work_->destroyed.push_back(module_.Module().id);
        }

        bool IsActive() const noexcept override {
            return !retired_;
        }

        void Retire() noexcept override {
            retired_ = true;
        }

        bool Finalize() noexcept override {
            if (!retired_ || work_->busy)
                return false;
            work_->finalized.push_back(module_.Module().id);
            return true;
        }

    private:
        VerifiedPackageModule module_;
        std::shared_ptr<RetainedRoleWork> work_;
        bool retired_{};
    };

    class RetainedRoleHost final : public IPackageModuleHost {
    public:
        std::shared_ptr<RetainedRoleWork> work{std::make_shared<RetainedRoleWork>()};

        std::string_view Identity() const noexcept override {
            return "fixture.retained";
        }

        bool Supports(Extensions::ExtensionModuleRole role) const noexcept override {
            return role == Extensions::ExtensionModuleRole::BackendCapability || role == Extensions::ExtensionModuleRole::ScriptProvider;
        }

        Result<std::unique_ptr<IPackageModuleActivation>> Activate(const VerifiedPackageModule &module,
                                                                   std::span<const std::shared_ptr<IPackageModuleActivation>>,
                                                                   const CancellationToken &) override {
            if (++work->activationCalls == work->failAt)
                return Result<std::unique_ptr<IPackageModuleActivation>>::Failure(MakeError(PackageLifecycleErrors::StorageFailed));
            return Result<std::unique_ptr<IPackageModuleActivation>>::Success(std::make_unique<RetainedRoleReceipt>(module, work));
        }
    };
}  // namespace

TEST_CASE("Package rollback retains callback and verified image until the external owner drains", "[packages][extensions][hybrid]") {
    Fixture fixture;
    fixture.service.reset();
    auto retirement = std::make_shared<PackageModuleRetirementQueue>(1);
    auto host = std::make_shared<RetainedRoleHost>();
    host->work->failAt = 2;
    PackageLifecycleConfiguration configuration{.temporaryRoot = std::filesystem::canonical(fixture.temporary.Path()),
                                                .artifactGate = ::Horo::Tests::CreateAcceptingArtifactGate()};
    configuration.capabilities = {"com.example.service"};
    fixture.trust.capabilities = {{"com.example.service"}};
    configuration.moduleHosts = {host};
    configuration.moduleRetirements = retirement;
    fixture.service = PackageLifecycleService::Create(fixture.install, fixture.trust, std::move(configuration)).Value();
    fixture.Install(HybridGraph());
    std::weak_ptr<const VerifiedPackageInstallRecord> oldInstall = fixture.install.InstalledRecord();
    const auto package = Parse<HoroPackageId>("fixture.package");
    const auto descriptor = Parse<PackagePath>("extensions/native/extension.json");
    const std::array enabled{EnabledPackageModule{package, descriptor, "fixture.backend",
                                                  Extensions::ExtensionModuleRole::BackendCapability},
                             EnabledPackageModule{package, descriptor, "fixture.script", Extensions::ExtensionModuleRole::ScriptProvider}};
    CHECK(fixture.service->ActivateModules(enabled, PackageActivationBoundary::Startup, fixture.cancellation.Token()).HasError());
    CHECK_FALSE(fixture.service->Active());
    CHECK(retirement->Pending() == 1);
    CHECK(retirement->Close().HasError());
    CHECK(host->work->destroyed.empty());
    CHECK(fixture.service->ActivateModules(enabled, PackageActivationBoundary::Quiescent, fixture.cancellation.Token()).HasError());
    CHECK(host->work->activationCalls == 2);
    fixture.Install(Graph(HORO_PACKAGE_EMPTY_FIXTURE));
    fixture.service.reset();
    CHECK_FALSE(oldInstall.expired());
    CHECK(retirement->Drain() == 1);
    host->work->busy = false;
    CHECK(retirement->Drain() == 0);
    CHECK(oldInstall.expired());
    CHECK(host->work->destroyed == std::vector<std::string>{"fixture.backend"});
    CHECK(retirement->Close().HasValue());
}

TEST_CASE("Package role retirement drains dependents before their provider across service destruction", "[packages][extensions][hybrid]") {
    Fixture fixture;
    fixture.service.reset();
    auto retirement = std::make_shared<PackageModuleRetirementQueue>(2);
    auto host = std::make_shared<RetainedRoleHost>();
    PackageLifecycleConfiguration configuration{.temporaryRoot = std::filesystem::canonical(fixture.temporary.Path()),
                                                .artifactGate = ::Horo::Tests::CreateAcceptingArtifactGate()};
    configuration.capabilities = {"com.example.service"};
    fixture.trust.capabilities = {{"com.example.service"}};
    configuration.moduleHosts = {host};
    configuration.moduleRetirements = retirement;
    fixture.service = PackageLifecycleService::Create(fixture.install, fixture.trust, std::move(configuration)).Value();
    fixture.Install(HybridGraph());
    const auto package = Parse<HoroPackageId>("fixture.package");
    const auto descriptor = Parse<PackagePath>("extensions/native/extension.json");
    // Consumer first deliberately exercises topology rather than input order.
    const std::array enabled{EnabledPackageModule{package, descriptor, "fixture.script", Extensions::ExtensionModuleRole::ScriptProvider},
                             EnabledPackageModule{package, descriptor, "fixture.backend",
                                                  Extensions::ExtensionModuleRole::BackendCapability}};
    REQUIRE(fixture.service->ActivateModules(enabled, PackageActivationBoundary::Startup, fixture.cancellation.Token()).HasValue());
    fixture.service.reset();
    CHECK(retirement->Drain() == 1);
    CHECK(host->work->finalized.empty());
    host->work->busy = false;
    CHECK(retirement->Drain() == 0);
    CHECK(host->work->finalized == std::vector<std::string>{"fixture.script", "fixture.backend"});
    CHECK(host->work->destroyed == host->work->finalized);
    CHECK(retirement->Close().HasValue());
}

namespace {
    /** @brief Builds a real binding context from exact package capability and provider evidence. */
    Result<Extensions::ScriptCapabilityContext> HybridScriptContext(HybridFixture &fixture, const std::uint64_t generation) {
        using namespace Horo::Extensions;
        using namespace Horo::Extensions::Tests::Support;
        auto grant = fixture.script->Capability(fixture.enabled[0].package, "fixture.script", generation, {"com.example.service"});
        if (grant.HasError())
            return Result<Extensions::ScriptCapabilityContext>::Failure(grant.ErrorValue());
        auto descriptors = MakeDescriptor();
        descriptors.moduleId = "fixture.script";
        auto exports = BuildScriptExportDescriptorSnapshot(std::array{descriptors});
        if (exports.HasError())
            return Result<Extensions::ScriptCapabilityContext>::Failure(exports.ErrorValue());
        ScriptCapabilityContextDescriptor descriptor{.scriptId = "fixture.consumer",
                                                     .policyRevision = 1,
                                                     .profile = ScriptCapabilityProfile::Gameplay,
                                                     .packageVerified = true,
                                                     .packageTrusted = true,
                                                     .scriptTrusted = true,
                                                     .runtimeCompatible = true,
                                                     .packageCapabilities = {{"com.example.service"}},
                                                     .scriptCapabilities = {{"com.example.service"}},
                                                     .projectCapabilities = {{"com.example.service"}},
                                                     .contextCapabilities = {{"com.example.service"}},
                                                     .scopes = {fixture.project.Lease(), fixture.runtime.Lease(), fixture.scene.Lease(),
                                                                fixture.operation.Lease()},
                                                     .imports = {{"com.example.api",
                                                                  {{1, 0, 0}, {1, 0, 0}},
                                                                  exports.Value(),
                                                                  grant.Value(),
                                                                  fixture.script->Provider(fixture.enabled[0].package, "fixture.script",
                                                                                           generation),
                                                                  ScriptCapabilityProfile::Gameplay,
                                                                  {"fetch"}}}};
        return ScriptCapabilityContext::Create(fixture.scripts, std::move(descriptor));
    }
}  // namespace

TEST_CASE("Verified package script provider admits typed bindings and revokes pending invocations on disable",
          "[packages][extensions][hybrid][script]") {
    using namespace Horo::Extensions;
    using namespace Horo::Extensions::Tests::Support;
    HybridFixture fixture(true);
    fixture.base.Install(HybridGraph());
    REQUIRE(fixture.Activate().HasValue());
    const auto generation = fixture.base.service->Active()->generation;
    auto context = HybridScriptContext(fixture, generation);
    REQUIRE(context.HasValue());
    auto binding = context.Value().Bind("com.example.api");
    REQUIRE(binding.HasValue());
    auto request = Request(DescriptorSnapshot());
    auto invocation = fixture.script->Begin(fixture.enabled[0].package, "fixture.script", generation, binding.Value(), request);
    REQUIRE(invocation.HasValue());
    REQUIRE(invocation.Value().Handle().Snapshot().has_value());
    fixture.base.service->Shutdown();
    CHECK_FALSE(binding.Value().IsUsable());
    CHECK(fixture.script->Begin(fixture.enabled[0].package, "fixture.script", generation, binding.Value(), std::move(request)).HasError());
    const auto events = context.Value().Drain();
    REQUIRE(events.HasValue());
    REQUIRE(events.Value().size() == 1);
    CHECK(events.Value().front().terminalResult.has_value());
}

TEST_CASE("Every required concrete host failure rolls back while optional editor failure preserves siblings",
          "[packages][extensions][hybrid]") {
    for (const unsigned failAt : {1U, 2U, 3U}) {
        for (const bool requiredGui : {false, true}) {
            HybridFixture fixture;
            fixture.failNativeAt = failAt;
            fixture.enabled[2].requirement = requiredGui ? PackageModuleRequirement::Required : PackageModuleRequirement::Optional;
            fixture.base.Install(HybridGraph());
            const auto install = fixture.base.install.InstalledRecord();
            const auto activated = fixture.Activate();
            if (failAt == 3 && !requiredGui) {
                REQUIRE(activated.HasValue());
                const auto snapshot = fixture.base.service->Active();
                CHECK(snapshot->modules[0].outcome == PackageModuleOutcome::Active);
                CHECK(snapshot->modules[1].outcome == PackageModuleOutcome::Active);
                CHECK(snapshot->modules[2].outcome == PackageModuleOutcome::Failed);
                CHECK(snapshot->modules[2].diagnostic->code.Value() == PackageLifecycleErrors::StorageFailed.code.Value());
            } else {
                REQUIRE(activated.HasError());
                CHECK(activated.ErrorValue().code.Value() == PackageLifecycleErrors::StorageFailed.code.Value());
                CHECK_FALSE(fixture.base.service->Active());
                fixture.editor->Update();
                CHECK(fixture.editor->Prepared().empty());
                CHECK(fixture.retirement->Drain() == 0);
            }
            CHECK(fixture.base.install.InstalledRecord() == install);
        }
    }
}

TEST_CASE("Hybrid disable update rollback and uninstall preserve explicit availability and generation ownership",
          "[packages][extensions][hybrid]") {
    HybridFixture fixture;
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        fixture.base.Install(HybridGraph());
        REQUIRE(fixture.Activate().HasValue());
        const auto previous = fixture.base.service->Active();
        const auto provider = fixture.script->Provider(fixture.enabled[0].package, "fixture.script", previous->generation);
        fixture.base.trust.deny = true;
        CHECK(fixture.Activate().HasError());
        CHECK(fixture.base.service->Active() == previous);
        CHECK(provider.IsUsable());
        fixture.base.trust.deny = false;
        REQUIRE(
            fixture.base.service->ActivateModules({}, PackageActivationBoundary::Quiescent, fixture.base.cancellation.Token()).HasValue());
        CHECK_FALSE(provider.IsUsable());
        CHECK(fixture.base.service->Active()->modules.empty());
        CHECK(fixture.retirement->Drain() == 0);
        fixture.editor->Update();
        CHECK(fixture.editor->Prepared().empty());
    }
    auto removed = std::make_shared<PackageRestoreGraph>(*fixture.base.install.ActiveGraph());
    removed->packages.clear();
    REQUIRE(fixture.base.install.Install(removed, fixture.base.cancellation.Token()).HasValue());
    REQUIRE(fixture.base.service->ActivateModules({}, PackageActivationBoundary::Quiescent, fixture.base.cancellation.Token()).HasValue());
    CHECK(fixture.base.service->Active()->install->Graph()->packages.empty());
    CHECK(fixture.retirement->Drain() == 0);
}

TEST_CASE("Hybrid trust and unsupported role identity fail before actual native host mutation", "[packages][extensions][hybrid]") {
    for (const unsigned failure : {0U, 1U, 2U}) {
        HybridFixture fixture;
        fixture.base.Install(HybridGraph());
        if (failure == 0)
            fixture.base.trust.deny = true;
        if (failure == 1)
            fixture.base.trust.stale = true;
        if (failure == 2)
            fixture.enabled[0].role = Extensions::ExtensionModuleRole::HeadlessTooling;
        CHECK(fixture.Activate().HasError());
        CHECK(fixture.nativeLoads == 0);
        CHECK_FALSE(fixture.base.service->Active());
    }
}

TEST_CASE("Script realm binds exact generations after skipped attempts and revokes replaced bindings",
          "[packages][extensions][hybrid][script]") {
    HybridFixture fixture(true);
    fixture.base.Install(HybridGraph());
    fixture.enabled[1].requirement = PackageModuleRequirement::Optional;
    fixture.failNativeAt = 2;
    REQUIRE(fixture.Activate().HasValue());
    const auto skipped = fixture.base.service->Active()->generation;
    CHECK_FALSE(fixture.script->Provider(fixture.enabled[0].package, "fixture.script", skipped).IsUsable());
    fixture.failNativeAt = 0;
    REQUIRE(fixture.Activate().HasValue());
    const auto first = fixture.base.service->Active()->generation;
    REQUIRE(first > skipped);
    auto context = HybridScriptContext(fixture, first);
    REQUIRE(context.HasValue());
    auto binding = context.Value().Bind("com.example.api");
    REQUIRE(binding.HasValue());
    REQUIRE(binding.Value().IsUsable());
    fixture.base.Install(HybridGraph());
    REQUIRE(fixture.Activate().HasValue());
    const auto updated = fixture.base.service->Active()->generation;
    CHECK(updated > first);
    CHECK_FALSE(binding.Value().IsUsable());
    auto next = HybridScriptContext(fixture, updated);
    REQUIRE(next.HasValue());
    CHECK(next.Value().Bind("com.example.api").Value().IsUsable());
    CHECK_FALSE(fixture.script->Provider(fixture.enabled[0].package, "fixture.wrong", updated).IsUsable());
}

TEST_CASE("Script realm aliases and wrong module realms fail without fallback", "[packages][extensions][hybrid][script]") {
    HybridFixture fixture(true);
    const auto package = fixture.enabled[0].package;
    auto aliased =
        std::make_shared<ScriptPackageModuleHost>(std::vector<PackageScriptRegistryRealm>{{package, "fixture.script", fixture.scripts},
                                                                                          {package, "fixture.other", fixture.scripts}},
                                                  fixture.native);
    CHECK_FALSE(aliased->Supports(Extensions::ExtensionModuleRole::ScriptProvider));
    auto duplicated = std::make_shared<
        ScriptPackageModuleHost>(std::vector<PackageScriptRegistryRealm>{{package, "fixture.script", fixture.scripts},
                                                                         {package, "fixture.script",
                                                                          std::make_shared<Extensions::ScriptInvocationRegistry>()}},
                                 fixture.native);
    CHECK_FALSE(duplicated->Supports(Extensions::ExtensionModuleRole::ScriptProvider));
    fixture.base.service.reset();
    auto wrong =
        std::make_shared<ScriptPackageModuleHost>(std::vector<PackageScriptRegistryRealm>{{package, "fixture.other", fixture.scripts}},
                                                  fixture.native);
    PackageLifecycleConfiguration configuration{.temporaryRoot = std::filesystem::canonical(fixture.base.temporary.Path()),
                                                .profile = Extensions::ExtensionHostProfile::Headless,
                                                .capabilities = {"com.example.service"},
                                                .artifactGate = ::Horo::Tests::CreateAcceptingArtifactGate()};
    configuration.moduleHosts = {fixture.native, wrong};
    configuration.moduleRetirements = fixture.retirement;
    fixture.base.service = PackageLifecycleService::Create(fixture.base.install, fixture.base.trust, std::move(configuration)).Value();
    fixture.base.Install(HybridGraph());
    CHECK(fixture.Activate().HasError());
    CHECK_FALSE(fixture.base.service->Active());
    CHECK_FALSE(wrong->Provider(package, "fixture.script", 1).IsUsable());
}

TEST_CASE("Live empty package compositions leave retirement capacity available across updates", "[packages][extensions][hybrid]") {
    HybridFixture fixture(true);
    fixture.base.Install(HybridGraph());
    for (unsigned update = 0; update < 6; ++update) {
        REQUIRE(
            fixture.base.service->ActivateModules({}, PackageActivationBoundary::Quiescent, fixture.base.cancellation.Token()).HasValue());
        REQUIRE(fixture.base.service->Active());
        CHECK(fixture.base.service->Active()->modules.empty());
        CHECK(fixture.nativeLoads == 0);
        REQUIRE(fixture.retirement->Close().HasValue());
    }
    REQUIRE(fixture.Activate().HasValue());
    CHECK(fixture.nativeLoads > 0);
    fixture.base.trust.deny = true;
    REQUIRE(fixture.Activate().HasError());
    CHECK(fixture.base.service->Active()->modules[0].outcome == PackageModuleOutcome::Active);
    fixture.base.service->Shutdown();
    REQUIRE(fixture.retirement->Close().HasValue());
}
