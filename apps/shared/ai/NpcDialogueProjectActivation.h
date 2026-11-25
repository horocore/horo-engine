#pragma once

/** @file NpcDialogueProjectActivation.h @brief Joins authored dialogue settings with explicit host disclosure approval. */

#include "Horo/Application/ProjectAiSettings.h"
#include "NpcDialogueHttpsProvider.h"

namespace Horo::App {
    /** @brief Host-owned approval for one captured project policy and exact remote route. */
    struct NpcDialogueEndpointApproval final {
        Sha256Digest projectSource;
        NpcDialogueHttpsConfig endpoint;
        std::span<const AI::NpcDialogueFactId> fields; /**< Increasing approved field IDs; borrowed for activation only. */
    };

    /** @brief Owned project activation. Empty binding means authored dialogue runs offline. */
    class NpcDialogueProjectActivation final {
    public:
        /** @brief Reads the canonical project AI profile and admits only project/host-shared fields.
         * @param project Captured project source from the authorized root.
         * @param generation Current host provider activation generation.
         * @param approval Explicit host grant, or null to activate offline without a provider.
         * @return No value when dialogue is absent, an offline/online activation, or a typed invalid configuration error.
         */
        [[nodiscard]] static Result<std::optional<NpcDialogueProjectActivation>> Create(
            const Application::ProjectSourceDocument &project, std::uint64_t generation, const NpcDialogueEndpointApproval *approval);

        /** @brief Borrows the activation's bounded disclosure grant on the host owner lane. */
        [[nodiscard]] AI::NpcDialogueGrant Grant() const noexcept;
        /** @brief Returns the admitted endpoint binding, or absence for offline activation. */
        [[nodiscard]] const std::optional<AI::NpcDialogueProviderBinding> &Binding() const noexcept;
        /** @brief Borrows the admitted interaction catalog until the activation is moved or retired. */
        [[nodiscard]] std::span<const Application::ProjectNpcDialogueTrigger> Triggers() const noexcept;

    private:
        AI::NpcDialogueRoute route_;
        std::uint64_t generation_{};
        std::array<AI::NpcDialogueFactId, AI::NpcDialogueTurn::MaximumFacts> fields_{};
        std::size_t count_{};
        std::optional<AI::NpcDialogueProviderBinding> binding_;
        std::vector<Application::ProjectNpcDialogueTrigger> triggers_;
    };
}  // namespace Horo::App
