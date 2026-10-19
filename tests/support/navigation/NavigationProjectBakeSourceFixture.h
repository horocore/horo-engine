#pragma once

/** @file NavigationProjectBakeSourceFixture.h
 * @brief Real headless project sources, sidecars and retained native read authority for bake automation tests.
 */

#include "Horo/Application/NavigationProjectBakeSource.h"
#include "Horo/Platform/NativeProjectReadFiles.h"
#include "OwnedTestDirectory.h"
#include "navigation/NavigationDataQualificationCorpus.h"

#include <bit>
#include <format>
#include <fstream>

namespace Horo::Application::NavigationSourceTestSupport {
    /** @brief Returns deterministic fixture asset identity independent of source names and container order. */
    inline Assets::AssetId Asset(const unsigned value) {
        return Assets::AssetId::Parse(std::format("00000000-0000-0000-0000-{:012x}", value)).Value();
    }

    /** @brief Owns only genuine canonical source; no editor session, provider, geometry mock or cache is constructed. */
    struct Project final {
        Tests::OwnedTestDirectory directory{"navigation project capture ü"};
        std::shared_ptr<const Platform::IProjectReadFiles> files;

        Project() {
            const auto release = CurrentEngineReleaseVersion();
            const auto *decision = BuiltInReleaseCompatibilityRegistry().Find(release);
            REQUIRE(decision);
            Write(".horo/project.json",
                  std::format(R"({{"horoVersion":"{}","persistentContract":"{}","projectId":"navigation-capture",)"
                              R"("name":"Navigation capture","projectVersion":"1.0.0","createdAt":"2026-10-19T00:00:00Z",)"
                              R"("settings":{{"renderBackend":"null"}}}})",
                              FormatHoroVersion(release.value), FormatPersistentContractHash(decision->persistentContract)));
            const NavigationProjectSettingsInput policy{1,
                                                        Asset(1),
                                                        "recast_detour",
                                                        {Navigation::TestSupport::Id<Navigation::NavigationProjectProfileId>(1),
                                                         Navigation::TestSupport::Id<Navigation::NavigationProjectProfileRevision>(1),
                                                         {64, 8, 128, 8, 1024 * 1024, 128 * 1024 * 1024, 8192},
                                                         {Navigation::NavigationQueryKind::Path,
                                                          Navigation::NavigationQualityLevel::Balanced,
                                                          {1024, 128, 1000}}}};
            const auto settings = NavigationProjectSettings::Create(policy);
            REQUIRE(settings.HasValue());
            Write(".horo/navigation.json", EncodeNavigationProjectSettings(settings.Value()));
            Navigation::TestSupport::NavigationDataQualificationCorpus corpus;
            const std::array support{Navigation::NavigationDefinitionRecordSupport()};
            const auto records = Navigation::NavigationSourceRecords::Create(Navigation::CurrentNavigationSourceSchemaVersion,
                                                                             {corpus.Record()}, {.supportedRecords = support});
            REQUIRE(records.HasValue());
            const auto bytes = Navigation::SerializeNavigationSourceRecords(records.Value());
            REQUIRE(bytes.HasValue());
            WriteBytes("Assets/navigation.horoasset", bytes.Value());
            Sidecar("Assets/navigation.horoasset", Asset(1), "core.navmesh");
            Scene("Assets/first.horo", Asset(2));
            Scene("Assets/second.horo", Asset(3));
            auto opened = Platform::CreateNativeProjectReadFiles(directory.Path(), "navigation-capture", 9);
            REQUIRE(opened.HasValue());
            files = std::move(opened).Value();
        }

        /** @brief Writes exact fixture bytes beneath the exclusively owned test root. */
        void WriteBytes(const std::string_view path, const std::span<const std::byte> bytes) const {
            const auto destination = directory.Path() / path;
            std::filesystem::create_directories(destination.parent_path());
            std::ofstream stream{destination, std::ios::binary};
            for (const auto byte : bytes)
                stream.put(std::bit_cast<char>(byte));
            REQUIRE(stream.good());
        }

        /** @brief Writes text through the same fixture-only byte owner. */
        void Write(const std::string_view path, const std::string_view text) const {
            WriteBytes(path, std::as_bytes(std::span{text}));
        }

        /** @brief Issues persistent identity metadata in the actual registry schema. */
        void Sidecar(const std::string_view source, const Assets::AssetId asset, const std::string_view type) const {
            Write(std::string{source} + ".horo",
                  std::format(R"({{"schemaVersion":1,"assetId":"{}","assetType":"{}"}})", asset.ToString(), type));
        }

        /** @brief Adds one real canonical Scene and stable sidecar, without inventing generated navigation data. */
        void Scene(const std::string_view path, const Assets::AssetId asset) const {
            Write(path, SceneSource::EncodeSceneSource({{}, {}}));
            Sidecar(path, asset, "core.scene");
        }

        /** @brief Retains the exact native opening and a finite complete-invocation deadline. */
        Platform::ProjectReadContext Context() const {
            return {"navigation-capture", 9, {}, std::chrono::steady_clock::now() + std::chrono::seconds{30}, {}};
        }

        /** @brief Runs the actual compatibility inspector before the source owner admits authoring bytes. */
        ProjectCompatibilitySnapshot Compatibility() const {
            return InspectProjectCompatibility(directory.Path());
        }

        /** @brief Captures through the production application boundary and requires its initial complete revalidation. */
        auto Capture() const {
            auto captured = NavigationProjectBakeSource::Capture(files, Context(), Compatibility());
            INFO("Project capture code: " << (captured.HasError() ? captured.ErrorValue().code.Value() : std::string_view{}));
            INFO("Project capture reason: " << (captured.HasError() ? captured.ErrorValue().message : std::string{}));
            REQUIRE(captured.HasValue());
            return std::move(captured).Value();
        }
    };
}  // namespace Horo::Application::NavigationSourceTestSupport
