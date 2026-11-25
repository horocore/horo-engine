#include "Horo/AI/NpcDialogue.h"

#include <algorithm>
#include <new>
#include <utility>

namespace Horo::AI {
    namespace {
        /** @brief Requires canonical distinct nonzero authored IDs. */
        template <typename Id> bool CanonicalIds(std::span<const Id> ids) noexcept {
            return (ids.empty() || ids.front().IsValid()) && std::ranges::is_sorted(ids) &&
                   std::ranges::adjacent_find(ids) == ids.end();
        }

        /** @brief Validates every source field identity and its order before projection. */
        bool CanonicalFacts(std::span<const NpcDialogueFact> facts) noexcept {
            return (facts.empty() || facts.front().field.IsValid()) &&
                   std::ranges::is_sorted(facts, {}, &NpcDialogueFact::field) &&
                   std::ranges::adjacent_find(facts, {}, &NpcDialogueFact::field) == facts.end();
        }

        /** @brief Compares the exact route, activation, and bounded list of approved fields. */
        bool Grants(const NpcDialogueGrant &grant, const NpcDialogueRoute &route, std::uint64_t generation) noexcept {
            return grant.route == route && grant.providerGeneration == generation &&
                   grant.fields.size() <= NpcDialogueTurn::MaximumFacts && CanonicalIds(grant.fields);
        }

        /** @brief Requires nonzero local identity, route and deadline for the declared turn. */
        bool ValidTurnIdentity(const NpcDialogueRequestIdentity &identity, const NpcDialogueRoute &route,
                               std::uint64_t deadline) noexcept {
            return identity.agent.IsValid() && identity.providerGeneration != 0 && identity.turn != 0 && deadline != 0 &&
                   route.provider.IsValid() && route.destination.IsValid() && route.version != 0;
        }

        /** @brief Checks identities and the owner's bounded authored input before copying anything. */
        Result<void> ValidateTurn(const NpcDialogueRequestIdentity &identity, const NpcDialogueRoute &route,
                                  std::span<const NpcDialogueFact> facts, std::span<const NpcDialogueCueId> cues,
                                  NpcDialogueCueId fallback, std::uint64_t deadline) {
            if (facts.size() > NpcDialogueTurn::MaximumFacts || cues.size() > NpcDialogueTurn::MaximumCues)
                return Result<void>::Failure(MakeError(AIErrors::DescriptorLimitExceeded));
            if (!ValidTurnIdentity(identity, route, deadline) || !CanonicalFacts(facts) || !CanonicalIds(cues) ||
                !std::ranges::binary_search(cues, fallback))
                return Result<void>::Failure(MakeError(AIErrors::TaskContextInvalid));
            return Result<void>::Success();
        }

        /** @brief Admits a moderated reply only when it names an authored cue. */
        bool IsApprovedCue(const std::optional<NpcDialogueReply> &reply, std::span<const NpcDialogueCueId> allowed) noexcept {
            return reply && reply->approved && std::ranges::binary_search(allowed, reply->cue);
        }
    }  // namespace

    /** @copydoc NpcDialogueTurn::Capture */
    Result<NpcDialogueTurn> NpcDialogueTurn::Capture(const NpcDialogueRequestIdentity &identity, const NpcDialogueRoute &route,
                                                     const NpcDialogueGrant &grant, std::span<const NpcDialogueFact> facts,
                                                     std::span<const NpcDialogueCueId> cues, NpcDialogueCueId fallback,
                                                     std::uint64_t deadlineTick) {
        using Turn = Result<NpcDialogueTurn>;
        if (auto validated = ValidateTurn(identity, route, facts, cues, fallback, deadlineTick); validated.HasError())
            return Turn::Failure(std::move(validated).ErrorValue());
        if (!Grants(grant, route, identity.providerGeneration))
            return Turn::Failure(MakeError(AIErrors::CapabilityUnavailable));

        NpcDialogueTurn turn;
        turn.identity_ = identity;
        turn.route_ = route;
        turn.deadlineTick_ = deadlineTick;
        turn.fallback_ = fallback;
        turn.cueCount_ = cues.size();
        std::ranges::copy(cues, turn.cues_.begin());
        for (const auto &fact : facts) {
            if (!std::ranges::binary_search(grant.fields, fact.field))
                continue;
            if (!fact.value.IsValid())
                return Turn::Failure(MakeError(AIErrors::TaskContextInvalid));
            turn.facts_[turn.factCount_++] = fact;
        }
        return Turn::Success(std::move(turn));
    }

