#pragma once

/**
 * @file GameModuleHost.h
 * @brief Validated ownership and unload ordering for one project gameplay dynamic library.
 */

#include "Horo/Gameplay/GameModule.h"
#include "Horo/Gameplay/PersistenceInstallation.h"
#include "Horo/Gameplay/SaveGameplayPersistence.h"

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
    class GameEventRegistry;

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
        /** @brief Acquires one declared durable adapter while exact-generation runtime admission remains open.
         * @param participant Stable identity registered through GameRegistrationContext::persistence.
         * @return Pinned adapter for explicit SaveParticipationClient registration, or typed missing/retiring error.
         * @pre Called by the runtime owner at a quiescent composition boundary.
         */
        [[nodiscard]] Result<std::shared_ptr<Runtime::GameplayPersistenceAdapter>> AcquirePersistence(
            const Runtime::SaveParticipantId &participant) const;
        /** @brief Acquires unforgeable installed-content evidence from an actual frozen persistence registration.
         * @pre The runtime owner serializes acquisition and subsequent owner-thread admission with this module's reload/shutdown.
         * @param participant Exact registered durable owner.
         * @return Generation-pinned installation, or typed missing/revoked admission failure; no callback is invoked.
         */
        [[nodiscard]] Result<Runtime::GameplayPersistenceInstallation> AcquireInstalledPersistence(
            const Runtime::SaveParticipantId &participant) const;
        /** @brief Returns frozen event callbacks; acquisition pins this exact native module generation. */
        [[nodiscard]] const GameEventRegistry &Events() const noexcept;
        /** @brief Returns active project-scoped services in provider-first order. */
        [[nodiscard]] std::span<const GameplayServiceId> ActiveServices() const noexcept;
        /** @brief Returns capabilities active for module startup and future scene runtimes. */
        [[nodiscard]] std::span<const GameplayCapabilityId> Capabilities() const noexcept;
        /** @brief Returns the cancellation token revoked before module shutdown or replacement. */
        [[nodiscard]] CancellationToken Cancellation() const noexcept;
        /** @brief Returns this module generation's explicit Physics binding, if composed by the host. */
        [[nodiscard]] std::shared_ptr<const GameplayPhysicsContext> PhysicsContext() const noexcept;
        /** @brief Returns this module generation navigation grant. @return Revocable host context. */
        [[nodiscard]] std::shared_ptr<const GameplayNavigationContext> NavigationContext() const noexcept;
        /**
         * @brief Cancels and quiesces module-owned work, captures state, and stops callbacks before unload.
         * @return Bounded module snapshot only when the generation proves it is safe to unload.
         */
        [[nodiscard]] Result<GameModuleReloadSnapshot> PrepareReload();
        /** @brief Closes new runtime admission and requests cancellation without destroying busy callback owners.
         * @pre Called on the module composition owner lane.
         */
        void RequestRetirement() noexcept;
        /** @brief Finalizes an inactive/retired generation only after all exact runtime leases drain.
         * @return Success after owner-lane native teardown, or a typed busy/quiescence error retaining this owner.
         * @pre The caller retains the module and its artifact on failure and retries on the composition owner lane.
         * @details Ordinary retirement calls Stop and destroys services/module after leases drain; it does not request or validate a reload
         * snapshot.
         */
        [[nodiscard]] Result<void> TryRetire();
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

    /** @brief Actual candidate resource owner, including rejected partial Register/Start work that still needs retirement. */
    struct GameModuleLoadCandidate final {
        std::unique_ptr<LoadedGameModule> owner;
        std::optional<Error> diagnostic; /**< Present when initialization failed; never an active module publication. */
    };

    /** @brief Loader used by editor play sessions and packaged runtime composition. */
    class GameModuleHost final {
    public:
        /**
         * @brief Creates a loader with an explicit immutable host capability set.
         * @param hostCapabilities Capabilities composed by the owning headless or graphical host.
         */
        explicit GameModuleHost(std::vector<GameplayCapabilityId> hostCapabilities = {});
        /** @brief Creates a loader bound to one explicitly admitted module and play world.
         * @param hostCapabilities Inert host grants used by registration validation.
         * @param physics Exact per-module binding, revoked before module shutdown; cannot be reused after retirement.
         */
        GameModuleHost(std::vector<GameplayCapabilityId> hostCapabilities, std::shared_ptr<GameplayPhysicsContext> physics);
        /** @brief Bind an explicit navigation principal to the next module generation.
         * @param hostCapabilities Immutable host grants. @param physics Optional Physics binding.
         * @param navigation Exact module/Scene navigation binding; revoked before module reload callbacks.
         */
        GameModuleHost(std::vector<GameplayCapabilityId> hostCapabilities, std::shared_ptr<GameplayPhysicsContext> physics,
                       std::shared_ptr<GameplayNavigationContext> navigation);
        /** @brief Checks native exports and exact SDK/generated identity without invoking module factories or lifecycle.
         * @param libraryPath Absolute candidate artifact. @param expectation Exact host-selected compatibility identity.
         * @return Success or the original load/descriptor failure. No initialized module or callback owner is produced.
         */
        [[nodiscard]] Result<void> ValidateArtifact(const std::filesystem::path &libraryPath,
                                                    const GameModuleLoadExpectation &expectation) const;
        /** @brief Loads through the SDK boundary while retaining partial-initialization resources on failure.
         * @param libraryPath Exact host-owned immutable artifact. @param expectation Exact approved SDK/module identity.
         * @return Early validation error, or an owned attempt whose diagnostic must be checked before publication.
         * @pre Retain an attempt with diagnostic until TryRetire succeeds; quiesce prior runtime generations before replacement.
         */
        [[nodiscard]] Result<GameModuleLoadCandidate> Load(const std::filesystem::path &libraryPath,
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
        [[nodiscard]] Result<GameModuleLoadCandidate> LoadShadowCopy(const std::filesystem::path &libraryPath,
                                                                     const std::filesystem::path &shadowRoot,
                                                                     const GameModuleLoadExpectation &expectation) const;

    private:
        /** @brief Load one validated artifact with explicit host-owned shadow cleanup from the first code-image lease.
         * @param libraryPath Absolute prepared artifact. @param expectation Exact module compatibility.
         * @param ownedShadow True only for the unique canonical artifact created by LoadShadowCopy.
         * @return Owned candidate, including failed initialization, or early failure with deferred native image cleanup.
         */
        [[nodiscard]] Result<GameModuleLoadCandidate> LoadArtifact(const std::filesystem::path &libraryPath,
                                                                   const GameModuleLoadExpectation &expectation, bool ownedShadow) const;
        std::vector<GameplayCapabilityId> hostCapabilities_;
        std::shared_ptr<GameplayPhysicsContext> physics_;
        std::shared_ptr<GameplayNavigationContext> navigation_;
    };
}  // namespace Horo::Gameplay
