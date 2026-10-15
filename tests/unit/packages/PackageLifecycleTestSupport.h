#pragma once
/** @file PackageLifecycleTestSupport.h @brief Shared verified-archive and concrete native lifecycle fixture ownership. */
#include "Horo/Extensions/EditorActivityHost.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Packages/NativePackageModuleHost.h"
#include "Horo/Packages/PackageLifecycle.h"
#include "Horo/Packages/PackageLifecycleErrors.h"
#include "Horo/Packages/ScriptPackageModuleHost.h"
#include "PackageArchiveTestSupport.h"
#include "SecurityTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <thread>

using namespace Horo;
using namespace Horo::Packages;

namespace Horo::Packages::Tests::Support {

    /** @brief Emits bounded test-only phase evidence without using Catch2 reporting
     * from a worker. */
    inline void LifecycleProgress(const char *stage) {
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        std::fprintf(stderr, "[lifecycle progress %.6f] %s\n", seconds, stage);
        std::fflush(stderr);
    }

    /** @brief Reports fixture construction and complete teardown without changing member lifetime order. */
    class LifecycleProgressScope final {
    public:
        LifecycleProgressScope() {
            LifecycleProgress("before fixture construction");
            std::error_code error;
            const auto bytes = std::filesystem::file_size(HORO_PACKAGE_NATIVE_FIXTURE, error);
            std::fprintf(stderr, "[lifecycle fixture] bytes=%llu error=%d\n", static_cast<unsigned long long>(bytes), error.value());
            std::fflush(stderr);
        }

        LifecycleProgressScope(const LifecycleProgressScope &) = delete;
        LifecycleProgressScope &operator=(const LifecycleProgressScope &) = delete;
        LifecycleProgressScope(LifecycleProgressScope &&) = delete;
        LifecycleProgressScope &operator=(LifecycleProgressScope &&) = delete;

        ~LifecycleProgressScope() {
            LifecycleProgress("after fixture teardown");
        }
    };

    template <class T> T Parse(std::string_view text) {
        auto result = T::Parse(text);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    inline std::string FileBytes(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        REQUIRE(input.good());
        return {std::istreambuf_iterator<char>{input}, {}};
    }

    inline nlohmann::json NativeDescriptor(const std::string &packageId, const std::string &nativeName, const bool emptyModule) {
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
        if (emptyModule)
            descriptorJson["contributions"] = nlohmann::json::array();
        return descriptorJson;
    }

    inline std::shared_ptr<PackageRestoreGraph> Graph(const std::filesystem::path &library, bool declaredArtifact = true,
                                                      bool declaredRoot = true, std::string packageId = "com.example.transaction",
                                                      nlohmann::json customDescriptor = {},
                                                      std::vector<std::pair<std::string, std::string>> extraFiles = {}) {
        const std::string root = "extensions/native";
        const std::string nativeName = "module" + library.extension().string();
        auto descriptorJson = NativeDescriptor(packageId, nativeName, library == std::filesystem::path{HORO_PACKAGE_EMPTY_FIXTURE});
        if (!customDescriptor.is_null())
            descriptorJson = std::move(customDescriptor);
        const auto descriptor = descriptorJson.dump();
        std::vector<std::pair<std::string, std::string>> files{{"horo-package.toml", "schemaVersion = 1\n"},
                                                               {root + "/extension.json", descriptor}};
        if (declaredArtifact)
            files.emplace_back(root + '/' + nativeName, FileBytes(library));
        files.insert(files.end(), std::make_move_iterator(extraFiles.begin()), std::make_move_iterator(extraFiles.end()));
        nlohmann::json inventory = nlohmann::json::array();
        for (const auto &[path, bytes] : files) {
            auto entry = ::Horo::Tests::Packages::FileInventoryEntry(path, bytes);
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
        auto verified = ValidatedPackageArchive::Verify(::Horo::Tests::Packages::FinalizeArchive(zip));
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
            return Result<PackageExtensionTrustApproval>::Success({stale ? Sha256Digest{} : package.lock.artifactDigest,
                                                                   package.lock.manifestDigest,
                                                                   {.revision = 1,
                                                                    .knownPermissions = permissions,
                                                                    .approvedPermissions = approvals,
                                                                    .availableCapabilities = capabilities}});
        }

        std::vector<Extensions::ExtensionCapabilityId> capabilities;
        std::vector<Extensions::ExtensionPermissionId> permissions;
        std::vector<Extensions::ExtensionPermissionId> approvals;
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
        ::Horo::Tests::Packages::TemporaryDirectory project{"horo lifecycle project ü"};
        ::Horo::Tests::Packages::TemporaryDirectory temporary{"horo lifecycle content ü"};
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
        bool progress{};

        explicit Fixture(std::size_t maximumRetired = 16U)
            : install(std::move(PackageInstallService::Create(files, std::filesystem::canonical(project.Path()))).Value()) {
            PackageLifecycleConfiguration configuration{.temporaryRoot = std::filesystem::canonical(temporary.Path()),
                                                        .artifactGate = ::Horo::Tests::CreateAcceptingArtifactGate()};
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
            if (progress)
                LifecycleProgress("before service destruction");
            service.reset();
            if (progress)
                LifecycleProgress("after service destruction");
        }

        void Install(std::shared_ptr<PackageRestoreGraph> graph = Graph(HORO_PACKAGE_NATIVE_FIXTURE)) {
            if (progress)
                LifecycleProgress("graph built; before install");
            const auto installed = install.Install(std::move(graph), CancellationSource{}.Token());
            if (installed.HasError())
                for (const Error *error = &installed.ErrorValue(); error; error = error->cause.Get())
                    UNSCOPED_INFO("Package install domain=" << error->domain.Value() << " code=" << error->code.Value()
                                                            << " detail=" << error->message);
            REQUIRE(installed.HasValue());
            if (progress)
                LifecycleProgress("after install");
        }

        Result<void> Activate() {
            if (progress)
                LifecycleProgress("before owner activation");
            auto result = service->Activate(enabled, PackageActivationBoundary::Quiescent, cancellation.Token());
            if (progress)
                LifecycleProgress("after owner activation");
            if (result.HasError()) {
                for (const Error *error = &result.ErrorValue(); error; error = error->cause.Get())
                    UNSCOPED_INFO("Package activation domain=" << error->domain.Value() << " code=" << error->code.Value()
                                                               << " detail=" << error->message);
            }
            return result;
        }
    };

}  // namespace Horo::Packages::Tests::Support
