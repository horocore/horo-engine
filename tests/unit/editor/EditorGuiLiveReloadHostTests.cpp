#include "EditorGuiLiveReload.h"
#include "EditorGuiLiveReloadBuildConfig.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>

TEST_CASE("Editor development GUI host starts transactionally and disabled builds stay inert", "[editor][live-ui-host]") {
    using namespace Horo::Editor;
    EditorGuiLiveReload host;
    CHECK_FALSE(host.Find(GuiLive::Id{"absent"}, GuiLive::Id{"absent"}).invoke);
    host.Update(0.016);
    if (!host.Registry()) {
        CHECK(host.Start({}).HasValue());
        CHECK(host.Registry() == nullptr);
        return;
    }

    const auto invalidConfiguration = host.Start({});
    REQUIRE(invalidConfiguration.HasError());
    CHECK(invalidConfiguration.ErrorValue().code.Value() == "live_ui.configuration_invalid");

    struct BuildDirectory {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     ("horo GUI host " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        ~BuildDirectory() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    } directory;

    auto configuration = BuildEditorGuiLiveReloadConfiguration();
    configuration.intermediateRoot = directory.path / std::filesystem::path{u8"objects ü"};
    const auto image = configuration.hostImage;
    configuration.hostImage = directory.path / "missing executable";
    const auto missingImage = host.Start(configuration);
    REQUIRE(missingImage.HasError());
    CHECK(missingImage.ErrorValue().code.Value() == "live_ui.build_directory_unavailable");
    host.Update(0.016);
    configuration.hostImage = image;
    const auto manifest = configuration.includeManifest;
    configuration.includeManifest = directory.path / "missing manifest";
    const auto missingManifest = host.Start(configuration);
    REQUIRE(missingManifest.HasError());
    CHECK(missingManifest.ErrorValue().code.Value() == "live_ui.include_manifest_unavailable");
    configuration.includeManifest = manifest;
    REQUIRE(host.Start(configuration).HasValue());
    // Successful start is idempotent and never retires the installed runtime mid-frame.
    CHECK(host.Start({}).HasValue());
    host.Update(-1.0);
    host.Update(2.0);
}
