#include "AllocationProbe.h"
#include "CollisionSettingsTestSupport.h"
#include "Horo/Platform/NativeProjectReadFiles.h"
#include "SceneAllocationQualification.h"

#include <cstdio>

namespace Horo::Tests::CollisionSettings {
    namespace {
        /** @brief Supplies deliberately malformed owned manifests to the canonical input validator without filesystem reads. */
        class ManifestSource final : public Assets::IAssetRegistrySource {
        public:
            explicit ManifestSource(Assets::AssetRegistrySourceManifest manifest) : manifest_(std::move(manifest)) {}

            /** @copydoc Assets::IAssetRegistrySource::Capture */
            Result<Assets::AssetRegistrySourceManifest> Capture() const override {
                return Result<Assets::AssetRegistrySourceManifest>::Success(manifest_);
            }

            /** @copydoc Assets::IAssetRegistrySource::ReadSidecar */
            Result<std::string> ReadSidecar(std::string_view, std::chrono::steady_clock::time_point) const override {
                ++reads_;
                return Result<std::string>::Failure(MakeError(Assets::AssetRegistrySourceErrors::Deadline));
            }

            /** @brief Reports whether a rejected manifest reached metadata access. @return Number of sidecar reads. */
            [[nodiscard]] unsigned ReadCount() const noexcept {
                return reads_;
            }

        private:
            Assets::AssetRegistrySourceManifest manifest_;
            mutable unsigned reads_{};
        };

        /** @brief Compares canonical candidate identity, ordering, status and all typed diagnostics. */
        void RequireParity(const Project &project, const Assets::IAssetRegistrySource &source) {
            auto legacy = Assets::PrepareAssetRegistryCandidate(project.root);
            auto native = Assets::PrepareAssetRegistryCandidate(source);
            REQUIRE(legacy.HasValue());
            REQUIRE(native.HasValue());
            CHECK(native.Value().status == legacy.Value().status);
            REQUIRE(native.Value().records.size() == legacy.Value().records.size());
            for (std::size_t i = 0; i < native.Value().records.size(); ++i) {
                const auto &a = native.Value().records[i];
                const auto &b = legacy.Value().records[i];
                CHECK(a.id == b.id);
                CHECK(a.type == b.type);
                CHECK(a.sourcePath.String() == b.sourcePath.String());
                CHECK(a.metadataPath.String() == b.metadataPath.String());
            }
            REQUIRE(native.Value().diagnostics.size() == legacy.Value().diagnostics.size());
            for (std::size_t i = 0; i < native.Value().diagnostics.size(); ++i) {
                const auto &a = native.Value().diagnostics[i];
                const auto &b = legacy.Value().diagnostics[i];
                CHECK(a.error.domain.Value() == b.error.domain.Value());
                CHECK(a.error.code.Value() == b.error.code.Value());
                CHECK(a.projectPath == b.projectPath);
            }
        }

        /** @brief Captures a small explicit inventory policy while retaining the real native root owner. */
        std::shared_ptr<const Assets::IAssetRegistrySource> WithPolicy(const Project &project,
                                                                       Editor::AssetRegistryInventoryPolicy policy) {
            auto source = Editor::CreateNativeAssetRegistrySource(project.root, "collision-project", 1, policy);
            REQUIRE(source.HasValue());
            return std::move(source).Value();
        }

