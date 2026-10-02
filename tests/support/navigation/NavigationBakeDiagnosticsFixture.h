#pragma once

#include "Horo/Application/NavigationBakeService.h"
#include "navigation/IncrementalBakeFixture.h"

#include <chrono>
#include <format>

namespace Horo::Application::DiagnosticsTestSupport {
    using namespace Navigation;
    using namespace Navigation::TestSupport;

    /** @brief Owns isolated files with realistic spaces and non-ASCII project names. */
    struct Directory {
        std::filesystem::path root{std::filesystem::current_path() /
                                   std::format("horo bake diagnostics ü {}", std::chrono::steady_clock::now().time_since_epoch().count())};

        Directory() {
            REQUIRE(std::filesystem::create_directory(root));
        }

        Directory(const Directory &) = delete;
        Directory &operator=(const Directory &) = delete;

        ~Directory() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    };

    [[nodiscard]] inline Assets::AssetId Asset(const std::string_view suffix = "1") {
        return Assets::AssetId::Parse(std::string{"00000000-0000-0000-0000-00000000000"} + std::string{suffix}).Value();
    }

    [[nodiscard]] inline NavigationBakeDiagnosticsConfig DiagnosticConfig(const Directory &directory, const std::size_t capacity = 128,
                                                                          const std::size_t limit = 64) {
        auto history = Diagnostics::OperationHistorySink::Create({.directory = directory.root / "history",
                                                                  .baseName = "navigation",
                                                                  .maxFileBytes = 16384,
                                                                  .maxRolledFiles = 2,
                                                                  .maxRecoveredRecords = 128});
        REQUIRE(history);
        return {.project = NavigationDiagnosticProjectId::Create(1).Value(),
                .definition = Asset(),
                .projectRoot = directory.root,
                .output = std::make_shared<BuildOutputStore>(capacity),
                .history = std::move(history),
                .capacity = capacity,
                .maximumRecordsPerOperation = limit};
    }

    /** @brief Starts one explicitly composed dispatcher and drains it before releasing sink owners. */
    struct TelemetryOwner {
        explicit TelemetryOwner(const std::shared_ptr<NavigationBakeDiagnostics> &journal) {
            REQUIRE(Telemetry::Runtime::Initialize({.queueCapacity = 512}, std::shared_ptr<Telemetry::ISink>{journal}));
        }

        TelemetryOwner(const TelemetryOwner &) = delete;
        TelemetryOwner &operator=(const TelemetryOwner &) = delete;

        ~TelemetryOwner() {
            static_cast<void>(Telemetry::Runtime::Shutdown());
        }
    };

    [[nodiscard]] inline NavigationDiagnosticSource Source(const IncrementalBakeFixture &fixture) {
        return {.observation = fixture.Observations().front(),
                .target = {.asset = Asset("2"), .scene = {7}, .object = {11}, .relativePath = "geometry ü source.scene"}};
    }

}  // namespace Horo::Application::DiagnosticsTestSupport
