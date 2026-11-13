#pragma once

/** @file ProjectCreationFixture.h @brief Shared real creation fixtures and native publication fault injection. */
#include "../NativePublicationFiles.h"
#include "Horo/Application/PlatformServicesAuthoring.h"
#include "Horo/Application/PlatformServicesDefinitionCodec.h"
#include "Horo/Application/ProjectCollisionSettings.h"
#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Application/ProjectSurfaceSettings.h"
#include "Horo/Editor/ProjectCreationService.h"
#include "Horo/Foundation/DataBus.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Security/SecureMemory.h"
#include "Horo/Security/SecurityErrors.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>

namespace Horo::Tests::ProjectCreation {
    /** @brief Injects failures around genuine durable preparation and irreversible native publication. */
    class CreationFiles final : public Horo::TestSupport::NativePublicationFiles {
    public:
        bool failWrite{};
        bool failSync{};
        bool failBeforeCommit{};
        bool failAfterCommit{};
        bool throwAfterCommit{};
        bool throwDuringWrite{};
        bool throwBeforeCommit{};
        bool allocationAfterCommit{};
        bool unknownAfterCommit{};

        Horo::Result<void> WriteDurable(const std::filesystem::path &path, std::span<const std::byte> bytes) override {
            if (throwDuringWrite)
                throw std::runtime_error("injected preparation exception");
            if (failWrite)
                return Failure();
            return native.WriteDurable(path, bytes);
        }

        Horo::Result<void> SyncDirectory(const std::filesystem::path &path) override {
            if (failSync)
                return Failure();
            return native.SyncDirectory(path);
        }

        Horo::Result<void> PublishDirectoryNoReplaceTracked(const std::filesystem::path &prepared, const std::filesystem::path &destination,
                                                            Horo::AtomicFileReplacementReceipt &receipt) override {
            if (throwBeforeCommit)
                throw std::runtime_error("injected publication exception");
            if (failBeforeCommit)
                return Failure();
            auto published = native.PublishDirectoryNoReplaceTracked(prepared, destination, receipt);
            if (published.HasError())
                return published;
            if (throwAfterCommit)
                throw std::runtime_error("injected post-commit failure");
            if (allocationAfterCommit)
                throw std::bad_alloc{};
            if (unknownAfterCommit)
                throw 41;
            if (failAfterCommit)
                return Failure();
            return published;
        }

    private:
        static Horo::Result<void> Failure() {
            return Horo::Result<void>::Failure(Horo::MakeError(Horo::SecurityErrors::EntropyUnavailable, "injected filesystem failure"));
        }
    };

    /** @brief Supplies deterministic nonzero entropy and can simulate zero/failure without native discovery. */
    class CreationRandom final : public Horo::Security::SecureRandomSource {
    public:
        bool fail{};
        bool zero{};
        std::uint32_t calls{};

        Horo::Result<void> Fill(const std::span<std::byte> destination) override {
            ++calls;
            std::ranges::fill(destination, zero ? std::byte{} : std::byte{0xA5});
            if (fail)
                return Horo::Result<void>::Failure(Horo::MakeError(Horo::SecurityErrors::EntropyUnavailable));
            return Horo::Result<void>::Success();
        }
    };

    class TemporaryDirectory {
    public:
        TemporaryDirectory() {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path_ = std::filesystem::temp_directory_path() / ("horo-project-service-" + std::to_string(stamp));
            std::filesystem::create_directories(path_);
        }

        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }

        const std::filesystem::path &Path() const noexcept {
            return path_;
        }