        /** @brief Exercises every allocation of the actual canonical decoder, retaining expected typed outcomes. */
        void RequireCompleteSidecarSweep(const std::string_view label, const std::string &bytes, const bool valid) {
            const auto baseline = Assets::DecodeAssetIdentitySidecar("Assets/Scene.horoasset", bytes);
            REQUIRE(baseline.HasValue() == valid);
            if (valid) {
                CHECK(baseline.Value().id.ToString() == "10213243-5465-7687-98a9-bacbdcedfe0f");
                CHECK(baseline.Value().type.Value() == "core.scene");
            } else {
                CHECK(baseline.ErrorValue().domain.Value() == "horo.asset");
                CHECK(baseline.ErrorValue().code.Value() == "asset.registry.sidecar_malformed");
                if (label == "duplicate")
                    CHECK(baseline.ErrorValue().message == "JSON contains a duplicate key.");
                if (label == "malformed") {
                    CHECK(baseline.ErrorValue().message.find("[json.exception.parse_error.101]") != std::string::npos);
                    CHECK(baseline.ErrorValue().message != "JSON contains a duplicate key.");
                }
            }
            std::size_t measured{};
            bool outcomeMatches{};
            {
                AllocationProbe::ScopedMeasurement observation;
                const auto result = Assets::DecodeAssetIdentitySidecar("Assets/Scene.horoasset", bytes);
                measured = observation.Snapshot().requests;
                outcomeMatches = result.HasValue() == valid;
            }
            REQUIRE(outcomeMatches);
            REQUIRE(measured > 0);
            std::size_t completed{};
            for (std::size_t failure = 0; failure < measured; ++failure) {
                INFO(label);
                INFO(failure);
                bool rejected{};
                try {
                    AllocationProbe::ScopedFailure fault{failure};
                    const auto result = Assets::DecodeAssetIdentitySidecar("Assets/Scene.horoasset", bytes);
                    rejected = result.HasError();
                } catch (const std::bad_alloc &) {
                    rejected = true;
                }
                REQUIRE(rejected);
                ++completed;
            }
            std::fprintf(stderr, "[collision-sidecar-allocation] %.*s measured=%zu completed=%zu\n", static_cast<int>(label.size()),
                         label.data(), measured, completed);
            REQUIRE(completed == measured);
        }

    }  // namespace

