#pragma once

/** @file PlatformServicesMigrationFixture.h @brief Real platform adoption sources shared by preparation and durable transaction tests. */

#include "Horo/Application/PlatformServicesAuthoring.h"
#include "Horo/Application/PlatformServicesDefinitionCodec.h"
#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Application/ProjectSurfaceSettings.h"
#include "Horo/Security/SecureMemory.h"
#include "Horo/Security/SecurityErrors.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>

namespace Horo::Tests::PlatformMigration {
    using namespace Application;
    using namespace PlatformServices;

    /** @brief Deterministic explicit entropy with observable failure/zero modes. */
    class Random final : public Security::SecureRandomSource {
    public:
        bool fail{};
        bool zero{};
        std::atomic<std::uint32_t>
            calls{}; /**< Fill may execute on worker threads while the owner observes the count; flags change only between executions. */

        Result<void> Fill(const std::span<std::byte> bytes) override {
            ++calls;
            std::ranges::fill(bytes, zero ? std::byte{} : std::byte{0xA5});
            return fail ? Result<void>::Failure(MakeError(SecurityErrors::EntropyUnavailable)) : Result<void>::Success();
        }
    };

    /** @brief Writes explicit test-input documents, not a production publication substitute. */
    inline void WriteInitialDocuments(const std::filesystem::path &root, const PlatformServicesInitialDocuments &initial) {
        const std::array<std::pair<std::string_view, std::string_view>, 6> sources{
            {{".horo/project.json", initial.projectSource},
             {PlatformServicesDocumentPaths::Ledger, initial.ledgerSource},
             {PlatformServicesDocumentPaths::Achievements, initial.achievementSource},
             {PlatformServicesDocumentPaths::Stats, initial.statSource},
             {PlatformServicesDocumentPaths::Leaderboards, initial.leaderboardSource},
             {PlatformServicesDocumentPaths::Presence, initial.presenceSource}}};
        for (const auto &[path, text] : sources) {
            std::ofstream stream(root / path, std::ios::binary | std::ios::trunc);
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            REQUIRE(stream.good());
        }
    }

    /** @brief Owns a real predecessor project in a unique temporary tree, including spaces and non-ASCII path segments. */
    class Project final {
    public:
        explicit Project(const std::string &projectId = "project.platform-adoption") {
            static std::atomic<std::uint64_t> sequence{};
            owner_ = std::filesystem::temp_directory_path() /
                     ("horo-platform-adoption-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                      std::to_string(sequence.fetch_add(1)));
            REQUIRE_FALSE(std::filesystem::exists(owner_));
            root = owner_ / std::filesystem::path{u8"proje alanı-ç"};
            REQUIRE(std::filesystem::create_directories(root / ".horo"));
            nlohmann::json metadata{{"horoVersion", "0.3.0"},
                                    {"persistentContract", "sha256:285bfdfdb77e09706085550de98315000d8bb576e8baef48d3f3be1992fc29d7"},
                                    {"projectId", projectId},
                                    {"name", "Platform migration"},
                                    {"projectVersion", "1.0.0"},
                                    {"createdAt", "2026-11-11T00:00:00Z"},
                                    {"settings", {{"assetCompression", "lz4"}, {"textureCompression", "bc7"}, {"renderBackend", "null"}}},
                                    {"unrelatedRootValue", {1, true, "preserved"}}};
            Write(".horo/project.json", metadata.dump(2) + "\n");
            auto surfaces = ProjectSurfaceSettings::Create({.projectId = projectId});
            REQUIRE(surfaces.HasValue());
            auto source = EncodeProjectSurfaceSettings(surfaces.Value());
            REQUIRE(source.HasValue());
            Write(".horo/surfaces.json", source.Value());
            Write("notes.txt", "unrelated source bytes\n");
        }

        Project(const Project &) = delete;
        Project &operator=(const Project &) = delete;

        ~Project() {
            std::error_code ignored;
            std::filesystem::remove_all(owner_, ignored);
        }

        void Write(const std::filesystem::path &path, const std::string_view text) const {
            std::filesystem::create_directories((root / path).parent_path());
            std::ofstream stream(root / path, std::ios::binary | std::ios::trunc);
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            REQUIRE(stream.good());
        }

        [[nodiscard]] std::string Read(const std::filesystem::path &path) const {
            std::ifstream stream(root / path, std::ios::binary);
            REQUIRE(stream.good());
            return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
        }

        /** @brief Materializes pre-existing source bytes as fixture input, independently of the operation under test. */
        void WriteInitial(const PlatformServicesInitialDocuments &initial) const {
            WriteInitialDocuments(root, initial);
        }

        /** @brief Prepares a complete current-contract fixture without exercising a migration under test. */
        void InitializeCurrent() const {
            auto rootDocument = nlohmann::json::parse(Read(".horo/project.json"));
            const auto *decision = BuiltInReleaseCompatibilityRegistry().Find(CurrentEngineReleaseVersion());
            REQUIRE(decision != nullptr);
            rootDocument["horoVersion"] = FormatHoroVersion(decision->release.value);
            rootDocument["persistentContract"] = FormatPersistentContractHash(decision->persistentContract);
            PlatformServicesIdSalt salt;
            salt.bytes.front() = std::byte{1};
            auto initial = PreparePlatformServicesInitialDocuments(rootDocument.dump(), salt);
            REQUIRE(initial.HasValue());
            WriteInitial(initial.Value());
        }

        [[nodiscard]] ProjectMetadata Metadata() const {
            auto source = DecodeProjectSourceDocument(Read(".horo/project.json"));
            REQUIRE(source.HasValue());
            return source.Value().metadata;
        }

        std::filesystem::path root;

    private:
        std::filesystem::path owner_;
    };
}  // namespace Horo::Tests::PlatformMigration
