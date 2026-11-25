#include "CliOutputContractFixture.h"
#include "Horo/Cli/CliErrors.h"
#include "Horo/Packages/PackageMigration.h"
#include "Horo/Packages/PackageRequest.h"
#include "Horo/Scene/SceneSource.h"
#include "PackageApplicationServiceTestSupport.h"
#include "PackageCliHost.h"

#include <array>
#include <catch2/generators/catch_generators.hpp>
#include <fstream>
#include <iterator>

namespace {
    using namespace Horo;
    using namespace Horo::Cli;
    using namespace Horo::Application;
    using namespace Horo::Application::Internal;

    class InvocationProbe final : public ICliCommandAdapter {
    public:
        explicit InvocationProbe(std::function<void(CliExecutionContext &)> callback)
            : descriptor_(DescribePackageCommands().front()), callback_(std::move(callback)) {
            descriptor_.timeout = {.defaultMilliseconds = 5'000, .maximumMilliseconds = 5'000};
        }

        const CliCommandDescriptor &GetDescriptor() const noexcept override {
            return descriptor_;
        }

        Result<CliCommandResult> Execute(const CliCommandRequest &, CliExecutionContext &context) override {
            callback_(context);
            return Result<CliCommandResult>::Success({});
        }

    private:
        CliCommandDescriptor descriptor_;
        std::function<void(CliExecutionContext &)> callback_;
    };

    // The dispatcher creates the real invocation authority; the probe never fabricates a context.
    void InInvocation(std::function<void(CliExecutionContext &)> callback, CancellationToken cancellation = {},
                      const std::uint64_t timeout = 5'000) {
        bool executed = false;
        auto adapter = std::make_unique<InvocationProbe>([&](CliExecutionContext &context) {
            executed = true;
            callback(context);
        });
        const auto descriptor = adapter->GetDescriptor();
        auto registry = CliCommandRegistry::Create(std::span{&descriptor, 1}, {.activeHost = CliHostKind::HoroEngine,
                                                                               .supportedContractVersion = {1, 0, 0},
                                                                               .grantedCapabilities = {{"horo.package.manage"}}});
        REQUIRE(registry.HasValue());
        std::vector<CliCommandAdapterRegistration> registrations;
        registrations.push_back({std::move(adapter), {{"horo.package.manage"}}});
        auto dispatcher = CliDispatcher::Create(std::move(registry).Value(), std::move(registrations),
                                                {.activeHost = CliHostKind::HoroEngine,
                                                 .maximumSideEffects = CliSideEffectPolicy::WritesFiles,
                                                 .grantedCapabilities = {{"horo.package.manage"}}});
        REQUIRE(dispatcher.HasValue());
        ConfigurationSchema schema;
        REQUIRE(schema.Seal().HasValue());
        auto configuration = ConfigurationResolver::Resolve(schema, {}, 1);
        REQUIRE(configuration.HasValue());
        auto admitted = std::move(dispatcher).Value();
        static_cast<void>(admitted.Dispatch({.command = descriptor.path}, {.invocation = {1},
                                                                           .configuration = &configuration.Value(),
                                                                           .cancellation = std::move(cancellation),
                                                                           .timeoutMilliseconds = timeout}));
        REQUIRE(executed);
    }

    struct NativeProject final {
        Tests::Packages::TemporaryDirectory project{"cli package project ü"}, storage{"cli package storage ü"};
        Tests::PackageApplicationService::AssetGraph graph;

        explicit NativeProject(bool native = false) : graph(native, native) {
            const nlohmann::json intent{{"sources", {{"fixture.source", {{"kind", "public-registry"}, {"registry", "official"}}}}},
                                        {"dependencies",
                                         {{"com.horo.application-fixture", {{"source", "fixture.source"}, {"version", "1.0.0"}}}}}};
            auto request = Packages::ValidatedPackageRequest::Parse(intent.dump());
            REQUIRE(request.HasValue());
            Packages::PackageResolutionPlan plan{.packages = {{graph.package, graph.version, graph.source, graph.archive.Digest(), {}}}};
            Packages::PackageLockArtifact artifact{.package = graph.package,
                                                   .version = graph.version,
                                                   .source = graph.source,
                                                   .artifactDigest = graph.archive.Digest(),
                                                   .manifestDigest = graph.archive.PackageManifestDigest(),
                                                   .fileManifestDigest = graph.archive.Manifest().Digest(),
                                                   .packageFormatVersion = 1,
                                                   .platforms = {},
                                                   .contributions = {native ? "native" : "assets"}};
            auto lock = Packages::ValidatedPackageLockfileV1::Generate(plan, std::span{&graph.package, 1U}, request.Value().Digest(),
                                                                       std::span{&artifact, 1U});
            REQUIRE(lock.HasValue());
            std::filesystem::create_directories(project.Path() / ".horo");
            std::filesystem::create_directories(storage.Path() / "temporary");
            Write("packages.json", intent.dump());
            Write("packages.lock", lock.Value().SerializeCanonical());
        }

        void Write(const char *name, const std::string &text) {
            std::ofstream output{project.Path() / ".horo" / name, std::ios::binary};
            output << text;
            REQUIRE(output.good());
        }

        void WriteScene(const char *name, const char *assetId, std::string_view bytes, bool imported = false) const {
            const auto folder = project.Path() / "Assets";
            std::filesystem::create_directories(folder);
            const auto asset = folder / name;
            auto sidecar = asset;
            sidecar += ".horo";
            std::ofstream{asset, std::ios::binary} << bytes;
            nlohmann::json metadata{{"schemaVersion", 1}, {"assetId", assetId}, {"assetType", "core.scene"}};
            if (imported)
                metadata["importerPackageId"] = graph.package.Value();
            std::ofstream{sidecar} << metadata;
        }

        void WriteImporterReference(const char *name, const char *assetId) const {
            WriteScene(name, assetId, "{}", true);
        }

        static CliParsedOption PathOption(const char *name, const std::filesystem::path &path) {
            const auto utf8 = path.generic_u8string();
            return {name, {{CliPathValue{std::string{utf8.begin(), utf8.end()}}, CliValueSource::Invocation, {}}}};
        }

