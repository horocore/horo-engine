#include "Horo/Packages/PackageInstall.h"

#include "Horo/Packages/PackageInstallErrors.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Packages {
    namespace {
        constexpr std::size_t MaximumInstalledPackages = 4096U;

        /** @brief Rejects forged or incomplete graph evidence before any project write. */
        [[nodiscard]] Result<void> ValidateGraph(const PackageRestoreGraph &graph) {
            if (graph.packages.size() > MaximumInstalledPackages || graph.platform.operatingSystem.empty() ||
                graph.platform.architecture.empty() || graph.platform.sdkAbi.empty())
                return Result<void>::Failure(MakeError(PackageInstallErrors::InvalidInput));

            std::set<std::string, std::less<>> identities;
            for (const auto &package : graph.packages) {
                if (!package.archive || !identities.insert(package.lock.package.Value()).second)
                    return Result<void>::Failure(MakeError(PackageInstallErrors::InvalidInput));
                if (package.archive->Digest() != package.lock.artifactDigest ||
                    package.archive->PackageManifestDigest() != package.lock.manifestDigest ||
                    package.archive->Manifest().Digest() != package.lock.fileManifestDigest)
                    return Result<void>::Failure(MakeError(PackageInstallErrors::EvidenceMismatch));
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

    /** @copydoc PackageInstallService::InstalledRecord */
    std::shared_ptr<const VerifiedPackageInstallRecord> PackageInstallService::InstalledRecord() const {
        return state_->InstalledRecord();
    }

    /** @copydoc PackageInstallService::ActiveGraph */
    std::shared_ptr<const PackageRestoreGraph> PackageInstallService::ActiveGraph() const {
        return state_->ActiveGraph();
    }
}  // namespace Horo::Packages
