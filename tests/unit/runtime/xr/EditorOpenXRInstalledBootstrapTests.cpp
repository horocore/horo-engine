#include "EditorOpenXRBootstrap.h"
#include "support/OpenXRInstalledProductTestFixture.h"

#include <catch2/generators/catch_generators.hpp>

namespace Horo::Editor::Tests {
    namespace {
        using Horo::Tests::OpenXRProduct::InstalledProductFixture;
        using Horo::Tests::OpenXRProduct::RequireValue;

        Result<std::unique_ptr<EditorOpenXRBootstrap>> StartInstalled(InstalledProductFixture &fixture, std::vector<std::string> &values) {
            std::vector<char *> arguments;
            for (auto &value : values)
                arguments.push_back(value.data());
            return EditorOpenXRBootstrap::Create(arguments, fixture.Root(), std::filesystem::canonical(fixture.storage.Path()) / "cache",
                                                 Runtime::SceneObjectId{1});
        }
    }  // namespace

    TEST_CASE("Executable XR bootstrap reopens only the verified offline package under independent approved roots",
              "[xr][host][bootstrap][install]") {
        InstalledProductFixture fixture{HORO_TEST_OPENXR_LOADER};
        auto arguments = fixture.BootstrapArguments();
        fixture.RetireInstaller();
        const auto cache = std::filesystem::canonical(fixture.storage.Path()) / "cache";
        REQUIRE_FALSE(std::filesystem::exists(cache / "native"));
        REQUIRE_FALSE(std::filesystem::exists(cache / "xr-native"));
        auto bootstrap = RequireValue(StartInstalled(fixture, arguments));
        REQUIRE(bootstrap);
        CHECK(std::filesystem::is_directory(cache / "native"));
        CHECK(std::filesystem::is_directory(cache / "xr-native"));
        const auto &services = bootstrap->Services();
        REQUIRE(services.packages);
        REQUIRE(services.bindings);
        CHECK(services.bindings->schema.actions.size() == 1);
        CHECK(services.trackingRoot == Runtime::SceneObjectId{1});
        REQUIRE(services.jobs);
        REQUIRE(services.validateAdmission);
        CHECK(services.validateAdmission().HasValue());
        auto retained = services.packages;
        const auto generation = retained->Generation();
        const auto record = retained->InstalledRecord(fixture.Root(), generation);
        REQUIRE(record.HasValue());
        REQUIRE(record.Value());
        CHECK(record.Value()->Graph()->packages.size() == 1);
        bootstrap.reset();
        // A copied package capability is closed before its borrowed executable jobs/files disappear.
        CHECK(retained->InstalledRecord(fixture.Root(), generation).HasError());
    }

    TEST_CASE("Executable XR bootstrap rejects files and redirects in its owned native cache parents",
              "[xr][host][bootstrap][install][security]") {
        InstalledProductFixture fixture{HORO_TEST_OPENXR_LOADER};
        auto arguments = fixture.BootstrapArguments();
        fixture.RetireInstaller();
        const auto cache = std::filesystem::canonical(fixture.storage.Path()) / "cache";
        RequireValue(fixture.files.CreateDirectoryPath(cache, DirectoryCreationMode::Single));
        const auto child = cache / GENERATE("native", "xr-native");
        const auto redirected = std::filesystem::canonical(fixture.storage.Path()) / "redirect-target";
        SECTION("regular file") {
            std::ofstream output{child, std::ios::binary};
            output << "preserve";
            output.close();
            REQUIRE(StartInstalled(fixture, arguments).HasError());
            std::ifstream input{child, std::ios::binary};
            CHECK(std::string(std::istreambuf_iterator<char>{input}, {}) == "preserve");
        }
        SECTION("directory redirect") {
            RequireValue(fixture.files.CreateDirectoryPath(redirected, DirectoryCreationMode::Single));
            std::filesystem::create_directory_symlink(redirected, child);
            REQUIRE(StartInstalled(fixture, arguments).HasError());
            CHECK(std::filesystem::is_symlink(child));
            CHECK(std::filesystem::is_empty(redirected));
        }
    }

    TEST_CASE("Executable signed XR bootstrap rejects independent root and catalog tampering before native discovery",
              "[xr][host][bootstrap][security]") {
        InstalledProductFixture fixture{HORO_TEST_OPENXR_LOADER};
        auto arguments = fixture.BootstrapArguments();
        fixture.RetireInstaller();
        SECTION("changed catalog bytes have no verified authority") {
            std::ofstream output{arguments[4], std::ios::binary | std::ios::app};
            output << ' ';
        }
        SECTION("operator revoked the independent signer") {
            std::ifstream input{arguments[2], std::ios::binary};
            auto roots = nlohmann::json::parse(input);
            roots["publishers"][0]["revoked"] = true;
            std::ofstream output{arguments[2], std::ios::binary | std::ios::trunc};
            output << roots.dump();
        }
        SECTION("signature path points at unsigned catalog") {
            arguments[6] = arguments[4];
        }
        CHECK(StartInstalled(fixture, arguments).HasError());
    }

    TEST_CASE("Installed XR bindings preserve signed UTF8 labels and reject malformed verified-package bytes",
              "[xr][host][bootstrap][install][security]") {
        auto bytes = InstalledProductFixture::BindingDocument();
        constexpr std::string_view label = "Seçim ✓";
        const auto offset = bytes.find("Select");
        REQUIRE(offset != std::string::npos);
        bytes.replace(offset, std::string_view{"Select"}.size(), label);
        bool malformed{};
        SECTION("valid localized label") {}
        SECTION("invalid UTF8 in a verified installed binding") {
            const auto nonAscii = bytes.find("ç");
            REQUIRE(nonAscii != std::string::npos);
            bytes[nonAscii] = static_cast<char>(0xff);
            malformed = true;
        }
        SECTION("truncated verified installed JSON") {
            bytes.pop_back();
            malformed = true;
        }
        InstalledProductFixture fixture{HORO_TEST_OPENXR_LOADER, bytes};
        const auto candidate = fixture.Candidate();
        const auto parsed =
            Application::Internal::ReadInstalledOpenXRBindings(candidate.component,
                                                               RequireValue(Packages::PackagePath::Parse("runtime/xr/bindings.json")));
        if (malformed) {
            CHECK(parsed.HasError());
        } else {
            REQUIRE(parsed.HasValue());
            REQUIRE(parsed.Value()->actionLabels.size() == 1);
            CHECK(parsed.Value()->actionLabels.front().text == label);
        }
    }
}  // namespace Horo::Editor::Tests
