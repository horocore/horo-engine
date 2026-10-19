#pragma once

/** @file NavigationProjectBakeSource.h
 * @brief Owned canonical project/registry/Scene/definition capture for navigation automation.
 */

#include "Horo/Application/NavigationProjectSettings.h"
#include "Horo/Application/ProjectSourceDocument.h"
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Navigation/NavigationBakeSourceValidation.h"
#include "Horo/Navigation/NavigationDefinition.h"
#include "Horo/Physics/PhysicsMaterialAsset.h"
#include "Horo/Platform/ProjectReadFiles.h"
#include "Horo/Scene/SceneSource.h"

namespace Horo::Application {
    /** @brief Finite whole-capture limits independent of interactive source queries. */
    struct NavigationProjectSourceLimits final {
        std::size_t maximumScenes{256};
        std::size_t maximumDefinitions{256};
        std::size_t maximumCapturedBytes{64U * 1024U * 1024U};
        Platform::ProjectInventoryLimits inventory;
        std::size_t maximumMaterials{4096};
    };

    /** @brief Canonically decoded authored Scene with its stable asset identity and exact source evidence. */
    struct NavigationSceneBakeSource final {
        Assets::AssetId asset;
        ProjectPath path;
        Sha256Digest digest;
        SceneSource::SceneSourceDocument document;
    };

    /** @brief Sole NAVDEF01 definition decoder output; source paths never become definition identity. */
    struct NavigationDefinitionBakeSource final {
        Assets::AssetId asset;
        ProjectPath path;
        Sha256Digest digest;
        std::shared_ptr<const Navigation::NavigationDefinition> definition;
    };

    /** @brief Canonically decoded physical material and exact bytes retained by the same project read authority. */
    struct NavigationMaterialBakeSource final {
        Assets::AssetId asset;
        ProjectPath path;
        Sha256Digest digest;
        Physics::PhysicsMaterialAsset material;
    };

    /** @brief Exact optional package source, retained for the package owner's separate restore/admission checks. */
    struct NavigationPackageBakeSource final {
        std::optional<std::string> intent;
        std::optional<std::string> lock;
    };

    namespace NavigationBakeDetail {
        struct ProjectSourceState;
    }

    /** @brief Immutable authored inputs from one explicitly authorized project opening.
     * @details Capture is synchronous tooling/background work, never frame work. It performs no migration,
     * package activation, provider selection, cache/index writes or publication. Geometry capture, package
     * admission and the bake service's source/publication fence remain required before cooking.
     */
    class NavigationProjectBakeSource final : public Navigation::INavigationBakeSourceValidation {
    public:
        /** @brief Captures canonical source through retained root handles and rechecks the complete capture before returning.
         * @param files Explicit host-authorized root capability.
         * @param context Exact opening identity/generation, cancellation and invocation deadline. The retained
         * authorityStopped callback must own its state or outlive this source. Each capture/verification also has
         * a finite inventory-duration ceiling; later verification never renews the invocation deadline.
         * @param compatibility Actual host project compatibility result, required to allow current source only.
         * @param limits Reducible count/byte/inventory ceilings; excess fails rather than truncating.
         * @return Owned source with every Scene and definition in the complete registry, or a typed failure.
         * @details Unknown Scene components are retained as explicit decoder evidence for the geometry owner;
         * they are not silently treated as supported geometry. Package bytes alone are not restore/trust proof.
         */
        [[nodiscard]] static Result<std::shared_ptr<const NavigationProjectBakeSource>> Capture(
            std::shared_ptr<const Platform::IProjectReadFiles> files, Platform::ProjectReadContext context,
            const ProjectCompatibilitySnapshot &compatibility, const NavigationProjectSourceLimits &limits = {});

        /** @brief Rechecks registry identities, source bytes and optional-control presence through the same root owner.
         * @return Success for exact retained inputs, or typed stale/cancel/deadline/read failure. Does not acquire publication authority.
         */
        [[nodiscard]] Result<void> VerifyCurrent() const;
        /** @brief Rechecks the same retained source for a background operation without extending its authority.
         * @param cancellation Additional operation cancellation, observed throughout inventory and file reads.
         * @return Exact source result with cancellation classified as BakeInputCancelled and its original platform cause;
         * captured trust/generation and deadlines remain unchanged.
         */
        [[nodiscard]] Result<void> ValidateCurrent(const CancellationToken &cancellation) const override;
        /** @brief Returns the canonical project document. @return Immutable metadata and source values. */
        [[nodiscard]] const ProjectSourceDocument &Project() const noexcept;
        /** @brief Returns the exact project navigation policy. @return Immutable settings, including authoritative profile. */
        [[nodiscard]] const NavigationProjectSettings &Settings() const noexcept;
        /** @brief Returns the complete registry pinned by this source. @return Snapshot independent of later publication. */
        [[nodiscard]] const Assets::AssetRegistrySnapshot &Registry() const noexcept;
        /** @brief Returns complete decoded Scenes in stable asset order. @return Owned immutable source view. */
        [[nodiscard]] std::span<const NavigationSceneBakeSource> Scenes() const noexcept;
        /** @brief Returns complete decoded definitions in stable asset order. @return Owned immutable definition view. */
        [[nodiscard]] std::span<const NavigationDefinitionBakeSource> Definitions() const noexcept;
        /** @brief Returns complete validated physical materials in stable asset order. @return Owned immutable source view. */
        [[nodiscard]] std::span<const NavigationMaterialBakeSource> Materials() const noexcept;
        /** @brief Returns exact optional package bytes without asserting restore or trust. @return Immutable source view. */
        [[nodiscard]] const NavigationPackageBakeSource &Packages() const noexcept;

    private:
        /** @brief Retains one completely captured, unpublished source owner. @param state Owned immutable capture state. */
        explicit NavigationProjectBakeSource(std::shared_ptr<NavigationBakeDetail::ProjectSourceState> state);
        std::shared_ptr<NavigationBakeDetail::ProjectSourceState> state_;
    };
}  // namespace Horo::Application
