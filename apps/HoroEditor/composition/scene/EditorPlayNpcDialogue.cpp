#include "EditorProjectSceneHostOwner.h"

#include <algorithm>
#include <chrono>

namespace Horo::Application::Internal::EditorProjectSceneDetail {
    namespace {
        /** @brief Returns a representable worker deadline without changing the configured budget. */
        auto WorkerDeadline(const Duration budget) noexcept {
            using Clock = std::chrono::steady_clock;
            const auto now = Clock::now();
            const auto duration = std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds{budget.ToNanoseconds()});
            const auto maximum = Clock::time_point::max();
            return now >= maximum - duration ? maximum : now + duration;
        }

        /** @brief Checks the complete explicit input and timing selection before reading a project. */
        bool ValidDialogueSelection(const EditorPlayNpcDialogueSelection &selection) noexcept {
            return selection.action.IsValid() && selection.trigger.IsValid() && selection.timeline &&
                   selection.workerBudget.ToNanoseconds() > 0 && selection.tickBudget != 0;
        }

        /** @brief Resolves authored text identities through the same validator used by the runtime caption renderer. */
        Result<void> AppendDialogueCaption(EditorPlayNpcDialogueSelection &selection, const ProjectNpcDialogueCaption &caption) {
            auto key = Runtime::Ui::UiMessageKey::Create(caption.messageNamespace, caption.messageKey);
            if (key.HasError())
                return Result<void>::Failure(key.ErrorValue());
            auto text = Runtime::Ui::LocalizedMessageRef::Create(std::move(key).Value(), {}, caption.fallbackText);
            if (text.HasError())
                return Result<void>::Failure(text.ErrorValue());
            const auto messageIndex = static_cast<std::uint32_t>(selection.messages.size());
            selection.messages.push_back(std::move(text).Value());
            selection.captions.push_back({caption.cue, messageIndex, 0, caption.durationTicks});
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ProjectPlaySceneHost::PrepareNpcDialogue */
    Result<void> ProjectPlaySceneHost::PrepareNpcDialogue(const std::filesystem::path &root, JobSystem &jobs,
                                                          const EditorProjectScenePolicy &policy) {
        if (!policy.npcDialogue)
            return policy.authoredNpcDialogue ? PrepareAuthoredNpcDialogue(root, jobs, *policy.authoredNpcDialogue) : Result<void>::Success();
        const auto &selection = *policy.npcDialogue;
        if (!ValidDialogueSelection(selection))
            return Result<void>::Failure(MakeError(InvalidProjectScene, "Play dialogue requires an explicit input, HUD and deadline."));
        const auto *approval = selection.approval ? &*selection.approval : nullptr;
        auto captured = App::NpcDialogueProjectActivation::Create(root, identity_.value, approval);
        if (captured.HasError())
            return Result<void>::Failure(captured.ErrorValue());
        auto selected = std::move(captured).Value();
        if (!selected || !std::ranges::binary_search(selected->Triggers(), selection.trigger, {}, &ProjectNpcDialogueTrigger::id))
            return Result<void>::Failure(MakeError(InvalidProjectScene, "Play dialogue input selects a trigger absent from the project."));
        dialogue_.emplace(NpcDialogueState{selection, std::move(*selected), &jobs});
        return Result<void>::Success();
    }

    /** @copydoc ProjectPlaySceneHost::PrepareAuthoredNpcDialogue */
    Result<void> ProjectPlaySceneHost::PrepareAuthoredNpcDialogue(const std::filesystem::path &root, JobSystem &jobs,
                                                                  const EditorPlayAuthoredNpcDialogue &port) {
        auto captured = App::NpcDialogueProjectActivation::Create(root, identity_.value, nullptr);
        if (captured.HasError())
            return Result<void>::Failure(captured.ErrorValue());
        auto activation = std::move(captured).Value();
        if (!activation || !activation->Presentation())
            return Result<void>::Success();

        const auto &authored = *activation->Presentation();
        EditorPlayNpcDialogueSelection selection{.action = port.action,
                                                  .trigger = authored.trigger,
                                                  .fallback = authored.fallback,
                                                  .timeline = port.timeline,
                                                  .workerBudget = Duration::FromMilliseconds(authored.workerMilliseconds),
                                                  .tickBudget = authored.deadlineTicks};
        if (!ValidDialogueSelection(selection))
            return Result<void>::Failure(MakeError(InvalidProjectScene, "The authored Play dialogue has no available input or HUD."));
        selection.messages.reserve(authored.captions.size());
        selection.captions.reserve(authored.captions.size());
        for (const auto &caption : authored.captions) {
            if (auto admitted = AppendDialogueCaption(selection, caption); admitted.HasError())
                return admitted;
        }
        dialogue_.emplace(NpcDialogueState{std::move(selection), std::move(*activation), &jobs});
        return Result<void>::Success();
    }

    /** @copydoc Editor::EditorPlaySceneHost::StagePresentationInput */
    void ProjectPlaySceneHost::StagePresentationInput(std::span<const Gameplay::GameplayInputAction> input) noexcept {
        if (dialogue_) {
            dialogue_->staged = std::ranges::any_of(input, [&](const Gameplay::GameplayInputAction &action) {
                return action.pressed && action.action == dialogue_->selection.action;
            });
        }
    }

    /** @copydoc ProjectPlaySceneHost::CommitNpcDialogueInput */
    void ProjectPlaySceneHost::CommitNpcDialogueInput(std::uint64_t tick) noexcept {
        if (dialogue_ && std::exchange(dialogue_->staged, false) && !dialogue_->committed)
            dialogue_->committed = tick;
    }

    /** @copydoc ProjectPlaySceneHost::BindNpcDialogueScene */
    Result<void> ProjectPlaySceneHost::BindNpcDialogueScene(NpcDialogueState &state, const AI::AiSceneActivationBinding &binding) {
        if (state.channel)
            RetireNpcDialogueScene(state);
        const auto sessionId = state.selection.timeline->NextSessionId();
        auto queue = Runtime::Ui::CaptionCueQueue::Create(sessionId, state.selection.messages);
        if (queue.HasError())
            return Result<void>::Failure(queue.ErrorValue());
        auto channel = App::NpcDialogueScenePresentation::Create(*state.jobs, *ai_, std::move(queue).Value(), state.selection.captions,
                                                                 state.selection.fallback, state.activation.Triggers(),
                                                                 state.activation.Binding());
        if (channel.HasError())
            return Result<void>::Failure(channel.ErrorValue());
        auto session = state.selection.timeline->Reset(sessionId);
        if (session.HasError())
            return session;
        state.binding = binding;
        state.channel.emplace(std::move(channel).Value());
        return Result<void>::Success();
    }

    /** @copydoc ProjectPlaySceneHost::AdvanceNpcDialogue */
    Result<void> ProjectPlaySceneHost::AdvanceNpcDialogue(const Runtime::FrameContext &frame) {
        if (!dialogue_ || !ai_)
            return Result<void>::Success();
        auto &state = *dialogue_;
        const auto binding = ai_->ActiveBinding();
        if (binding.HasError()) {
            RetireNpcDialogueScene(state);
            return Result<void>::Success();
        }
        if (state.binding != binding.Value())
            if (auto bound = BindNpcDialogueScene(state, binding.Value()); bound.HasError())
                return bound;

        const auto tick = frame.completedSimulationTick;
        auto &timeline = *state.selection.timeline;
        timeline.BeginFrame(tick);
        static_cast<void>(state.channel->Pump(state.activation.Grant(), tick));
        DispatchCommittedNpcDialogue(state, frame);
        static_cast<void>(state.channel->Drain(timeline));
        return Result<void>::Success();
    }

    /** @copydoc ProjectPlaySceneHost::DispatchCommittedNpcDialogue */
    void ProjectPlaySceneHost::DispatchCommittedNpcDialogue(NpcDialogueState &state, const Runtime::FrameContext &frame) {
        const auto tick = frame.completedSimulationTick;
        if (!state.committed || *state.committed > tick)
            return;
        state.committed.reset();
        const auto maxWait = std::numeric_limits<std::uint64_t>::max() - tick;
        if (maxWait == 0)
            return;
        const auto deadline = tick + std::min(maxWait, state.selection.tickBudget);
        static_cast<void>(state.channel->StartAuthored(state.selection.trigger, state.activation.Grant(), tick, deadline,
                                                      WorkerDeadline(state.selection.workerBudget), frame.cancellation));
    }

    /** @copydoc ProjectPlaySceneHost::RetireNpcDialogueScene */
    void ProjectPlaySceneHost::RetireNpcDialogueScene(NpcDialogueState &state) noexcept {
        state.binding = {};
        state.staged = false;
        state.committed.reset();
        if (state.channel) {
            state.channel->Retire();
            state.channel.reset();
            static_cast<void>(state.selection.timeline->Reset(0));
        }
    }

    /** @copydoc ProjectPlaySceneHost::CloseNpcDialogue */
    void ProjectPlaySceneHost::CloseNpcDialogue() noexcept {
        if (dialogue_)
            RetireNpcDialogueScene(*dialogue_);
        dialogue_.reset();
    }
}  // namespace Horo::Application::Internal::EditorProjectSceneDetail
