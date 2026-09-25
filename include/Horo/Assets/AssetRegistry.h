#pragma once

/**
 * @file AssetRegistry.h
 * @brief Immutable asset-registry snapshots, derived-index persistence, and sidecar rebuilding.
 */

#include "Horo/Assets/AssetId.h"
#include "Horo/Foundation/Paths.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Assets {
    /** @brief Monotonic process-local revision of one published registry snapshot. */
    struct AssetRegistryRevision {
        std::uint64_t value{};
        [[nodiscard]] constexpr auto operator<=>(const AssetRegistryRevision &) const noexcept = default;
    };

    /** @brief One validated source asset and its committed identity sidecar. */
    struct AssetRecord {
        AssetId id;
        AssetTypeId type;
        ProjectPath sourcePath;
        ProjectPath metadataPath;
    };

    /** @brief Outcome of a complete registry candidate build. */
    enum class AssetRegistryBuildStatus : std::uint8_t {
        Complete,
        Degraded,
        Failed
    };

    /** @brief Bounded diagnostic associated with one registry rebuild. */
    struct AssetRegistryDiagnostic {
        Error error;
        std::optional<std::string> projectPath;
    };

    /** @brief Result of a rebuild and optional atomic publish. */
    struct AssetRegistryBuildReport {
        AssetRegistryBuildStatus status{AssetRegistryBuildStatus::Failed};
        AssetRegistryRevision publishedRevision;
        std::size_t registeredAssets{};
        std::vector<AssetRegistryDiagnostic> diagnostics;
    };

    /** @brief Validated unpublished registry state prepared without replacing the authoritative snapshot. */
    struct AssetRegistryCandidate {
        AssetRegistryBuildStatus status{AssetRegistryBuildStatus::Failed};
        std::vector<AssetRecord> records;
        std::vector<AssetRegistryDiagnostic> diagnostics;
    };

    class AssetRegistry;

    /** @brief Thread-safe immutable registry snapshot pinned independently from later replacements. */
    class AssetRegistrySnapshot final {
    public:
        AssetRegistrySnapshot() = default;
        /** @brief Returns the revision pinned by this snapshot. @return Zero for an empty snapshot. */
        [[nodiscard]] AssetRegistryRevision Revision() const noexcept;
        /** @brief Returns all records in deterministic AssetId order. @return Borrowed records pinned by this snapshot. */
        [[nodiscard]] std::span<const AssetRecord> Records() const noexcept;
        /** @brief Finds a record by stable identity without allocation. @param id Identity to resolve. @return Borrowed
         * record or null when absent. */
        [[nodiscard]] const AssetRecord *Find(AssetId id) const noexcept;
        /** @brief Finds a record by normalized project path without allocation. @param normalizedProjectPath Canonical
         * forward-slash ProjectPath text. @return Borrowed record or null when absent. */
        [[nodiscard]] const AssetRecord *FindByPath(std::string_view normalizedProjectPath) const noexcept;

    private:
        struct State;
        friend class AssetRegistry;

        explicit AssetRegistrySnapshot(std::shared_ptr<const State> state) noexcept : state_(std::move(state)) {}

        std::shared_ptr<const State> state_;
    };

    /** @brief Owner-thread publisher of immutable registry snapshots. Readers retain pinned snapshots. */
    class AssetRegistry final {
    public:
        /** @brief Creates an empty revision-zero registry. */
        AssetRegistry();
        AssetRegistry(const AssetRegistry &) = delete;
        AssetRegistry &operator=(const AssetRegistry &) = delete;
        /** @brief Pins the currently published immutable state. @return Thread-safe snapshot unaffected by replacements. */
        [[nodiscard]] AssetRegistrySnapshot Snapshot() const noexcept;

        /** @brief Validates and atomically publishes a complete candidate. Global ambiguity preserves the previous state.
         * @param candidate Mutable owner-thread build result consumed by this call. @param diagnostics Bounded per-record
         * diagnostics accumulated while building the candidate. @return Complete, degraded, or failed publish report. */
        [[nodiscard]] AssetRegistryBuildReport Publish(std::vector<AssetRecord> candidate,
                                                       std::vector<AssetRegistryDiagnostic> diagnostics = {});
        /** @brief Publishes a previously prepared candidate on the registry owner thread. */
        [[nodiscard]] AssetRegistryBuildReport Publish(AssetRegistryCandidate candidate);

    private:
        std::shared_ptr<const AssetRegistrySnapshot::State> state_;
        std::uint64_t nextRevision_{1};
    };

    /** @brief Determines whether a successful rebuild may replace the derived index on disk. */
    enum class AssetRegistryOpenMode : std::uint8_t {
        ReadOnly,
        Edit
    };

    /** @brief Loads and stores the all-or-nothing derived asset index. */
    class AssetIndexStore final {
    public:
        /** @brief Loads the derived index as one all-or-nothing candidate. @param path Native index path. @return Parsed
         * records or a typed malformed/I/O error; partial records are never returned. */
        [[nodiscard]] static Result<std::vector<AssetRecord>> Load(const std::filesystem::path &path);
        /** @brief Deterministically serializes and atomically replaces the derived index. @param path Native destination.
         * @param snapshot Immutable state to serialize. @return Success or a typed I/O error. */
        [[nodiscard]] static Result<void> SaveAtomically(const std::filesystem::path &path, const AssetRegistrySnapshot &snapshot);
    };

    /** @brief Rebuilds a registry from source files and committed sidecars beneath one project root. @param registry
     * Owner-thread registry to publish. @param projectRoot Native project root containing an Assets/ or legacy assets/ directory. @param
     * mode Controls whether the derived index may be replaced. @return Rebuild report or a root/I/O failure. */
    [[nodiscard]] Result<AssetRegistryBuildReport> RebuildAssetRegistry(AssetRegistry &registry, const std::filesystem::path &projectRoot,
                                                                        AssetRegistryOpenMode mode);

    /** @brief Loads the derived index or deterministically falls back to a sidecar rebuild. @param registry Owner-thread
     * registry to publish. @param projectRoot Native project root. @param mode Controls derived-index writes during
     * fallback. @return Load/rebuild report or a root/I/O failure. */
    [[nodiscard]] Result<AssetRegistryBuildReport> LoadAssetRegistry(AssetRegistry &registry, const std::filesystem::path &projectRoot,
                                                                     AssetRegistryOpenMode mode);

    /** @brief Builds an unpublished asset registry candidate by loading the index or rebuilding sidecars. */
    [[nodiscard]] Result<AssetRegistryCandidate> PrepareAssetRegistryCandidate(const std::filesystem::path &projectRoot);
}  // namespace Horo::Assets
