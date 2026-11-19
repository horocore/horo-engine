#pragma once

/**
 * @file ProjectGameplayRegistry.h
 * @brief Project-scoped discovery and ownership of editor-visible gameplay behaviors.
 */

#include "EditorPlaySaveSession.h"
#include "Horo/Gameplay/BehaviorRegistry.h"
#include "Horo/Gameplay/ComponentRegistry.h"
#include "Horo/Gameplay/GameAssetTypeRegistry.h"
#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Gameplay/GameModuleRetirement.h"
#include "Horo/Gameplay/LuaBehavior.h"
#include "Horo/Network/ReplicationMetadataCapture.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::AI {
    class DecisionBlackboardProviderMetadata;
    class DecisionNodeGeneration;
}  // namespace Horo::AI

namespace Horo::Gameplay {
    class LuaDecisionNodeProgram;
}

namespace Horo::Editor {
    /** @brief One gameplay source diagnostic that blocks starting a play session. */
    struct ProjectGameplayDiagnostic {
        std::filesystem::path source;
        Error error;
    };

    /** @brief Preserved old-generation artifact identity used only for native reload rollback. */
    struct NativeGameplayRollbackArtifact {
        std::filesystem::path path;
        std::string moduleId;
        std::uint64_t descriptorRevision{};

        /** @brief Creates an empty non-owning artifact value. */
        NativeGameplayRollbackArtifact() = default;
        /**
         * @brief Takes cleanup ownership of one preserved rollback artifact.
         * @param artifactPath Editor-owned preserved artifact removed on destruction.
         * @param owningModuleId Exact module identity recorded before retirement.
         * @param revision Exact descriptor revision recorded before retirement.
         */
        NativeGameplayRollbackArtifact(std::filesystem::path artifactPath, std::string owningModuleId, std::uint64_t revision);
        /** @brief Removes the owned rollback artifact, if any. */
        ~NativeGameplayRollbackArtifact();
        NativeGameplayRollbackArtifact(const NativeGameplayRollbackArtifact &) = delete;
        NativeGameplayRollbackArtifact &operator=(const NativeGameplayRollbackArtifact &) = delete;
        /** @brief Transfers cleanup ownership. */
        NativeGameplayRollbackArtifact(NativeGameplayRollbackArtifact &&other) noexcept;
        /** @brief Replaces this owned artifact and transfers cleanup ownership. */
        NativeGameplayRollbackArtifact &operator=(NativeGameplayRollbackArtifact &&other) noexcept;
    };

    /** @brief One exact last-good Lua program and its watcher baseline. */
    struct ProjectLuaProgramSnapshot {
        std::unique_ptr<Gameplay::LuaBehaviorProgram> program;
        std::filesystem::path source;
        std::filesystem::file_time_type sourceWriteTime;
        std::filesystem::file_time_type metadataWriteTime;
        std::uintmax_t sourceSize{};
        std::uintmax_t metadataSize{};
    };

    /** @brief Watcher observation for one source and its metadata sidecar. */
    struct ProjectLuaSourceObservation final {
        std::filesystem::file_time_type sourceWriteTime;
        std::filesystem::file_time_type metadataWriteTime;
        std::uintmax_t sourceSize{};
        std::uintmax_t metadataSize{};

        [[nodiscard]] auto operator<=>(const ProjectLuaSourceObservation &) const = default;
    };

    /** @brief One admitted Decision script and the observation used to replace it transactionally. */
    struct ProjectLuaDecisionProgramSnapshot final {
        std::filesystem::path source;
        std::shared_ptr<Gameplay::LuaDecisionNodeProgram> program;
        ProjectLuaSourceObservation observation;
    };

    /** @brief Exact last-good Lua generation retained across one native reload transaction. */
    struct ProjectLuaGenerationSnapshot {
        std::vector<ProjectLuaProgramSnapshot> programs;
        std::vector<ProjectLuaDecisionProgramSnapshot> decisions;
    };

    /** @brief Reacquires only the published package role's exact revocable Decision leaves on the Project owner lane.
     * The composition callback borrows the package owner. It must not retain any executable generation between reads.
     */
    using ProjectPackageDecisionNodeSource = std::function<Result<std::vector<std::shared_ptr<const AI::DecisionNodeGeneration>>>()>;

    /** @brief Owns discovered project behavior programs and their frozen registry snapshot. */
    class ProjectGameplayRegistry final {
        struct ConstructionToken {};

