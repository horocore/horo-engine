#pragma once
/** @file GameplayPackageModuleHost.h
 * @brief Explicit package-to-gameplay SDK composition with exact permission and generation ownership.
 */
#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Gameplay/LuaDecisionNode.h"
#include "Horo/Gameplay/LuaPerceptionSense.h"
#include "Horo/Packages/PackageModuleHost.h"
#include "Horo/Security/ArtifactSignature.h"

#include <functional>

namespace Horo::Packages {
    /** @brief Exact locked package attribution accompanying its real acquired node generation. */
    struct PackageDecisionNodeGeneration final {
        HoroPackageId package;
        PackageVersion version;
        Sha256Digest artifact;
        std::uint64_t activationGeneration{};
        std::shared_ptr<const AI::DecisionNodeGeneration> nodes;
    };

    /** @brief Exact verified package attribution paired with its actual native custom-sense generation. */
    struct PackagePerceptionSenseGeneration final {
        HoroPackageId package;
        PackageVersion version;
        Sha256Digest artifact;
        std::uint64_t activationGeneration{};
        std::shared_ptr<const AI::PerceptionSenseGeneration> senses;
    };

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
         * @param package Owning package. @param selectedModule Exact module identity. @param generation Exact activation generation.
         * @return Read-only SDK owner, or null after withdrawal/mismatch. Release on any thread; owner-lane drain performs teardown.
         * @pre Application exposes this only after the matching PackageLifecycleService snapshot publishes.
         */
        [[nodiscard]] std::shared_ptr<const Gameplay::LoadedGameModule> Acquire(const HoroPackageId &package,
                                                                                std::string_view selectedModule,
                                                                                std::uint64_t generation) const;
        /** @brief Acquires generated contributions from the exact trusted, locked and live package activation.
         * Package versions remain selected by the existing resolver/lock; no highest-version or load-order fallback occurs.
         */
        [[nodiscard]] Result<PackageDecisionNodeGeneration> AcquireDecisionNodes(const HoroPackageId &package, std::string_view moduleId,
                                                                                 std::uint64_t generation) const;
        /** @brief Acquires custom senses from the selected verified native role, retaining actual owner revocation.
         * @param package Exact installed package. @param module Selected gameplay module. @param generation Current role generation.
         * @return Locked package attribution and native sense lease, or typed stale/unavailable failure.
         */
        [[nodiscard]] Result<PackagePerceptionSenseGeneration> AcquirePerceptionSenses(const HoroPackageId &package,
                                                                                       std::string_view module,
                                                                                       std::uint64_t generation) const;
        /** @brief Compiles actual declared package script bytes under that same native activation's revocation/lifetime.
         * @param source Exact declared .horo_script inventory identity, never an arbitrary physical file.
         * @param type Canonical sidecar/selection identity. @param provider Stable provider identity.
         * @param descriptorVersion Node contract version, distinct from the actual locked package version.
         * @details No script invocation provider or manifest flag is treated as an executable node implementation.
         */
        [[nodiscard]] Result<std::unique_ptr<Gameplay::LuaDecisionNodeProgram>> CompileDecisionNode(
            const HoroPackageId &package, std::string_view moduleId, std::uint64_t generation, const PackagePath &source,
            AI::DecisionNodeTypeId type, AI::DecisionProviderId provider, std::uint32_t descriptorVersion,
            Gameplay::LuaBehaviorLimits limits = {}) const;

        /** @brief Compiles a declared package spatial predicate under the actual native perception parent's lifetime.
         * @param package Exact installed package. @param module Selected verified role. @param generation Current activation.
         * @param source Declared .horo_script inventory path. @param contract Host-selected Script identities and bounds.
         * @param limits Enforced VM memory and whole-evaluation instruction ceilings.
         * @return Owned program or typed source, admission or VM failure; no arbitrary filesystem source is read.
         */
        [[nodiscard]] Result<std::unique_ptr<Gameplay::LuaPerceptionSenseProgram>> CompilePerceptionSense(
            const HoroPackageId &package, std::string_view module, std::uint64_t generation, const PackagePath &source,
            AI::PerceptionSenseContract contract, Gameplay::LuaBehaviorLimits limits = {256U * 1024U, 4096}) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Packages
