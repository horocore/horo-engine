#pragma once

/** @file NpcDialogueScenePresentation.h @brief Scene-owned publication of NPC dialogue as authored captions. */

#include "Horo/AI/NpcDialogue.h"
#include "Horo/Runtime/Ui/UiCaptions.h"

#include <variant>

namespace Horo::App {
    /** @brief Host-authored caption metadata keyed by one provider-selectable cue. */
    struct NpcDialogueCaption final {
        AI::NpcDialogueCueId cue;
        std::uint32_t text{};
        std::uint32_t speaker{};
        std::uint64_t durationTicks{};
    };

    /** @brief One dialogue presentation channel for a Scene incarnation. All operations run on the owner lane. */
    class NpcDialogueScenePresentation final {
    public:
        /** @brief Pins authored cues and claims a semantic caption queue for the lifetime of this channel.
         * @param jobs Host scheduler, which outlives the channel. @param scene Exact Scene incarnation.
         * @param queue Host-assigned caption session and immutable text catalog, owned exclusively after admission.
         * @param captions Increasing cue IDs with valid catalog references. @param fallback Cue used without an admitted reply.
         * @param provider Optional approved provider binding. An empty binding selects authored offline dialogue.
         * @return A complete channel or a typed invalid configuration error.
         */
        [[nodiscard]] static Result<NpcDialogueScenePresentation> Create(
            const JobSystem &jobs, AI::AiRuntimeIncarnation scene, std::unique_ptr<Runtime::Ui::CaptionCueQueue> queue,
            std::span<const NpcDialogueCaption> captions, AI::NpcDialogueCueId fallback,
            std::optional<AI::NpcDialogueProviderBinding> provider = std::nullopt);

        /** @brief Accepts a strictly newer turn while the channel is idle.
         * @param request Local turn identity with a live agent from this Scene. @param route Approved authored route.
         * @param grant Current host-owned grant. @param facts Authored context ordered by field ID.
         * @param tick Current simulation tick. @param deadlineTick Exclusive simulation deadline.
         * @param deadline Exclusive worker deadline. @param cancellation Parent cancellation for this turn.
         * @return Success if the channel owns the turn, including when it uses the offline fallback.
         */
        [[nodiscard]] Result<void> Start(const AI::NpcDialogueRequestIdentity &request, const AI::NpcDialogueRoute &route,
                                         const AI::NpcDialogueGrant &grant, std::span<const AI::NpcDialogueFact> facts, std::uint64_t tick,
                                         std::uint64_t deadlineTick, std::chrono::steady_clock::time_point deadline,
                                         const CancellationToken &cancellation);

        /** @brief Polls the active turn and publishes at most one caption.
         * @param current Live agent and turn identity, or a different identity to retire stale work.
         * @param grant Current host grant. @param tick Current simulation tick.
         * @return Queue admission when a cue was submitted, empty while pending or on retirement.
         */
        [[nodiscard]] std::optional<Accessibility::CaptionAdmission> Pump(const AI::NpcDialogueRequestIdentity &current,
                                                                            const AI::NpcDialogueGrant &grant, std::uint64_t tick);

        /** @brief Feeds this channel's already authored cues to the existing HUD timeline.
         * @param timeline UI owner prepared for the same caption session. @return Number lost at timeline capacity.
         */
        [[nodiscard]] std::size_t Drain(Runtime::Ui::CaptionTimeline &timeline) noexcept;

        /** @brief Cancels pending work and drops undelivered captions before Scene retirement. */
        void Retire() noexcept;

    private:
        NpcDialogueScenePresentation(const JobSystem &jobs, AI::AiRuntimeIncarnation scene,
                                      std::unique_ptr<Runtime::Ui::CaptionCueQueue> queue,
                                      std::optional<AI::NpcDialogueProviderBinding> provider) noexcept;
        const JobSystem *jobs_;
        AI::AiRuntimeIncarnation scene_;
        std::unique_ptr<Runtime::Ui::CaptionCueQueue> queue_;
        std::optional<AI::NpcDialogueProviderBinding> provider_;
        std::array<NpcDialogueCaption, AI::NpcDialogueTurn::MaximumCues> captions_{};
        std::array<AI::NpcDialogueCueId, AI::NpcDialogueTurn::MaximumCues> cues_{};
        std::size_t count_{};
        AI::NpcDialogueCueId fallback_;
        std::uint64_t lastTurn_{};
        CancellationToken cancellation_;
        std::variant<std::monostate, AI::NpcDialogueTurn, AI::NpcDialogueJob> pending_;
    };
}  // namespace Horo::App