    /** @copydoc NpcDialogueTurn::NpcDialogueTurn */
    NpcDialogueTurn::NpcDialogueTurn(NpcDialogueTurn &&other) noexcept
        : identity_(other.identity_), route_(other.route_), deadlineTick_(other.deadlineTick_), facts_(other.facts_), cues_(other.cues_),
          factCount_(other.factCount_), cueCount_(other.cueCount_), fallback_(other.fallback_), consumed_(std::exchange(other.consumed_, true)) {}

    /** @copydoc NpcDialogueTurn::AllowsProjection */
    bool NpcDialogueTurn::AllowsProjection(const NpcDialogueGrant &grant) const noexcept {
        return Grants(grant, route_, identity_.providerGeneration) &&
               std::ranges::all_of(std::span{facts_}.first(factCount_), [&grant](const NpcDialogueFact &fact) {
                   return std::ranges::binary_search(grant.fields, fact.field);
               });
    }

    /** @copydoc NpcDialogueTurn::Context */
    std::optional<std::span<const NpcDialogueFact>> NpcDialogueTurn::Context(const NpcDialogueRequestIdentity &current,
                                                                           const NpcDialogueGrant &grant, std::uint64_t tick,
                                                                           const CancellationToken &cancellation) const noexcept {
        if (consumed_ || current != identity_ || tick >= deadlineTick_ || cancellation.IsCancellationRequested() || !AllowsProjection(grant))
            return std::nullopt;
        return std::span{facts_}.first(factCount_);
    }

    /** @copydoc NpcDialogueTurn::Resolve */
    NpcDialogueOutcome NpcDialogueTurn::Resolve(const NpcDialogueRequestIdentity &current, const NpcDialogueGrant &grant,
                                                 std::uint64_t tick, const CancellationToken &cancellation,
                                                 const std::optional<NpcDialogueReply> &reply) noexcept {
        using enum NpcDialogueStatus;
        if (current != identity_ || (reply && reply->origin != identity_))
            return {Stale};
        if (std::exchange(consumed_, true))
            return {Consumed};
        if (cancellation.IsCancellationRequested())
            return {Cancelled};
        if (tick < deadlineTick_ && AllowsProjection(grant) && IsApprovedCue(reply, std::span{cues_}.first(cueCount_)))
            return {Selected, reply->cue};
        return {Fallback, fallback_};
    }

    /** @brief The worker owns its payload and pin. Only a terminal snapshot permits the owner to read the reply. */
    struct NpcDialogueJob::Work final {
        NpcDialogueProviderBinding binding;
        std::array<NpcDialogueFact, NpcDialogueTurn::MaximumFacts> facts{};
        std::size_t count{};
        std::chrono::steady_clock::time_point deadline;
        CancellationSource cancellation;
        std::optional<NpcDialogueReply> reply;

        /** @brief Copies the projection while the owner-lane grant is live. */
        Work(const NpcDialogueProviderBinding &provider, std::span<const NpcDialogueFact> context,
             std::chrono::steady_clock::time_point cutoff, const CancellationToken &parent)
            : binding(provider), count(context.size()), deadline(cutoff), cancellation(parent) {
            std::ranges::copy(context, facts.begin());
        }