        CliCommandRequest Request() const {
            return {.command = {{"package", "restore"}},
                    .options = {PathOption("project", std::filesystem::canonical(project.Path())),
                                PathOption("package-cache-root", std::filesystem::canonical(storage.Path()) / "cache"),
                                PathOption("package-temporary-root", std::filesystem::canonical(storage.Path()) / "temporary")}};
        }
    };

    /** @brief Writes a resolved asset candidate bound to the fixture's source and archive. */
    void WriteAssetCandidate(NativeProject &fixture, const Packages::PackageVersion &version,
                             const Packages::ValidatedPackageArchive &archive, std::string_view contribution = "assets") {
        const auto intent = nlohmann::json{
            {"sources", {{"fixture.source", {{"kind", "public-registry"}, {"registry", "official"}}}}},
            {"dependencies", {{fixture.graph.package.Value(), {{"source", "fixture.source"}, {"version", version.ToString()}}}}}}.dump();
        const auto request = Packages::ValidatedPackageRequest::Parse(intent);
        REQUIRE(request.HasValue());
        const Packages::PackageResolutionPlan plan{
            .packages = {{fixture.graph.package, version, fixture.graph.source, archive.Digest(), {}}}};
        const Packages::PackageLockArtifact entry{.package = fixture.graph.package,
                                                 .version = version,
                                                 .source = fixture.graph.source,
                                                 .artifactDigest = archive.Digest(),
                                                 .manifestDigest = archive.PackageManifestDigest(),
                                                 .fileManifestDigest = archive.Manifest().Digest(),
                                                 .packageFormatVersion = 1,
                                                 .contributions = {std::string{contribution}}};
        const auto lock = Packages::ValidatedPackageLockfileV1::Generate(
            plan, std::span{&fixture.graph.package, 1}, request.Value().Digest(), std::span{&entry, 1});
        REQUIRE(lock.HasValue());
        fixture.Write("candidate.json", intent);
        fixture.Write("candidate.lock", lock.Value().SerializeCanonical());
    }

    /** @brief Replaces the fixture's initial graph with a valid empty request and lock. */
    void WriteEmptyPackageGraph(NativeProject &fixture) {
        constexpr auto intent = R"({"sources":{},"dependencies":{}})";
        const auto request = Packages::ValidatedPackageRequest::Parse(intent);
        REQUIRE(request.HasValue());
        const auto lock = Packages::ValidatedPackageLockfileV1::Generate({}, {}, request.Value().Digest(), {});
        REQUIRE(lock.HasValue());
        fixture.Write("packages.json", intent);
        fixture.Write("packages.lock", lock.Value().SerializeCanonical());
    }

    /** @brief Checks that offline review names the package owner blocker at the expected archive path. */
    void CheckOfflineAdditionBlocker(const Result<CliCommandResult> &result, std::string_view path) {
        REQUIRE(result.HasError());
        REQUIRE_FALSE(result.ErrorValue().diagnostics.empty());
        const auto &blocker = result.ErrorValue().diagnostics.front();
        CHECK(blocker.code.Value() == "packages.host.update_archive_requires_owner");
        CHECK(blocker.path == path);
    }

    TEST_CASE("Offline package update inspects contributions of newly added archives", "[packages][application][cli][update]") {
        using namespace Tests::PackageApplicationService;
        enum class Addition { Assets, NativeDeclared, NativeDeclaredAsAssets };
        const auto mode = GENERATE(Addition::Assets, Addition::NativeDeclared, Addition::NativeDeclaredAsAssets);
        NativeProject fixture;
        WriteEmptyPackageGraph(fixture);
        const auto archive = mode == Addition::Assets ? fixture.graph.archive : Verify(NativeAdmissionArchiveBytes());
        WriteAssetCandidate(fixture, fixture.graph.version, archive, mode == Addition::NativeDeclared ? "native" : "assets");
        const auto root = std::filesystem::canonical(fixture.project.Path());
        const auto metadata = root / ".horo";
        const auto cacheRoot = std::filesystem::canonical(fixture.storage.Path()) / "cache";
        NativeDurableFileSystem files;
        const auto oldLock = files.ReadFileBounded(metadata / "packages.lock", 1U << 20U, {});
        REQUIRE(oldLock.HasValue());
        auto cache = Packages::PackageCacheStore::Create(files, cacheRoot);
        REQUIRE(cache.HasValue());
        REQUIRE(std::move(cache).Value().Publish(archive).HasValue());
        const auto preview = MakePackageUpdatePreviewCommandAdapter();
        const auto apply = MakePackageApplyUpdateCommandAdapter();
        const CliCommandRequest command{.command = preview.adapter->GetDescriptor().path,
                                        .options = {NativeProject::PathOption("project", root),
                                                    NativeProject::PathOption("candidate-request", metadata / "candidate.json"),
                                                    NativeProject::PathOption("candidate-lock", metadata / "candidate.lock")}};
        InInvocation([&](CliExecutionContext &context) {
            const auto approval = preview.adapter->Execute(command, context);
            REQUIRE(approval.HasValue());
            auto reviewed = command;
            reviewed.options.push_back(NativeProject::PathOption("package-cache-root", cacheRoot));
            const auto previewed = preview.adapter->Execute(reviewed, context);
            reviewed.command = apply.adapter->GetDescriptor().path;
            const auto fingerprint = std::get<std::string>(approval.Value().fields.back().value);
            reviewed.options.push_back({"approve", {{fingerprint, CliValueSource::Invocation, {}}}});
            const auto applied = apply.adapter->Execute(reviewed, context);
            const auto installed = files.ReadFileBounded(metadata / "packages.lock", 1U << 20U, {});
            REQUIRE(installed.HasValue());
            if (mode == Addition::Assets) {
                REQUIRE(previewed.HasValue());
                REQUIRE(applied.HasValue());
                const auto candidate = files.ReadFileBounded(metadata / "candidate.lock", 1U << 20U, {});
                REQUIRE(candidate.HasValue());
                CHECK(installed.Value() == candidate.Value());
                return;
            }
            CHECK(installed.Value() == oldLock.Value());
            CHECK_FALSE(std::filesystem::exists(metadata / "local/package-metadata.pending"));
            const auto *path = mode == Addition::NativeDeclaredAsAssets ? "extensions/native/extension.json" : "";
            CheckOfflineAdditionBlocker(previewed, path);
            CheckOfflineAdditionBlocker(applied, path);
        });
    }

