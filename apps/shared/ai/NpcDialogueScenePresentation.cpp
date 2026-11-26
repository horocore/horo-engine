#include "NpcDialogueScenePresentation.h"
#include "Horo/AI/AIRuntimeCapability.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace Horo::App {
    namespace {
        using namespace AI;
        using Runtime::Ui::CaptionCueQueue;

        /** @brief Tests exact identity against the UI's bounded visible and pending cues. */
        bool ContainsCue(const Runtime::Ui::CaptionTimeline &timeline, Accessibility::CaptionCueId id) {
            return std::ranges::any_of(timeline.Cues(), [id](const auto &entry) { return entry.cue.id == id; });
        }

        /** @brief Checks catalog references and canonical cue order before the Scene adopts them. */
        bool ValidCaptions(std::span<const NpcDialogueCaption> captions, std::size_t catalogSize) {
            if (captions.size() > NpcDialogueTurn::MaximumCues ||
                !std::ranges::is_sorted(captions, {}, &NpcDialogueCaption::cue) ||
                std::ranges::adjacent_find(captions, {}, &NpcDialogueCaption::cue) != captions.end())
                return false;
            return std::ranges::all_of(captions, [catalogSize](const NpcDialogueCaption &cue) {
                return cue.cue.IsValid() && cue.text < catalogSize && cue.speaker <= catalogSize && cue.durationTicks != 0;
            });
        }

        /** @brief Checks lookup order and the allocation bounds before copying the catalog. */
        bool ValidTriggerCatalog(std::span<const Application::ProjectNpcDialogueTrigger> triggers) {
            constexpr auto id = &Application::ProjectNpcDialogueTrigger::id;
            if (triggers.size() > Application::ProjectNpcDialogueSettings::MaximumTriggers || !std::ranges::is_sorted(triggers, {}, id) ||
                std::ranges::adjacent_find(triggers, {}, id) != triggers.end())
                return false;
            return std::ranges::all_of(triggers, [](const Application::ProjectNpcDialogueTrigger &trigger) {
                return trigger.id.IsValid() && trigger.agent.IsValid() && trigger.facts.size() <= NpcDialogueTurn::MaximumFacts;
            });
        }
    }  // namespace

    /** @copydoc NpcDialogueScenePresentation::ProviderDisclosure::ProviderDisclosure */
    NpcDialogueScenePresentation::ProviderDisclosure::ProviderDisclosure(const AI::NpcDialogueGrant &grant,
                                                                         std::span<const AI::NpcDialogueFact> projection)
        : route(grant.route), generation(grant.providerGeneration), fields{}, count(projection.size()) {
        std::ranges::transform(projection, fields.begin(), &AI::NpcDialogueFact::field);
    }

    /** @copydoc NpcDialogueScenePresentation::ProviderDisclosure::IsCurrent */
    bool NpcDialogueScenePresentation::ProviderDisclosure::IsCurrent(const AI::NpcDialogueGrant &grant) const noexcept {
        return route == grant.route && generation == grant.providerGeneration &&
               grant.HasCanonicalFields() &&
               std::ranges::includes(grant.fields, std::span{fields}.first(count));
    }

    /** @brief Binds the Scene scheduler and caption owner without touching gameplay state. */
    NpcDialogueScenePresentation::NpcDialogueScenePresentation(const JobSystem &jobs, const AI::AiSceneRuntime &runtime,
                                                               AI::AiSceneActivationBinding scene,
                                                               std::unique_ptr<Runtime::Ui::CaptionCueQueue> queue,
                                                               std::optional<AI::NpcDialogueProviderBinding> provider) noexcept
        : jobs_(&jobs), runtime_(&runtime), scene_(scene), queue_(std::move(queue)), provider_(std::move(provider)) {}

    /** @copydoc NpcDialogueScenePresentation::SceneIsCurrent */
    bool NpcDialogueScenePresentation::SceneIsCurrent() const {
        const auto binding = runtime_->ActiveBinding();
        return binding.HasValue() && binding.Value() == scene_;
    }

    /** @copydoc NpcDialogueScenePresentation::AgentIsActive */
    bool NpcDialogueScenePresentation::AgentIsActive(AI::AgentHandle agent, AI::AgentId subject) const {
        const auto record = runtime_->Find(agent);
        return record.HasValue() && record.Value().state == AI::AiAgentActivationState::Active && record.Value().agent.agent == subject;
    }

    /** @copydoc NpcDialogueScenePresentation::FindTrigger */
    const Application::ProjectNpcDialogueTrigger *NpcDialogueScenePresentation::FindTrigger(Application::NpcDialogueTriggerId trigger) const {
        const auto found = std::ranges::lower_bound(triggers_, trigger, {}, &Application::ProjectNpcDialogueTrigger::id);
        return found != triggers_.end() && found->id == trigger ? &*found : nullptr;
    }

    /** @copydoc NpcDialogueScenePresentation::CanStartTurn */
    bool NpcDialogueScenePresentation::CanStartTurn() const noexcept {
        return !queuedAuthority_ && std::holds_alternative<std::monostate>(pending_) &&
               lastTurn_ != std::numeric_limits<std::uint64_t>::max();
    }

    /** @copydoc NpcDialogueScenePresentation::Create */
    Result<NpcDialogueScenePresentation> NpcDialogueScenePresentation::Create(
        const JobSystem &jobs, const AI::AiSceneRuntime &runtime, std::unique_ptr<Runtime::Ui::CaptionCueQueue> queue,
        std::span<const NpcDialogueCaption> captions, AI::NpcDialogueCueId fallback,
        std::span<const Application::ProjectNpcDialogueTrigger> triggers,
        std::optional<AI::NpcDialogueProviderBinding> provider) {
        using Channel = Result<NpcDialogueScenePresentation>;
        const auto scene = runtime.ActiveBinding();
        if (scene.HasError())
            return Channel::Failure(scene.ErrorValue());
        if (!queue || queue->Producer() != Accessibility::CaptionProducerKind::Semantic || !ValidTriggerCatalog(triggers))
            return Channel::Failure(MakeError(AI::AIErrors::TaskContextInvalid));
        Accessibility::CaptionCue queued;
        if (queue->TryDequeue(queued))
            return Channel::Failure(MakeError(AI::AIErrors::TaskContextInvalid));
        if (!ValidCaptions(captions, queue->Catalog()->size()) ||
            !std::ranges::binary_search(captions, fallback, {}, &NpcDialogueCaption::cue))
            return Channel::Failure(MakeError(AI::AIErrors::TaskContextInvalid));
        NpcDialogueScenePresentation channel{jobs, runtime, scene.Value(), std::move(queue), std::move(provider)};
        std::ranges::copy(captions, channel.captions_.begin());
        std::ranges::transform(captions, channel.cues_.begin(), &NpcDialogueCaption::cue);
        channel.count_ = captions.size();
        channel.fallback_ = fallback;
        channel.triggers_.assign(triggers.begin(), triggers.end());
        return Channel::Success(std::move(channel));
    }

    /** @copydoc NpcDialogueScenePresentation::Start */
    Result<void> NpcDialogueScenePresentation::Start(AI::AgentHandle agent, Application::NpcDialogueTriggerId trigger,
                                                    const AI::NpcDialogueGrant &grant, std::uint64_t tick, std::uint64_t deadlineTick,
                                                    std::chrono::steady_clock::time_point deadline,
                                                    const CancellationToken &cancellation) {
        const auto *binding = FindTrigger(trigger);
        if (!binding)
            return Result<void>::Failure(MakeError(AI::AIErrors::TaskContextInvalid));
        if (!queue_ || !SceneIsCurrent() || !AgentIsActive(agent, binding->agent))
            return Result<void>::Failure(MakeError(AI::AIErrors::HandleInvalid));
        if (!CanStartTurn())
            return Result<void>::Failure(MakeError(AI::AIErrors::TaskCapacityExceeded));
        const auto lifetime = runtime_->CapabilityAtSafePoint().CaptureAgentLifetime(agent);
        if (lifetime.HasError())
            return Result<void>::Failure(lifetime.ErrorValue());
        auto turnCancellation = CancellationToken::AnyOf(cancellation, lifetime.Value());
        const AI::NpcDialogueRequestIdentity request{agent, grant.providerGeneration, lastTurn_ + 1};
        auto captured = AI::NpcDialogueTurn::Capture(request, grant.route, grant, binding->facts, std::span{cues_}.first(count_), fallback_,
                                                      deadlineTick);
        if (captured.HasError())
            return Result<void>::Failure(std::move(captured).ErrorValue());
        auto turn = std::move(captured).Value();
        lastTurn_ = request.turn;
        request_ = request;
        subject_ = binding->agent;
        cancellation_ = std::move(turnCancellation);
        if (provider_) {
            if (const auto projection = turn.Context(request, grant, tick, cancellation_)) {
                ProviderDisclosure disclosure{grant, *projection};
                auto job = AI::NpcDialogueJob::Start(*jobs_, std::move(turn), request, grant, *provider_, tick, deadline, cancellation_);
                if (job.HasValue()) {
                    pendingDisclosure_.emplace(std::move(disclosure));
                    pending_.emplace<AI::NpcDialogueJob>(std::move(job).Value());
                    return Result<void>::Success();
                }
            }
        }
        pending_.emplace<AI::NpcDialogueTurn>(std::move(turn));
        return Result<void>::Success();
    }

    /** @copydoc NpcDialogueScenePresentation::StartAuthored */
    Result<void> NpcDialogueScenePresentation::StartAuthored(Application::NpcDialogueTriggerId trigger, const AI::NpcDialogueGrant &grant,
                                                            std::uint64_t tick, std::uint64_t deadlineTick,
                                                            std::chrono::steady_clock::time_point deadline,
                                                            const CancellationToken &cancellation) {
        const auto *binding = FindTrigger(trigger);
        if (!binding)
            return Result<void>::Failure(MakeError(AI::AIErrors::TaskContextInvalid));
        if (!queue_ || !SceneIsCurrent())
            return Result<void>::Failure(MakeError(AI::AIErrors::HandleInvalid));
        const auto agent = runtime_->CapabilityAtSafePoint().ResolveAgent(binding->agent);
        if (agent.HasError())
            return Result<void>::Failure(agent.ErrorValue());
        return Start(agent.Value(), trigger, grant, tick, deadlineTick, deadline, cancellation);
    }

    /** @copydoc NpcDialogueScenePresentation::Pump */
    std::optional<Accessibility::CaptionAdmission> NpcDialogueScenePresentation::Pump(const AI::NpcDialogueGrant &grant,
                                                                                       std::uint64_t tick) {
        if (std::holds_alternative<std::monostate>(pending_))
            return std::nullopt;
        if (!SceneIsCurrent()) {
            Retire();
            return std::nullopt;
        }
        const auto current = AgentIsActive(request_.agent, subject_) ? request_ : AI::NpcDialogueRequestIdentity{};
        std::optional<AI::NpcDialogueOutcome> outcome;
        if (auto *job = std::get_if<AI::NpcDialogueJob>(&pending_))
            outcome = job->Poll(current, grant, tick);
        else if (auto *turn = std::get_if<AI::NpcDialogueTurn>(&pending_))
            outcome = turn->Resolve(current, grant, tick, cancellation_, std::nullopt);
        if (!outcome)
            return std::nullopt;
        pending_.emplace<std::monostate>();
        auto cancellation = std::exchange(cancellation_, {});
        auto disclosure = std::exchange(pendingDisclosure_, std::nullopt);
        const auto captions = std::span{captions_}.first(count_);
        const auto cue = std::ranges::lower_bound(captions, outcome->cue, {}, &NpcDialogueCaption::cue);
        if (cue == captions.end() || cue->cue != outcome->cue)
            return std::nullopt;
        const auto admitted = queue_->TryEnqueue({{queue_->Session(), current.turn}, Accessibility::CaptionCueKind::Dialogue,
                                                 cue->text, cue->speaker, tick, cue->durationTicks});
        if (admitted == Accessibility::CaptionAdmission::Accepted) {
            if (outcome->status != AI::NpcDialogueStatus::Selected)
                disclosure.reset();
            queuedAuthority_.emplace(CaptionAuthority{current.agent, subject_, std::move(cancellation), std::move(disclosure)});
        }
        return admitted;
    }

    /** @copydoc NpcDialogueScenePresentation::Drain */
    std::size_t NpcDialogueScenePresentation::Drain(Runtime::Ui::CaptionTimeline &timeline, const AI::NpcDialogueGrant &grant) {
        const bool sceneCurrent = queue_ && SceneIsCurrent();
        ReconcileDisplayed(timeline, grant, sceneCurrent);
        if (queue_ && !sceneCurrent)
            Retire();
        if (!queuedAuthority_)
            return 0;
        auto authority = std::exchange(queuedAuthority_, std::nullopt);
        if (!CaptionIsCurrent(*authority, grant)) {
            Accessibility::CaptionCue withdrawn;
            static_cast<void>(queue_->TryDequeue(withdrawn));
            return 0;
        }
        const auto dropped = timeline.Drain(*queue_);
        const Accessibility::CaptionCueId id{queue_->Session(), request_.turn};
        if (!ContainsCue(timeline, id))
            return dropped;
        const auto free = std::ranges::find_if(displayed_, [](const auto &entry) { return !entry; });
        if (free == displayed_.end()) {
            static_cast<void>(timeline.Withdraw(id));
            return dropped + 1;
        }
        free->emplace(DisplayedCue{id, std::move(*authority)});
        return dropped;
    }

    /** @copydoc NpcDialogueScenePresentation::CaptionIsCurrent */
    bool NpcDialogueScenePresentation::CaptionIsCurrent(const CaptionAuthority &authority, const AI::NpcDialogueGrant &grant) const {
        return !authority.cancellation.IsCancellationRequested() && AgentIsActive(authority.agent, authority.subject) &&
               (!authority.disclosure || authority.disclosure->IsCurrent(grant));
    }

    /** @copydoc NpcDialogueScenePresentation::ReconcileDisplayed */
    void NpcDialogueScenePresentation::ReconcileDisplayed(Runtime::Ui::CaptionTimeline &timeline, const AI::NpcDialogueGrant &grant,
                                                           const bool sceneCurrent) {
        for (auto &entry : displayed_) {
            if (!entry)
                continue;
            const auto id = entry->id;
            if (!ContainsCue(timeline, id)) {
                entry.reset();
                continue;
            }
            if (!sceneCurrent || !CaptionIsCurrent(entry->authority, grant)) {
                static_cast<void>(timeline.Withdraw(id));
                entry.reset();
            }
        }
    }

    /** @copydoc NpcDialogueScenePresentation::Retire */
    void NpcDialogueScenePresentation::Retire() noexcept {
        pending_.emplace<std::monostate>();
        cancellation_ = {};
        pendingDisclosure_.reset();
        queuedAuthority_.reset();
        queue_.reset();
        provider_.reset();
    }
}  // namespace Horo::App
