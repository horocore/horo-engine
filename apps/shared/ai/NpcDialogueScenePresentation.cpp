#include "NpcDialogueScenePresentation.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace Horo::App {
    namespace {
        using namespace AI;
        using Runtime::Ui::CaptionCueQueue;

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
    }  // namespace

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
    bool NpcDialogueScenePresentation::AgentIsActive(AI::AgentHandle agent) const {
        const auto record = runtime_->Find(agent);
        return record.HasValue() && record.Value().state == AI::AiAgentActivationState::Active;
    }

    /** @copydoc NpcDialogueScenePresentation::Create */
    Result<NpcDialogueScenePresentation> NpcDialogueScenePresentation::Create(
        const JobSystem &jobs, const AI::AiSceneRuntime &runtime, std::unique_ptr<Runtime::Ui::CaptionCueQueue> queue,
        std::span<const NpcDialogueCaption> captions, AI::NpcDialogueCueId fallback,
        std::optional<AI::NpcDialogueProviderBinding> provider) {
        using Channel = Result<NpcDialogueScenePresentation>;
        const auto scene = runtime.ActiveBinding();
        if (scene.HasError())
            return Channel::Failure(scene.ErrorValue());
        if (!queue || queue->Producer() != Accessibility::CaptionProducerKind::Semantic)
            return Channel::Failure(MakeError(AI::AIErrors::TaskContextInvalid));
        if (!ValidCaptions(captions, queue->Catalog()->size()) ||
            !std::ranges::binary_search(captions, fallback, {}, &NpcDialogueCaption::cue))
            return Channel::Failure(MakeError(AI::AIErrors::TaskContextInvalid));
        NpcDialogueScenePresentation channel{jobs, runtime, scene.Value(), std::move(queue), std::move(provider)};
        std::ranges::copy(captions, channel.captions_.begin());
        std::ranges::transform(captions, channel.cues_.begin(), &NpcDialogueCaption::cue);
        channel.count_ = captions.size();
        channel.fallback_ = fallback;
        return Channel::Success(std::move(channel));
    }

    /** @copydoc NpcDialogueScenePresentation::Start */
    Result<void> NpcDialogueScenePresentation::Start(AI::AgentHandle agent, const AI::NpcDialogueRoute &route,
                                                    const AI::NpcDialogueGrant &grant, std::span<const AI::NpcDialogueFact> facts,
                                                    std::uint64_t tick, std::uint64_t deadlineTick,
                                                    std::chrono::steady_clock::time_point deadline,
                                                    const CancellationToken &cancellation) {
        if (!queue_ || !SceneIsCurrent() || !AgentIsActive(agent))
            return Result<void>::Failure(MakeError(AI::AIErrors::HandleInvalid));
        if (!std::holds_alternative<std::monostate>(pending_) || lastTurn_ == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(AI::AIErrors::TaskCapacityExceeded));
        const AI::NpcDialogueRequestIdentity request{agent, grant.providerGeneration, lastTurn_ + 1};
        auto captured = AI::NpcDialogueTurn::Capture(request, route, grant, facts, std::span{cues_}.first(count_), fallback_, deadlineTick);
        if (captured.HasError())
            return Result<void>::Failure(std::move(captured).ErrorValue());
        auto turn = std::move(captured).Value();
        lastTurn_ = request.turn;
        request_ = request;
        cancellation_ = cancellation;
        if (provider_) {
            auto job = AI::NpcDialogueJob::Start(*jobs_, std::move(turn), request, grant, *provider_, tick, deadline, cancellation);
            if (job.HasValue()) {
                pending_.emplace<AI::NpcDialogueJob>(std::move(job).Value());
                return Result<void>::Success();
            }
        }
        pending_.emplace<AI::NpcDialogueTurn>(std::move(turn));
        return Result<void>::Success();
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
        const auto current = AgentIsActive(request_.agent) ? request_ : AI::NpcDialogueRequestIdentity{};
        std::optional<AI::NpcDialogueOutcome> outcome;
        if (auto *job = std::get_if<AI::NpcDialogueJob>(&pending_))
            outcome = job->Poll(current, grant, tick);
        else if (auto *turn = std::get_if<AI::NpcDialogueTurn>(&pending_))
            outcome = turn->Resolve(current, grant, tick, cancellation_, std::nullopt);
        if (!outcome)
            return std::nullopt;
        pending_.emplace<std::monostate>();
        cancellation_ = {};
        const auto captions = std::span{captions_}.first(count_);
        const auto cue = std::ranges::lower_bound(captions, outcome->cue, {}, &NpcDialogueCaption::cue);
        if (cue == captions.end() || cue->cue != outcome->cue)
            return std::nullopt;
        return queue_->TryEnqueue({{queue_->Session(), current.turn}, Accessibility::CaptionCueKind::Dialogue,
                                    cue->text, cue->speaker, tick, cue->durationTicks});
    }

    /** @copydoc NpcDialogueScenePresentation::Drain */
    std::size_t NpcDialogueScenePresentation::Drain(Runtime::Ui::CaptionTimeline &timeline) {
        if (queue_ && !SceneIsCurrent())
            Retire();
        return queue_ ? timeline.Drain(*queue_) : 0;
    }

    /** @copydoc NpcDialogueScenePresentation::Retire */
    void NpcDialogueScenePresentation::Retire() noexcept {
        pending_.emplace<std::monostate>();
        cancellation_ = {};
        queue_.reset();
        provider_.reset();
    }
}  // namespace Horo::App