    TEST_CASE("Headless package update preview reads candidate documents without repairing or modifying the project",
              "[packages][application][cli][update]") {
        NativeProject fixture;
        const auto root = std::filesystem::canonical(fixture.project.Path());
        const auto metadata = root / ".horo";
        const auto read = [](const std::filesystem::path &path) {
            const std::ifstream input{path, std::ios::binary};
            REQUIRE(input.good());
            return std::string{std::istreambuf_iterator<char>{input.rdbuf()}, {}};
        };
        const auto originalRequest = read(metadata / "packages.json");
        const auto originalLock = read(metadata / "packages.lock");
        auto candidate = nlohmann::json::parse(originalRequest);
        candidate["dependencies"] = nlohmann::json::object();
        const auto intent = Packages::ValidatedPackageRequest::Parse(candidate.dump());
        REQUIRE(intent.HasValue());
        const auto lock = Packages::ValidatedPackageLockfileV1::Generate({}, {}, intent.Value().Digest(), {});
        REQUIRE(lock.HasValue());
        fixture.Write("candidate.json", candidate.dump());
        fixture.Write("candidate.lock", lock.Value().SerializeCanonical());
        auto registration = MakePackageUpdatePreviewCommandAdapter();
        const auto descriptor = registration.adapter->GetDescriptor();
        auto applyRegistration = MakePackageApplyUpdateCommandAdapter();
        const auto applyDescriptor = applyRegistration.adapter->GetDescriptor();
        auto rollbackPreview = MakePackageRollbackPreviewCommandAdapter();
        auto rollbackApply = MakePackageRollbackUpdateCommandAdapter();
        const auto rollbackPreviewPath = rollbackPreview.adapter->GetDescriptor().path;
        const auto rollbackApplyPath = rollbackApply.adapter->GetDescriptor().path;
        const std::array descriptors{descriptor, applyDescriptor, rollbackPreview.adapter->GetDescriptor(),
                                     rollbackApply.adapter->GetDescriptor()};
        CHECK(descriptor.sideEffects == CliSideEffectPolicy::ReadsState);
        CHECK(applyDescriptor.sideEffects == CliSideEffectPolicy::WritesFiles);
        const CliDispatchPolicy policy{.activeHost = CliHostKind::HoroEngine,
                                       .maximumSideEffects = CliSideEffectPolicy::WritesFiles,
                                       .grantedCapabilities = registration.capabilities};
        auto registry = CliCommandRegistry::Create(descriptors, {.activeHost = policy.activeHost,
                                                                 .supportedContractVersion = {1, 0, 0},
                                                                 .grantedCapabilities = policy.grantedCapabilities});
        REQUIRE(registry.HasValue());
        std::vector<CliCommandAdapterRegistration> adapters;
        adapters.push_back(std::move(registration));
        adapters.push_back(std::move(applyRegistration));
        adapters.push_back(std::move(rollbackPreview));
        adapters.push_back(std::move(rollbackApply));
        auto dispatcher = CliDispatcher::Create(std::move(registry).Value(), std::move(adapters), policy);
        REQUIRE(dispatcher.HasValue());
        auto admitted = std::move(dispatcher).Value();
        ConfigurationSchema schema;
        REQUIRE(schema.Seal().HasValue());
        const auto configuration = ConfigurationResolver::Resolve(schema, {}, 1);
        REQUIRE(configuration.HasValue());
        const CliCommandRequest command{.command = descriptor.path,
                                        .options = {NativeProject::PathOption("project", root),
                                                    NativeProject::PathOption("candidate-request", metadata / "candidate.json"),
                                                    NativeProject::PathOption("candidate-lock", metadata / "candidate.lock")}};
        const auto cacheRoot = std::filesystem::canonical(fixture.storage.Path()) / "cache";
        const auto execute = [&](const bool cacheReview = false) {
            auto preview = command;
            if (cacheReview)
                preview.options.push_back(NativeProject::PathOption("package-cache-root", cacheRoot));
            return admitted.Dispatch(preview, {.invocation = {1}, .configuration = &configuration.Value()});
        };
        const auto apply = [&](const std::string &fingerprint) {
            auto approved = command;
            approved.command = applyDescriptor.path;
            approved.options.push_back({"approve", {{fingerprint, CliValueSource::Invocation, {}}}});
            approved.options.push_back(NativeProject::PathOption("package-cache-root", cacheRoot));
            return admitted.Dispatch(approved, {.invocation = {2}, .configuration = &configuration.Value()});
        };
        SECTION("the preview reports the exact removal") {
            const auto result = execute();
            REQUIRE(result.Outcome().HasValue());
            const auto &fields = result.Outcome().Value().fields;
            REQUIRE(fields.size() == 5);
            CHECK(std::get<bool>(fields[0].value));
            CHECK(std::get<std::int64_t>(fields[1].value) == 1);
            CHECK(std::get<std::int64_t>(fields[2].value) == -1);
            CHECK(std::get<std::string>(fields[3].value).starts_with("com.horo.application-fixture [1.0.0 fixture.source "));
            CHECK(std::get<std::string>(fields[3].value).ends_with("] -> [absent]"));
            const auto fingerprint = std::get<std::string>(fields[4].value);
            const auto blocked = apply(fingerprint);
            REQUIRE(blocked.Outcome().HasError());
            CHECK(blocked.Outcome().ErrorValue().code.Value() == "packages.host.update_requires_migration_review");
            fixture.Write("candidate.json", candidate.dump() + '\n');
            const auto stale = apply(fingerprint);
            REQUIRE(stale.Outcome().HasError());
            CHECK(stale.Outcome().ErrorValue().code.Value() == "packages.host.update_approval_mismatch");
        }
        SECTION("a cache-backed review reports a version change even when archive bytes match") {
            const auto version = Packages::PackageVersion::Parse("2.0.0").Value();
            const auto &archive = fixture.graph.archive;
            WriteAssetCandidate(fixture, version, archive);
            const auto missing = execute(true);
            REQUIRE(missing.Outcome().HasError());
            CHECK(missing.Outcome().ErrorValue().code.Value() == "packages.host.update_archive_evidence_unavailable");
            CHECK_FALSE(std::filesystem::exists(cacheRoot));

            NativeDurableFileSystem files;
            auto cache = Packages::PackageCacheStore::Create(files, cacheRoot);
            REQUIRE(cache.HasValue());
            auto verifiedCache = std::move(cache).Value();
            REQUIRE(verifiedCache.Publish(archive).HasValue());
            const auto reviewed = execute(true);
            REQUIRE(reviewed.Outcome().HasError());
            const auto &why = reviewed.Outcome().ErrorValue();
            REQUIRE(why.diagnostics.size() == 1);
            CHECK(why.diagnostics.front().code.Value() == "packages.host.update_version_migration_required");
            CHECK(read(metadata / "packages.lock") == originalLock);
        }
        SECTION("an approved update can change a request while preserving the verified locked packages") {
            auto next = nlohmann::json::parse(originalRequest);
            next["dependencies"][fixture.graph.package.Value()]["version"] = "^1.0.0";
            const auto updated = Packages::ValidatedPackageRequest::Parse(next.dump());
            REQUIRE(updated.HasValue());
            auto nextLock = nlohmann::json::parse(originalLock);
            nextLock["requestHash"] = FormatSha256(updated.Value().Digest());
            fixture.Write("candidate.json", next.dump());
            fixture.Write("candidate.lock", nextLock.dump());
            NativeDurableFileSystem files;
            auto cache = Packages::PackageCacheStore::Create(files, cacheRoot);
            REQUIRE(cache.HasValue());
            auto verifiedCache = std::move(cache).Value();
            REQUIRE(verifiedCache.Publish(fixture.graph.archive).HasValue());
            const auto result = execute();
            REQUIRE(result.Outcome().HasValue());
            const auto fingerprint = std::get<std::string>(result.Outcome().Value().fields.back().value);
            const auto applied = apply(fingerprint);
            REQUIRE(applied.Outcome().HasValue());
            CHECK_FALSE(std::get<bool>(applied.Outcome().Value().fields.front().value));
            CHECK(read(metadata / "packages.json") == next.dump());
            CHECK(read(metadata / "packages.lock") == nextLock.dump());

            CliCommandRequest rollback{.command = rollbackPreviewPath, .options = {NativeProject::PathOption("project", root)}};
            const auto reviewed = admitted.Dispatch(rollback, {.invocation = {3}, .configuration = &configuration.Value()});
            REQUIRE(reviewed.Outcome().HasValue());
            const auto rollbackFingerprint = std::get<std::string>(reviewed.Outcome().Value().fields.back().value);
            CHECK(rollbackFingerprint != fingerprint);
            rollback.command = rollbackApplyPath;
            rollback.options.push_back({"approve", {{rollbackFingerprint, CliValueSource::Invocation, {}}}});
            rollback.options.push_back(NativeProject::PathOption("package-cache-root", cacheRoot));
            fixture.Write("packages.json", next.dump() + '\n');
            const auto stale = admitted.Dispatch(rollback, {.invocation = {4}, .configuration = &configuration.Value()});
            REQUIRE(stale.Outcome().HasError());
            CHECK(read(metadata / "packages.json") == next.dump() + '\n');
            fixture.Write("packages.json", next.dump());
            const auto restored = admitted.Dispatch(rollback, {.invocation = {5}, .configuration = &configuration.Value()});
            REQUIRE(restored.Outcome().HasValue());
            CHECK(read(metadata / "packages.json") == originalRequest);
            CHECK(read(metadata / "packages.lock") == originalLock);
            return;
        }
        SECTION("an asset-only replacement preserves mounted paths and rejects changed bytes") {
            NativeDurableFileSystem files;
            auto cache = Packages::PackageCacheStore::Create(files, cacheRoot);
            REQUIRE(cache.HasValue());
            auto verifiedCache = std::move(cache).Value();
            REQUIRE(verifiedCache.Publish(fixture.graph.archive).HasValue());
            for (const bool preserves : {false, true}) {
                const auto bytes = Tests::Packages::ValidPackageArchiveBytes(preserves ? "verified bytes" : "changed bytes", "new asset");
                const auto archive = Packages::ValidatedPackageArchive::Verify(bytes);
                REQUIRE(archive.HasValue());
                REQUIRE(verifiedCache.Publish(archive.Value()).HasValue());
                auto nextLock = nlohmann::json::parse(originalLock);
                auto &locked = nextLock["packages"].front();
                locked["artifactSha256"] = FormatSha256(archive.Value().Digest());
                locked["fileManifestSha256"] = FormatSha256(archive.Value().Manifest().Digest());
                fixture.Write("candidate.json", originalRequest);
                fixture.Write("candidate.lock", nextLock.dump());
                const auto preview = execute();
                REQUIRE(preview.Outcome().HasValue());
                const auto reviewed = execute(true);
                const auto result = apply(std::get<std::string>(preview.Outcome().Value().fields.back().value));
                if (!preserves) {
                    REQUIRE(reviewed.Outcome().HasError());
                    const auto &why = reviewed.Outcome().ErrorValue();
                    CHECK(why.code.Value() == "packages.host.update_requires_migration_review");
                    REQUIRE(why.diagnostics.size() == 1);
                    CHECK(why.diagnostics.front().code.Value() == "packages.host.update_asset_content_changed");
                    CHECK(why.diagnostics.front().path == "assets/data.bin");
                    REQUIRE(result.Outcome().HasError());
                    CHECK(result.Outcome().ErrorValue().code.Value() == "packages.host.update_requires_migration_review");
                    REQUIRE(result.Outcome().ErrorValue().diagnostics.size() == 1);
                    CHECK(result.Outcome().ErrorValue().diagnostics.front().path == "assets/data.bin");
                    CHECK(read(metadata / "packages.lock") == originalLock);
                } else {
                    REQUIRE(reviewed.Outcome().HasValue());
                    REQUIRE(result.Outcome().HasValue());
                    CHECK(read(metadata / "packages.lock") == nextLock.dump());
                    const auto rollback = admitted.Dispatch({.command = rollbackPreviewPath,
                                                             .options = {NativeProject::PathOption("project", root),
                                                                         NativeProject::PathOption("package-cache-root", cacheRoot)}},
                                                            {.invocation = {3}, .configuration = &configuration.Value()});
                    REQUIRE(rollback.Outcome().HasError());
                    const auto &why = rollback.Outcome().ErrorValue();
                    REQUIRE(why.diagnostics.size() == 1);
                    CHECK(why.diagnostics.front().code.Value() == "packages.host.update_asset_removed");
                    CHECK(why.diagnostics.front().path == "assets/extra.bin");
                    CHECK(read(metadata / "packages.lock") == nextLock.dump());
                }
            }
            CHECK(read(metadata / "packages.json") == originalRequest);
            return;
        }
        SECTION("a pending publication is left intact") {
            std::filesystem::create_directory(metadata / "local");
            fixture.Write("local/package-metadata.pending", "pending publication");
            CHECK(execute().Outcome().HasError());
            CHECK(read(metadata / "local/package-metadata.pending") == "pending publication");
        }
        CHECK(read(metadata / "packages.json") == originalRequest);
        CHECK(read(metadata / "packages.lock") == originalLock);
    }

