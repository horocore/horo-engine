#include "Horo/AI/LocalLearnedPolicy.h"

#include "Horo/AI/AIErrors.h"
#include "Horo/AI/AITaskScheduler.h"

#include <algorithm>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Horo::AI {
    namespace {
        constexpr std::uint32_t TensorSchemaV1 = 1;

        template <typename Identity> bool ValidIdentities(const std::span<const Identity> ids) noexcept {
            for (std::size_t index = 0; index < ids.size(); ++index) {
                const auto previous = ids.first(index);
                if (!ids[index].IsValid() || std::ranges::find(previous, ids[index]) != previous.end())
                    return false;
            }
            return true;
        }

        bool MatchesTask(const AiCapabilityDescriptorSet &tasks, const TaskId id,
                         const std::optional<BlackboardSchemaId> blackboard) noexcept {
            const auto declaration = tasks.Find(id);
            return declaration && (!declaration->blackboard || declaration->blackboard == blackboard);
        }

        bool ValidRequest(const LocalPolicyRequestIdentity &identity) noexcept {
            return identity.agent.IsValid() && identity.sceneGeneration != 0 && identity.plan.IsValid() &&
                   identity.providerGeneration != 0 && identity.inputRevision != 0 && identity.requestGeneration != 0 &&
                   identity.deadlineTick != 0;
        }

        /** @brief Encodes signed values by their canonical little-endian unsigned representation. */
        template <typename Integer> void HashNumber(Sha256Builder &digest, const Integer value) noexcept {
            const auto raw = static_cast<std::make_unsigned_t<Integer>>(value);
            std::array<std::byte, sizeof(Integer)> bytes{};
            for (std::size_t index = 0; index < bytes.size(); ++index)
                bytes[index] = std::byte((raw >> (8 * index)) & 0xffU);
            (void)digest.Update(bytes);
        }
    }  // namespace

    Result<LocalLearnedPolicy> LocalLearnedPolicy::Cook(const LocalPolicySource &source, const LocalPolicyCookTarget &target,
                                                        const AiCapabilityDescriptorSet &tasks,
                                                        const std::optional<BlackboardSchemaId> blackboard) {
        using Model = Result<LocalLearnedPolicy>;
        if (!source.model.IsValid() || source.tensorSchemaVersion != TensorSchemaV1 || source.features.empty() ||
            source.features.size() > MaximumFeatures || source.rows.empty() || source.rows.size() > MaximumRows ||
            !ValidIdentities(source.features) || (blackboard && !blackboard->IsValid()))
            return Model::Failure(MakeError(AIErrors::TaskContextInvalid));
        if (!target.id.IsValid() || !target.supportsInt16LinearV1 || source.targets.size() > MaximumTargets ||
            !ValidIdentities(source.targets) || std::ranges::find(source.targets, target.id) == source.targets.end())
            return Model::Failure(MakeError(AIErrors::CapabilityUnavailable));

        LocalLearnedPolicy result;
        result.featureCount_ = source.features.size();
        result.rowCount_ = source.rows.size();
        if (target.maximumModelBytes < sizeof(LocalLearnedPolicy) || target.maximumWorkUnits < result.RequiredWork())
            return Model::Failure(MakeError(AIErrors::SchedulerBudgetExhausted));
        result.target_ = target.id;
        result.blackboard_ = blackboard;
        result.tensorSchemaVersion_ = source.tensorSchemaVersion;
        std::ranges::copy(source.features, result.features_.begin());
        for (std::size_t index = 0; index < source.rows.size(); ++index) {
            const auto &row = source.rows[index];
            const auto previous = std::span{result.rows_}.first(index);
            if (!MatchesTask(tasks, row.task, blackboard) || row.weights.size() != source.features.size() ||
                std::ranges::find(previous, row.task, &Row::task) != previous.end())
                return Model::Failure(MakeError(AIErrors::TaskContextInvalid));
            result.rows_[index].task = row.task;
            result.rows_[index].bias = row.bias;
            std::ranges::copy(row.weights, result.rows_[index].weights.begin());
        }
        result.digest_ = result.Fingerprint(source.model);
        return Model::Success(std::move(result));
    }

    std::size_t LocalLearnedPolicy::RequiredWork() const noexcept {
        return featureCount_ + rowCount_ * (featureCount_ + 1);
    }

    Sha256Digest LocalLearnedPolicy::Fingerprint(const LocalPolicyModelId model) const noexcept {
        Sha256Builder digest;
        constexpr std::string_view format{"horo.local-policy.int16-linear.v1"};
        (void)digest.Update(std::as_bytes(std::span{format.data(), format.size()}));
        HashNumber(digest, model.Value());
        HashNumber(digest, target_.Value());
        HashNumber(digest, blackboard_ ? blackboard_->Value() : std::uint64_t{});
        HashNumber(digest, tensorSchemaVersion_);
        HashNumber(digest, static_cast<std::uint32_t>(featureCount_));
        HashNumber(digest, static_cast<std::uint32_t>(rowCount_));
        for (const auto id : std::span{features_}.first(featureCount_))
            HashNumber(digest, id.Value());
        for (const auto &row : std::span{rows_}.first(rowCount_)) {
            HashNumber(digest, row.task.Value());
            HashNumber(digest, row.bias);
            for (const auto weight : std::span{row.weights}.first(featureCount_))
                HashNumber(digest, weight);
        }
        return digest.Finalize();
    }

    LocalPolicyProposal LocalLearnedPolicy::Infer(
        const LocalPolicyRequestIdentity &identity, const LocalPolicyTargetId target, const Sha256Digest &expectedModel,
        const std::uint32_t tensorSchemaVersion, const std::span<const LocalPolicyFeature> features, const std::uint64_t tick,
        AiWorkBudget &work, const CancellationToken &cancellation) const noexcept {
        using enum LocalPolicyStatus;
        LocalPolicyProposal result;
        if (!ValidRequest(identity))
            return result;
        result.identity_ = identity;
        result.modelDigest_ = digest_;
        const auto fail = [&](const LocalPolicyStatus status) {
            result.status_ = status;
            return std::move(result);
        };
        if (cancellation.IsCancellationRequested())
            return fail(Cancelled);
        if (tick == 0 || tensorSchemaVersion != tensorSchemaVersion_ || features.size() != featureCount_)
            return result;
        if (tick > identity.deadlineTick)
            return fail(Expired);
        if (target != target_ || expectedModel != digest_)
            return fail(Stale);
        if (!work.Consume(RequiredWork()))
            return fail(BudgetExhausted);
        if (!std::ranges::equal(features, std::span{features_}.first(featureCount_), {}, &LocalPolicyFeature::id))
            return result;

        auto best = std::numeric_limits<std::int64_t>::min();
        for (const auto &row : std::span{rows_}.first(rowCount_)) {
            if (cancellation.IsCancellationRequested())
                return fail(Cancelled);
            std::int64_t score = row.bias;
            for (std::size_t index = 0; index < features.size(); ++index)
                score += std::int64_t{row.weights[index]} * features[index].value;
            if (score > best || (score == best && row.task < result.task_)) {
                best = score;
                result.task_ = row.task;
            }
        }
        if (cancellation.IsCancellationRequested())
            return fail(Cancelled);
        result.status_ = Proposed;
        return result;
    }

    LocalPolicyProposal::LocalPolicyProposal(LocalPolicyProposal &&other) noexcept
        : status_(std::exchange(other.status_, LocalPolicyStatus::Consumed)), identity_(other.identity_),
          modelDigest_(other.modelDigest_), task_(std::exchange(other.task_, {})) {}

    LocalPolicyOutcome LocalPolicyRequestSlot::Consume(
        LocalPolicyProposal &proposal, const LocalPolicyRequestIdentity &current, const LocalLearnedPolicy &model,
        const AiCapabilityDescriptorSet &tasks, const std::uint64_t tick, const CancellationToken &cancellation) noexcept {
        using enum LocalPolicyStatus;
        if (retired_)
            return {Consumed, {}};
        if (current != identity_) {
            retired_ = true;
            return {Stale, {}};
        }
        if (proposal.Status() == Consumed)
            return {Consumed, {}};
        if (proposal.identity_ != identity_)
            return {Stale, {}};
        retired_ = true;
        const auto status = std::exchange(proposal.status_, Consumed);
        const auto task = std::exchange(proposal.task_, {});
        if (cancellation.IsCancellationRequested())
            return {Cancelled, {}};
        if (model.Digest() != proposal.modelDigest_)
            return {Stale, {}};
        if (tick == 0 || tick > identity_.deadlineTick)
            return {Expired, {}};
        if (status != Proposed)
            return {status, {}};
        if (!MatchesTask(tasks, task, model.blackboard_))
            return {MissingTask, {}};
        return {Proposed, task};
    }
}  // namespace Horo::AI
