#pragma once

#include "Horo/Application/ProjectVersion.h"
#include "Horo/Foundation/Sha256.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Horo::Application {
    /**
     * @file ProjectCompatibility.h
     * @brief Immutable release compatibility and read-only project inspection contracts.
     */

    /** @brief Release decision category retained in the generated compatibility catalog. */
    enum class CompatibilityDecisionKind : std::uint8_t {
        EstablishBaseline,
        CompatibleReleaseLine,
    };

    /** @brief Exact release-to-contract compatibility decision. */
    struct ReleaseCompatibilityDecision {
        EngineReleaseVersion release;
        ContractBaselineVersion contractBaseline;
        PersistentContractHash persistentContract;
        CompatibilityDecisionHash decisionHash;
        CompatibilityDecisionKind kind{CompatibilityDecisionKind::EstablishBaseline};
    };

    /** @brief Optional signed proof embedded in project metadata for an exact release decision. */
    struct CompatibilityProof {
        EngineReleaseVersion release;
        ContractBaselineVersion contractBaseline;
        CompatibilityDecisionHash decisionHash;
        std::string signature;
    };

    /** @brief Immutable validated release compatibility lookup. */
    class ReleaseCompatibilityRegistry {
    public:
        /**
         * @brief Validates and freezes exact release decisions.
         * @param decisions Candidate release decisions.
         * @return Immutable registry or a typed catalog diagnostic.
         */
        [[nodiscard]] static Result<ReleaseCompatibilityRegistry> Create(std::span<const ReleaseCompatibilityDecision> decisions);

        /**
         * @brief Finds an exact release decision.
         * @param release Exact release identity.
         * @return Stable pointer owned by this registry, or null when unknown.
         */
        [[nodiscard]] const ReleaseCompatibilityDecision *Find(const EngineReleaseVersion &release) const noexcept;
        /** @brief Returns immutable decisions in deterministic SemVer order. */
        [[nodiscard]] std::span<const ReleaseCompatibilityDecision> Decisions() const noexcept;

    private:
        explicit ReleaseCompatibilityRegistry(std::vector<ReleaseCompatibilityDecision> decisions);
        std::vector<ReleaseCompatibilityDecision> decisions_;
    };

    /** @brief Verification boundary for future-patch compatibility proof envelopes. */
    class ICompatibilityProofVerifier {
    public:
        virtual ~ICompatibilityProofVerifier() = default;
        /**
         * @brief Verifies that a proof binds its exact release decision to the expected contract.
         * @param proof Proof envelope read from project metadata.
         * @param expectedContract Contract trusted by the current release line.
         * @return Success only when the proof is trusted and correctly bound.
         */
        [[nodiscard]] virtual Result<void> Verify(const CompatibilityProof &proof,
                                                  const PersistentContractHash &expectedContract) const = 0;
    };

    /** @brief Production-safe baseline verifier that rejects unknown proof envelopes. */
    class RejectingCompatibilityProofVerifier final : public ICompatibilityProofVerifier {
    public:
        /** @copydoc ICompatibilityProofVerifier::Verify */
        [[nodiscard]] Result<void> Verify(const CompatibilityProof &proof, const PersistentContractHash &expectedContract) const override;
    };

    /** @brief Startup-relevant durable subset of `.horo/project.json`. */
    struct ProjectMetadata {
        EngineReleaseVersion horoVersion;
        PersistentContractHash persistentContract;
        std::optional<CompatibilityProof> compatibilityProof;
        std::string projectId;
        std::string name;
        std::string projectVersion;
        std::string createdAt;
        std::string renderBackend;
        std::optional<MigrationHistoryHead> migrationHistoryHead;
    };

    /** @brief Compatibility classification produced before a writable project session exists. */
    enum class ProjectCompatibilityStatus : std::uint8_t {
        Current,
        CompatibleReleaseLine,
        AutomaticMigrationRequired,
        RecoveryRequired,
        FutureVersion,
        MigrationPathMissing,
        RequiredProviderUnavailable,
        Corrupt,
        Inaccessible,
    };

    /** @brief Immutable read-only project compatibility result. */
    struct ProjectCompatibilitySnapshot {
        ProjectCompatibilityStatus status{ProjectCompatibilityStatus::Corrupt};
        std::optional<ProjectMetadata> metadata;
        EngineReleaseVersion targetVersion;
        std::optional<ContractBaselineVersion> sourceBaseline;
        bool markerUpdateRequired{false};
        std::optional<Error> diagnostic;
        std::optional<Sha256Digest>
            sourceFingerprint; /**< Exact bytes used by this classification; revision evidence, never trust proof. */
    };

    /** @brief Read-only bounded project compatibility inspector with injected trust policy. */
    class ProjectCompatibilityInspector {
    public:
        /**
         * @brief Creates a read-only inspector over immutable release policy.
         * @param registry Validated release decision registry that outlives the inspector.
         * @param currentRelease Exact running engine release.
         * @param proofVerifier Trust boundary that outlives the inspector.
         */
        ProjectCompatibilityInspector(const ReleaseCompatibilityRegistry &registry, const EngineReleaseVersion &currentRelease,
                                      const ICompatibilityProofVerifier &proofVerifier) noexcept;

        /**
         * @brief Reads and classifies one project without mutating it.
         * @param projectRoot Root containing `.horo/project.json`.
         * @return Typed compatibility snapshot with bounded metadata and diagnostics.
         */
        [[nodiscard]] ProjectCompatibilitySnapshot Inspect(const std::filesystem::path &projectRoot) const;

    private:
        const ReleaseCompatibilityRegistry &registry_;
        EngineReleaseVersion currentRelease_;
        const ICompatibilityProofVerifier &proofVerifier_;
    };

    /**
     * @brief Loads and validates bounded project metadata without compatibility classification.
     * @param projectRoot Root containing `.horo/project.json`.
     * @return Parsed durable metadata or a typed read/validation error.
     */
    [[nodiscard]] Result<ProjectMetadata> LoadProjectMetadata(const std::filesystem::path &projectRoot);

    /** @brief Returns the immutable generated release compatibility catalog. */
    [[nodiscard]] const ReleaseCompatibilityRegistry &BuiltInReleaseCompatibilityRegistry();

    /** @brief Returns the current exact engine release generated from CMake `PROJECT_VERSION`. */
    [[nodiscard]] EngineReleaseVersion CurrentEngineReleaseVersion();

    /**
     * @brief Inspects a project using the built-in catalog and fail-closed proof verifier.
     * @param projectRoot Root containing `.horo/project.json`.
     * @return Read-only compatibility snapshot.
     */
    [[nodiscard]] ProjectCompatibilitySnapshot InspectProjectCompatibility(const std::filesystem::path &projectRoot);
}  // namespace Horo::Application
