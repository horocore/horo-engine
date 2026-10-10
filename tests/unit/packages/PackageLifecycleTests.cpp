#include "Horo/Packages/PackageLifecycle.h"
#include "Horo/Packages/PackageLifecycleErrors.h"
#include "PackageArchiveTestSupport.h"
#include "SecurityTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <iterator>
#include <thread>

using namespace Horo;
using namespace Horo::Packages;

namespace {
    template <class T> T Parse(std::string_view text) {
        auto result = T::Parse(text);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    std::string FileBytes(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        REQUIRE(input.good());
        return {std::istreambuf_iterator<char>{input}, {}};
    }

    std::shared_ptr<PackageRestoreGraph> Graph(const std::filesystem::path &library, bool declaredArtifact = true, bool declaredRoot = true,
                                               std::string packageId = "com.example.transaction") {
        const std::string root = "extensions/native";
        const std::string nativeName = "module" + library.extension().string();
        auto descriptorJson =
            nlohmann::json{{"id", packageId},
                           {"version", "1.0.0"},
                           {"modules",
                            {{{"id", packageId + ".native"},
                              {"version", "1.0.0"},
                              {"kind", "native"},
                              {"roles", {"backend-capability"}},
                              {"entry", nativeName}}}},
                           {"contributions",
                            {{{"type", "asset.importer"}, {"id", "fixture.importer"}, {"module", packageId + ".native"}}}}};
        if (library == std::filesystem::path{HORO_PACKAGE_EMPTY_FIXTURE})
            descriptorJson["contributions"] = nlohmann::json::array();
        const auto descriptor = descriptorJson.dump();
        std::vector<std::pair<std::string, std::string>> files{{"horo-package.toml", "schemaVersion = 1\n"},
                                                               {root + "/extension.json", descriptor}};
        if (declaredArtifact)
            files.emplace_back(root + '/' + nativeName, FileBytes(library));
        nlohmann::json inventory = nlohmann::json::array();
        for (const auto &[path, bytes] : files) {
            auto entry = Tests::Packages::FileInventoryEntry(path, bytes);
            if (path.starts_with(root + '/'))
                entry["contributionRoot"] = declaredRoot ? nlohmann::json(root) : nlohmann::json{};
            inventory.push_back(std::move(entry));
        }
        const auto manifest = nlohmann::json{{"schemaVersion", 1}, {"files", inventory}}.dump();
        mz_zip_archive zip{};
        REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0));
        for (const auto &[path, bytes] : files)
            REQUIRE(mz_zip_writer_add_mem(&zip, path.c_str(), bytes.data(), bytes.size(), MZ_BEST_COMPRESSION));
        REQUIRE(mz_zip_writer_add_mem(&zip, "files.manifest.json", manifest.data(), manifest.size(), MZ_BEST_COMPRESSION));
        auto verified = ValidatedPackageArchive::Verify(Tests::Packages::FinalizeArchive(zip));
        REQUIRE(verified.HasValue());
        auto archive = std::make_shared<const ValidatedPackageArchive>(std::move(verified).Value());
        LockedPackage lock{.package = Parse<HoroPackageId>(packageId),
                           .version = Parse<PackageVersion>("1.0.0"),
                           .source = Parse<HoroPackageSourceId>("fixture.source"),
                           .artifactDigest = archive->Digest(),
                           .manifestDigest = archive->PackageManifestDigest(),
                           .fileManifestDigest = archive->Manifest().Digest(),
                           .contributions = {"native"}};
        return std::make_shared<PackageRestoreGraph>(
            PackageRestoreGraph{{}, {"fixture", "fixture", "fixture"}, {{std::move(lock), archive, false, {}}}});
    }

    class Trust final : public IPackageExtensionTrust {
    public:
        Result<PackageExtensionTrustApproval> Approve(const PackageRestorePackage &package,
                                                      const Extensions::ExtensionManifest &) const override {
            ++calls;
            if (deny)
                return Result<PackageExtensionTrustApproval>::Failure(MakeError(PackageLifecycleErrors::TrustRequired));
            return Result<PackageExtensionTrustApproval>::Success(
                {stale ? Sha256Digest{} : package.lock.artifactDigest, package.lock.manifestDigest, {.revision = 1}});
        }

        mutable unsigned calls{};
        bool deny{};
        bool stale{};
    };

    class TrackedLibrary final : public Platform::DynamicLibrary {
    public:
        TrackedLibrary(std::unique_ptr<Platform::DynamicLibrary> library, std::string id, std::vector<std::string> &retired)
            : library_(std::move(library)), id_(std::move(id)), retired_(retired) {}

        ~TrackedLibrary() override {
            retired_.push_back(id_);
        }

        void *GetSymbol(std::string_view name) const noexcept override {
            return library_->GetSymbol(name);
        }

    private:
        std::unique_ptr<Platform::DynamicLibrary> library_;
        std::string id_;
        std::vector<std::string> &retired_;
    };

    struct Fixture {
        Tests::Packages::TemporaryDirectory project{"horo lifecycle project ü"};
        Tests::Packages::TemporaryDirectory temporary{"horo lifecycle content ü"};
        NativeDurableFileSystem files;
        Trust trust;
        CancellationSource cancellation;
        PackageInstallService install;
        std::unique_ptr<PackageLifecycleService> service;
        std::vector<EnabledPackageExtension> enabled{
            {Parse<HoroPackageId>("com.example.transaction"), Parse<PackagePath>("extensions/native/extension.json")}};
        std::vector<std::string> loadedIds;
        std::vector<std::string> retiredIds;
        unsigned loads{};
        bool failLoad{};
        bool cancelOnLoad{};
        bool replaceInstallOnLoad{};
        bool journalObserved{};
        bool shutdownOnLoad{};

        explicit Fixture(std::size_t maximumRetired = 16U)
            : install(std::move(PackageInstallService::Create(files, std::filesystem::canonical(project.Path()))).Value()) {
            PackageLifecycleConfiguration configuration{.temporaryRoot = std::filesystem::canonical(temporary.Path()),
                                                        .artifactGate = Tests::CreateAcceptingArtifactGate()};
            configuration.maximumRetiredCompositions = maximumRetired;
            configuration.libraryLoader = [this](const std::string &path) -> Result<std::unique_ptr<Platform::DynamicLibrary>> {
                ++loads;
                journalObserved = service->State().outcome == PackageActivationOutcome::Preparing;
                if (replaceInstallOnLoad)
                    REQUIRE(install.Install(Graph(HORO_PACKAGE_NATIVE_FIXTURE), CancellationSource{}.Token()).HasValue());
                if (cancelOnLoad)
                    cancellation.RequestCancellation();
                if (shutdownOnLoad)
                    service->Shutdown();
                if (failLoad)
                    return Result<std::unique_ptr<Platform::DynamicLibrary>>::Failure(MakeError(PackageLifecycleErrors::StorageFailed));
                auto library = Platform::LoadDynamicLibrary(path);
                if (library.HasError())
                    return library;
                const auto descriptor = nlohmann::json::parse(FileBytes(std::filesystem::path{path}.parent_path() / "extension.json"));
                const auto id = descriptor.at("id").get<std::string>();
                loadedIds.push_back(id);
                return Result<std::unique_ptr<Platform::DynamicLibrary>>::Success(
                    std::make_unique<TrackedLibrary>(std::move(library).Value(), id, retiredIds));
            };
            auto created = PackageLifecycleService::Create(install, trust, std::move(configuration));
            REQUIRE(created.HasValue());
            service = std::move(created).Value();
        }

        ~Fixture() {
            service.reset();
        }

        void Install(std::shared_ptr<PackageRestoreGraph> graph = Graph(HORO_PACKAGE_NATIVE_FIXTURE)) {
            REQUIRE(install.Install(std::move(graph), CancellationSource{}.Token()).HasValue());
        }

        Result<void> Activate() {
            auto result = service->Activate(enabled, PackageActivationBoundary::Quiescent, cancellation.Token());
            if (result.HasError()) {
                for (const Error *error = &result.ErrorValue(); error; error = error->cause.Get())
                    UNSCOPED_INFO("Package activation domain=" << error->domain.Value() << " code=" << error->code.Value()
                                                               << " detail=" << error->message);
            }
            return result;
        }
    };

    /** @brief Exercises owner-lane, runtime and cancellation rejection before any native preparation. */
    void CheckRuntimeBoundaryRejection(Fixture &fixture) {
        SECTION("running host") {
            fixture.Install();
            CHECK(fixture.service->Activate(fixture.enabled, PackageActivationBoundary::Running, fixture.cancellation.Token()).HasError());
        }
        SECTION("off owner lane") {
            fixture.Install();
            std::thread worker{[&fixture] {
                CHECK(fixture.Activate().HasError());
            }};
            worker.join();
        }
        SECTION("cancelled before load") {
            fixture.Install();
            fixture.cancellation.RequestCancellation();
            CHECK(fixture.Activate().HasError());
            CHECK(fixture.service->State().outcome == PackageActivationOutcome::Cancelled);
        }
    }
}  // namespace

