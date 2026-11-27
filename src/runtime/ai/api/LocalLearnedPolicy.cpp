#include "Horo/AI/LocalLearnedPolicy.h"

#include "Horo/AI/AIErrors.h"
#include "Horo/AI/AITaskScheduler.h"

#include <algorithm>
#include <bit>
#include <iterator>
#include <limits>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Horo::AI {
    namespace {
        constexpr std::uint32_t TensorSchemaV1 = 1;
        constexpr std::string_view WireFormat{"horo.local-policy.int16-linear.v1"};
        constexpr std::size_t WireHeaderBytes = WireFormat.size() + 3 * sizeof(std::uint64_t) + 3 * sizeof(std::uint32_t);

        constexpr std::size_t WireSize(const std::size_t features, const std::size_t rows) noexcept {
            return WireHeaderBytes + sizeof(std::uint64_t) * features +
                   rows * (sizeof(std::uint64_t) + sizeof(std::int32_t) + sizeof(std::int16_t) * features);
        }

        /** @brief Reads only after its caller admits the exact fixed header or payload size. */
        struct WireCursor final {
            std::span<const std::uint8_t> remaining;

            template <typename Integer> Integer Number() noexcept {
                std::uint64_t raw{};
                for (std::size_t offset = 0; offset < sizeof(Integer); ++offset)
                    raw |= std::uint64_t{remaining[offset]} << (offset * 8U);
                remaining = remaining.subspan(sizeof(Integer));
                return std::bit_cast<Integer>(static_cast<std::make_unsigned_t<Integer>>(raw));
            }

            template <typename Identity> Identity Id() {
                auto parsed = Identity::Create(Number<std::uint64_t>());
                return parsed.HasValue() ? parsed.Value() : Identity{};
            }
        };

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
        template <typename Write, typename Integer> void WriteNumber(const Write &write, const Integer value) {
            const auto raw = static_cast<std::make_unsigned_t<Integer>>(value);
            std::array<std::byte, sizeof(Integer)> bytes{};
            for (std::size_t index = 0; index < bytes.size(); ++index)
                bytes[index] = std::byte((raw >> (8 * index)) & 0xffU);
            write(bytes);
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
        result.model_ = source.model;
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
        result.digest_ = result.Fingerprint();
        return Model::Success(std::move(result));
    }

    std::size_t LocalLearnedPolicy::RequiredWork() const noexcept {
        return featureCount_ + rowCount_ * (featureCount_ + 1);
    }

    template <typename Write> void LocalLearnedPolicy::WriteCanonical(const Write write) const {
        write(std::as_bytes(std::span{WireFormat.data(), WireFormat.size()}));
        WriteNumber(write, model_.Value());
        WriteNumber(write, target_.Value());
        WriteNumber(write, blackboard_ ? blackboard_->Value() : std::uint64_t{});
        WriteNumber(write, tensorSchemaVersion_);
        WriteNumber(write, static_cast<std::uint32_t>(featureCount_));
        WriteNumber(write, static_cast<std::uint32_t>(rowCount_));
        for (const auto id : std::span{features_}.first(featureCount_))
            WriteNumber(write, id.Value());
        for (const auto &row : std::span{rows_}.first(rowCount_)) {
            WriteNumber(write, row.task.Value());
            WriteNumber(write, row.bias);
            for (const auto weight : std::span{row.weights}.first(featureCount_))
                WriteNumber(write, weight);
        }
    }

    Sha256Digest LocalLearnedPolicy::Fingerprint() const noexcept {
        Sha256Builder digest;
        WriteCanonical([&](const std::span<const std::byte> bytes) {
            (void)digest.Update(bytes);
        });
        return digest.Finalize();
    }

    Result<std::vector<std::uint8_t>> LocalLearnedPolicy::EncodeCooked() const {
        using Encoded = Result<std::vector<std::uint8_t>>;
        try {
            std::vector<std::uint8_t> output;
            output.reserve(WireSize(featureCount_, rowCount_));
            WriteCanonical([&](const std::span<const std::byte> bytes) {
                std::ranges::transform(bytes, std::back_inserter(output), [](const std::byte byte) {
                    return std::to_integer<std::uint8_t>(byte);
                });
            });
            return Encoded::Success(std::move(output));
        } catch (const std::bad_alloc &) {
            return Encoded::Failure(MakeError(AIErrors::DecisionAssetStorageUnavailable));
        }
    }

    Result<LocalLearnedPolicy> LocalLearnedPolicy::DecodeCooked(const std::span<const std::uint8_t> bytes,
                                                                const Sha256Digest &expectedDigest, const LocalPolicyCookTarget &target,
                                                                const AiCapabilityDescriptorSet &tasks,
                                                                const std::optional<BlackboardSchemaId> blackboard) {
        using Model = Result<LocalLearnedPolicy>;
        if (bytes.size() < WireHeaderBytes || bytes.size() > WireSize(MaximumFeatures, MaximumRows) ||
            !std::ranges::equal(bytes.first(WireFormat.size()), WireFormat))
            return Model::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));
        if (ComputeSha256(std::as_bytes(bytes)) != expectedDigest)
            return Model::Failure(MakeError(AIErrors::DecisionAssetActivationInvalid));

        try {
            WireCursor wire{bytes.subspan(WireFormat.size())};
            const auto id = wire.Id<LocalPolicyModelId>();
            const auto artifactTarget = wire.Id<LocalPolicyTargetId>();
            const auto encodedSchema = wire.Number<std::uint64_t>();
            const auto version = wire.Number<std::uint32_t>();
            const auto features = wire.Number<std::uint32_t>();
            const auto rows = wire.Number<std::uint32_t>();
            if (features == 0 || features > MaximumFeatures || rows == 0 || rows > MaximumRows ||
                bytes.size() != WireSize(features, rows) || encodedSchema != (blackboard ? blackboard->Value() : 0U))
                return Model::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));

            std::array<LocalPolicyFeatureId, MaximumFeatures> fields{};
            for (auto &field : std::span{fields}.first(features))
                field = wire.Id<LocalPolicyFeatureId>();
            std::array<std::array<std::int16_t, MaximumFeatures>, MaximumRows> weights{};
            std::array<LocalPolicyRowSource, MaximumRows> sources{};
            for (std::size_t index = 0; index < rows; ++index) {
                const auto task = wire.Id<TaskId>();
                const auto bias = wire.Number<std::int32_t>();
                auto row = std::span{weights[index]}.first(features);
                for (auto &value : row)
                    value = wire.Number<std::int16_t>();
                sources[index] = {task, bias, row};
            }
            const std::array targets{artifactTarget};
            return Cook({id, version, targets, std::span{fields}.first(features), std::span{sources}.first(rows)}, target, tasks,
                        blackboard);
        } catch (const std::bad_alloc &) {
            return Model::Failure(MakeError(AIErrors::DecisionAssetStorageUnavailable));
        }
    }

    LocalPolicyProposal LocalLearnedPolicy::Infer(const LocalPolicyRequestIdentity &identity, const LocalPolicyTargetId target,
                                                  const Sha256Digest &expectedModel, const std::uint32_t tensorSchemaVersion,
                                                  const std::span<const LocalPolicyFeature> features, const std::uint64_t tick,
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
        : status_(std::exchange(other.status_, LocalPolicyStatus::Consumed)), identity_(other.identity_), modelDigest_(other.modelDigest_),
          task_(std::exchange(other.task_, {})) {}

    LocalPolicyOutcome LocalPolicyRequestSlot::Consume(LocalPolicyProposal &proposal, const LocalPolicyRequestIdentity &current,
                                                       const LocalLearnedPolicy &model, const AiCapabilityDescriptorSet &tasks,
                                                       const std::uint64_t tick, const CancellationToken &cancellation) noexcept {
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