    private:
        std::filesystem::path path_;
    };

    inline std::string Read(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    inline Horo::Editor::ProjectCreationRequest ValidRequest(const std::filesystem::path &root) {
        return {
            .templateId = "3d-starter",
            .projectName = "MyGame",
            .projectRoot = root,
            .projectVersion = "0.1.0",
            .defaultScene = "Assets/Scenes/main.horo",
            .renderBackend = "opengl",
            .physicsEnabled = true,
            .targetFrameRate = 60,
            .buildProfile = "desktop-debug",
            .assetCompression = "lz4",
            .textureCompression = "bc7",
            .targetPlatform = "host",
            .compilerFamily = "default",
            .minimumCxxStandard = 20,
            .initializeGit = true,
            .restorePackages = true,
            .includeStarterContent = true,
            .generateCMakeProject = true,
        };
    }

    inline Horo::Editor::ProjectCreationSnapshot WaitForTerminal(Horo::Editor::ProjectCreationService &service,
                                                                 Horo::Editor::ProjectCreationOperationId id) {
        for (int attempt = 0; attempt < 2000; ++attempt) {
            const auto snapshot = service.Query(id);
            REQUIRE((snapshot.has_value()));
            if (snapshot->state == Horo::Editor::ProjectCreationOperationState::Succeeded ||
                snapshot->state == Horo::Editor::ProjectCreationOperationState::Failed ||
                snapshot->state == Horo::Editor::ProjectCreationOperationState::Cancelled) {
                return *snapshot;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        REQUIRE((false && "project creation did not finish"));
        return {};
    }

    /** @brief Checks every created platform document against the single root salt and complete empty ledger. */
    inline void CheckPlatformAuthority(const std::filesystem::path &root, const std::string &project) {
        const auto source = Horo::Application::DecodeProjectSourceDocument(project);
        REQUIRE(source.HasValue());
        const auto salt = Horo::Application::ResolveProjectPlatformServicesSalt(source.Value());
        REQUIRE(salt.HasValue());
        const auto ledgerBytes = Read(root / Horo::Application::PlatformServicesDocumentPaths::Ledger);
        const auto ids = Horo::Application::DecodePlatformServicesLedger(ledgerBytes, source.Value().metadata.projectId, salt.Value());
        REQUIRE(ids.HasValue());
        CHECK(ids.Value().Entries().empty());
        CHECK(ledgerBytes.find("psid1:") == std::string::npos);
        const auto achievements =
            Horo::Application::DecodePlatformAchievementDefinitions(Read(root /
                                                                         Horo::Application::PlatformServicesDocumentPaths::Achievements),
                                                                    ids.Value());
        const auto stats =
            Horo::Application::DecodePlatformStatDefinitions(Read(root / Horo::Application::PlatformServicesDocumentPaths::Stats),
                                                             ids.Value());
        REQUIRE(achievements.HasValue());
        REQUIRE(stats.HasValue());
        REQUIRE(
            Horo::Application::DecodePlatformLeaderboardDefinitions(Read(root /
                                                                         Horo::Application::PlatformServicesDocumentPaths::Leaderboards),
                                                                    ids.Value(), stats.Value())
                .HasValue());
        REQUIRE(
            Horo::Application::DecodePlatformPresenceDefinitions(Read(root / Horo::Application::PlatformServicesDocumentPaths::Presence),
                                                                 ids.Value())
                .HasValue());
    }

    /** @brief Checks the real collision and surface catalogs authored by the same whole-project promotion. */
    inline void CheckPhysicsCatalogs(const std::filesystem::path &root, const std::string &projectId) {
        const auto collision = Horo::Application::LoadProjectCollisionSettings(root, projectId);
        REQUIRE(collision.HasValue());
        CHECK(collision.Value().settings->Input().revision == 1);
        CHECK(collision.Value().settings->Schema()->Profiles().size() == 1);
        CHECK(collision.Value().settings->Capture(1).HasValue());
        const auto surfaces = Horo::Application::LoadProjectSurfaceSettings(root, projectId);
        REQUIRE(surfaces.HasValue());
        REQUIRE(surfaces.Value().settings->Input().defaultSurfaceId.has_value());
        REQUIRE(surfaces.Value().settings->Catalog()->Entries().size() == 1);
        CHECK(surfaces.Value().settings->Catalog()->Contains(*surfaces.Value().settings->Input().defaultSurfaceId));
        CHECK(surfaces.Value().settings->Capture(1).HasValue());
    }

}  // namespace Horo::Tests::ProjectCreation