    public:
        /** @brief Discovers the native module and bounded Lua behavior assets for a project. */
        [[nodiscard]] static std::unique_ptr<ProjectGameplayRegistry> Discover(
            const std::filesystem::path &projectRoot, std::shared_ptr<Gameplay::GameModuleRetirementQueue> retirements = {});
        /**
         * @brief Loads one host-preserved old artifact for transactional rollback.
         * @param projectRoot Absolute project root used for manifest watching and shadow storage.
         * @param artifact Preserved library path and exact old-generation identity.
         * @param luaGeneration Exact in-memory Lua generation captured before retirement.
         * @return Owning combined registry with diagnostics when restoration cannot be prepared.
         */
        [[nodiscard]] static std::unique_ptr<ProjectGameplayRegistry> DiscoverRollback(
            const std::filesystem::path &projectRoot, const NativeGameplayRollbackArtifact &artifact,
            const ProjectLuaGenerationSnapshot &luaGeneration, std::shared_ptr<Gameplay::GameModuleRetirementQueue> retirements = {});
        /**
         * @brief Loads the current native manifest with an exact last-good Lua generation.
         * @param projectRoot Absolute project root used for native discovery and shadow storage.
         * @param luaGeneration Immutable Lua generation captured before native retirement.
         * @return Owning combined registry without rereading Lua source files.
         */
        [[nodiscard]] static std::unique_ptr<ProjectGameplayRegistry> DiscoverNativeGeneration(
            const std::filesystem::path &projectRoot, const ProjectLuaGenerationSnapshot &luaGeneration,
            std::shared_ptr<Gameplay::GameModuleRetirementQueue> retirements = {});

        explicit ProjectGameplayRegistry(ConstructionToken, std::shared_ptr<Gameplay::GameModuleRetirementQueue> retirements = {});
        ~ProjectGameplayRegistry();

        ProjectGameplayRegistry(const ProjectGameplayRegistry &) = delete;
        ProjectGameplayRegistry &operator=(const ProjectGameplayRegistry &) = delete;

        /** @brief Returns the immutable registry used by authoring and new play sessions. */
        [[nodiscard]] const Gameplay::BehaviorRegistry &Registry() const noexcept;
        /** @brief Returns current game-owned asset metadata or an empty frozen missing-code registry. */
        [[nodiscard]] const Gameplay::GameAssetTypeRegistry &AssetTypes() const noexcept;
        /** @brief Returns current game-owned component metadata or an empty frozen missing-code registry. */
        [[nodiscard]] const Gameplay::ComponentRegistry &Components() const noexcept;
        /** @brief Borrows detached Decision descriptors from the admitted Project catalog.
         * @return Exact immutable metadata, or null when no authoring catalog was admitted. No executable lease escapes.
         */
        [[nodiscard]] const std::shared_ptr<const AI::DecisionBlackboardProviderMetadata> &DecisionUsageMetadata() const noexcept;

        /** @brief Temporarily acquires the current native and script Decision declarations for source compilation.
         * @return The admitted generation or a typed unavailable result. Callers release it before returning to the workspace. */
        [[nodiscard]] Result<std::shared_ptr<const AI::DecisionNodeGeneration>> AcquireDecisionAuthoringGeneration() const;

        /** @brief Atomically updates package contributions to the detached authoring catalog at the owner's safe point.
         * @param source Read-only source bound to the exact live package activation. An empty source withdraws all package contributions.
         * @return Success or a typed admission/duplicate/storage failure preserving the prior catalog.
         * @details The application calls this on package publication, retirement and Project rebind. The catalog retains no executable
         * lease.
         */
        [[nodiscard]] Result<void> ReplacePackageDecisionNodeSource(ProjectPackageDecisionNodeSource source);

        /** @brief Returns source-addressed compile and registration failures. */
        [[nodiscard]] const std::vector<ProjectGameplayDiagnostic> &Diagnostics() const noexcept;
        /** @brief Reports whether source failures prevent a coherent registry snapshot. */
        [[nodiscard]] bool HasBlockingDiagnostics() const noexcept;
        /** @brief Validates changed Lua candidates and swaps compatible programs at callback safe points. */
        [[nodiscard]] std::vector<ProjectGameplayDiagnostic> ReloadChangedLuaSources();
        /** @brief Consumes a native artifact-manifest create, replace, or remove transition. */
        [[nodiscard]] bool ConsumeNativeArtifactChange();
        /**
         * @brief Copies the active shadow artifact before unload so rollback cannot observe overwritten build output.
         * @param destination Absolute editor-owned rollback directory.
         * @return Preserved generation identity or a typed file/ownership failure.
         */
        [[nodiscard]] Result<NativeGameplayRollbackArtifact> PreserveNativeArtifactForRollback(
            const std::filesystem::path &destination) const;
        /** @brief Clones the exact active Lua program generation and watcher baseline for native rollback. */
        [[nodiscard]] Result<ProjectLuaGenerationSnapshot> CaptureLuaGeneration() const;
        /**
         * @brief Requests cancellation, proves module quiescence, captures state, and stops the old generation.
         * @return Bounded snapshot only when native unload is safe.
         */
        [[nodiscard]] Result<Gameplay::GameModuleReloadSnapshot> PrepareNativeReload();
        /**
         * @brief Restores captured module state into this newly loaded generation.
         * @param snapshot State captured from the previous compatible generation.
         * @return Success or a typed restore failure.
         */
        [[nodiscard]] Result<void> RestoreNativeReload(const Gameplay::GameModuleReloadSnapshot &snapshot);
        /** @brief Acquires every declared durable native source from the actual current module generation.
         * @return Complete bounded bindings or original stale/missing/capacity failure; no partial inventory is published.
         * @details The core Scene participant consumes one registry slot. Lua or unregistered owners gain no implied save support.
         */
        [[nodiscard]] Result<std::vector<PlaySaveGameplayBinding>> AcquireSaveBindings() const;
        /** @brief Reports whether a native generation is loaded. */
        [[nodiscard]] bool HasNativeModule() const noexcept;
        /** @brief Returns deep-owned metadata of the actual loaded generation, without executable leases.
         * @return Immutable capture, or null when capture failed; Diagnostics retains the original cause. */
        [[nodiscard]] const Network::ReplicationMetadataCapturePtr &ReplicationMetadata() const noexcept;
        /** @brief Returns nonadmitting inspection evidence, including opaque unavailable declarations.
         * @return Capture-owned assessment or null when structural capture failed. */
        [[nodiscard]] const Network::ReplicationDeclarationAssessment *ReplicationInspection() const noexcept;
        /** @brief Returns the current source/candidate capture error without discarding last-good inspection metadata.
         * @return Borrowed typed failure while Cook must fail closed, otherwise null. */
        [[nodiscard]] const Error *ReplicationMetadataError() const noexcept;