    TEST_CASE("Registry source rejects malformed relative manifests before reading sidecars", "[collision][inventory]") {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
        for (const auto path : {"/outside.png", "Assets/../outside.png", "Assets/./outside.png"}) {
            ManifestSource source{{"Assets", {{path, Assets::AssetRegistrySourceKind::Regular}}, deadline}};
            CHECK(Assets::PrepareAssetRegistryCandidate(source).HasError());
            CHECK(source.ReadCount() == 0);
        }
        ManifestSource duplicate{
            {"Assets",
             {{"Assets/a.png", Assets::AssetRegistrySourceKind::Regular}, {"Assets/a.png", Assets::AssetRegistrySourceKind::Regular}},
             deadline}};
        CHECK(Assets::PrepareAssetRegistryCandidate(duplicate).HasError());
        CHECK(duplicate.ReadCount() == 0);
        ManifestSource invalidKind{{"Assets", {{"Assets/a.png", static_cast<Assets::AssetRegistrySourceKind>(255)}}, deadline}};
        CHECK(Assets::PrepareAssetRegistryCandidate(invalidKind).HasError());
        CHECK(invalidKind.ReadCount() == 0);
        ManifestSource expired{{"Assets", {}, {}}};
        const auto result = Assets::PrepareAssetRegistryCandidate(expired);
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == Assets::AssetRegistrySourceErrors::Deadline.code.Value());
        CHECK(expired.ReadCount() == 0);
    }

    TEST_CASE("Registry source rejects malformed UTF8 and NUL identities before native conversion", "[collision][inventory]") {
        std::string path{"Assets/bad"};
        SECTION("invalid UTF8") {
            path += "\xc0\xaf.png";
        }
        SECTION("embedded NUL") {
            path.push_back('\0');
            path += "ignored.png";
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
        ManifestSource source{{"Assets", {{path, Assets::AssetRegistrySourceKind::Regular}}, deadline}};
        const auto candidate = Assets::PrepareAssetRegistryCandidate(source);
        REQUIRE(candidate.HasError());
        CHECK(candidate.ErrorValue().code.Value() == "asset.registry.root_invalid");
        CHECK(source.ReadCount() == 0);
        const auto decoded = Assets::DecodeAssetIdentitySidecar(
            path, R"({"schemaVersion":1,"assetId":"10213243-5465-7687-98a9-bacbdcedfe0f","assetType":"core.texture"})");
        REQUIRE(decoded.HasError());
        CHECK(decoded.ErrorValue().code.Value() == "asset.registry.root_invalid");
    }

    TEST_CASE("Registry publication retains its prior root after malformed UTF8 source identity", "[collision][inventory]") {
        const auto valid = Assets::DecodeAssetIdentitySidecar(
            "Assets/a.png", R"({"schemaVersion":1,"assetId":"10213243-5465-7687-98a9-bacbdcedfe0f","assetType":"core.texture"})");
        REQUIRE(valid.HasValue());
        Assets::AssetRegistry registry;
        REQUIRE(registry.Publish({valid.Value()}).status == Assets::AssetRegistryBuildStatus::Complete);
        const auto before = registry.Snapshot();
        auto malformed = valid.Value();
        malformed.sourcePath = ProjectPath::Parse(std::string{"Assets/"} + "\xc0\xaf.png").Value();
        const auto rejected = registry.Publish({malformed});
        CHECK(rejected.status == Assets::AssetRegistryBuildStatus::Failed);
        REQUIRE(rejected.diagnostics.size() == 1);
        CHECK(rejected.diagnostics.front().error.code.Value() == "asset.registry.root_invalid");
        CHECK(registry.Snapshot().Revision() == before.Revision());
        REQUIRE(registry.Snapshot().Records().size() == 1);
        CHECK(registry.Snapshot().Records().front().sourcePath.String() == "Assets/a.png");
    }

    TEST_CASE("Native registry source rejects invalid host budgets before admitting a source", "[collision][inventory]") {
        Project project;
        for (const auto &policy : {Editor::AssetRegistryInventoryPolicy{0}, Editor::AssetRegistryInventoryPolicy{(1U << 20U) + 1},
                                   Editor::AssetRegistryInventoryPolicy{1, 0}, Editor::AssetRegistryInventoryPolicy{1, (64U << 20U) + 1},
                                   Editor::AssetRegistryInventoryPolicy{1, 1, std::chrono::milliseconds{0}},
                                   Editor::AssetRegistryInventoryPolicy{1, 1, std::chrono::milliseconds{30001}}}) {
            const auto source = Editor::CreateNativeAssetRegistrySource(project.root, "collision-project", 1, policy);
            REQUIRE(source.HasError());
            CHECK(source.ErrorValue().code.Value() == Platform::ProjectReadErrors::Invalid.code.Value());
        }
    }

    TEST_CASE("Native registry source preserves empty roots casing and canonical sidecar diagnostics", "[collision][inventory]") {
        Project project;
        RequireParity(project, *project.source);
        std::filesystem::rename(project.root / "Assets", project.root / "assets");
        RequireParity(project, *project.source);
        CHECK(project.source->Capture().Value().assetRoot == "assets");
        std::filesystem::create_directory(project.root / "Assets");
        // Case-insensitive volumes expose one spelling; distinct roots can only be tested where both exist.
        if (!std::filesystem::equivalent(project.root / "Assets", project.root / "assets"))
            CHECK(project.source->Capture().Value().assetRoot == "Assets");
        const std::filesystem::path root{project.source->Capture().Value().assetRoot};
        Write(project.root / root / "missing.png", "source");
        Write(project.root / root / "orphan.png.horo", "{}");
        Write(project.root / root / "bad.png", "source");
        Write(project.root / root / "bad.png.horo", "malformed");
        RequireParity(project, *project.source);
    }

    TEST_CASE("Native registry source retains UTF8 registered names through the canonical decoder", "[collision][inventory]") {
        Project project;
        std::string extension{".png"};
        std::string assetType{"core.texture"};
        SECTION("texture identity") {}
        SECTION("mixed-case Prefab extension") {
            extension = ".PREFAB";
            assetType = "core.prefab";
        }
        // Universal escapes keep this fixture independent of the compiler's source code page.
        std::filesystem::path filename{u8"\u0131\u015f\u0131k \u56fe"};
        filename += extension;
        const std::string expected = std::string{"Assets/\xc4\xb1\xc5\x9f\xc4\xb1k \xe5\x9b\xbe"} + extension;
        const auto encodedName = filename.generic_u8string();
        CHECK("Assets/" + std::string{encodedName.begin(), encodedName.end()} == expected);
        const auto source = project.root / "Assets" / filename;
        auto metadata = source;
        metadata += ".horo";
        Write(source, "source bytes");
        Write(metadata,
              std::string{R"({"schemaVersion":1,"assetId":"10213243-5465-7687-98a9-bacbdcedfe0f","assetType":")"} + assetType + R"("})");
        RequireParity(project, *project.source);
        auto candidate = Assets::PrepareAssetRegistryCandidate(*project.source);
        REQUIRE(candidate.HasValue());
        REQUIRE(candidate.Value().records.size() == 1);
        CHECK(candidate.Value().records.front().sourcePath.String() == expected);
        CHECK(candidate.Value().records.front().metadataPath.String() == expected + ".horo");
    }

    TEST_CASE("Native registry source preserves hardlinked sidecars without weakening query reads", "[collision][inventory]") {
        Project project;
        project.Scene(SceneSource::EncodeSceneSource({{}, {}}));
        std::error_code error;
        std::filesystem::create_hard_link(project.root / "Assets/Scene.horoasset.horo", project.root / "metadata-link", error);
        if (error)
            SKIP("The test filesystem cannot create hard links.");
        RequireParity(project, *project.source);
        auto files = Platform::CreateNativeProjectReadFiles(project.root, "collision-project", 1);
        REQUIRE(files.HasValue());
        Platform::ProjectReadContext context{"collision-project", 1, {}, std::chrono::steady_clock::now() + std::chrono::seconds{30}, {}};
        CHECK(files.Value()->Text("Assets/Scene.horoasset.horo", context, {}).HasError());
        CHECK(files.Value()->InventoryText("Assets/Scene.horoasset.horo", context).HasValue());
    }

    TEST_CASE("Native registry source reports symlinks without traversal and ignores unrelated native names", "[collision][inventory]") {
        Project project;
        Write(project.root / "Assets/ordinary.unrelated", "not a supported asset");
        std::filesystem::create_directories(project.root / "Assets/empty");
#ifndef _WIN32
        Write(project.root / "Assets/COM1.unrelated", "not a supported asset");
        std::filesystem::create_directories(project.root / "Assets/CON");
        auto native = Platform::CreateNativeProjectReadFiles(project.root, "collision-project", 1);
        REQUIRE(native.HasValue());
        const Platform::ProjectReadContext context{"collision-project",
                                                   1,
                                                   {},
                                                   std::chrono::steady_clock::now() + std::chrono::seconds{30},
                                                   {}};
        CHECK(native.Value()->Inventory("Assets/CON", Platform::ProjectInventoryDepth::Recursive, context, {}).HasValue());
        CHECK(native.Value()->Files("Assets/CON", context, {}).HasError());
#endif
        RequireParity(project, *project.source);
        const auto unrelated = Assets::PrepareAssetRegistryCandidate(*project.source);
        REQUIRE(unrelated.HasValue());
        CHECK(unrelated.Value().status == Assets::AssetRegistryBuildStatus::Complete);
        CHECK(unrelated.Value().records.empty());
        CHECK(unrelated.Value().diagnostics.empty());
        std::error_code error;
        std::filesystem::create_directory_symlink(project.root, project.root / "Assets/cycle", error);
        if (error)
            SKIP("The test filesystem cannot create directory symlinks.");
        RequireParity(project, *project.source);
        auto candidate = Assets::PrepareAssetRegistryCandidate(*project.source);
        REQUIRE(candidate.HasValue());
        CHECK(candidate.Value().status == Assets::AssetRegistryBuildStatus::Degraded);
        REQUIRE(candidate.Value().diagnostics.size() == 1);
        CHECK(candidate.Value().diagnostics.front().projectPath == "Assets/cycle");
    }

    TEST_CASE("Native collision registry source refuses a symbolic asset root instead of following its alias", "[collision][inventory]") {
        Project project;
        OwnedTestDirectory external{"external asset root"};
        std::filesystem::remove(project.root / "Assets");
        std::error_code error;
        std::filesystem::create_directory_symlink(external.Path(), project.root / "Assets", error);
        if (error)
            SKIP("The test filesystem cannot create directory symlinks.");
        CHECK(Assets::PrepareAssetRegistryCandidate(project.root).HasValue());
        const auto captured = project.source->Capture();
        REQUIRE(captured.HasError());
        CHECK(captured.ErrorValue().code.Value() == Platform::ProjectReadErrors::UnsafePath.code.Value());
    }

    TEST_CASE("Native registry source preserves duplicate identity rejection and malformed sidecar evidence", "[collision][inventory]") {
        Project project;
        project.Scene(SceneSource::EncodeSceneSource({{}, {}}));
        Write(project.root / "Assets/Second.horoasset", "{}");
        Write(project.root / "Assets/Second.horoasset.horo",
              R"({"schemaVersion":1,"assetId":"10213243-5465-7687-98a9-bacbdcedfe0f","assetType":"core.scene"})");
        RequireParity(project, *project.source);
        auto candidate = Assets::PrepareAssetRegistryCandidate(*project.source);
        REQUIRE(candidate.HasValue());
        CHECK(candidate.Value().status == Assets::AssetRegistryBuildStatus::Failed);
    }

    TEST_CASE("Native registry source retains decoder errors and typed sidecar capacity failures", "[collision][inventory]") {
        Project project;
        project.Scene(SceneSource::EncodeSceneSource({{}, {}}));
        Write(project.root / "Assets/Scene.horoasset.horo", "");
        RequireParity(project, *project.source);
        std::string bounded = R"({"schemaVersion":1,"assetId":"10213243-5465-7687-98a9-bacbdcedfe0f","assetType":"core.scene"})";
        bounded.resize(1U << 20U, ' ');
        Write(project.root / "Assets/Scene.horoasset.horo", bounded);
        RequireParity(project, *project.source);
        const auto exact = Assets::PrepareAssetRegistryCandidate(*project.source);
        REQUIRE(exact.HasValue());
        CHECK(exact.Value().status == Assets::AssetRegistryBuildStatus::Complete);
        bounded.push_back(' ');
        Write(project.root / "Assets/Scene.horoasset.horo", bounded);
        auto candidate = Assets::PrepareAssetRegistryCandidate(*project.source);
        REQUIRE(candidate.HasValue());
        CHECK(candidate.Value().status == Assets::AssetRegistryBuildStatus::Degraded);
        REQUIRE(candidate.Value().diagnostics.size() == 1);
        CHECK(candidate.Value().diagnostics.front().error.code.Value() == Platform::ProjectReadErrors::Capacity.code.Value());
    }

    TEST_CASE("Native registry sidecar reads preserve each explicit capture deadline without ambient renewal", "[collision][inventory]") {
        Project project;
        project.Scene(SceneSource::EncodeSceneSource({{}, {}}));
        const auto first = project.source->Capture();
        const auto second = project.source->Capture();
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        const auto expired = project.source->ReadSidecar("Assets/Scene.horoasset.horo", {});
        REQUIRE(expired.HasError());
        CHECK(expired.ErrorValue().code.Value() == Platform::ProjectReadErrors::Deadline.code.Value());
        CHECK(project.source->ReadSidecar("Assets/Scene.horoasset.horo", first.Value().deadline).HasValue());
        CHECK(project.source->ReadSidecar("Assets/Scene.horoasset.horo", second.Value().deadline).HasValue());
    }

    TEST_CASE("Native registry source applies exact independent entry and name-byte boundaries", "[collision][inventory]") {
        Project project;
        Write(project.root / "Assets/ignored.bin", "ignored");
        Platform::ProjectReadContext context{"collision-project", 1, {}, std::chrono::steady_clock::now() + std::chrono::seconds{30}, {}};
        auto files = Platform::CreateNativeProjectReadFiles(project.root, "collision-project", 1);
        REQUIRE(files.HasValue());
        Platform::ProjectInventoryLimits limits{1, std::string_view{"Assets/ignored.bin"}.size() + 1, std::chrono::seconds{30}};
        REQUIRE(files.Value()->Inventory("Assets", Platform::ProjectInventoryDepth::Recursive, context, limits).HasValue());
        --limits.maximumManifestBytes;
        CHECK(files.Value()->Inventory("Assets", Platform::ProjectInventoryDepth::Recursive, context, limits).HasError());
        limits = {};
        limits.maximumEntries = 0;
        CHECK(files.Value()->Inventory("Assets", Platform::ProjectInventoryDepth::Recursive, context, limits).HasError());
        auto limited = WithPolicy(project, {1, 64U << 20U, std::chrono::seconds{30}});
        const auto exhausted = limited->Capture();
        REQUIRE(exhausted.HasError());
        CHECK(exhausted.ErrorValue().code.Value() == Platform::ProjectReadErrors::Capacity.code.Value());
        context.deadline = std::chrono::steady_clock::now();
        const auto expired = files.Value()->Inventory("Assets", Platform::ProjectInventoryDepth::Recursive, context, {});
        REQUIRE(expired.HasError());
        CHECK(expired.ErrorValue().code.Value() == Platform::ProjectReadErrors::Deadline.code.Value());
        context.deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
        context.authorityStopped = [] {
            return true;
        };
        const auto cancelled = files.Value()->Inventory("Assets", Platform::ProjectInventoryDepth::Recursive, context, {});
        REQUIRE(cancelled.HasError());
        CHECK(cancelled.ErrorValue().code.Value() == Platform::ProjectReadErrors::Cancelled.code.Value());
        context.authorityStopped = {};
        ++context.projectGeneration;
        const auto stale = files.Value()->Inventory("Assets", Platform::ProjectInventoryDepth::Recursive, context, {});
        REQUIRE(stale.HasError());
        CHECK(stale.ErrorValue().code.Value() == Platform::ProjectReadErrors::Stale.code.Value());
        --context.projectGeneration;
        CHECK(files.Value()->Inventory("Assets", Platform::ProjectInventoryDepth::Recursive, context, {}).HasValue());
    }

    TEST_CASE("Native registry inventory recovers after every one-shot allocation failure", "[collision][allocation][inventory]") {
        RequireSupportedSceneAllocationInjection();
        Project project;
        project.Scene(SceneSource::EncodeSceneSource({{}, {}}));
        const auto nested = project.root / "Assets/nested/deep/empty";
        std::filesystem::create_directories(nested);
        std::size_t allocations{};
        {
            AllocationProbe::ScopedMeasurement measurement;
            auto captured = project.source->Capture();
            allocations = measurement.Snapshot().requests;
            REQUIRE(captured.HasValue());
        }
        REQUIRE(allocations > 0);
        std::size_t completed{};
        for (std::size_t failure = 0; failure < allocations; ++failure) {
            INFO(failure);
            REQUIRE(std::filesystem::is_directory(nested));
            auto captured = [&] {
                AllocationProbe::ScopedFailure fault{failure};
                return project.source->Capture();
            }();
            REQUIRE(captured.HasError());
            REQUIRE(project.source->Capture().HasValue());
            ++completed;
        }
        std::fprintf(stderr, "[collision-allocation] inventory measured=%zu completed=%zu\n", allocations, completed);
        REQUIRE(completed == allocations);
    }

    TEST_CASE("Canonical sidecar decoding unwinds every allocation across valid invalid and deep compound DOMs",
              "[collision][allocation][sidecar]") {
        RequireSupportedSceneAllocationInjection();
        const std::string valid = R"({"schemaVersion":1,"assetId":"10213243-5465-7687-98a9-bacbdcedfe0f","assetType":"core.scene"})";
        auto deep = valid.substr(0, valid.size() - 1) + R"(,"extra":)";
        for (unsigned level = 0; level < 96; ++level)
            deep += R"({"nested":[)";
        deep += R"({"quoted":"[\"{not structural}\"]","value":1})";
        for (unsigned level = 0; level < 96; ++level)
            deep += "]}";
        deep += '}';
        RequireCompleteSidecarSweep("valid", valid, true);
        RequireCompleteSidecarSweep("duplicate", R"({"schemaVersion":1,"schemaVersion":1})", false);
        RequireCompleteSidecarSweep("malformed", R"({"schemaVersion":1,"schemaVersion":1,"assetId":[})", false);
        RequireCompleteSidecarSweep("deep", deep, true);
    }
}  // namespace Horo::Tests::CollisionSettings