        /** @brief Dispatches only approved tokens and stamps a successful reply with its local origin. */
        void Run(const NpcDialogueRequestIdentity &origin, const CancellationToken &token) {
            if (token.IsCancellationRequested() || std::chrono::steady_clock::now() >= deadline)
                return;
            try {
                const auto cue = binding.provider->RequestCue(std::span{facts}.first(count), deadline, token);
                if (cue.HasValue() && !token.IsCancellationRequested() && std::chrono::steady_clock::now() < deadline)
                    reply = NpcDialogueReply{origin, cue.Value(), true};
            } catch (...) {
                // The owner selects a fallback. Provider exception text must not enter the scheduler's diagnostics.
            }
        }
    };

    /** @copydoc NpcDialogueJob::Start */
    Result<NpcDialogueJob> NpcDialogueJob::Start(const JobSystem &jobs, NpcDialogueTurn &&turn, const NpcDialogueRequestIdentity &current,
                                                 const NpcDialogueGrant &grant, const NpcDialogueProviderBinding &binding, std::uint64_t tick,
                                                 std::chrono::steady_clock::time_point deadline, const CancellationToken &cancellation) {
        using Accepted = Result<NpcDialogueJob>;
        if (!binding.provider || !binding.imageLease || binding.route != grant.route || binding.generation != grant.providerGeneration)
            return Accepted::Failure(MakeError(AIErrors::CapabilityUnavailable));
        const auto context = turn.Context(current, grant, tick, cancellation);
        if (!context || std::chrono::steady_clock::now() >= deadline)
            return Accepted::Failure(MakeError(AIErrors::TaskContextInvalid));
        try {
            auto work = std::make_shared<Work>(binding, *context, deadline, cancellation);
            auto job = jobs.Submit({.parentCancellation = work->cancellation.Token(), .requirement = JobRequirement::Optional,
                                    .resource = JobResource::Io},
                                   [work, current](const CancellationToken &token) { work->Run(current, token); });
            if (job.HasError())
                return Accepted::Failure(std::move(job).ErrorValue());
            return Accepted::Success(NpcDialogueJob{std::move(turn), std::move(job).Value(), std::move(work)});
        } catch (const std::bad_alloc &) {
            return Accepted::Failure(MakeError(AIErrors::TaskCapacityExceeded));
        }
    }

    /** @brief Takes ownership of a successfully queued turn and its scheduler receipt. */
    NpcDialogueJob::NpcDialogueJob(NpcDialogueTurn &&turn, JobHandle &&job, std::shared_ptr<Work> work) noexcept
        : turn_(std::move(turn)), job_(std::move(job)), work_(std::move(work)) {}

    /** @copydoc NpcDialogueJob::~NpcDialogueJob */
    NpcDialogueJob::~NpcDialogueJob() {
        if (work_)
            work_->cancellation.RequestCancellation();
    }

    /** @copydoc NpcDialogueJob::Poll */
    std::optional<NpcDialogueOutcome> NpcDialogueJob::Poll(const NpcDialogueRequestIdentity &current, const NpcDialogueGrant &grant,
                                                           std::uint64_t tick) {
        using enum JobState;
        if (!work_)
            return NpcDialogueOutcome{NpcDialogueStatus::Consumed};
        const auto token = work_->cancellation.Token();
        if (!turn_.Context(current, grant, tick, token) || std::chrono::steady_clock::now() >= work_->deadline) {
            const auto outcome = turn_.Resolve(current, grant, tick, token, std::nullopt);
            work_->cancellation.RequestCancellation();
            work_.reset();
            return outcome;
        }
        const auto snapshot = job_.Snapshot();
        if (snapshot && (snapshot->state == Queued || snapshot->state == Running))
            return std::nullopt;
        // The scheduler publishes terminal state under its record lock after the callback has returned.
        const auto reply = snapshot && snapshot->state == Succeeded ? work_->reply : std::nullopt;
        const auto outcome = turn_.Resolve(current, grant, tick, token, reply);
        work_.reset();
        return outcome;
    }
}  // namespace Horo::AI