TEST_CASE("Package lifecycle activates the exact installed native module and publishes real registrations and leases",
          "[packages][activation]") {
    Fixture fixture;
    auto graph = Graph(HORO_PACKAGE_NATIVE_FIXTURE);
    fixture.Install(graph);
    const auto installed = fixture.install.InstalledRecord();
    graph->packages.clear();  // A retained mutable restore alias cannot rewrite sealed install evidence.
    REQUIRE(installed->Graph()->packages.size() == 1U);
    CHECK_FALSE(fixture.service->Active());
    REQUIRE(fixture.Activate().HasValue());
    auto active = fixture.service->Active();
    REQUIRE(active);
    CHECK(active->install == installed);
    CHECK(active->generation == 1U);
    REQUIRE(active->activations.size() == 1U);
    CHECK(active->activations.front().IsUsable());
    CHECK(active->activations.front().Activation().ExtensionId() == "com.example.transaction");
    CHECK(active->activations.front().Activation().ModuleId() == "com.example.transaction.native");
    CHECK(active->activations.front().Activation().Generation() == active->generation);
    REQUIRE(active->importers);
    REQUIRE(active->importers->FindById("fixture.importer"));
    CHECK(fixture.loads == 1U);
    CHECK(fixture.journalObserved);
    fixture.service->Shutdown();
    CHECK_FALSE(fixture.service->Active());
    CHECK_FALSE(active->activations.front().IsUsable());
    CHECK(fixture.service->State().restartRequired);
    active.reset();
    fixture.service->FinalizeRetirements();
    fixture.service->Shutdown();
    CHECK(fixture.Activate().HasError());
}

