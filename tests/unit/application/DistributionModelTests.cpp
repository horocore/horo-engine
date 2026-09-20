#include "Horo/Release/DistributionModel.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace {
    using namespace Horo;
    using namespace Horo::Release;

    ReleaseSemanticVersion Version(const std::string_view text = "1.2.3") {
        auto parsed = ParseReleaseVersion(text);
        REQUIRE(parsed.HasValue());
        return std::move(parsed).Value();
    }

    DistributionArtifactIdentity Artifact(const DistributionProductKind product = DistributionProductKind::Editor,
                                          const DistributionPlatform platform = DistributionPlatform::Windows) {
        return {{product, {}},
                EngineProductVersion{Version()},
                platform,
                DistributionArchitecture::X64,
                {"build-abc123"},
                {"package-1"},
                DistributionInstallationId{"installation-1"},
                DistributionArtifactClass::InstallableProduct};
    }

    void RequireError(const auto &result, const std::string_view code) {
        REQUIRE(result.HasError());
        REQUIRE(result.ErrorValue().domain.Value() == "horo.release");
        REQUIRE(result.ErrorValue().code.Value() == code);
    }

    TEST_CASE("Package formats expose explicit platform capabilities", "[unit][application][release][distribution][headless]") {
        const auto msi = DescribeDistributionPackageFormat(DistributionPackageFormat::WindowsMsi, DistributionPlatform::Windows);
        REQUIRE(msi.HasValue());
        CHECK(msi.Value().installLayout == DistributionInstallLayout::SystemManaged);
        CHECK(msi.Value().uninstall == DistributionCapability::Required);
        CHECK(msi.Value().signing == DistributionCapability::Required);

        const auto appImage = DescribeDistributionPackageFormat(DistributionPackageFormat::LinuxAppImage, DistributionPlatform::Linux);
        REQUIRE(appImage.HasValue());
        CHECK(appImage.Value().installLayout == DistributionInstallLayout::Portable);
        CHECK(appImage.Value().desktopIntegration == DistributionCapability::Supported);
        CHECK(appImage.Value().rollback == DistributionCapability::Supported);

        const auto store = DescribeDistributionPackageFormat(DistributionPackageFormat::StorePackage, DistributionPlatform::MacOS);
        REQUIRE(store.HasValue());
        CHECK(store.Value().installLayout == DistributionInstallLayout::StoreManaged);
        CHECK(store.Value().updates == DistributionCapability::Required);
    }

    TEST_CASE("Distribution identities use one canonical bounded grammar", "[unit][application][release][distribution][headless]") {
        CHECK(IsValidDistributionIdentity("release.profile-1"));
        CHECK(IsValidDistributionIdentity(std::string(MaximumDistributionIdentityBytes, 'a')));
        CHECK_FALSE(IsValidDistributionIdentity("Release.Profile"));
        CHECK_FALSE(IsValidDistributionIdentity("release/profile"));
        CHECK_FALSE(IsValidDistributionIdentity(std::string(MaximumDistributionIdentityBytes + 1U, 'a')));
    }

    TEST_CASE("Package format is never inferred from the target platform", "[unit][application][release][distribution][headless]") {
        const std::array supported{std::pair{DistributionPackageFormat::WindowsMsi, DistributionPlatform::Windows},
                                   std::pair{DistributionPackageFormat::WindowsExeInstaller, DistributionPlatform::Windows},
                                   std::pair{DistributionPackageFormat::ZipArchive, DistributionPlatform::Windows},
                                   std::pair{DistributionPackageFormat::ZipArchive, DistributionPlatform::MacOS},
                                   std::pair{DistributionPackageFormat::MacDmg, DistributionPlatform::MacOS},
                                   std::pair{DistributionPackageFormat::MacPkg, DistributionPlatform::MacOS},
                                   std::pair{DistributionPackageFormat::MacAppBundle, DistributionPlatform::MacOS},
                                   std::pair{DistributionPackageFormat::LinuxAppImage, DistributionPlatform::Linux},
                                   std::pair{DistributionPackageFormat::TarGzip, DistributionPlatform::Linux},
                                   std::pair{DistributionPackageFormat::LinuxDeb, DistributionPlatform::Linux},
                                   std::pair{DistributionPackageFormat::LinuxRpm, DistributionPlatform::Linux}};
        for (const auto &[format, platform] : supported)
            REQUIRE(DescribeDistributionPackageFormat(format, platform).HasValue());

        RequireError(DescribeDistributionPackageFormat(DistributionPackageFormat::WindowsMsi, DistributionPlatform::Linux),
                     "release.distribution.combination_unsupported");
        RequireError(DescribeDistributionPackageFormat(DistributionPackageFormat::MacDmg, DistributionPlatform::Windows),
                     "release.distribution.combination_unsupported");
        RequireError(DescribeDistributionPackageFormat(DistributionPackageFormat::TarGzip, DistributionPlatform::MacOS),
                     "release.distribution.combination_unsupported");
        RequireError(DescribeDistributionPackageFormat(static_cast<DistributionPackageFormat>(255), DistributionPlatform::Windows),
                     "release.distribution.combination_unsupported");
    }

    TEST_CASE("Product identities remain distinct through validated selections", "[unit][application][release][distribution][headless]") {
        static_assert(!std::is_same_v<DistributionBuildId, DistributionPackageId>);
        static_assert(!std::is_same_v<DistributionPackageId, DistributionInstallationId>);

        for (const DistributionProductKind product :
             {DistributionProductKind::Editor, DistributionProductKind::EngineCli, DistributionProductKind::PackageToolCli}) {
            const auto selected = ValidateDistributionPackageSelection(Artifact(product), DistributionPackageFormat::ZipArchive);
            REQUIRE(selected.HasValue());
            CHECK(selected.Value().artifact.product.kind == product);
        }

        auto sdk = Artifact(DistributionProductKind::PublicSdk, DistributionPlatform::Linux);
        REQUIRE(ValidateDistributionPackageSelection(sdk, DistributionPackageFormat::LinuxDeb).HasValue());
        RequireError(ValidateDistributionPackageSelection(sdk, DistributionPackageFormat::LinuxAppImage),
                     "release.distribution.combination_unsupported");

        auto renderer = Artifact(DistributionProductKind::RendererComponent);
        renderer.product.componentId = "horo.renderer.vulkan";
        REQUIRE(ValidateDistributionPackageSelection(renderer, DistributionPackageFormat::ZipArchive).HasValue());
        RequireError(ValidateDistributionPackageSelection(renderer, DistributionPackageFormat::WindowsMsi),
                     "release.distribution.combination_unsupported");
    }

    TEST_CASE("Game and engine version identities cannot cross product boundaries",
              "[unit][application][release][distribution][headless]") {
        auto game = Artifact(DistributionProductKind::GameRuntime, DistributionPlatform::MacOS);
        game.version = GameProductVersion{Version("4.0.0-preview.1+store.8")};
        REQUIRE(ValidateDistributionPackageSelection(game, DistributionPackageFormat::StorePackage).HasValue());

        game.version = EngineProductVersion{Version()};
        RequireError(ValidateDistributionPackageSelection(game, DistributionPackageFormat::StorePackage),
                     "release.distribution.identity_invalid");

        auto editor = Artifact();
        editor.version = GameProductVersion{Version()};
        RequireError(ValidateDistributionPackageSelection(editor, DistributionPackageFormat::WindowsMsi),
                     "release.distribution.identity_invalid");
    }

    TEST_CASE("Canonical semantic versions round-trip through distribution admission",
              "[unit][application][release][distribution][headless]") {
        auto engine = Artifact();
        engine.version = EngineProductVersion{Version("3.2.1-rc.2+build.7")};
        const auto selectedEngine = ValidateDistributionPackageSelection(engine, DistributionPackageFormat::WindowsMsi);
        REQUIRE(selectedEngine.HasValue());
        CHECK(selectedEngine.Value().artifact.version == engine.version);

        auto game = Artifact(DistributionProductKind::GameRuntime, DistributionPlatform::MacOS);
        game.version = GameProductVersion{Version("4.0.0-preview.1+store.8")};
        const auto selectedGame = ValidateDistributionPackageSelection(game, DistributionPackageFormat::StorePackage);
        REQUIRE(selectedGame.HasValue());
        CHECK(selectedGame.Value().artifact.version == game.version);
    }

    TEST_CASE("Symbols and diagnostics cannot impersonate ordinary installations", "[unit][application][release][distribution][headless]") {
        auto symbols = Artifact();
        symbols.artifactClass = DistributionArtifactClass::Symbols;
        symbols.installation.reset();
        REQUIRE(ValidateDistributionPackageSelection(symbols, DistributionPackageFormat::ZipArchive).HasValue());

        auto diagnostics = Artifact(DistributionProductKind::EngineCli, DistributionPlatform::Linux);
        diagnostics.artifactClass = DistributionArtifactClass::Diagnostics;
        diagnostics.installation.reset();
        REQUIRE(ValidateDistributionPackageSelection(diagnostics, DistributionPackageFormat::TarGzip).HasValue());

        symbols.installation = DistributionInstallationId{"ordinary-installation"};
        RequireError(ValidateDistributionPackageSelection(symbols, DistributionPackageFormat::ZipArchive),
                     "release.distribution.combination_unsupported");
        symbols.installation = DistributionInstallationId{"Installation Invalid"};
        RequireError(ValidateDistributionPackageSelection(symbols, DistributionPackageFormat::ZipArchive),
                     "release.distribution.identity_invalid");
        diagnostics.installation.reset();
        RequireError(ValidateDistributionPackageSelection(diagnostics, DistributionPackageFormat::LinuxDeb),
                     "release.distribution.combination_unsupported");
    }

    TEST_CASE("Malformed and unsupported distribution identities fail admission", "[unit][application][release][distribution][headless]") {
        auto artifact = Artifact();
        artifact.build.value = "Build Uppercase";
        RequireError(ValidateDistributionPackageSelection(artifact, DistributionPackageFormat::WindowsMsi),
                     "release.distribution.identity_invalid");

        artifact = Artifact();
        artifact.package.value = std::string(MaximumDistributionIdentityBytes + 1, 'a');
        RequireError(ValidateDistributionPackageSelection(artifact, DistributionPackageFormat::WindowsMsi),
                     "release.distribution.identity_invalid");

        artifact = Artifact();
        artifact.package.value = "package bad";
        RequireError(ValidateDistributionPackageSelection(artifact, DistributionPackageFormat::WindowsMsi),
                     "release.distribution.identity_invalid");

        artifact = Artifact();
        artifact.build.value = std::string(MaximumDistributionIdentityBytes, 'a');
        REQUIRE(ValidateDistributionPackageSelection(artifact, DistributionPackageFormat::WindowsMsi).HasValue());

        artifact = Artifact(DistributionProductKind::RendererComponent);
        RequireError(ValidateDistributionPackageSelection(artifact, DistributionPackageFormat::ZipArchive),
                     "release.distribution.identity_invalid");

        artifact = Artifact();
        artifact.product.componentId = "horo.renderer.vulkan";
        RequireError(ValidateDistributionPackageSelection(artifact, DistributionPackageFormat::ZipArchive),
                     "release.distribution.identity_invalid");

        artifact = Artifact();
        artifact.architecture = static_cast<DistributionArchitecture>(255);
        RequireError(ValidateDistributionPackageSelection(artifact, DistributionPackageFormat::WindowsMsi),
                     "release.distribution.identity_invalid");

        artifact = Artifact();
        artifact.product.kind = static_cast<DistributionProductKind>(255);
        RequireError(ValidateDistributionPackageSelection(artifact, DistributionPackageFormat::WindowsMsi),
                     "release.distribution.identity_invalid");

        artifact = Artifact();
        artifact.artifactClass = static_cast<DistributionArtifactClass>(255);
        RequireError(ValidateDistributionPackageSelection(artifact, DistributionPackageFormat::WindowsMsi),
                     "release.distribution.identity_invalid");

        artifact = Artifact();
        artifact.installation.reset();
        RequireError(ValidateDistributionPackageSelection(artifact, DistributionPackageFormat::WindowsMsi),
                     "release.distribution.combination_unsupported");

        RequireError(DescribeDistributionPackageFormat(DistributionPackageFormat::WindowsMsi, static_cast<DistributionPlatform>(255)),
                     "release.distribution.combination_unsupported");

        auto server = Artifact(DistributionProductKind::GameDedicatedServer, DistributionPlatform::Linux);
        server.version = GameProductVersion{Version()};
        RequireError(ValidateDistributionPackageSelection(server, DistributionPackageFormat::LinuxAppImage),
                     "release.distribution.combination_unsupported");
    }

    TEST_CASE("Malformed semantic version payloads fail distribution admission", "[unit][application][release][distribution][headless]") {
        auto artifact = Artifact();
        artifact.version = EngineProductVersion{ReleaseSemanticVersion{1, 2, 3, "01", {}}};
        RequireError(ValidateDistributionPackageSelection(artifact, DistributionPackageFormat::WindowsMsi),
                     "release.distribution.identity_invalid");

        artifact.version = EngineProductVersion{ReleaseSemanticVersion{1, 2, 3, {}, "build value"}};
        RequireError(ValidateDistributionPackageSelection(artifact, DistributionPackageFormat::WindowsMsi),
                     "release.distribution.identity_invalid");
    }
}  // namespace