    TEST_CASE("Asset replacements can carry and retire historical migration metadata", "[packages][application][cli][update]") {
        NativeProject fixture;
        const auto root = std::filesystem::canonical(fixture.project.Path());
        const auto metadata = root / ".horo";
        const auto cacheRoot = std::filesystem::canonical(fixture.storage.Path()) / "cache";
        const auto version = Packages::PackageVersion::Parse("1.0.0").Value();
        const nlohmann::json transition{{"fromVersion", "0.9.0"},
                                       {"fromArtifactSha256", FormatSha256(Sha256Digest{})},
                                       {"renames", {{{"from", "assets/legacy.bin"}, {"to", "assets/data.bin"}}}}};
        const nlohmann::json declaration{{"schemaVersion", 1},
                                         {"packageId", fixture.graph.package.Value()},
                                         {"toVersion", version.ToString()},
                                         {"transitions", nlohmann::json::array({transition})}};
        const std::vector<std::pair<std::string, std::string>> entries{
            {"horo-package.toml", "schemaVersion = 1\n"},
            {"assets/data.bin", "verified bytes"},
            {std::string{Packages::PackageAssetRenameDeclarationPath}, declaration.dump()}};
        const auto carrier = Packages::ValidatedPackageArchive::Verify(Tests::Packages::PackageArchiveBytes(entries));
        REQUIRE(carrier.HasValue());
        NativeDurableFileSystem files;
        auto cacheResult = Packages::PackageCacheStore::Create(files, cacheRoot);
        REQUIRE(cacheResult.HasValue());
        auto cache = std::move(cacheResult).Value();
        const auto preview = MakePackageUpdatePreviewCommandAdapter();
        const auto apply = MakePackageApplyUpdateCommandAdapter();
        const CliCommandRequest command{.command = preview.adapter->GetDescriptor().path,
                                        .options = {NativeProject::PathOption("project", root),
                                                    NativeProject::PathOption("candidate-request", metadata / "candidate.json"),
                                                    NativeProject::PathOption("candidate-lock", metadata / "candidate.lock")}};
        const auto cacheOption = NativeProject::PathOption("package-cache-root", cacheRoot);
        const std::array<const Packages::ValidatedPackageArchive *, 2> archives{&carrier.Value(), &fixture.graph.archive};
        REQUIRE(cache.Publish(fixture.graph.archive).HasValue());
        REQUIRE(cache.Publish(carrier.Value()).HasValue());
        for (const auto *archive : archives) {
            WriteAssetCandidate(fixture, version, *archive);
            InInvocation([&](CliExecutionContext &context) {
                auto reviewed = command;
                reviewed.options.push_back(cacheOption);
                REQUIRE(preview.adapter->Execute(reviewed, context).HasValue());
                const auto approval = preview.adapter->Execute(command, context);
                REQUIRE(approval.HasValue());
                const auto approved = std::get<std::string>(approval.Value().fields.back().value);
                reviewed.command = apply.adapter->GetDescriptor().path;
                reviewed.options.push_back({"approve", {{approved, CliValueSource::Invocation, {}}}});
                REQUIRE(apply.adapter->Execute(reviewed, context).HasValue());
            });
            const auto installed = files.ReadFileBounded(metadata / "packages.lock", 1U << 20U, {});
            const auto candidate = files.ReadFileBounded(metadata / "candidate.lock", 1U << 20U, {});
            REQUIRE(installed.HasValue());
            REQUIRE(candidate.HasValue());
            CHECK(installed.Value() == candidate.Value());
        }
    }