    private:
        using LuaSourceStat = ProjectLuaSourceObservation;

        [[nodiscard]] static LuaSourceStat ReadLuaSourceStat(const std::filesystem::path &source, std::error_code &error);

        void DiscoverNativeModule(const std::filesystem::path &projectRoot);

        [[nodiscard]] bool PrepareNativeManifestPath(const std::filesystem::path &projectRoot, const std::filesystem::path &manifestPath);

        void LoadNativeModule(const std::filesystem::path &projectRoot, const std::filesystem::path &artifactPath,
                              std::string_view moduleId, std::uint64_t descriptorRevision);

        void DiscoverLuaPrograms(const std::filesystem::path &projectRoot);
        void InstallLuaGeneration(const ProjectLuaGenerationSnapshot &snapshot);
        /** @brief Rebuilds detached metadata after initial discovery or coherent source replacement. */
        [[nodiscard]] Result<void> RefreshDecisionAuthoringCatalog(const ProjectPackageDecisionNodeSource &packages);
        /** @brief Acquires and composes real leaf owners without altering the admitted catalog. */
        [[nodiscard]] Result<std::shared_ptr<const AI::DecisionNodeGeneration>> BuildDecisionAuthoringGeneration(
            const ProjectPackageDecisionNodeSource &packages) const;
        /** @brief Replaces changed Decision scripts only after catalog validation succeeds. */
        void ReloadChangedDecisionSources(std::vector<ProjectGameplayDiagnostic> &diagnostics);
        void CaptureReplicationMetadata(const std::filesystem::path &projectRoot);
        [[nodiscard]] Result<Network::ReplicationMetadataCapturePtr> BuildReplicationMetadata(
            const std::filesystem::path &projectRoot, const Gameplay::LuaBehaviorProgram *replacement = nullptr,
            std::size_t replacementIndex = 0) const;
        [[nodiscard]] Result<void> ReplaceLuaGeneration(std::size_t index, std::unique_ptr<Gameplay::LuaBehaviorProgram> candidate);
        [[nodiscard]] Result<void> RefreshReplicationSource();

        std::shared_ptr<Gameplay::GameModuleRetirementQueue> moduleRetirements_;
        std::optional<Gameplay::GameModuleRetirementQueue::Reservation> retirementAdmission_;
        std::unique_ptr<Gameplay::LoadedGameModule> nativeModule_;
        std::shared_ptr<const AI::DecisionBlackboardProviderMetadata> decisionUsageMetadata_;
        mutable std::weak_ptr<const AI::DecisionNodeGeneration> decisionAuthoringGeneration_;
        ProjectPackageDecisionNodeSource packageDecisionNodes_;
        bool nativeDecisionAvailable_{};
        Gameplay::GameAssetTypeRegistry missingAssetTypes_{"game.missing"};
        Gameplay::ComponentRegistry missingComponents_;
        std::vector<std::unique_ptr<Gameplay::LuaBehaviorProgram>> luaPrograms_;
        std::vector<std::filesystem::path> luaSources_;
        std::vector<LuaSourceStat> luaSourceStats_;
        std::vector<ProjectLuaDecisionProgramSnapshot> luaDecisionPrograms_;
        std::filesystem::path nativeManifestPath_;
        std::string nativeModuleId_;
        std::uint64_t nativeDescriptorRevision_{};
        std::optional<std::filesystem::file_time_type> nativeManifestWriteTime_;
        Gameplay::BehaviorRegistry registry_;
        std::vector<ProjectGameplayDiagnostic> diagnostics_;
        std::filesystem::path projectRoot_;
        Network::ReplicationMetadataCapturePtr replicationMetadata_;
        std::optional<Network::ReplicationDeclarationAssessment> replicationInspection_;
        std::optional<Error> replicationError_;
        std::optional<std::pair<std::filesystem::file_time_type, std::uintmax_t>> replicationSourceStamp_;
    };
}  // namespace Horo::Editor
