#pragma once

/**
 * @file GameModule.h
 * @brief Exact-SDK-generation native project gameplay module boundary.
 */

#include "Horo/Gameplay/Behavior.h"
#include "Horo/Gameplay/GameAsset.h"
#include "Horo/Gameplay/GameplayRegistration.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#define HORO_GAME_EXPORT __declspec(dllexport)
#else
#define HORO_GAME_EXPORT __attribute__((visibility("default")))
#endif

namespace Horo::Gameplay {
    class ComponentRegistry;
    class GameAssetTypeRegistry;
    class GameServiceRegistry;
    class ReplicationRegistrationRegistry;
    class SystemRegistry;

    inline constexpr std::uint32_t GameplaySdkBoundaryVersion = 6;
    inline constexpr std::uint32_t GameplayDescriptorBundleSchemaVersion = 1;
    inline constexpr std::uint32_t GameModuleReloadSnapshotSchemaVersion = 1;
    inline constexpr std::size_t MaximumGeneratedBehaviorDescriptors = 4096;
    inline constexpr std::size_t MaximumGeneratedDescriptorDiagnostics = 256;
    inline constexpr std::size_t MaximumGeneratedDiagnosticCodeBytes = 160;
    inline constexpr std::size_t MaximumGeneratedDiagnosticMessageBytes = 1024;
    inline constexpr std::size_t MaximumGameModuleReloadStateBytes = 16U * 1024U * 1024U;
    inline constexpr std::string_view GetGameModuleDescriptorSymbol = "GetGameModuleDescriptor";
    inline constexpr std::string_view GetGameplayDescriptorBundleSymbol = "GetGameplayDescriptorBundle";
    inline constexpr std::string_view CreateGameModuleSymbol = "CreateGameModule";
    inline constexpr std::string_view DestroyGameModuleSymbol = "DestroyGameModule";

    /** @brief Returns the exact compiler, platform, configuration, and SDK fingerprint required by this host. */
    [[nodiscard]] std::string_view CurrentGameplayBuildFingerprint() noexcept;

    /** @brief Static compatibility metadata read before any project gameplay object is created. */
    struct GameModuleDescriptor {
        std::uint32_t structSize{sizeof(GameModuleDescriptor)};
        std::uint32_t sdkBoundaryVersion{GameplaySdkBoundaryVersion};
        const char *moduleId{};
        const char *buildFingerprint{};
    };

    /** @brief Narrow generation-scoped runtime capabilities supplied after project services start. */
    struct GameRuntimeContext {
        CancellationToken cancellation;                     /**< Revoked before module shutdown or replacement. */
        std::span<const GameplayServiceId> activeServices;  /**< Active project-scoped services in dependency order. */
        std::span<const GameplayCapabilityId> capabilities; /**< Explicit host and project-service capability grants. */
    };

    /** @brief Module-global state captured only after owned work and callbacks are quiescent. */
    struct GameModuleReloadSnapshot {
        std::uint32_t schemaVersion{GameModuleReloadSnapshotSchemaVersion};
        std::vector<std::byte> payload;
    };

    /** @brief Declarative registration capabilities exposed before gameplay startup. */
    struct GameRegistrationContext {
        std::string_view moduleId;
        ComponentRegistry &components;
        SystemRegistry &systems;
        GameServiceRegistry &services;
        GameAssetTypeRegistry &assetTypes;
        ReplicationRegistrationRegistry &replication;
    };