    TEST_CASE("Cached update previews bind asset rename declarations to the exact previous artifact",
              "[packages][application][cli][update]") {
        NativeProject fixture;
        const auto root = std::filesystem::canonical(fixture.project.Path());
        const auto cacheRoot = std::filesystem::canonical(fixture.storage.Path()) / "cache";
        enum class Source { Verified, ChangedBytes, Stale, OutsideAssets, MissingTarget, Duplicate };
        const auto mode = GENERATE(Source::Verified, Source::ChangedBytes, Source::Stale, Source::OutsideAssets, Source::MissingTarget,
                                   Source::Duplicate);
        const auto id = fixture.graph.package.Value();
        const auto previousDigest = FormatSha256(fixture.graph.archive.Digest());
        const auto version = Packages::PackageVersion::Parse("2.0.0").Value();
        nlohmann::json transition{{"fromVersion", "1.0.0"},
                                   {"fromArtifactSha256", mode == Source::Stale ? FormatSha256(Sha256Digest{}) : previousDigest},
                                   {"renames", {{{"from", mode == Source::OutsideAssets ? "../data.bin" : "assets/data.bin"},
                                                  {"to", mode == Source::MissingTarget ? "assets/not-in-archive.bin" : "assets/moved.bin"}}}}};
        nlohmann::json transitions = nlohmann::json::array({transition});
        if (mode == Source::Duplicate)
            transitions.push_back(transition);
        const nlohmann::json declaration{{"schemaVersion", 1}, {"packageId", id}, {"toVersion", version.ToString()},
                                         {"transitions", std::move(transitions)}};
        const std::vector<std::pair<std::string, std::string>> entries{
            {"horo-package.toml", "schemaVersion = 1\n"},
            {"assets/moved.bin", mode == Source::ChangedBytes ? "replaced bytes" : "verified bytes"},
            {std::string{Packages::PackageAssetRenameDeclarationPath}, declaration.dump()}};
        const auto archive = Packages::ValidatedPackageArchive::Verify(Tests::Packages::PackageArchiveBytes(entries));
        REQUIRE(archive.HasValue());
        WriteAssetCandidate(fixture, version, archive.Value());
        NativeDurableFileSystem files;
        auto cache = Packages::PackageCacheStore::Create(files, cacheRoot);
        REQUIRE(cache.HasValue());
        auto published = std::move(cache).Value();
        REQUIRE(published.Publish(fixture.graph.archive).HasValue());
        REQUIRE(published.Publish(archive.Value()).HasValue());
        const auto preview = MakePackageUpdatePreviewCommandAdapter();
        const auto metadata = root / ".horo";
        const auto unchanged = files.ReadFileBounded(metadata / "packages.lock", 1U << 20U, {});
        REQUIRE(unchanged.HasValue());
        const CliCommandRequest command{.command = preview.adapter->GetDescriptor().path,
                                        .options = {NativeProject::PathOption("project", root),
                                                    NativeProject::PathOption("candidate-request", metadata / "candidate.json"),
                                                    NativeProject::PathOption("candidate-lock", metadata / "candidate.lock"),
                                                    NativeProject::PathOption("package-cache-root", cacheRoot)}};
        InInvocation([&](CliExecutionContext &context) {
            const auto result = preview.adapter->Execute(command, context);
            REQUIRE(result.HasError());
            const auto &error = result.ErrorValue();
            if (mode == Source::OutsideAssets) {
                CHECK(error.code.Value() == "packages.migration.invalid_path");
            } else if (mode == Source::MissingTarget) {
                CHECK(error.code.Value() == "packages.migration.archive_paths_mismatch");
            } else if (mode == Source::Duplicate) {
                CHECK(error.code.Value() == "packages.migration.duplicate_transition");
            } else {
                CHECK(error.code.Value() == "packages.host.update_requires_migration_review");
                const auto proposed = std::ranges::find_if(error.diagnostics, [](const auto &diagnostic) {
                    return diagnostic.code.Value().starts_with("packages.host.update_declared_asset_rename");
                });
                if (mode == Source::Stale) {
                    CHECK(proposed == error.diagnostics.end());
                } else {
                    REQUIRE(proposed != error.diagnostics.end());
                    CHECK(proposed->path == "assets/data.bin");
                    CHECK(proposed->message.ends_with("assets/moved.bin"));
                    CHECK(proposed->code.Value().ends_with("_changes_bytes") == (mode == Source::ChangedBytes));
                }
            }
        });
        const auto after = files.ReadFileBounded(metadata / "packages.lock", 1U << 20U, {});
        REQUIRE(after.HasValue());
        CHECK(after.Value() == unchanged.Value());
    }