TEST_CASE("Package lifecycle fails closed before native loading for missing enablement evidence trust and runtime boundaries",
          "[packages][activation]") {
    Fixture fixture;
    SECTION("not installed") {
        CHECK(fixture.Activate().HasError());
    }
    SECTION("untrusted") {
        fixture.Install();
        fixture.trust.deny = true;
        CHECK(fixture.Activate().HasError());
    }
    SECTION("stale digest approval") {
        fixture.Install();
        fixture.trust.stale = true;
        CHECK(fixture.Activate().HasError());
    }
    SECTION("undeclared binary") {
        fixture.Install(Graph(HORO_PACKAGE_NATIVE_FIXTURE, false));
        CHECK(fixture.Activate().HasError());
    }
    SECTION("undeclared root") {
        fixture.Install(Graph(HORO_PACKAGE_NATIVE_FIXTURE, true, false));
        CHECK(fixture.Activate().HasError());
    }
    SECTION("duplicate selection") {
        fixture.Install();
        fixture.enabled.push_back(fixture.enabled.front());
        CHECK(fixture.Activate().HasError());
    }
    SECTION("excess selection") {
        fixture.Install();
        fixture.enabled.assign(65U, fixture.enabled.front());
        CHECK(fixture.Activate().HasError());
    }
    CheckRuntimeBoundaryRejection(fixture);
    CHECK(fixture.loads == 0U);
    CHECK_FALSE(fixture.service->Active());
    CHECK(std::filesystem::is_empty(fixture.temporary.Path()));
}

