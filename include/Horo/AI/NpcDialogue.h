#pragma once

/** @file NpcDialogue.h @brief Approved provider payloads and presentation-only NPC dialogue outcomes. */

#include "Horo/AI/DecisionIdentity.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/JobSystem.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace Horo::AI {
    /** @brief Current project and HTTPS dialogue interchange version. */
    inline constexpr std::uint32_t NpcDialogueWireVersion = 1;

    /** @brief Stable identifier for a host-approved destination. */
    using NpcDialogueDestinationId = AiStableIdentity<struct NpcDialogueDestinationIdentityTag>;
    /** @brief Stable authored context field. */
    using NpcDialogueFactId = AiStableIdentity<struct NpcDialogueFactIdentityTag>;
    /** @brief Stable authored value token for a context field. */
    using NpcDialogueValueId = AiStableIdentity<struct NpcDialogueValueIdentityTag>;
    /** @brief Stable authored dialogue cue for presentation. */
    using NpcDialogueCueId = AiStableIdentity<struct NpcDialogueCueIdentityTag>;

    /** @brief Stable authored token pair, with no free text or runtime object handles. */
    struct NpcDialogueFact final {
        NpcDialogueFactId field;
        NpcDialogueValueId value;
    };

    /** @brief Exact destination and provider schema identity selected by the host. */
    struct NpcDialogueRoute final {
        DecisionProviderId provider;
        NpcDialogueDestinationId destination;
        std::uint32_t version{};
        bool operator==(const NpcDialogueRoute &) const noexcept = default;
    };

    /** @brief Local-only generations used by the presentation owner to fence a response. */
    struct NpcDialogueRequestIdentity final {
        AgentHandle agent;
        std::uint64_t providerGeneration{};
        std::uint64_t turn{};
        bool operator==(const NpcDialogueRequestIdentity &) const noexcept = default;
    };

    /** @brief Explicit host disclosure grant for this route and provider activation. */
    struct NpcDialogueGrant final {
        NpcDialogueRoute route;
        std::uint64_t providerGeneration{};
        std::span<const NpcDialogueFactId> fields; /**< Explicitly permitted fields in increasing ID order. */

        /** @brief Checks the bounded disclosure list before it authorizes a request or cue.
         * @return Whether all field IDs are valid, strictly increasing, and within the turn capacity.
         */
        [[nodiscard]] bool HasCanonicalFields() const noexcept;
    };

    /** @brief Input from a host decoder. Approval includes protocol validation and moderation. */
    struct NpcDialogueReply final {
        NpcDialogueRequestIdentity origin; /**< Stamped locally by the host that owns the transport request. */
        NpcDialogueCueId cue;
        bool approved{};
    };

    /** @brief Owner decision after evaluating one reply or turn deadline. */
    enum class NpcDialogueStatus : std::uint8_t {
        Selected,
        Fallback,
        Stale,
        Cancelled,
        Consumed
    };

    /** @brief Authored cue to present, or an empty cue after retirement. No gameplay intent is exposed. */
    struct NpcDialogueOutcome final {
        NpcDialogueStatus status{NpcDialogueStatus::Stale};
        NpcDialogueCueId cue;
    };

    /** @brief A bounded dialogue turn owned by the host on its presentation lane. */
    class NpcDialogueTurn final {
    public:
        static constexpr std::size_t MaximumFacts = 16;
        static constexpr std::size_t MaximumCues = 16;

        /** @brief Copies the exact granted projection and authored presentation policy.
         * @param identity Local session and turn identity. @param route Host-selected provider and destination.
         * @param grant Current project/product disclosure grant. @param facts Context in increasing field ID order.
         * @param cues Legal authored cues in increasing ID order. @param fallback A legal cue to present on failure, including offline.
         * @param deadlineTick Exclusive deadline on the owner's clock.
         * @return A complete bounded turn or an error before any provider data is exposed.
         */
        [[nodiscard]] static Result<NpcDialogueTurn> Capture(const NpcDialogueRequestIdentity &identity, const NpcDialogueRoute &route,
                                                            const NpcDialogueGrant &grant, std::span<const NpcDialogueFact> facts,
                                                            std::span<const NpcDialogueCueId> cues, NpcDialogueCueId fallback,
                                                            std::uint64_t deadlineTick);

        NpcDialogueTurn(const NpcDialogueTurn &) = delete;
        NpcDialogueTurn &operator=(const NpcDialogueTurn &) = delete;
        /** @brief Transfers the turn and prevents the moved-from owner from publishing it. */
        NpcDialogueTurn(NpcDialogueTurn &&other) noexcept;
        NpcDialogueTurn &operator=(NpcDialogueTurn &&) = delete;

        /** @brief Borrows only the allowed payload before handing a copy to the host transport.
         * @param current Current local identity. @param grant Current disclosure grant. @param tick Current owner tick.
         * @param cancellation Turn cancellation token.
         * @return Call-duration view for immediate dispatch on the owner lane, or no payload on denial or expiry.
         */
        [[nodiscard]] std::optional<std::span<const NpcDialogueFact>> Context(const NpcDialogueRequestIdentity &current,
                                                                            const NpcDialogueGrant &grant, std::uint64_t tick,
                                                                            const CancellationToken &cancellation) const noexcept;

        /** @brief Consumes the reply once, selecting an allowed cue or the deterministic fallback.
         * @param current Current local identity. @param grant Current disclosure grant.
         * @param tick Tick at the owner presentation boundary.
         * @param cancellation Turn cancellation token. @param reply Approved decoder outcome, or empty on failure.
         * @return Presentation-only outcome. Stale, cancelled and consumed turns produce no cue.
         */
        [[nodiscard]] NpcDialogueOutcome Resolve(const NpcDialogueRequestIdentity &current, const NpcDialogueGrant &grant, std::uint64_t tick,
                                                  const CancellationToken &cancellation,
                                                  const std::optional<NpcDialogueReply> &reply) noexcept;

    private:
        NpcDialogueTurn() = default;
        /** @brief Checks whether the captured fields remain permitted by the live grant. */
        [[nodiscard]] bool AllowsProjection(const NpcDialogueGrant &grant) const noexcept;
        NpcDialogueRequestIdentity identity_;
        NpcDialogueRoute route_;
        std::uint64_t deadlineTick_{};
        std::array<NpcDialogueFact, MaximumFacts> facts_{};
        std::array<NpcDialogueCueId, MaximumCues> cues_{};
        std::size_t factCount_{};
        std::size_t cueCount_{};
        NpcDialogueCueId fallback_;
        bool consumed_{};
    };

    /** @brief Host-owned decoder and transport for one approved route. */
    class INpcDialogueProvider {
    public:
        virtual ~INpcDialogueProvider() = default;

        /** @brief Requests a cue using only approved authored tokens on a dedicated I/O worker.
         * @param facts Call-duration payload authorized by the host. @param deadline Exclusive transport deadline.
         * @param cancellation Check before disclosure and while waiting on I/O.
         * @return Moderated, decoded cue, or a failure. Never returns free-form text or an executable action.
         */
        [[nodiscard]] virtual Result<NpcDialogueCueId> RequestCue(std::span<const NpcDialogueFact> facts,
                                                                std::chrono::steady_clock::time_point deadline,
                                                                const CancellationToken &cancellation) const = 0;
    };

    /** @brief Explicit host binding of a provider implementation and its code lifetime. */
    struct NpcDialogueProviderBinding final {
        NpcDialogueRoute route;
        std::uint64_t generation{};
        std::shared_ptr<const void> imageLease;
        std::shared_ptr<const INpcDialogueProvider> provider;
        CancellationToken approval; /**< Nonempty host-owned disclosure authority required to queue provider work. */
    };

    /** @brief Owner-lane tracking for one detached, bounded NPC dialogue request. */
    class NpcDialogueJob final {
    public:
        /** @brief Queues a copied projection on the host's dedicated I/O lane.
         * @param jobs Borrowed host scheduler with I/O capacity. @param turn Turn to transfer on successful submission.
         * @param current Exact turn identity. @param grant Live grant. @param binding Approved provider and image lease.
         * @param tick Current owner tick. @param deadline Exclusive worker deadline. @param cancellation Parent revocation token.
         * @return A tracked request, or a failure leaving the original turn available for fallback.
         */
        [[nodiscard]] static Result<NpcDialogueJob> Start(const JobSystem &jobs, NpcDialogueTurn &&turn,
                                                        const NpcDialogueRequestIdentity &current, const NpcDialogueGrant &grant,
                                                        const NpcDialogueProviderBinding &binding, std::uint64_t tick,
                                                        std::chrono::steady_clock::time_point deadline,
                                                        const CancellationToken &cancellation);

        NpcDialogueJob(const NpcDialogueJob &) = delete;
        NpcDialogueJob &operator=(const NpcDialogueJob &) = delete;
        /** @brief Transfers the only owner of the pending turn. */
        NpcDialogueJob(NpcDialogueJob &&) noexcept = default;
        NpcDialogueJob &operator=(NpcDialogueJob &&) = delete;
        /** @brief Requests cancellation without waiting for a worker to return. */
        ~NpcDialogueJob();

        /** @brief Polls from the owner lane during normal presentation and whenever policy changes.
         * @param current Latest turn identity. @param grant Latest disclosure grant. @param tick Current owner tick.
         * @return No value while work remains pending, or a terminal presentation outcome.
         */
        [[nodiscard]] std::optional<NpcDialogueOutcome> Poll(const NpcDialogueRequestIdentity &current, const NpcDialogueGrant &grant,
                                                            std::uint64_t tick);

    private:
        struct Work;
        NpcDialogueJob(NpcDialogueTurn &&turn, JobHandle &&job, std::shared_ptr<Work> work) noexcept;
        NpcDialogueTurn turn_;
        JobHandle job_;
        std::shared_ptr<Work> work_;
    };
}  // namespace Horo::AI
