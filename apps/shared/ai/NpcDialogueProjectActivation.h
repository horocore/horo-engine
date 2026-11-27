#pragma once

/** @file NpcDialogueProjectActivation.h @brief Joins authored dialogue settings with explicit host disclosure approval. */

#include "Horo/Application/ProjectAiSettings.h"
#include "NpcDialogueHttpsProvider.h"

#include <filesystem>

namespace Horo::App {
    /** @brief Host-owned approval for one captured project policy and exact remote route. */
    struct NpcDialogueEndpointApproval final {
        std::filesystem::path projectRoot; /**< Absolute host-approved project root. */
        Sha256Digest projectSource;
        NpcDialogueHttpsConfig endpoint;
        std::vector<AI::NpcDialogueFactId> fields; /**< Increasing approved field IDs owned by the host approval. */
        CancellationToken lifetime;                /**< Required live disclosure authority for remote activation. */
    };

    /** @brief Owned project activation. Empty binding means authored dialogue runs offline. */
    class NpcDialogueProjectActivation final {
    public:
        /** @brief Reads the canonical project AI profile and admits only project/host-shared fields.
         * A rejected remote approval or endpoint leaves the authored fallback available offline.
         * @param projectRoot Authorized root from which the activation captures the exact project source.
         * @param generation Current host provider activation generation.
         * @param approval Explicit host grant, or null to activate offline without a provider.
         * @return No value when dialogue is absent, an offline/online activation, or a typed project or generation error.
         */
        [[nodiscard]] static Result<std::optional<NpcDialogueProjectActivation>> Create(
            const std::filesystem::path &projectRoot, std::uint64_t generation, const NpcDialogueEndpointApproval *approval);

        /** @brief Borrows the activation's bounded disclosure grant on the host owner lane. */
        [[nodiscard]] AI::NpcDialogueGrant Grant() const noexcept;
        /** @brief Returns the admitted endpoint binding, or absence for offline activation. */
        [[nodiscard]] const std::optional<AI::NpcDialogueProviderBinding> &Binding() const noexcept;
        /** @brief Borrows the admitted interaction catalog until the activation is moved or retired. */
        [[nodiscard]] std::span<const Application::ProjectNpcDialogueTrigger> Triggers() const noexcept;
        /** @brief Borrows the authored local presentation plan, if the project supplies one. */
        [[nodiscard]] const std::optional<Application::ProjectNpcDialoguePresentation> &Presentation() const noexcept;

    private:
        AI::NpcDialogueRoute route_;
        std::uint64_t generation_{};
        std::array<AI::NpcDialogueFactId, AI::NpcDialogueTurn::MaximumFacts> fields_{};
        std::size_t count_{};
        std::optional<AI::NpcDialogueProviderBinding> binding_;
        std::vector<Application::ProjectNpcDialogueTrigger> triggers_;
        std::optional<Application::ProjectNpcDialoguePresentation> presentation_;
    };
}  // namespace Horo::App
