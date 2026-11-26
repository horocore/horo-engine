#include "Horo/Packages/PackageInstall.h"

#include "Horo/Packages/PackageInstallErrors.h"

#include <algorithm>
#include <format>
#include <limits>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::Packages {
    namespace {
        constexpr std::size_t MaximumInstalledPackages = 4096U;

        /** @brief Separates the exact installed package selector from a portable inventory path. */
        struct MountedAssetReference final {
            HoroPackageId package;
            std::optional<PackageVersion> version;
            PackagePath path;
        };

        /** @brief Parses mounted and explicitly pinned references without URL decoding or authority fallback. */
        Result<MountedAssetReference> ParseMountedAsset(std::string_view reference) {
            using Output = Result<MountedAssetReference>;
            constexpr std::string_view mounted{"package://"};
            constexpr std::string_view pinned{"package-pinned://"};
            constexpr std::size_t maximumReferenceBytes = 2048U;
            const bool isPinned = reference.starts_with(pinned);
            const auto scheme = isPinned ? pinned : mounted;
            if (reference.size() > maximumReferenceBytes || !reference.starts_with(scheme) ||
                reference.find_first_of("%?#") != std::string_view::npos)
                return Output::Failure(MakeError(PackageInstallErrors::InvalidInput));
            reference.remove_prefix(scheme.size());
            const auto slash = reference.find('/');
            if (slash == std::string_view::npos)
                return Output::Failure(MakeError(PackageInstallErrors::InvalidInput));
            const auto authority = reference.substr(0, slash);
            const auto separator = authority.find('@');
            if (isPinned != (separator != std::string_view::npos))
                return Output::Failure(MakeError(PackageInstallErrors::InvalidInput));
            auto package = HoroPackageId::Parse(authority.substr(0, separator));
            if (package.HasError())
                return Output::Failure(package.ErrorValue());
            std::optional<PackageVersion> version;
            if (isPinned) {
                auto parsed = PackageVersion::Parse(authority.substr(separator + 1));
                if (parsed.HasError())
                    return Output::Failure(parsed.ErrorValue());
                version.emplace(std::move(parsed).Value());
            }
            auto path = PackagePath::Parse(reference.substr(slash + 1));
            if (path.HasError())
                return Output::Failure(path.ErrorValue());
            return Output::Success({std::move(package).Value(), std::move(version), std::move(path).Value()});
        }

        /** @brief Requires an exact non-executable inventory member belonging to the package asset root. */
        bool IsMountedAsset(const PackageRestorePackage &package, const PackagePath &path) {
            if (!path.Value().starts_with("assets/") ||
                std::ranges::find(package.lock.contributions, "assets") == package.lock.contributions.end())
                return false;
            const auto files = package.archive->Manifest().Entries();
            const auto entry = std::ranges::find_if(files, [&](const PackageFileEntry &file) {
                return file.id.path.Value() == path.Value();
            });
            return entry != files.end() && !entry->executable &&
                   (!entry->contributionRoot || entry->contributionRoot->Value() == "assets");
        }

        /** @brief Rejects forged or incomplete graph evidence before any project write. */
        [[nodiscard]] Result<void> ValidateGraph(const PackageRestoreGraph &graph) {
            if (graph.packages.size() > MaximumInstalledPackages || graph.platform.operatingSystem.empty() ||
                graph.platform.architecture.empty() || graph.platform.sdkAbi.empty())
                return Result<void>::Failure(MakeError(PackageInstallErrors::InvalidInput));

            std::map<std::string_view, const LockedPackage *, std::less<>> identities;
            for (const auto &package : graph.packages) {
                if (!package.archive || package.lock.dependencies.size() > MaximumInstalledPackages ||
                    !identities.try_emplace(package.lock.package.Value(), &package.lock).second)
                    return Result<void>::Failure(MakeError(PackageInstallErrors::InvalidInput));
                if (package.archive->Digest() != package.lock.artifactDigest ||
                    package.archive->PackageManifestDigest() != package.lock.manifestDigest ||
                    package.archive->Manifest().Digest() != package.lock.fileManifestDigest)
                    return Result<void>::Failure(MakeError(PackageInstallErrors::EvidenceMismatch));
            }
            for (const auto &package : graph.packages) {
                for (const auto &dependency : package.lock.dependencies) {
                    const auto installed = identities.find(dependency.package.Value());
                    if (installed == identities.end() || installed->second == &package.lock ||
                        installed->second->version != dependency.version)
                        return Result<void>::Failure(
                            MakeError(PackageInstallErrors::InvalidInput,
                                      std::format("Package {} requires {} at version {}.", package.lock.package.Value(),
                                                  dependency.package.Value(), dependency.version.ToString())));
                }
            }
            return Result<void>::Success();
        }

        /** @brief Emits deterministic portable install evidence without paths or trust decisions. */
        [[nodiscard]] std::string SerializeRecord(const PackageRestoreGraph &graph) {
            std::vector<const PackageRestorePackage *> ordered;
            ordered.reserve(graph.packages.size());
            for (const auto &package : graph.packages)
                ordered.push_back(&package);
            std::ranges::sort(ordered, {}, [](const PackageRestorePackage *package) -> const std::string & {
                return package->lock.package.Value();
            });

            nlohmann::json packages = nlohmann::json::array();
            for (const auto *package : ordered) {
                packages.push_back({{"id", package->lock.package.Value()},
                                    {"version", package->lock.version.ToString()},
                                    {"source", package->lock.source.Value()},
                                    {"artifactDigest", FormatSha256(package->lock.artifactDigest)},
                                    {"manifestDigest", FormatSha256(package->lock.manifestDigest)},
                                    {"fileManifestDigest", FormatSha256(package->lock.fileManifestDigest)}});
            }
            return nlohmann::json{{"schemaVersion", 1},
                                  {"requestHash", FormatSha256(graph.requestHash)},
                                  {"platform",
                                   {{"operatingSystem", graph.platform.operatingSystem},
                                    {"architecture", graph.platform.architecture},
                                    {"sdkAbi", graph.platform.sdkAbi}}},
                                  {"packages", std::move(packages)}}
                       .dump() +
                   '\n';
        }
    }  // namespace

    /** @copydoc VerifiedPackageInstallRecord::ReadMountedAsset */
    Result<std::vector<std::byte>> VerifiedPackageInstallRecord::ReadMountedAsset(const std::string_view reference,
                                                                                  const std::uint64_t maximumBytes) const {
        using Output = Result<std::vector<std::byte>>;
        if (maximumBytes == 0)
            return Output::Failure(MakeError(PackageInstallErrors::InvalidInput));
        auto parsed = ParseMountedAsset(reference);
        if (parsed.HasError())
            return Output::Failure(parsed.ErrorValue());
        const auto &asset = parsed.Value();
        const auto &installed = graph_->packages;
        const auto package = std::ranges::find_if(installed, [&](const PackageRestorePackage &entry) {
            return entry.lock.package == asset.package;
        });
        if (package == installed.end() || (asset.version && package->lock.version != *asset.version) ||
            !IsMountedAsset(*package, asset.path))
            return Output::Failure(MakeError(PackageInstallErrors::MountedAssetUnavailable));
        return package->archive->ReadDeclaredFile(asset.path, maximumBytes);
    }

    class PackageInstallService::Impl final {
    public:
        Impl(DurableFileSystem &files, std::filesystem::path root) : files_(files), root_(std::move(root)) {}

        /** @brief Serializes installation and holds the project lease through the complete commit attempt. */
        Result<void> Install(std::shared_ptr<const PackageRestoreGraph> graph, const CancellationToken &cancellation) {
            std::lock_guard guard(mutex_);
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(PackageInstallErrors::Cancelled));

            const auto metadata = root_ / ".horo";
            if (auto lock = files_.TryAcquireExclusive(metadata / "packages.install.lock", "package install"); lock.HasValue())
                return CommitRecord(std::move(graph), cancellation, metadata);
            return Result<void>::Failure(MakeError(PackageInstallErrors::LockUnavailable));
        }

        const std::filesystem::path &ProjectRoot() const noexcept {
            return root_;
        }

        /** @brief Reads the graph under the same mutex that protects publication. */
        std::shared_ptr<const PackageRestoreGraph> ActiveGraph() const {
            std::lock_guard guard(mutex_);
            return record_ ? record_->Graph() : nullptr;
        }

        /** @brief Returns install evidence while publication is serialized. */
        std::shared_ptr<const VerifiedPackageInstallRecord> InstalledRecord() const {
            std::lock_guard guard(mutex_);
            return record_;
        }

    private:
        /** @brief Publishes a record while the caller retains both the mutex and exclusive project lease. */
        Result<void> CommitRecord(std::shared_ptr<const PackageRestoreGraph> graph, const CancellationToken &cancellation,
                                  const std::filesystem::path &metadata) {
            if (revision_ == std::numeric_limits<std::uint64_t>::max())
                return Result<void>::Failure(MakeError(PackageInstallErrors::InvalidInput));
            auto candidate = PackageInstallService::FreezeRecord(std::move(graph), revision_ + 1U);
            const std::string record = SerializeRecord(*candidate->Graph());
            const auto pending = metadata / "packages.install.pending";
            const auto committed = metadata / "packages.installed.json";
            const auto bytes = std::as_bytes(std::span{record.data(), record.size()});
            if (auto written = files_.WriteDurable(pending, bytes); written.HasError())
                return Result<void>::Failure(MakeError(PackageInstallErrors::CommitFailed));
            if (cancellation.IsCancellationRequested()) {
                static_cast<void>(files_.RemoveDurable(pending));
                return Result<void>::Failure(MakeError(PackageInstallErrors::Cancelled));
            }
            if (auto replaced = files_.AtomicReplace(pending, committed); replaced.HasError()) {
                static_cast<void>(files_.RemoveDurable(pending));
                return Result<void>::Failure(MakeError(PackageInstallErrors::CommitFailed));
            }
            record_ = std::move(candidate);
            ++revision_;
            return Result<void>::Success();
        }

        DurableFileSystem &files_;
        std::filesystem::path root_;
        // Serializes file transactions and graph publication/readback for every calling thread.
        mutable std::mutex mutex_;
        std::uint64_t revision_{};
        std::shared_ptr<const VerifiedPackageInstallRecord> record_;
    };

    /** @copydoc PackageInstallService::Create */
    Result<PackageInstallService> PackageInstallService::Create(DurableFileSystem &files, const std::filesystem::path &projectRoot) {
        std::error_code error;
        const auto canonical = std::filesystem::canonical(projectRoot, error);
        if (error || !projectRoot.is_absolute() || canonical != projectRoot.lexically_normal())
            return Result<PackageInstallService>::Failure(MakeError(PackageInstallErrors::InvalidInput));
        if (!std::filesystem::is_directory(canonical, error) || error)
            return Result<PackageInstallService>::Failure(MakeError(PackageInstallErrors::InvalidInput));
        if (const auto metadataStatus = std::filesystem::symlink_status(canonical / ".horo", error);
            (error && error != std::errc::no_such_file_or_directory) ||
            (std::filesystem::exists(metadataStatus) && !std::filesystem::is_directory(metadataStatus)))
            return Result<PackageInstallService>::Failure(MakeError(PackageInstallErrors::InvalidInput));
        return Result<PackageInstallService>::Success(PackageInstallService{std::make_unique<Impl>(files, canonical)});
    }

    /** @copydoc PackageInstallService::PackageInstallService */
    PackageInstallService::PackageInstallService(std::unique_ptr<Impl> state) : state_(std::move(state)) {}

    PackageInstallService::PackageInstallService(PackageInstallService &&) noexcept = default;
    PackageInstallService &PackageInstallService::operator=(PackageInstallService &&) noexcept = default;
    PackageInstallService::~PackageInstallService() noexcept = default;

    /** @copydoc PackageInstallService::Install */
    Result<void> PackageInstallService::Install(std::shared_ptr<const PackageRestoreGraph> graph, const CancellationToken &cancellation) {
        if (!graph)
            return Result<void>::Failure(MakeError(PackageInstallErrors::InvalidInput));
        if (auto valid = ValidateGraph(*graph); valid.HasError())
            return valid;

        return state_->Install(std::move(graph), cancellation);
    }

    /** @copydoc PackageInstallService::FreezeRecord */
    std::shared_ptr<const VerifiedPackageInstallRecord> PackageInstallService::FreezeRecord(
        std::shared_ptr<const PackageRestoreGraph> graph, const std::uint64_t revision) {
        auto snapshot = std::make_shared<const PackageRestoreGraph>(*graph);
        // make_shared cannot invoke the private constructor that seals verified install evidence.
        return std::shared_ptr<const VerifiedPackageInstallRecord>{// NOSONAR(cpp:S5950) Sealed private constructor.
                                                                   new VerifiedPackageInstallRecord{std::move(snapshot), revision}};
    }

    /** @copydoc PackageInstallService::ProjectRoot */
    const std::filesystem::path &PackageInstallService::ProjectRoot() const noexcept {
        return state_->ProjectRoot();
    }

    /** @copydoc PackageInstallService::InstalledRecord */
    std::shared_ptr<const VerifiedPackageInstallRecord> PackageInstallService::InstalledRecord() const {
        return state_->InstalledRecord();
    }

    /** @copydoc PackageInstallService::ActiveGraph */
    std::shared_ptr<const PackageRestoreGraph> PackageInstallService::ActiveGraph() const {
        return state_->ActiveGraph();
    }
}  // namespace Horo::Packages
