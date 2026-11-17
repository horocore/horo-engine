#pragma once

/** @file ProjectOpenFixture.h @brief Shared native project-open fixtures and deterministic source revocation contributors. */
#include "Horo/Application/ProjectCollisionSettings.h"
#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Application/ProjectSurfaceSettings.h"
#include "Horo/Editor/ProjectOpenService.h"
#include "Horo/Foundation/JobSystem.h"
#include "PlatformServicesMigrationFixture.h"
#include "editor/project_model/RendererAvailability.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string_view>
#include <thread>

namespace Horo::Tests::ProjectOpen {
    using namespace Horo;
    using namespace Horo::Application;
    using namespace Horo::Editor;

    struct TempProject {
        std::filesystem::path root =
            std::filesystem::temp_directory_path() /
            ("horo-project-open-service-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        TempProject() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
            std::filesystem::create_directories(root / ".horo");
            std::filesystem::create_directories(root / "assets/scenes");
            const auto *decision = BuiltInReleaseCompatibilityRegistry().Find(CurrentEngineReleaseVersion());
            std::ofstream out(root / ".horo/project.json");
            out << "{\"horoVersion\":\"" << FormatHoroVersion(decision->release.value) << "\",\"persistentContract\":\""
                << FormatPersistentContractHash(decision->persistentContract)
                << "\",\"projectId\":\"test-project\",\"name\":\"Open Test\","
                   "\"projectVersion\":\"0.1.0\",\"createdAt\":\"2026-07-19T00:00:00Z\","
                   "\"settings\":{\"renderBackend\":\"opengl\","
                   "\"defaultScene\":\"assets/scenes/main.horo\"}}\n";
            std::ofstream(root / "assets/scenes/main.horo") << R"({"schemaVersion":1,"objects":[]})";
            auto surfaces = ProjectSurfaceSettings::Create({.projectId = "test-project"});
            REQUIRE(surfaces.HasValue());
            auto source = EncodeProjectSurfaceSettings(surfaces.Value());
            REQUIRE(source.HasValue());
            std::ofstream(root / ".horo/surfaces.json") << source.Value();
            out.close();
            std::ifstream input(root / ".horo/project.json", std::ios::binary);
            const std::string metadata{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
            input.close();
            PlatformServices::PlatformServicesIdSalt salt;
            salt.bytes.front() = std::byte{1};
            const auto initial = PreparePlatformServicesInitialDocuments(metadata, salt);
            REQUIRE(initial.HasValue());
            Tests::PlatformMigration::WriteInitialDocuments(root, initial.Value());
        }

        ~TempProject() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    };

    struct LegacyProject {
        std::filesystem::path root =
            std::filesystem::temp_directory_path() /
            ("horo-project-open-migration-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        LegacyProject() {
            const std::filesystem::path fixture =
                std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "tests/fixtures/projects/horo_0_0_1_compression";
            std::filesystem::copy(fixture, root, std::filesystem::copy_options::recursive);
            nlohmann::json metadata;
            {
                std::ifstream input(root / ".horo/project.json");
                input >> metadata;
            }
            metadata["settings"]["defaultScene"] = "assets/scenes/main.horo";
            std::ofstream(root / ".horo/project.json", std::ios::trunc) << metadata.dump(2) << '\n';
            std::filesystem::create_directories(root / "assets/scenes");
            std::ofstream(root / "assets/scenes/main.horo") << R"({"schemaVersion":1,"objects":[]})";
        }

        ~LegacyProject() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    };

    [[nodiscard]] inline nlohmann::json ReadJson(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        return nlohmann::json::parse(input);
    }

    [[nodiscard]] inline std::string ReadText(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    struct SlowPreparedState final : IPreparedProjectOpenDerivedState {
        explicit SlowPreparedState(std::thread::id &installThread) : installThread_(installThread) {}

        Result<std::string> Install() override {
            installThread_ = std::this_thread::get_id();
            return Result<std::string>::Success("7");
        }

        std::thread::id &installThread_;
    };

    struct SlowContributor final : IProjectOpenDerivedStateContributor {
        std::string_view Id() const noexcept override {
            return "test.slow";
        }

        Result<std::unique_ptr<IPreparedProjectOpenDerivedState>> Prepare(const std::filesystem::path &,
                                                                          const CancellationToken &) override {
            prepareThread = std::this_thread::get_id();
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            return Result<std::unique_ptr<IPreparedProjectOpenDerivedState>>::Success(std::make_unique<SlowPreparedState>(installThread));
        }

        std::thread::id prepareThread;
        std::thread::id installThread;
    };

    /** @brief Invalidates an already captured source at a deterministic preparation boundary. */
    struct SourceMutationContributor final : IProjectOpenDerivedStateContributor {
        struct Prepared final : IPreparedProjectOpenDerivedState {
            explicit Prepared(bool &installed) : installed_(installed) {}

            Result<std::string> Install() override {
                installed_ = true;
                return Result<std::string>::Success("source-mutation");
            }

            bool &installed_;
        };

        std::string_view path;
        bool installed{};

        std::string_view Id() const noexcept override {
            return "test.source-mutation";
        }

        Result<std::unique_ptr<IPreparedProjectOpenDerivedState>> Prepare(const std::filesystem::path &root,
                                                                          const CancellationToken &) override {
            std::ofstream output(root / path, std::ios::binary | std::ios::app);
            output << ' ';
            REQUIRE(output.good());
            output.close();
            return Result<std::unique_ptr<IPreparedProjectOpenDerivedState>>::Success(std::make_unique<Prepared>(installed));
        }
    };

    inline ProjectOpenProgressSnapshot PumpToTerminal(ProjectOpenService &service, ProjectOpenOperationId id) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        float previousProgress = 0.0F;
        while (std::chrono::steady_clock::now() < deadline) {
            service.PumpOwnerThread();
            auto snapshot = service.Query(id);
            REQUIRE((snapshot.has_value()));
            REQUIRE((snapshot->progress >= previousProgress));
            previousProgress = snapshot->progress;
            if (snapshot->outcome != ProjectOpenOutcome::Running)
                return *snapshot;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE((false && "project open did not terminate"));
        return {};
    }
}  // namespace Horo::Tests::ProjectOpen