TEST_CASE("Package lifecycle preserves last good native graph through host failure cancellation and changed install generations",
          "[packages][activation]") {
    Fixture fixture;
    fixture.Install();
    REQUIRE(fixture.Activate().HasValue());
    const auto previous = fixture.service->Active();
    SECTION("host loader failure") {
        fixture.failLoad = true;
    }
    SECTION("native initialization failure") {
        fixture.Install(Graph(HORO_PACKAGE_FAILING_FIXTURE));
    }
    SECTION("cancellation during native staging") {
        fixture.cancelOnLoad = true;
    }
    SECTION("installation changes during staging") {
        fixture.replaceInstallOnLoad = true;
    }
    auto result = fixture.Activate();
    REQUIRE(result.HasError());
    CHECK(fixture.service->Active() == previous);
    CHECK(previous->activations.front().IsUsable());
    CHECK(previous->importers->FindById("fixture.importer"));
    CHECK(fixture.service->State().diagnostic.has_value());
    CHECK(fixture.service->State().attemptedGeneration == 2U);
    CHECK(std::distance(std::filesystem::directory_iterator(fixture.temporary.Path()), {}) == 1);
    fixture.failLoad = false;
    fixture.cancelOnLoad = false;
    fixture.replaceInstallOnLoad = false;
    fixture.cancellation = CancellationSource{};
    fixture.Install();
    REQUIRE(fixture.Activate().HasValue());
    CHECK(fixture.service->Active()->generation == 3U);
    CHECK_FALSE(previous->activations.front().IsUsable());
}

TEST_CASE("Package lifecycle empty enablement remains inactive and never loads or approves code", "[packages][activation]") {
    Fixture fixture;
    fixture.Install();
    fixture.enabled.clear();
    REQUIRE(fixture.Activate().HasValue());
    CHECK(fixture.service->Active()->activations.empty());
    CHECK(fixture.loads == 0U);
    CHECK(fixture.trust.calls == 0U);
}

TEST_CASE("Package lifecycle orders actual native providers before dependants and retires in reverse order", "[packages][activation]") {
    Fixture fixture;
    auto graph = Graph(HORO_PACKAGE_EMPTY_FIXTURE, true, true, "com.example.dependent");
    auto provider = Graph(HORO_PACKAGE_EMPTY_FIXTURE, true, true, "com.example.provider");
    graph->packages.front().lock.dependencies.push_back({provider->packages.front().lock.package, provider->packages.front().lock.version});
    graph->packages.push_back(provider->packages.front());
    fixture.Install(graph);
    fixture.enabled = {{Parse<HoroPackageId>("com.example.dependent"), Parse<PackagePath>("extensions/native/extension.json")},
                       {Parse<HoroPackageId>("com.example.provider"), Parse<PackagePath>("extensions/native/extension.json")}};
    REQUIRE(fixture.Activate().HasValue());
    CHECK(fixture.loadedIds == std::vector<std::string>{"com.example.provider", "com.example.dependent"});
    REQUIRE(fixture.service->Active()->activations.size() == 2U);
    fixture.service->Shutdown();
    CHECK(fixture.retiredIds == std::vector<std::string>{"com.example.dependent", "com.example.provider"});
    CHECK(std::filesystem::is_empty(fixture.temporary.Path()));
}

TEST_CASE("Package lifecycle rejects missing and cyclic native dependency plans before loading", "[packages][activation]") {
    Fixture fixture;
    auto graph = Graph(HORO_PACKAGE_EMPTY_FIXTURE, true, true, "com.example.dependent");
    auto provider = Graph(HORO_PACKAGE_EMPTY_FIXTURE, true, true, "com.example.provider");
    graph->packages.front().lock.dependencies.push_back({provider->packages.front().lock.package, provider->packages.front().lock.version});
    SECTION("missing dependency") {}
    SECTION("cyclic dependency") {
        provider->packages.front().lock.dependencies.push_back(
            {graph->packages.front().lock.package, graph->packages.front().lock.version});
        graph->packages.push_back(provider->packages.front());
        fixture.enabled.push_back({Parse<HoroPackageId>("com.example.provider"), Parse<PackagePath>("extensions/native/extension.json")});
    }
    fixture.enabled.front().package = Parse<HoroPackageId>("com.example.dependent");
    fixture.Install(graph);
    CHECK(fixture.Activate().HasError());
    CHECK(fixture.loads == 0U);
}