    TEST_CASE("Cached updates preview saved behavior rename impact", "[packages][application][cli][update]") {
        NativeProject fixture;
        constexpr std::string_view oldType = "game.tests.legacy_behavior";
        constexpr std::string_view newType = "game.tests.current_behavior";
        SceneSource::SceneObjectSnapshot object{.id = {7}, .name = "Actor"};
        object.components.behaviors = {{.instanceId = {3}, .typeId = Gameplay::BehaviorTypeId::Parse(oldType).Value(), .enabled = false}};
        fixture.WriteScene("Migration.horoasset", "10213243-5465-7687-98a9-bacbdcedfe0e",
                           SceneSource::EncodeSceneSource({.objects = std::span{&object, 1}}));
        const bool applies = GENERATE(true, false);
        const auto source = applies ? fixture.graph.archive.Digest() : Sha256Digest{};
        const nlohmann::json transition{{"fromVersion", "1.0.0"}, {"fromArtifactSha256", FormatSha256(source)},
                                       {"renames", {{{"from", oldType}, {"to", newType}}}}};
        const nlohmann::json declaration{{"schemaVersion", 1}, {"packageId", fixture.graph.package.Value()}, {"toVersion", "2.0.0"},
                                         {"transitions", nlohmann::json::array({transition})}};
        const std::vector<std::pair<std::string, std::string>> entries{
            {"horo-package.toml", "schemaVersion = 1\n"},
            {"assets/data.bin", "verified bytes"},
            {std::string{Packages::PackageBehaviorRenameDeclarationPath}, declaration.dump()}};
        const auto archive = Packages::ValidatedPackageArchive::Verify(Tests::Packages::PackageArchiveBytes(entries));
        REQUIRE(archive.HasValue());
        WriteAssetCandidate(fixture, Packages::PackageVersion::Parse("2.0.0").Value(), archive.Value());
        const auto root = std::filesystem::canonical(fixture.project.Path());
        const auto cacheRoot = std::filesystem::canonical(fixture.storage.Path()) / "cache";
        NativeDurableFileSystem files;
        auto cacheResult = Packages::PackageCacheStore::Create(files, cacheRoot);
        REQUIRE(cacheResult.HasValue());
        auto cache = std::move(cacheResult).Value();
        REQUIRE(cache.Publish(fixture.graph.archive).HasValue());
        REQUIRE(cache.Publish(archive.Value()).HasValue());
        const auto preview = MakePackageUpdatePreviewCommandAdapter();
        const CliCommandRequest command{.command = preview.adapter->GetDescriptor().path,
                                        .options = {NativeProject::PathOption("project", root),
                                                    NativeProject::PathOption("candidate-request", root / ".horo/candidate.json"),
                                                    NativeProject::PathOption("candidate-lock", root / ".horo/candidate.lock"),
                                                    NativeProject::PathOption("package-cache-root", cacheRoot)}};
        InInvocation([&](CliExecutionContext &context) {
            const auto result = preview.adapter->Execute(command, context);
            REQUIRE(result.HasError());
            const auto &failure = result.ErrorValue();
            CHECK(failure.code.Value() == "packages.host.update_requires_migration_review");
            const auto match = std::ranges::find_if(failure.diagnostics, [](const auto &diagnostic) {
                return diagnostic.code.Value() == "packages.host.update_possible_behavior_rename";
            });
            REQUIRE((match != failure.diagnostics.end()) == applies);
            if (applies) {
                CHECK(match->path == "Assets/Migration.horoasset");
                CHECK(match->message.ends_with("game.tests.legacy_behavior -> game.tests.current_behavior (object 7, attachment 3)"));
            }
        });
    }

