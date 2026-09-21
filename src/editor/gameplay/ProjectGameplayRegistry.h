#pragma once

/**
 * @file ProjectGameplayRegistry.h
 * @brief Project-scoped discovery and ownership of editor-visible gameplay behaviors.
 */

#include "Horo/Gameplay/BehaviorRegistry.h"
#include "Horo/Gameplay/ComponentRegistry.h"
#include "Horo/Gameplay/GameAssetTypeRegistry.h"
#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Gameplay/LuaBehavior.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

    /** @brief Exact last-good Lua generation retained across one native reload transaction. */
    struct ProjectLuaGenerationSnapshot {
        std::vector<ProjectLuaProgramSnapshot> programs;
    };

    /** @brief Owns discovered project behavior programs and their frozen registry snapshot. */
    class ProjectGameplayRegistry final {
        struct ConstructionToken {};

    public:
        /** @brief Discovers the native module and bounded Lua behavior assets for a project. */
        [[nodiscard]] static std::unique_ptr<ProjectGameplayRegistry> Discover(const std::filesystem::path &projectRoot);
        /**
         * @brief Loads one host-preserved old artifact for transactional rollback.
         * @param projectRoot Absolute project root used for manifest watching and shadow storage.
         * @param artifact Preserved library path and exact old-generation identity.
         * @param luaGeneration Exact in-memory Lua generation captured before retirement.
         * @return Owning combined registry with diagnostics when restoration cannot be prepared.
         */
        [[nodiscard]] static std::unique_ptr<ProjectGameplayRegistry> DiscoverRollback(const std::filesystem::path &projectRoot,
                                                                                       const NativeGameplayRollbackArtifact &artifact,
                                                                                       const ProjectLuaGenerationSnapshot &luaGeneration);
        /**
         * @brief Loads the current native manifest with an exact last-good Lua generation.
         * @param projectRoot Absolute project root used for native discovery and shadow storage.
         * @param luaGeneration Immutable Lua generation captured before native retirement.
         * @return Owning combined registry without rereading Lua source files.
         */
        [[nodiscard]] static std::unique_ptr<ProjectGameplayRegistry> DiscoverNativeGeneration(
            const std::filesystem::path &projectRoot, const ProjectLuaGenerationSnapshot &luaGeneration);

        explicit ProjectGameplayRegistry(ConstructionToken);

        ProjectGameplayRegistry(const ProjectGameplayRegistry &) = delete;
        ProjectGameplayRegistry &operator=(const ProjectGameplayRegistry &) = delete;

        /** @brief Returns the immutable registry used by authoring and new play sessions. */
        [[nodiscard]] const Gameplay::BehaviorRegistry &Registry() const noexcept;
        /** @brief Returns current game-owned asset metadata or an empty frozen missing-code registry. */
        [[nodiscard]] const Gameplay::GameAssetTypeRegistry &AssetTypes() const noexcept;
        /** @brief Returns current game-owned component metadata or an empty frozen missing-code registry. */
        [[nodiscard]] const Gameplay::ComponentRegistry &Components() const noexcept;
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
        /** @brief Reports whether a native generation is loaded. */
        [[nodiscard]] bool HasNativeModule() const noexcept;

    private:
        struct LuaSourceStat {
            std::filesystem::file_time_type sourceWriteTime;
            std::filesystem::file_time_type metadataWriteTime;
            std::uintmax_t sourceSize{};
            std::uintmax_t metadataSize{};

            [[nodiscard]] auto operator<=>(const LuaSourceStat &) const = default;
        };

        [[nodiscard]] static LuaSourceStat ReadLuaSourceStat(const std::filesystem::path &source, std::error_code &error);

        void DiscoverNativeModule(const std::filesystem::path &projectRoot);

        [[nodiscard]] bool PrepareNativeManifestPath(const std::filesystem::path &projectRoot, const std::filesystem::path &manifestPath);

        void LoadNativeModule(const std::filesystem::path &projectRoot, const std::filesystem::path &artifactPath,
                              std::string_view moduleId, std::uint64_t descriptorRevision);

        void DiscoverLuaPrograms(const std::filesystem::path &projectRoot);
        void InstallLuaGeneration(const ProjectLuaGenerationSnapshot &snapshot);

        std::unique_ptr<Gameplay::LoadedGameModule> nativeModule_;
        Gameplay::GameAssetTypeRegistry missingAssetTypes_{"game.missing"};
        Gameplay::ComponentRegistry missingComponents_;
        std::vector<std::unique_ptr<Gameplay::LuaBehaviorProgram>> luaPrograms_;
        std::vector<std::filesystem::path> luaSources_;
        std::vector<LuaSourceStat> luaSourceStats_;
        std::filesystem::path nativeManifestPath_;
        std::string nativeModuleId_;
        std::uint64_t nativeDescriptorRevision_{};
        std::optional<std::filesystem::file_time_type> nativeManifestWriteTime_;
        Gameplay::BehaviorRegistry registry_;
        std::vector<ProjectGameplayDiagnostic> diagnostics_;
    };
}  // namespace Horo::Editor
