#pragma once
/** @file GameplayPackageModuleHost.h
 * @brief Explicit package-to-gameplay SDK composition with exact permission and generation ownership.
 */
#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Packages/PackageModuleHost.h"
#include "Horo/Security/ArtifactSignature.h"

#include <functional>

namespace Horo::Packages {
    /** @brief Exact application-selected gameplay SDK identity and per-generation world authority. */
    struct GameplayPackageModuleBinding final {
        HoroPackageId package;
        std::string module;
        std::string buildFingerprint;
        std::uint64_t descriptorRevision{};
        std::vector<Gameplay::GameplayCapabilityId> capabilities;
        std::function<Result<std::shared_ptr<Gameplay::GameplayPhysicsContext>>(const VerifiedPackageModule &)> physics;
    };

    /** @brief Host-private staging and mandatory native integrity authority; no backend is discovered. */
    struct GameplayPackageHostConfiguration final {
        std::filesystem::path temporaryRoot;
        std::shared_ptr<const Security::NativeArtifactGate> artifactGate;
    };

    /** @brief Dedicated owner-lane composition adapter; package lifecycle has no dependency on Gameplay.
     * Same-module replacement requires explicit disable and successful retirement drain before a new SDK generation starts.
     */
    class GameplayPackageModuleHost final : public IPackageModuleHost {
    public:
        /** @brief Composes exact runtime owners without invoking factories.
         * @param configuration Private staging and native verification policy.
         * @param bindings Exact package/module SDK identities and host-provided world capabilities.
         */
        GameplayPackageModuleHost(GameplayPackageHostConfiguration configuration, std::vector<GameplayPackageModuleBinding> bindings);
        ~GameplayPackageModuleHost() override;
        /** @copydoc IPackageModuleHost::Identity */
        [[nodiscard]] std::string_view Identity() const noexcept override;
        /** @copydoc IPackageModuleHost::Supports */
        [[nodiscard]] bool Supports(Extensions::ExtensionModuleRole role) const noexcept override;
        /** @copydoc IPackageModuleHost::Activate */
        [[nodiscard]] Result<std::unique_ptr<IPackageModuleActivation>> Activate(
            const VerifiedPackageModule &module, std::span<const std::shared_ptr<IPackageModuleActivation>> dependencies,
            const CancellationToken &cancellation) override;
        /** @brief Acquires an exact live SDK module; the lease pins verified bytes and prevents finalization while borrowed.
         * @param package Owning package. @param module Exact module identity. @param generation Exact activation generation.
         * @return Read-only SDK owner, or null after withdrawal/mismatch. Release on any thread; owner-lane drain performs teardown.
         * @pre Application exposes this only after the matching PackageLifecycleService snapshot publishes.
         */
        [[nodiscard]] std::shared_ptr<const Gameplay::LoadedGameModule> Acquire(const HoroPackageId &package, std::string_view module,
                                                                                std::uint64_t generation) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Packages