    TEST_CASE("Headless package removal previews show when an extension requires a live owner", "[packages][application][cli][remove]") {
        NativeProject fixture{true};
        const auto root = std::filesystem::canonical(fixture.project.Path());
        const auto id = fixture.graph.package.Value();
        auto preview = MakePackageRemovalPreviewCommandAdapter();
        const CliCommandRequest command{.command = preview.adapter->GetDescriptor().path,
                                        .options = {NativeProject::PathOption("project", root),
                                                    {"package-id", {{id, CliValueSource::Invocation, {}}}}}};
        std::filesystem::create_directories(root / "Assets");
        InInvocation([&](CliExecutionContext &context) {
            const auto reviewed = preview.adapter->Execute(command, context);
            REQUIRE(reviewed.HasError());
            CHECK(reviewed.ErrorValue().code.Value() == "packages.host.removal_requires_lifecycle_owner");
            REQUIRE(reviewed.ErrorValue().diagnostics.size() == 1);
            CHECK(reviewed.ErrorValue().diagnostics.front().path == id);

            fixture.WriteImporterReference("sample.horoasset", "174ee050-f0a1-4351-89c4-04a5895bff81");
            const auto blocked = preview.adapter->Execute(command, context);
            REQUIRE(blocked.HasError());
            const auto &diagnostics = blocked.ErrorValue().diagnostics;
            REQUIRE(diagnostics.size() == 2);
            CHECK(diagnostics[0].code.Value() == "packages.host.removal_requires_lifecycle_owner");
            CHECK(diagnostics[1].code.Value() == "packages.host.importer_in_use");
            CHECK(diagnostics[1].path == "Assets/sample.horoasset");
        });
    }

    TEST_CASE("Headless package removal requires exact approval and checks saved references", "[packages][application][cli][remove]") {
        NativeProject fixture;
        const auto root = std::filesystem::canonical(fixture.project.Path());
        const auto assets = root / "Assets";
        std::filesystem::create_directories(assets);
        const auto cacheRoot = std::filesystem::canonical(fixture.storage.Path()) / "cache";
        NativeDurableFileSystem files;
        auto cache = Packages::PackageCacheStore::Create(files, cacheRoot);
        REQUIRE(cache.HasValue());
        auto verifiedCache = std::move(cache).Value();
        REQUIRE(verifiedCache.Publish(fixture.graph.archive).HasValue());
        auto preview = MakePackageRemovalPreviewCommandAdapter();
        auto apply = MakePackageApplyRemovalCommandAdapter();
        Tests::CliContract::ValidateSchema("package.preview-remove", preview.adapter->GetDescriptor().output.id,
                                           preview.adapter->GetDescriptor().output.version);
        Tests::CliContract::ValidateSchema("package.remove", apply.adapter->GetDescriptor().output.id,
                                           apply.adapter->GetDescriptor().output.version);
        const auto id = fixture.graph.package.Value();
        CliCommandRequest command{.command = preview.adapter->GetDescriptor().path,
                                  .options = {NativeProject::PathOption("project", root),
                                              {"package-id", {{id, CliValueSource::Invocation, {}}}}}};
        const auto reviewCommand = command;
        InInvocation([&](CliExecutionContext &context) {
            const auto reviewed = preview.adapter->Execute(command, context);
            REQUIRE(reviewed.HasValue());
            CHECK(std::get<std::int64_t>(reviewed.Value().fields[1].value) == 1);
            CHECK(std::get<std::string>(reviewed.Value().fields[3].value).ends_with("] -> [absent]"));
            const auto approval = std::get<std::string>(reviewed.Value().fields.back().value);
            command.command = apply.adapter->GetDescriptor().path;
            command.options.push_back(NativeProject::PathOption("package-cache-root", cacheRoot));
            command.options.push_back({"approve", {{std::string{"unreviewed"}, CliValueSource::Invocation, {}}}});
            const auto wrongApproval = apply.adapter->Execute(command, context);
            REQUIRE(wrongApproval.HasError());
            CHECK(wrongApproval.ErrorValue().code.Value() == "packages.host.update_approval_mismatch");
            command.options.back().values.front().value = approval;
            fixture.WriteImporterReference("sample.horoasset", "174ee050-f0a1-4351-89c4-04a5895bff81");
            fixture.WriteImporterReference("second.horoasset", "174ee050-f0a1-4351-89c4-04a5895bff82");
            const auto blockedReview = preview.adapter->Execute(reviewCommand, context);
            REQUIRE(blockedReview.HasError());
            CHECK(blockedReview.ErrorValue().code.Value() == "packages.host.importer_in_use");
            const auto &references = blockedReview.ErrorValue().diagnostics;
            REQUIRE(references.size() == 2);
            CHECK(references[0].path.ends_with("sample.horoasset"));
            CHECK(references[1].path.ends_with("second.horoasset"));
            const auto blocked = apply.adapter->Execute(command, context);
            REQUIRE(blocked.HasError());
            CHECK(blocked.ErrorValue().code.Value() == "packages.host.importer_in_use");
            CHECK(blocked.ErrorValue().diagnostics.size() == references.size());
            for (const auto name : {"sample.horoasset", "sample.horoasset.horo", "second.horoasset", "second.horoasset.horo"})
                REQUIRE(std::filesystem::remove(assets / name));
            const auto removed = apply.adapter->Execute(command, context);
            REQUIRE(removed.HasValue());
            CHECK_FALSE(std::get<bool>(removed.Value().fields.front().value));
            CHECK(preview.adapter->Execute({.options = {command.options[0], command.options[1]}}, context).HasError());
        });
    }

