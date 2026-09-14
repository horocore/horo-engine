#pragma once

/**
 * @file GameModuleHost.h
 * @brief Validated ownership and unload ordering for one project gameplay dynamic library.
 */

#include "Horo/Gameplay/GameModule.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Horo::Gameplay {
    class BehaviorRegistry;
    class ComponentRegistry;
    class GameAssetTypeRegistry;
    class GameServiceRegistry;
    class ReplicationRegistrationRegistry;
    class SystemRegistry;

    /** @brief Loaded module whose registry and callable objects are destroyed before its library unloads. */
    class LoadedGameModule final {
    public:
        ~LoadedGameModule();
        LoadedGameModule(const LoadedGameModule &) = delete;
        LoadedGameModule &operator=(const LoadedGameModule &) = delete;
        /** @brief Returns copied compatibility metadata. */
        [[nodiscard]] const std::string &ModuleId() const noexcept;
        /** @brief Returns copied build fingerprint. */
        [[nodiscard]] const std::string &BuildFingerprint() const noexcept;
        /** @brief Returns the complete generated descriptor revision loaded from the module. */
        [[nodiscard]] std::uint64_t DescriptorRevision() const noexcept;
        /** @brief Returns the artifact path actually loaded by the platform library loader. */
        [[nodiscard]] const std::filesystem::path &LoadedArtifactPath() const noexcept;
        /** @brief Returns the frozen descriptor registry while the module is loaded. */
        [[nodiscard]] const BehaviorRegistry &Registry() const noexcept;
        /**
         * @brief Copies native behavior registrations into an unfrozen host registry and binds their module generation.
         * @param destination Host-owned aggregate registry that will expose the copied native factories.
         * @return Success or the first typed registration failure.
         */
        [[nodiscard]] Result<void> ContributeBehaviorsTo(BehaviorRegistry &destination) const;
        /** @brief Returns the frozen project component metadata while the module is loaded. */
        [[nodiscard]] const ComponentRegistry &Components() const noexcept;
        /** @brief Returns frozen project asset metadata and exact-generation processing bindings. */
        [[nodiscard]] const GameAssetTypeRegistry &AssetTypes() const noexcept;
        /** @brief Returns the frozen project service descriptors while the module is loaded. */
        [[nodiscard]] const GameServiceRegistry &Services() const noexcept;
        /** @brief Returns the frozen project system schedule while the module is loaded. */
        [[nodiscard]] const SystemRegistry &Systems() const noexcept;
        /** @brief Returns frozen native replication registrations and generation-safe lease acquisition. */
        [[nodiscard]] const ReplicationRegistrationRegistry &Replication() const noexcept;
        /** @brief Returns active project-scoped services in provider-first order. */
        [[nodiscard]] std::span<const GameplayServiceId> ActiveServices() const noexcept;
        /** @brief Returns capabilities active for module startup and future scene runtimes. */
        [[nodiscard]] std::span<const GameplayCapabilityId> Capabilities() const noexcept;
        /** @brief Returns the cancellation token revoked before module shutdown or replacement. */
        [[nodiscard]] CancellationToken Cancellation() const noexcept;
        /**
         * @brief Cancels and quiesces module-owned work, captures state, and stops callbacks before unload.
         * @return Bounded module snapshot only when the generation proves it is safe to unload.
         */
        [[nodiscard]] Result<GameModuleReloadSnapshot> PrepareReload();
        /**
         * @brief Restores compatible state into this newly started module generation.
         * @param snapshot State captured from the previous compatible generation.
         * @return Success or a typed restore failure.
         */
        [[nodiscard]] Result<void> RestoreReload(const GameModuleReloadSnapshot &snapshot);

    private:
        friend class GameModuleHost;
        struct Impl;
        explicit LoadedGameModule(std::shared_ptr<Impl> impl) noexcept;
        std::shared_ptr<Impl> impl_;
    };

    /** @brief Loader used by editor play sessions and packaged runtime composition. */
    class GameModuleHost final {
    public:
        /**
         * @brief Creates a loader with an explicit immutable host capability set.
         * @param hostCapabilities Capabilities composed by the owning headless or graphical host.
         */
        explicit GameModuleHost(std::vector<GameplayCapabilityId> hostCapabilities = {});
        /**
         * @brief Loads and starts one gameplay candidate after complete compatibility validation.
         * @param libraryPath Absolute dynamic-library artifact path.
         * @param expectation Manifest identity selected by the active engine/toolchain build.
         * @return Owned active module or a typed failure with no remaining project callable.
         */
        [[nodiscard]] Result<std::unique_ptr<LoadedGameModule>> Load(const std::filesystem::path &libraryPath,
                                                                     const GameModuleLoadExpectation &expectation) const;
        /**
         * @brief Copies a candidate to a unique artifact before validating and loading it.
         * @param libraryPath Absolute source artifact produced by the project build.
         * @param shadowRoot Absolute editor-owned directory for reload candidates.
         * @param expectation Manifest identity selected by the active engine/toolchain build.
         * @return Independently loaded candidate; its shadow artifact is removed after unload.
         *
         * A native hot-reload caller must invoke this only after behavior instances and the
         * previous LoadedGameModule have been destroyed. The host does not implicitly sequence
         * generations or make two project modules safe to coexist.
         */
        [[nodiscard]] Result<std::unique_ptr<LoadedGameModule>> LoadShadowCopy(const std::filesystem::path &libraryPath,
                                                                               const std::filesystem::path &shadowRoot,
                                                                               const GameModuleLoadExpectation &expectation) const;

    private:
        std::vector<GameplayCapabilityId> hostCapabilities_;
    };
}  // namespace Horo::Gameplay