TEST_CASE("Package lifecycle generation and file storage survive retained old catalogs until drain", "[packages][activation]") {
    Fixture fixture;
    fixture.Install();
    REQUIRE(fixture.Activate().HasValue());
    auto previous = fixture.service->Active();
    REQUIRE(fixture.Activate().HasValue());
    CHECK_FALSE(previous->activations.front().IsUsable());
    CHECK(fixture.retiredIds.empty());
    CHECK(std::distance(std::filesystem::directory_iterator(fixture.temporary.Path()), {}) == 2);
    previous.reset();
    fixture.service->FinalizeRetirements();
    CHECK(fixture.retiredIds.size() == 1U);
    CHECK(std::distance(std::filesystem::directory_iterator(fixture.temporary.Path()), {}) == 1);
    fixture.service->Shutdown();
    CHECK(fixture.retiredIds.size() == 2U);
}

TEST_CASE("Package lifecycle shutdown during native preparation cannot publish or resurrect a graph", "[packages][activation]") {
    Fixture fixture;
    fixture.Install();
    std::shared_ptr<const PackageActivationSnapshot> previous;
    SECTION("initial activation") {}
    SECTION("replacement revokes the previous published graph") {
        REQUIRE(fixture.Activate().HasValue());
        previous = fixture.service->Active();
    }
    fixture.shutdownOnLoad = true;
    const auto result = fixture.Activate();
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == PackageLifecycleErrors::Cancelled.code.Value());
    CHECK_FALSE(fixture.service->Active());
    CHECK(fixture.service->State().outcome == PackageActivationOutcome::Closed);
    if (previous) {
        CHECK_FALSE(previous->activations.front().IsUsable());
        CHECK(fixture.service->State().restartRequired);
        previous.reset();
        fixture.service->FinalizeRetirements();
    }
    CHECK_FALSE(fixture.service->State().restartRequired);
    CHECK(std::filesystem::is_empty(fixture.temporary.Path()));
    fixture.shutdownOnLoad = false;
    CHECK(fixture.Activate().HasError());
}

TEST_CASE("Package lifecycle storage failure keeps prior activated graph and typed failure evidence", "[packages][activation]") {
    Fixture fixture;
    fixture.Install();
    REQUIRE(fixture.Activate().HasValue());
    const auto previous = fixture.service->Active();
    const auto unavailable = fixture.temporary.Path().string() + "-unavailable";
    std::filesystem::rename(fixture.temporary.Path(), unavailable);
    const auto result = fixture.Activate();
    std::filesystem::rename(unavailable, fixture.temporary.Path());
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == PackageLifecycleErrors::StorageFailed.code.Value());
    CHECK(fixture.service->Active() == previous);
    CHECK(previous->activations.front().IsUsable());
    CHECK(fixture.loads == 1U);
}

TEST_CASE("Package lifecycle quarantines failed native teardown and bounds rejected composition owners", "[packages][activation]") {
    Fixture fixture{1U};
    fixture.Install();
    REQUIRE(fixture.Activate().HasValue());
    const auto previous = fixture.service->Active();
    fixture.Install(Graph(HORO_PACKAGE_TEARDOWN_FAILING_FIXTURE));
    REQUIRE(fixture.Activate().HasError());
    CHECK(fixture.service->Active() == previous);
    CHECK(previous->activations.front().IsUsable());
    CHECK(previous->importers->FindById("fixture.importer"));
    CHECK(fixture.loads == 2U);
    CHECK(fixture.retiredIds.empty());
    CHECK(fixture.service->State().restartRequired);
    CHECK(std::distance(std::filesystem::directory_iterator(fixture.temporary.Path()), {}) == 2);
    fixture.service->FinalizeRetirements();
    CHECK(fixture.service->State().restartRequired);
    const auto rejected = fixture.Activate();
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == PackageLifecycleErrors::InvalidLifecycle.code.Value());
    CHECK(fixture.loads == 2U);
    CHECK(std::distance(std::filesystem::directory_iterator(fixture.temporary.Path()), {}) == 2);
    CHECK(fixture.service->Active() == previous);
}