    TEST_CASE("Concrete package CLI host rejects malformed typed paths before constructing a project", "[packages][application][cli]") {
        NativeProject fixture;
        auto request = fixture.Request();
        SECTION("missing project") {
            request.options.erase(request.options.begin());
        }
        SECTION("missing cache") {
            request.options.erase(request.options.begin() + 1);
        }
        SECTION("missing temporary") {
            request.options.pop_back();
        }
        SECTION("wrong typed value") {
            request.options.front().values.front().value = std::string{"not a typed path"};
        }
        SECTION("multiple values") {
            request.options.front().values.push_back(request.options.front().values.front());
        }
        SECTION("relative") {
            request.options.front().values.front().value = CliPathValue{"relative/project"};
        }
        SECTION("not normalized") {
            request.options.front().values.front().value =
                CliPathValue{std::filesystem::canonical(fixture.project.Path()).string() + "/../project"};
        }
        PackageCliHost host;
        InInvocation([&](CliExecutionContext &context) {
            auto result = host.Bind(request, context);
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == CliErrors::ParseFailed.code.Value());
        });
        CHECK(host.Project() == nullptr);
        REQUIRE(host.Boundary().HasError());
    }

    TEST_CASE("Concrete package CLI binding retains its exact project and rejects path rebinding", "[packages][application][cli]") {
        NativeProject fixture;
        const auto request = fixture.Request();
        PackageCliHost host;
        InInvocation([&](CliExecutionContext &context) {
            auto first = host.Bind(request, context);
            REQUIRE(first.HasValue());
            REQUIRE(host.Project() != nullptr);
            const auto generation = host.Project()->Generation();
            REQUIRE(generation > 0);
            auto repeated = host.Bind(request, context);
            REQUIRE(repeated.HasValue());
            CHECK(first.Value() == repeated.Value());
            CHECK(host.Boundary().Value() == Packages::PackageActivationBoundary::Quiescent);
            for (std::size_t index = 0; index < request.options.size(); ++index) {
                auto different = request;
                std::get<CliPathValue>(different.options[index].values.front().value).value += "/another";
                auto rejected = host.Bind(different, context);
                REQUIRE(rejected.HasError());
                CHECK(rejected.ErrorValue().code.Value() == CliErrors::ExecutionContextInvalid.code.Value());
                CHECK(host.Project()->Generation() == generation);
            }
        });
    }

    TEST_CASE("Concrete package CLI host observes admitted cancellation and deadline before metadata admission",
              "[packages][application][cli]") {
        NativeProject fixture;
        CancellationSource stop;
        PackageCliHost host;
        SECTION("cooperative cancellation") {
            InInvocation([&](CliExecutionContext &context) {
                stop.RequestCancellation();
                auto result = host.Bind(fixture.Request(), context);
                REQUIRE(result.HasError());
                CHECK(result.ErrorValue().code.Value() == CliErrors::ExecutionCancelled.code.Value());
            }, stop.Token());
        }
        SECTION("finite deadline") {
            InInvocation([&](CliExecutionContext &context) {
                const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds{1};
                while (!context.IsStopRequested() && std::chrono::steady_clock::now() < limit)
                    std::this_thread::yield();
                REQUIRE(context.IsStopRequested());
                auto result = host.Bind(fixture.Request(), context);
                REQUIRE(result.HasError());
                CHECK(result.ErrorValue().code.Value() == CliErrors::ExecutionTimedOut.code.Value());
            }, {}, 1);
        }
        REQUIRE(host.Project() != nullptr);
        CHECK(host.Project()->Generation() == 0);
    }

    TEST_CASE("Concrete MCP package binding exposes safe generation identity and retains closed application leases",
              "[packages][application][cli][mcp]") {
        NativeProject fixture;
        std::shared_ptr<PackageApplication> retained;
        {
            PackageCliHost host;
            InInvocation([&](CliExecutionContext &context) {
                auto bound = host.BindMcp(fixture.Request(), context);
                REQUIRE(bound.HasValue());
                auto binding = std::move(bound).Value();
                retained = binding.application;
                CHECK(binding.projectIdentity.starts_with("horo.project."));
                CHECK(binding.projectIdentity.find(fixture.project.Path().string()) == std::string::npos);
                REQUIRE(binding.generation > 0);
                CHECK(binding.currentGeneration() == binding.generation);
                REQUIRE(binding.pump().HasValue());
                auto again = host.BindMcp(fixture.Request(), context);
                REQUIRE(again.HasValue());
                CHECK(again.Value().projectIdentity == binding.projectIdentity);
                REQUIRE(host.Project()->Retire(std::filesystem::canonical(fixture.project.Path())).HasValue());
                CHECK(binding.currentGeneration() > binding.generation);
                CHECK(retained->Restore().HasError());
            });
        }
        REQUIRE(retained);
        CHECK(retained->Install().HasError());
        REQUIRE(retained->Close().HasValue());
    }
}  // namespace
