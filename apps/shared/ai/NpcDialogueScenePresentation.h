#pragma once

/** @file NpcDialogueScenePresentation.h @brief Scene-owned publication of NPC dialogue as authored captions. */

#include "Horo/AI/AISceneActivation.h"
#include "Horo/AI/NpcDialogue.h"
#include "Horo/Application/ProjectAiSettings.h"
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
         * @param jobs Host scheduler, which outlives the channel.
         * @param runtime Active Scene AI owner, which outlives the channel and stays at a stable address.
         * @param queue Host-assigned caption session and immutable text catalog, owned exclusively after admission.
         * @param captions Increasing cue IDs with valid catalog references. @param fallback Cue used without an admitted reply.
         * @param triggers Validated project interaction catalog in increasing trigger ID order, copied during admission.
         * @param provider Optional approved provider binding. An empty binding selects authored offline dialogue.
         * @return A complete channel or a typed invalid configuration error.
         */
        [[nodiscard]] static Result<NpcDialogueScenePresentation> Create(
            const JobSystem &jobs, const AI::AiSceneRuntime &runtime, std::unique_ptr<Runtime::Ui::CaptionCueQueue> queue,
            std::span<const NpcDialogueCaption> captions, AI::NpcDialogueCueId fallback,
            std::span<const Application::ProjectNpcDialogueTrigger> triggers,
            std::optional<AI::NpcDialogueProviderBinding> provider = std::nullopt);

        /** @brief Assigns a new turn to an active NPC while the channel is idle.
         * @param agent Live, active agent from this Scene, matching the trigger's authored identity.
         * @param trigger ID in the admitted project catalog. @param grant Current host-owned grant.
         * @param tick Current simulation tick. @param deadlineTick Exclusive simulation deadline.
         * @param deadline Exclusive worker deadline. @param cancellation Parent cancellation for this turn.
         * @return Success if the channel owns the turn, including when it uses the offline fallback.
         */
        [[nodiscard]] Result<void> Start(AI::AgentHandle agent, Application::NpcDialogueTriggerId trigger,
                                         const AI::NpcDialogueGrant &grant, std::uint64_t tick,
                                         std::uint64_t deadlineTick, std::chrono::steady_clock::time_point deadline,
                                         const CancellationToken &cancellation);

        /** @brief Polls the active turn and publishes at most one caption.
         * @param grant Current host grant. @param tick Current simulation tick.
         * @return Queue admission when a cue was submitted, empty while pending or on retirement.
         */
        [[nodiscard]] std::optional<Accessibility::CaptionAdmission> Pump(const AI::NpcDialogueGrant &grant, std::uint64_t tick);

        /** @brief Feeds this channel's already authored cues to the existing HUD timeline.
         * @param timeline UI owner prepared for the same caption session. @return Number lost at timeline capacity.
         */
        [[nodiscard]] std::size_t Drain(Runtime::Ui::CaptionTimeline &timeline);

        /** @brief Cancels pending work and drops undelivered captions before Scene retirement. */
        void Retire() noexcept;

    private:
        NpcDialogueScenePresentation(const JobSystem &jobs, const AI::AiSceneRuntime &runtime, AI::AiSceneActivationBinding scene,
                                      std::unique_ptr<Runtime::Ui::CaptionCueQueue> queue,
                                      std::optional<AI::NpcDialogueProviderBinding> provider) noexcept;
        /** @brief Checks whether the exact Scene publication still owns this channel. */
        [[nodiscard]] bool SceneIsCurrent() const;
        /** @brief Checks whether the Scene still owns the active agent generation. */
        [[nodiscard]] bool AgentIsActive(AI::AgentHandle agent, AI::AgentId subject) const;
        const JobSystem *jobs_;
        const AI::AiSceneRuntime *runtime_;
        AI::AiSceneActivationBinding scene_;
        std::unique_ptr<Runtime::Ui::CaptionCueQueue> queue_;
        std::optional<AI::NpcDialogueProviderBinding> provider_;
        std::vector<Application::ProjectNpcDialogueTrigger> triggers_;
        std::array<NpcDialogueCaption, AI::NpcDialogueTurn::MaximumCues> captions_{};
        std::array<AI::NpcDialogueCueId, AI::NpcDialogueTurn::MaximumCues> cues_{};
        std::size_t count_{};
        AI::NpcDialogueCueId fallback_;
        std::uint64_t lastTurn_{};
        AI::NpcDialogueRequestIdentity request_;
        AI::AgentId subject_;
        CancellationToken cancellation_;
        std::variant<std::monostate, AI::NpcDialogueTurn, AI::NpcDialogueJob> pending_;
    };
}  // namespace Horo::App