    /** @brief Project-owned module lifecycle valid only for one exact compatible SDK generation. */
    class IGameModule {
    public:
        virtual ~IGameModule() = default;
        /**
         * @brief Registers project-owned component, system, service, and asset metadata without activating runtime behavior.
         * @param context Host-owned open registration transaction.
         * @return Success or a typed validation error that prevents startup.
         */
        [[nodiscard]] virtual Result<void> Register(GameRegistrationContext &context) = 0;
        /**
         * @brief Starts module-owned runtime services after every registry is frozen.
         * @param context Narrow runtime capabilities for the active module generation.
         * @return Success or a typed startup error.
         */
        [[nodiscard]] virtual Result<void> Start(GameRuntimeContext &context) = 0;
        /**
         * @brief Stops module-owned runtime services before registered metadata and code unload.
         * @param context Runtime context used by the active generation.
         */
        virtual void Stop(GameRuntimeContext &context) noexcept = 0;
        /**
         * @brief Closes callback/job admission, joins owned work, and captures reloadable module state.
         * @param context Active generation context whose cancellation token has been revoked by the host.
         * @return Bounded snapshot only when the module proves it is safe to unload; otherwise restart-required.
         */
        [[nodiscard]] virtual Result<GameModuleReloadSnapshot> PrepareReload(GameRuntimeContext &context);
        /**
         * @brief Restores compatible module-global state after the replacement generation starts.
         * @param snapshot Snapshot captured from the previous generation.
         * @param context Active replacement generation context.
         * @return Success or a typed restore failure that triggers rollback.
         */
        [[nodiscard]] virtual Result<void> RestoreReload(const GameModuleReloadSnapshot &snapshot, GameRuntimeContext &context);
    };

    using GetGameModuleDescriptorFunction = const GameModuleDescriptor *(*)() noexcept;
    using CreateGameModuleFunction = IGameModule *(*)() noexcept;
    using DestroyGameModuleFunction = void (*)(IGameModule *) noexcept;

    /** @brief Module-owned implementation binding paired with one generated behavior identity. */
    struct GeneratedBehaviorFactoryBinding {
        BehaviorTypeId typeId;
        BehaviorFactoryBinding factory;
    };

    /** @brief Bounded code and context emitted when descriptor generation cannot produce an activatable snapshot. */
    struct GeneratedDescriptorDiagnostic {
        const char *code{};
        const char *message{};
    };

    /** @brief Module-owned lifecycle callbacks validated before project code activation. */
    struct GameModuleLifecycleCallbacks {
        CreateGameModuleFunction create{};
        DestroyGameModuleFunction destroy{};
    };

    /** @brief Complete generated behavior snapshot tied to one module build fingerprint. */
    struct GeneratedGameplayDescriptorBundle {
        std::uint32_t structSize{sizeof(GeneratedGameplayDescriptorBundle)};
        std::uint32_t schemaVersion{GameplayDescriptorBundleSchemaVersion};
        std::uint32_t sdkBoundaryVersion{GameplaySdkBoundaryVersion};
        const char *moduleId{};
        const char *buildFingerprint{};
        std::uint64_t descriptorRevision{};
        const BehaviorDescriptor *behaviors{};
        std::size_t behaviorCount{};
        const GeneratedBehaviorFactoryBinding *nativeFactoryBindings{};
        std::size_t nativeFactoryBindingCount{};
        const GeneratedDescriptorDiagnostic *diagnostics{};
        std::size_t diagnosticCount{};
        GameModuleLifecycleCallbacks lifecycle;
    };

    using GetGameplayDescriptorBundleFunction = const GeneratedGameplayDescriptorBundle *(*)() noexcept;

    /** @brief Exact manifest identity that a candidate must satisfy before its lifecycle starts. */
    struct GameModuleLoadExpectation {
        std::string_view moduleId;
        std::string_view buildFingerprint;
        std::uint64_t descriptorRevision{};
    };

    /**
     * @brief Validates static module compatibility metadata before generated records are read.
     * @param descriptor Candidate module descriptor.
     * @param expectation Identity selected by the validated artifact manifest.
     * @return Success or a stable module-descriptor compatibility error.
     */
    [[nodiscard]] Result<void> ValidateGameModuleDescriptor(const GameModuleDescriptor &descriptor,
                                                            const GameModuleLoadExpectation &expectation);

    /**
     * @brief Validates a complete generated snapshot and all module-owned bindings before activation.
     * @param bundle Candidate generated descriptor bundle.
     * @param expectation Identity selected by the validated artifact manifest.
     * @return Success or a stable bundle validation error.
     */
    [[nodiscard]] Result<void> ValidateGeneratedGameplayDescriptorBundle(const GeneratedGameplayDescriptorBundle &bundle,
                                                                         const GameModuleLoadExpectation &expectation);
}  // namespace Horo::Gameplay
