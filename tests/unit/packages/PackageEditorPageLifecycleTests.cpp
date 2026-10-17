#include "EditorPagePackageSupport.h"
#include "EditorPageTestSupport.h"
#include "Horo/Extensions/EditorPageAbi.h"
#include "PackageLifecycleTestSupport.h"

using namespace Horo::Packages::Tests::Support;

namespace {
    /** @brief Seals the actual ABI1.5 page module in the verified package archive used by production activation. */
    std::shared_ptr<PackageRestoreGraph> PageGraph() {
        Horo::Tests::EditorPagePackage package;
        auto descriptor = nlohmann::json::parse(FileBytes(package.root / "extension.json"));
        descriptor["modules"][0]["entry"] = "module" + std::filesystem::path{HORO_EDITOR_PAGE_FIXTURE}.extension().string();
        return Graph(HORO_EDITOR_PAGE_FIXTURE, true, true, "fixture.package", std::move(descriptor));
    }

    /** @brief Keeps page authorities alive until the verified lifecycle has retired every native callback. */
    struct PageFixture final {
        JobSystem jobs;
        Extensions::Tests::PageSupport::Store store;
        Extensions::EditorSurfaceRegistry registry;
        std::shared_ptr<Extensions::EditorPageHost> pages{
            std::make_shared<Extensions::EditorPageHost>(jobs, registry, store, Extensions::Tests::PageSupport::Policy())};
        std::shared_ptr<PackageModuleRetirementQueue> retirement{std::make_shared<PackageModuleRetirementQueue>(4)};
        Fixture base;

        PageFixture() {
            base.service.reset();
            base.trust.capabilities = {{HORO_EDITOR_PAGE_HOST_CAPABILITY}};
            PackageLifecycleConfiguration configuration{.temporaryRoot = std::filesystem::canonical(base.temporary.Path()),
                                                        .profile = Extensions::ExtensionHostProfile::Interactive,
                                                        .capabilities = {HORO_EDITOR_PAGE_HOST_CAPABILITY},
                                                        .artifactGate = Horo::Tests::CreateAcceptingArtifactGate()};
            configuration.editorHosts.pages = pages;
            configuration.moduleHosts = {std::make_shared<NativePackageModuleHost>(configuration)};
            configuration.moduleRetirements = retirement;
            auto created = PackageLifecycleService::Create(base.install, base.trust, std::move(configuration));
            REQUIRE(created.HasValue());
            base.service = std::move(created).Value();
        }
    };
}  // namespace

TEST_CASE("Verified package page-only presentation registers and retires actual settings and modal providers",
          "[packages][extensions][editor-page]") {
    PageFixture fixture;
    fixture.base.Install(PageGraph());
    const std::vector<EnabledPackageModule> enabled{{Parse<HoroPackageId>("fixture.package"),
                                                     Parse<PackagePath>("extensions/native/extension.json"), "fixture.module",
                                                     Extensions::ExtensionModuleRole::EditorPresentation}};
    const auto activated =
        fixture.base.service->ActivateModules(enabled, PackageActivationBoundary::Quiescent, fixture.base.cancellation.Token());
    if (activated.HasError())
        UNSCOPED_INFO(activated.ErrorValue().message);
    REQUIRE(activated.HasValue());
    fixture.pages->Update();
    REQUIRE(fixture.pages->Prepared().size() == 3);
    const auto provider = fixture.pages->Prepared().front().surface.provider;
    REQUIRE(fixture.pages->Begin(provider, "fixture.settings").HasValue());
    fixture.pages->Update();
    fixture.base.service->Shutdown();
    fixture.pages->Update();
    CHECK(fixture.pages->Prepared().empty());
    CHECK(fixture.pages->Begin(provider, "fixture.settings").HasError());
    CHECK(fixture.retirement->Drain() == 0);
}

TEST_CASE("Native package editor presentation requires an explicit interactive publication owner", "[packages][extensions][editor-page]") {
    PageFixture fixture;
    PackageLifecycleConfiguration configuration;
    configuration.editorHosts.pages = fixture.pages;
    CHECK_FALSE(NativePackageModuleHost{configuration}.Supports(Extensions::ExtensionModuleRole::EditorPresentation));
    configuration.profile = Extensions::ExtensionHostProfile::Interactive;
    CHECK(NativePackageModuleHost{configuration}.Supports(Extensions::ExtensionModuleRole::EditorPresentation));
    configuration.editorHosts = {};
    CHECK_FALSE(NativePackageModuleHost{configuration}.Supports(Extensions::ExtensionModuleRole::EditorPresentation));
}
