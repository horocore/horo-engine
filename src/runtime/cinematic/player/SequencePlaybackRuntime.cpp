#include "Horo/Cinematic/SequencePlaybackRuntime.h"

#include "Horo/Cinematic/SequencePlaybackRuntimeErrors.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

namespace Horo::Cinematic {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failed(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        template <typename Identity> [[nodiscard]] bool IdentityLess(const Identity &left, const Identity &right) noexcept {
            if (left.stableValue != right.stableValue)
                return left.stableValue < right.stableValue;
            return left.generation < right.generation;
        }

        [[nodiscard]] bool SameStableTarget(const SequenceRestoreTargetId left, const SequenceRestoreTargetId right) noexcept {
            return left.stableValue == right.stableValue;
        }

        [[nodiscard]] bool SameStableTarget(const SequenceAuthorityTargetId left, const SequenceAuthorityTargetId right) noexcept {
            return left.stableValue == right.stableValue;
        }

        [[nodiscard]] bool SameAuthorityKey(const SequenceAuthorityClaim &left, const SequenceAuthorityClaim &right) noexcept {
            return SameStableTarget(left.target, right.target) && left.channel == right.channel;
        }

        [[nodiscard]] bool SameAuthorityKey(const SequenceAuthorityClaim &claim, const SequenceGameplayWriteRequest &request) noexcept {
            return SameStableTarget(claim.target, request.target) && claim.channel == request.channel;
        }

        [[nodiscard]] const SequenceRestoreTargetSnapshot *FindRestoreTarget(const std::span<const SequenceRestoreTargetSnapshot> targets,
                                                                             const SequenceRestoreTargetId target) noexcept {
            for (const SequenceRestoreTargetSnapshot &candidate : targets) {
                if (SameStableTarget(candidate.target, target))
                    return &candidate;
            }
            return nullptr;
        }

        /** @brief Classifies every captured target before any owner write is allowed. */
        [[nodiscard]] SequenceRestoreResult PreflightRestoreTargets(const std::span<const SequenceRestoreEntry> entries,
                                                                    const std::span<const SequenceRestoreTargetSnapshot> targets,
                                                                    const std::span<SequenceRestoreDiagnostic> diagnostics) noexcept {
            SequenceRestoreResult result{};
            for (std::size_t index = 0; index < entries.size(); ++index) {
                const SequenceRestoreEntry &entry = entries[index];
                SequenceRestoreDiagnostic &diagnostic = diagnostics[index];
                diagnostic = {entry.track, entry.target, SequenceRestoreOutcome::TargetMissing};
                const SequenceRestoreTargetSnapshot *target = FindRestoreTarget(targets, entry.target);
                if (target == nullptr || !target->target.IsValid() || target->context == nullptr || target->apply == nullptr) {
                    ++result.missing;
                    continue;
                }
                if (target->target.generation != entry.target.generation || target->targetRevision != entry.targetRevision ||
                    std::ranges::count_if(targets, [&](const SequenceRestoreTargetSnapshot &candidate) {
                    return SameStableTarget(candidate.target, entry.target);
                }) != 1) {
                    diagnostic.outcome = SequenceRestoreOutcome::StaleGeneration;
                    ++result.stale;
                    continue;
                }
                diagnostic.outcome = SequenceRestoreOutcome::Restored;
            }
            return result;
        }

        [[nodiscard]] bool AuthorityClaimLess(const SequenceAuthorityClaim &left, const SequenceAuthorityClaim &right) noexcept {
            if (left.target.stableValue != right.target.stableValue)
                return left.target.stableValue < right.target.stableValue;
            if (left.channel != right.channel)
                return left.channel < right.channel;
            if (left.priority != right.priority)
                return left.priority > right.priority;
            if (left.player.player != right.player.player)
                return IdentityLess(left.player.player, right.player.player);
            if (left.player.session != right.player.session)
                return IdentityLess(left.player.session, right.player.session);
            return left.target.generation < right.target.generation;
        }

        [[nodiscard]] bool SamePlayerClaim(const SequenceAuthorityClaim &left, const SequenceAuthorityClaim &right) noexcept {
            return left.target == right.target && left.channel == right.channel && left.player == right.player;
        }

        [[nodiscard]] bool IsValidChannel(const CinematicControlChannel channel) noexcept {
            return channel < CinematicControlChannel::Count;
        }

        [[nodiscard]] bool IsValidClaimMode(const CinematicClaimMode mode) noexcept {
            return mode < CinematicClaimMode::Count;
        }

        [[nodiscard]] bool IsBlendableChannel(const CinematicControlChannel channel) noexcept {
            return channel == CinematicControlChannel::SkeletalPose || channel == CinematicControlChannel::AnimationParameter;
        }

        [[nodiscard]] bool IsTerminal(const SequencePlaybackState state) noexcept {
            return state == SequencePlaybackState::Stopped || state == SequencePlaybackState::Failed;
        }

        /** @brief Binds activation-owned baselines and edge durations to one evaluation call. */
        void ConfigureBlendScratch(SequenceFrameScratch &scratch, const SequencePlaybackBlendSettings &settings,
                                   const std::span<const float> baselines) noexcept {
            scratch.blendBaselines = baselines;
            scratch.blendInDuration = settings.blendIn.mode == SequenceBlendMode::Blend ? settings.blendIn.duration : 0;
            scratch.blendOutDuration = settings.blendOut.mode == SequenceBlendMode::Blend ? settings.blendOut.duration : 0;
        }

        [[nodiscard]] std::uint32_t SaturatingAdd(const std::uint32_t left, const std::uint32_t right) noexcept {
            if (right > std::numeric_limits<std::uint32_t>::max() - left)
                return std::numeric_limits<std::uint32_t>::max();
            return left + right;
        }

        [[nodiscard]] std::uint64_t SaturatingAdd(const std::uint64_t left, const std::uint64_t right) noexcept {
            if (right > std::numeric_limits<std::uint64_t>::max() - left)
                return std::numeric_limits<std::uint64_t>::max();
            return left + right;
        }

        /** @brief Scaled source interval and fractional carry for the next clock sample. */
        struct ScaledClockDelta final {
            SequenceTime value{};
            std::uint64_t remainder{};
        };

        /** @brief Scales a non-negative source interval without overflowing an intermediate product. */
        [[nodiscard]] Result<ScaledClockDelta> ScaleClockDelta(const SequenceTime rawDelta, const SequencePlaybackRate scale,
                                                               const std::uint64_t remainder) {
            const auto numerator = static_cast<std::uint64_t>(scale.numerator);
            const auto denominator = static_cast<std::uint64_t>(scale.denominator);
            const auto whole = static_cast<std::uint64_t>(rawDelta) / denominator;
            const auto fraction = static_cast<std::uint64_t>(rawDelta) % denominator;
            // The bounded 32-bit ratio fields keep this product and carry within uint64_t.
            const std::uint64_t fractionalProduct = fraction * numerator + remainder;
            if (const auto wholeLimit = static_cast<std::uint64_t>(std::numeric_limits<SequenceTime>::max());
                numerator != 0 && whole > (wholeLimit - fractionalProduct / denominator) / numerator)
                return Failed<ScaledClockDelta>(SequencePlaybackRuntimeErrors::ClockInvalid);
            return Result<ScaledClockDelta>::Success(
                {static_cast<SequenceTime>(whole * numerator + fractionalProduct / denominator), fractionalProduct % denominator});
        }

        /** @brief Checks source identity, scale shape, and monotonic ordering within an epoch. */
        [[nodiscard]] bool IsValidClockSample(const SequenceClockSample &sample, const SequenceClockSource source, const bool baselineValid,
                                              const std::uint64_t epoch, const SequenceTime position) noexcept {
            return sample.source == source && sample.position >= 0 && sample.epoch != 0 && sample.gameplayScale.numerator >= 0 &&
                   sample.gameplayScale.denominator != 0 &&
                   (!baselineValid || (sample.epoch >= epoch && (sample.epoch != epoch || sample.position >= position)));
        }

        [[nodiscard]] SequenceEvaluationUsage AddUsage(const SequenceEvaluationUsage &left, const SequenceEvaluationUsage &right) noexcept {
            return {SaturatingAdd(left.activePlayers, right.activePlayers), SaturatingAdd(left.aggregateTracks, right.aggregateTracks),
                    SaturatingAdd(left.boundaryOccurrences, right.boundaryOccurrences),
                    SaturatingAdd(left.retainedBytes, right.retainedBytes),
                    std::max(left.maximumLoopCrossings, right.maximumLoopCrossings)};
        }

        [[nodiscard]] bool IsCanonicalBudget(const SequenceEvaluationBudget &budget) {
            auto canonical = GetSequenceEvaluationBudget(budget.tier);
            return canonical.HasValue() && canonical.Value() == budget;
        }

        [[nodiscard]] Result<void> ValidateAuthorityClaim(const SequenceAuthorityClaim &claim, const std::uint64_t authorityRevision,
                                                          const std::uint64_t eligibleSimulationTick) {
            using enum CinematicClaimMode;
            if (!claim.player.IsValid() || !claim.target.IsValid() || !IsValidChannel(claim.channel) || !IsValidClaimMode(claim.mode) ||
                claim.authorityRevision != authorityRevision || claim.eligibleSimulationTick != eligibleSimulationTick)
                return Failed<void>(SequencePlaybackRuntimeErrors::ActivationInvalid);
            if (claim.mode == PresentationOverlay && claim.channel != CinematicControlChannel::SkeletalPose)
                return Failed<void>(SequencePlaybackRuntimeErrors::AuthorityConflict);
            if (claim.mode == Blend && !IsBlendableChannel(claim.channel))
                return Failed<void>(SequencePlaybackRuntimeErrors::AuthorityConflict);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateAuthorityClaimPair(const SequenceAuthorityClaim &left, const SequenceAuthorityClaim &right) {
            using enum CinematicClaimMode;
            if (left.target.generation != right.target.generation)
                return Failed<void>(SequencePlaybackRuntimeErrors::AuthorityConflict);
            if (left.required && right.required && left.mode == Exclusive && right.mode == Exclusive && left.priority == right.priority)
                return Failed<void>(SequencePlaybackRuntimeErrors::AuthorityConflict);
            if (const bool requiredExclusiveBlendConflict =
                    left.required && right.required &&
                    ((left.mode == Exclusive && right.mode == Blend) || (left.mode == Blend && right.mode == Exclusive));
                requiredExclusiveBlendConflict)
                return Failed<void>(SequencePlaybackRuntimeErrors::AuthorityConflict);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateAuthorityClaims(const std::span<const SequenceAuthorityClaim> claims) {
            for (std::size_t index = 1; index < claims.size(); ++index) {
                if (SamePlayerClaim(claims[index - 1], claims[index]))
                    return Failed<void>(SequencePlaybackRuntimeErrors::AuthorityConflict);
            }
            for (std::size_t left = 0; left < claims.size(); ++left) {
                for (std::size_t right = left + 1; right < claims.size() && SameAuthorityKey(claims[left], claims[right]); ++right) {
                    if (auto valid = ValidateAuthorityClaimPair(claims[left], claims[right]); valid.HasError())
                        return valid;
                }
            }
            return Result<void>::Success();
        }

    }  // namespace

    /** @copydoc GetSequenceEvaluationBudget */
    Result<SequenceEvaluationBudget> GetSequenceEvaluationBudget(const SequenceCookTier tier) {
        using enum SequenceCookTier;
        switch (tier) {
            case Compact:
                return Result<SequenceEvaluationBudget>::Success({tier, 2, 32, 64, 64, 4, 1'048'576});
            case Standard:
                return Result<SequenceEvaluationBudget>::Success({tier, 8, 256, 2'048, 512, 8, 8'388'608});
            case Large:
                return Result<SequenceEvaluationBudget>::Success({tier, 32, 1'024, 32'768, 2'048, 16, 33'554'432});
            case Count:
                break;
        }
        return Failed<SequenceEvaluationBudget>(SequencePlaybackRuntimeErrors::BudgetInvalid);
    }

    /** @copydoc AdmitSequenceEvaluationUsage */
    Result<void> AdmitSequenceEvaluationUsage(const SequenceEvaluationUsage &current, const SequenceEvaluationUsage &additional,
                                              const SequenceEvaluationBudget &budget) {
        if (!IsCanonicalBudget(budget))
            return Failed<void>(SequencePlaybackRuntimeErrors::BudgetInvalid);
        if (const SequenceEvaluationUsage total = AddUsage(current, additional);
            total.activePlayers > budget.maximumActivePlayers || total.aggregateTracks > budget.maximumAggregateTracks ||
            total.boundaryOccurrences > budget.maximumBoundaryOccurrences || total.retainedBytes > budget.maximumRetainedBytes ||
            total.maximumLoopCrossings > budget.maximumLoopCrossings)
            return Failed<void>(SequencePlaybackRuntimeErrors::CapacityExceeded);
        return Result<void>::Success();
    }

    /** @copydoc ValidateSequencePlaybackBlendSettings */
    Result<void> ValidateSequencePlaybackBlendSettings(const SequencePlaybackBlendSettings &settings) {
        if (const auto validateWindow =
                [](const SequenceBlendWindow &window) {
            if (window.mode >= SequenceBlendMode::Count || window.duration < 0)
                return false;
            if (window.mode == SequenceBlendMode::Cut)
                return window.duration == 0;
            return window.duration > 0;
        };
            !validateWindow(settings.blendIn) || !validateWindow(settings.blendOut) ||
            settings.restorePolicy >= SequenceRestorePolicy::Count)
            return Failed<void>(SequencePlaybackRuntimeErrors::ActivationInvalid);
        return Result<void>::Success();
    }

    /** @copydoc EvaluateSequenceBlendWeight */
    Result<float> EvaluateSequenceBlendWeight(const SequenceTime elapsed, const SequenceTime duration) {
        if (elapsed < 0 || duration <= 0)
            return Failed<float>(SequencePlaybackRuntimeErrors::ActivationInvalid);
        if (elapsed >= duration)
            return Result<float>::Success(1.0F);
        const double weight = static_cast<double>(elapsed) / static_cast<double>(duration);
        if (!std::isfinite(weight))
            return Failed<float>(SequencePlaybackRuntimeErrors::ActivationInvalid);
        return Result<float>::Success(static_cast<float>(weight));
    }

    /** @copydoc BlendSequenceScalar */
    Result<float> BlendSequenceScalar(const float baseline, const float cinematic, const float weight) {
        if (!std::isfinite(baseline) || !std::isfinite(cinematic) || !std::isfinite(weight) || weight < 0.0F || weight > 1.0F)
            return Failed<float>(SequencePlaybackRuntimeErrors::ActivationInvalid);
        const float value = std::lerp(baseline, cinematic, weight);
        if (!std::isfinite(value))
            return Failed<float>(SequencePlaybackRuntimeErrors::ActivationInvalid);
        return Result<float>::Success(value);
    }

    /** @copydoc MakeSequencePlaybackCoordinationSettings */
    SequencePlaybackCoordinationSettings MakeSequencePlaybackCoordinationSettings(const SequencePlaybackSettings &settings) noexcept {
        return {settings.clockSource, settings.pausePolicy, settings.dilationPolicy, settings.pauseGameplay, settings.hideHud};
    }

    /** @copydoc ValidateSequencePlaybackCoordinationSettings */
    Result<void> ValidateSequencePlaybackCoordinationSettings(const SequencePlaybackCoordinationSettings &settings) {
        if (settings.clockSource >= SequenceClockSource::Count || settings.pausePolicy >= SequencePausePolicy::Count ||
            settings.dilationPolicy >= SequenceDilationPolicy::Count)
            return Failed<void>(SequencePlaybackRuntimeErrors::ActivationInvalid);
        if (settings.clockSource == SequenceClockSource::CommittedSimulation &&
            (settings.pausePolicy != SequencePausePolicy::FollowGameplay ||
             settings.dilationPolicy != SequenceDilationPolicy::SourceNative || settings.pauseGameplay))
            return Failed<void>(SequencePlaybackRuntimeErrors::ActivationInvalid);
        if (settings.clockSource == SequenceClockSource::External && settings.dilationPolicy != SequenceDilationPolicy::SourceNative)
            return Failed<void>(SequencePlaybackRuntimeErrors::ActivationInvalid);
        if (settings.pauseGameplay && settings.pausePolicy != SequencePausePolicy::PlayerOnly)
            return Failed<void>(SequencePlaybackRuntimeErrors::ActivationInvalid);
        return Result<void>::Success();
    }

    /** @copydoc SequenceRestoreSnapshot::Create */
    Result<SequenceRestoreSnapshot> SequenceRestoreSnapshot::Create(const std::span<const SequenceRestoreEntry> entries) {
        if (entries.size() > MaximumSequenceRestoreEntries)
            return Failed<SequenceRestoreSnapshot>(SequencePlaybackRuntimeErrors::CapacityExceeded);
        std::vector<SequenceRestoreEntry> canonical(entries.begin(), entries.end());
        for (const SequenceRestoreEntry &entry : canonical) {
            if (!entry.track.IsValid() || !entry.target.IsValid() || entry.targetRevision == 0 || !std::isfinite(entry.value))
                return Failed<SequenceRestoreSnapshot>(SequencePlaybackRuntimeErrors::RestoreInvalid);
        }
        std::ranges::sort(canonical, [](const SequenceRestoreEntry &left, const SequenceRestoreEntry &right) {
            if (left.target != right.target)
                return IdentityLess(left.target, right.target);
            return IdentityLess(left.track, right.track);
        });
        for (std::size_t index = 1; index < canonical.size(); ++index) {
            if (SameStableTarget(canonical[index - 1].target, canonical[index].target))
                return Failed<SequenceRestoreSnapshot>(SequencePlaybackRuntimeErrors::RestoreInvalid);
        }
        return Result<SequenceRestoreSnapshot>::Success(SequenceRestoreSnapshot{std::move(canonical)});
    }

    /** @copydoc SequenceRestoreSnapshot::Entries */
    std::span<const SequenceRestoreEntry> SequenceRestoreSnapshot::Entries() const noexcept {
        return entries_;
    }

    /** @copydoc SequenceRestoreSnapshot::Size */
    std::size_t SequenceRestoreSnapshot::Size() const noexcept {
        return entries_.size();
    }

    SequenceRestoreSnapshot::SequenceRestoreSnapshot(std::vector<SequenceRestoreEntry> entries) noexcept : entries_(std::move(entries)) {}

    /** @copydoc ApplySequenceRestoreSnapshot */
    Result<SequenceRestoreResult> ApplySequenceRestoreSnapshot(const SequenceRestoreSnapshot &snapshot,
                                                               const std::span<const SequenceRestoreTargetSnapshot> targets,
                                                               const std::span<SequenceRestoreDiagnostic> diagnostics) {
        if (diagnostics.size() < snapshot.Size())
            return Failed<SequenceRestoreResult>(SequencePlaybackRuntimeErrors::RestoreInvalid);
        using enum SequenceRestoreOutcome;
        const auto entries = snapshot.Entries();
        // Resolve every generation before invoking any owner write; a destroyed target
        // selects keep-final for the entire activation rather than a partial restore.
        SequenceRestoreResult result = PreflightRestoreTargets(entries, targets, diagnostics);
        if (result.missing != 0 || result.stale != 0) {
            for (std::size_t index = 0; index < entries.size(); ++index) {
                if (diagnostics[index].outcome == Restored) {
                    diagnostics[index].outcome = KeptFinalDueToMissingTarget;
                    ++result.keptFinal;
                }
            }
            return Result<SequenceRestoreResult>::Success(result);
        }
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const SequenceRestoreEntry &entry = entries[index];
            SequenceRestoreDiagnostic &diagnostic = diagnostics[index];
            const SequenceRestoreTargetSnapshot *target = FindRestoreTarget(targets, entry.target);
            if (!target->apply(target->context, entry.value)) {
                diagnostic.outcome = WriteRejected;
                ++result.rejected;
                continue;
            }
            diagnostic.outcome = Restored;
            ++result.restored;
        }
        return Result<SequenceRestoreResult>::Success(result);
    }

    /** @copydoc SequenceAuthorityPlan::Create */
    Result<SequenceAuthorityPlan> SequenceAuthorityPlan::Create(const std::uint64_t authorityRevision,
                                                                const std::uint64_t eligibleSimulationTick,
                                                                const std::span<const SequenceAuthorityClaim> claims) {
        if (authorityRevision == 0 || eligibleSimulationTick == 0 || claims.size() > MaximumSequenceAuthorityClaims)
            return Failed<SequenceAuthorityPlan>(SequencePlaybackRuntimeErrors::ActivationInvalid);
        std::vector<SequenceAuthorityClaim> canonical(claims.begin(), claims.end());
        for (const SequenceAuthorityClaim &claim : canonical) {
            if (auto valid = ValidateAuthorityClaim(claim, authorityRevision, eligibleSimulationTick); valid.HasError())
                return Result<SequenceAuthorityPlan>::Failure(std::move(valid).ErrorValue());
        }
        std::ranges::sort(canonical, AuthorityClaimLess);
        if (auto valid = ValidateAuthorityClaims(std::span<const SequenceAuthorityClaim>{canonical}); valid.HasError())
            return Result<SequenceAuthorityPlan>::Failure(std::move(valid).ErrorValue());
        return Result<SequenceAuthorityPlan>::Success(
            SequenceAuthorityPlan{authorityRevision, eligibleSimulationTick, std::move(canonical)});
    }

    /** @copydoc SequenceAuthorityPlan::ResolveGameplayWrite */
    Result<SequenceGameplayWriteResult> SequenceAuthorityPlan::ResolveGameplayWrite(const SequenceGameplayWriteRequest &request) const {
        using enum CinematicClaimMode;
        if (!request.target.IsValid() || !IsValidChannel(request.channel))
            return Failed<SequenceGameplayWriteResult>(SequencePlaybackRuntimeErrors::ActivationInvalid);
        if (request.authorityRevision != authorityRevision_ || request.simulationTick != eligibleSimulationTick_)
            return Result<SequenceGameplayWriteResult>::Success({SequenceGameplayWriteOutcome::StaleAuthority, std::nullopt});

        const SequenceAuthorityClaim *winner = nullptr;
        bool staleTargetGenerationFound = false;
        const auto first =
            std::ranges::lower_bound(claims_, request, []<typename Left, typename Right>(const Left &left, const Right &right) {
            if (left.target.stableValue != right.target.stableValue)
                return left.target.stableValue < right.target.stableValue;
            return left.channel < right.channel;
        });
        for (auto claim = first; claim != claims_.end() && SameAuthorityKey(*claim, request); ++claim) {
            if (claim->target != request.target) {
                staleTargetGenerationFound = true;
                continue;
            }
            if (claim->mode == ObserveOnly || claim->mode == PresentationOverlay)
                continue;
            winner = std::to_address(claim);
            break;
        }
        if (winner == nullptr) {
            if (staleTargetGenerationFound)
                return Result<SequenceGameplayWriteResult>::Success({SequenceGameplayWriteOutcome::StaleAuthority, std::nullopt});
            return Result<SequenceGameplayWriteResult>::Success({SequenceGameplayWriteOutcome::AcceptedGameplay, std::nullopt});
        }
        if (winner->mode == Exclusive)
            return Result<SequenceGameplayWriteResult>::Success({SequenceGameplayWriteOutcome::SuppressedByCinematic, winner->player});
        if (winner->mode == Blend)
            return Result<SequenceGameplayWriteResult>::Success({SequenceGameplayWriteOutcome::AcceptedForOwnerBlend, winner->player});
        return Result<SequenceGameplayWriteResult>::Success({SequenceGameplayWriteOutcome::UnsupportedAuthorityMode, winner->player});
    }

    /** @copydoc SequenceAuthorityPlan::Claims */
    std::span<const SequenceAuthorityClaim> SequenceAuthorityPlan::Claims() const noexcept {
        return claims_;
    }

    /** @copydoc SequenceAuthorityPlan::AuthorityRevision */
    std::uint64_t SequenceAuthorityPlan::AuthorityRevision() const noexcept {
        return authorityRevision_;
    }

    /** @copydoc SequenceAuthorityPlan::EligibleSimulationTick */
    std::uint64_t SequenceAuthorityPlan::EligibleSimulationTick() const noexcept {
        return eligibleSimulationTick_;
    }

    SequenceAuthorityPlan::SequenceAuthorityPlan(const std::uint64_t authorityRevision, const std::uint64_t eligibleSimulationTick,
                                                 std::vector<SequenceAuthorityClaim> claims) noexcept
        : authorityRevision_(authorityRevision), eligibleSimulationTick_(eligibleSimulationTick), claims_(std::move(claims)) {}

    /** @copydoc CinematicRuntimeService::Evaluate */
    Result<SequenceFrameEvaluationResult> CinematicRuntimeService::Evaluate(const SequencePlayerHandle &handle,
                                                                            const SequenceTime sourceDelta,
                                                                            const SequenceFrameScratch &scratch,
                                                                            const SequenceFrameHooks &hooks) {
        auto slot = ResolveSlot(handle);
        if (slot.HasError())
            return Result<SequenceFrameEvaluationResult>::Failure(slot.ErrorValue());
        Instance &instance = *slots_[slot.Value()].instance;
        const SequencePlayerSnapshot snapshot = instance.player.Snapshot();
        if (snapshot.state == SequencePlaybackState::Playing && instance.gameplayPaused &&
            instance.coordination.pausePolicy == SequencePausePolicy::FollowGameplay) {
            return Result<SequenceFrameEvaluationResult>::Success(
                {snapshot.position, snapshot.position, instance.cursor.traversal, instance.cursor.evaluationRevision, 0, 0, 0, false});
        }
        if (snapshot.state == SequencePlaybackState::Playing && instance.resumeBaselinePending) {
            instance.resumeBaselinePending = false;
            return Result<SequenceFrameEvaluationResult>::Success(
                {snapshot.position, snapshot.position, instance.cursor.traversal, instance.cursor.evaluationRevision, 0, 0, 0, false});
        }
        SequenceFrameScratch boundedScratch = scratch;
        ConfigureBlendScratch(boundedScratch, instance.blend, instance.blendBaselines);
        if (boundedScratch.maximumBoundaryOccurrences == 0 ||
            boundedScratch.maximumBoundaryOccurrences > budget_.maximumBoundaryOccurrences)
            boundedScratch.maximumBoundaryOccurrences = budget_.maximumBoundaryOccurrences;
        auto evaluated = instance.plan.Evaluate(instance.player.Snapshot(), sourceDelta, instance.cursor, boundedScratch, hooks);
        if (evaluated.HasError())
            return evaluated;
        SequenceFrameEvaluationResult result = evaluated.Value();
        const SequencePlayerOperationFence fence = instance.cursor.controlFence;
        if (auto committed = instance.player.CommitEvaluationPosition(fence, result.position); committed.HasError())
            return Result<SequenceFrameEvaluationResult>::Failure(committed.ErrorValue());
        if (result.reachedEnd) {
            if (auto stopping = instance.player.Stop(handle); stopping.HasError())
                return Result<SequenceFrameEvaluationResult>::Failure(stopping.ErrorValue());
            if (auto stopped = instance.player.FinishStop(handle); stopped.HasError())
                return Result<SequenceFrameEvaluationResult>::Failure(stopped.ErrorValue());
            ReleaseCoordination(instance);
            if (hooks.finishedHook != nullptr)
                hooks.finishedHook(hooks.finishedContext, handle);
        }
        return Result<SequenceFrameEvaluationResult>::Success(result);
    }

    /** @copydoc CinematicRuntimeService::EvaluateClock */
    Result<SequenceFrameEvaluationResult> CinematicRuntimeService::EvaluateClock(const SequencePlayerHandle &handle,
                                                                                 const SequenceClockSample &sample,
                                                                                 const SequenceFrameScratch &scratch,
                                                                                 const SequenceFrameHooks &hooks) {
        auto slot = ResolveSlot(handle);
        if (slot.HasError())
            return Result<SequenceFrameEvaluationResult>::Failure(slot.ErrorValue());
        Instance &instance = *slots_[slot.Value()].instance;
        if (const SequencePlaybackState state = instance.player.Snapshot().state;
            state == SequencePlaybackState::Closing || state == SequencePlaybackState::Stopped || state == SequencePlaybackState::Failed)
            return Failed<SequenceFrameEvaluationResult>(SequencePlaybackRuntimeErrors::ActivationInvalid);
        if (!IsValidClockSample(sample, instance.coordination.clockSource, instance.clockBaselineValid, instance.clockEpoch,
                                instance.clockPosition))
            return Failed<SequenceFrameEvaluationResult>(SequencePlaybackRuntimeErrors::ClockInvalid);

        const SequencePlayerSnapshot player = instance.player.Snapshot();
        const auto unchanged = [&]() {
            return Result<SequenceFrameEvaluationResult>::Success(
                {player.position, player.position, instance.cursor.traversal, instance.cursor.evaluationRevision, 0, 0, 0, false});
        };
        const bool epochChanged = instance.clockBaselineValid && sample.epoch != instance.clockEpoch;
        const bool rebase = !instance.clockBaselineValid || epochChanged || sample.hostSuspended || instance.hostWasSuspended ||
                            instance.resumeBaselinePending;
        if (sample.hostSuspended || player.state == SequencePlaybackState::Paused ||
            (instance.gameplayPaused && instance.coordination.pausePolicy == SequencePausePolicy::FollowGameplay)) {
            instance.clockPosition = sample.position;
            instance.clockEpoch = sample.epoch;
            instance.clockBaselineValid = true;
            instance.hostWasSuspended = sample.hostSuspended;
            return unchanged();
        }
        if (rebase) {
            if (epochChanged) {
                if (auto synchronized = SynchronizeCursor(instance, SequenceCursorResetPolicy::SuppressCurrentBoundary);
                    synchronized.HasError())
                    return Result<SequenceFrameEvaluationResult>::Failure(synchronized.ErrorValue());
            }
            instance.clockPosition = sample.position;
            instance.clockEpoch = sample.epoch;
            instance.clockBaselineValid = true;
            instance.hostWasSuspended = false;
            instance.resumeBaselinePending = false;
            if (instance.appliedScale != sample.gameplayScale)
                instance.scaleRemainder = 0;
            instance.appliedScale = sample.gameplayScale;
            return unchanged();
        }

        return EvaluateClockDelta(handle, sample, scratch, hooks, instance);
    }

    /** @copydoc CinematicRuntimeService::EvaluateClockDelta */
    Result<SequenceFrameEvaluationResult> CinematicRuntimeService::EvaluateClockDelta(const SequencePlayerHandle &handle,
                                                                                      const SequenceClockSample &sample,
                                                                                      const SequenceFrameScratch &scratch,
                                                                                      const SequenceFrameHooks &hooks, Instance &instance) {
        const SequenceTime rawDelta = sample.position - instance.clockPosition;
        SequenceTime sourceDelta = rawDelta;
        std::uint64_t nextRemainder = instance.scaleRemainder;
        if (instance.coordination.dilationPolicy == SequenceDilationPolicy::ApplyGameplayScale) {
            if (sample.gameplayScale != instance.appliedScale)
                nextRemainder = 0;
            auto scaled = ScaleClockDelta(rawDelta, sample.gameplayScale, nextRemainder);
            if (scaled.HasError())
                return Result<SequenceFrameEvaluationResult>::Failure(scaled.ErrorValue());
            sourceDelta = scaled.Value().value;
            nextRemainder = scaled.Value().remainder;
        }
        auto evaluated = Evaluate(handle, sourceDelta, scratch, hooks);
        if (evaluated.HasError())
            return evaluated;
        // A completion hook may retire this player at the terminal boundary.
        if (auto current = ResolveSlot(handle); current.HasValue()) {
            Instance &live = *slots_[current.Value()].instance;
            live.clockPosition = sample.position;
            live.clockEpoch = sample.epoch;
            live.scaleRemainder = nextRemainder;
            live.appliedScale = sample.gameplayScale;
        }
        return evaluated;
    }

    /** @copydoc CinematicRuntimeService::ResolveGameplayPause */
    Result<SequenceGameplayPauseResult> CinematicRuntimeService::ResolveGameplayPause(const SequencePlayerHandle &handle,
                                                                                      const SequenceGameplayPauseRequest &request) {
        using enum SequenceGameplayPauseOutcome;
        auto slot = ResolveSlot(handle);
        if (slot.HasError())
            return Result<SequenceGameplayPauseResult>::Failure(slot.ErrorValue());
        if (request.authorityRevision == 0)
            return Failed<SequenceGameplayPauseResult>(SequencePlaybackRuntimeErrors::ActivationInvalid);
        Instance &instance = *slots_[slot.Value()].instance;
        if (const SequencePlaybackState state = instance.player.Snapshot().state;
            state == SequencePlaybackState::Closing || state == SequencePlaybackState::Stopped || state == SequencePlaybackState::Failed)
            return Failed<SequenceGameplayPauseResult>(SequencePlaybackRuntimeErrors::ActivationInvalid);
        if (request.authorityRevision < instance.gameplayPauseRevision ||
            (request.authorityRevision == instance.gameplayPauseRevision && request.paused != instance.gameplayPaused))
            return Result<SequenceGameplayPauseResult>::Success({StaleAuthority, instance.gameplayPauseRevision});
        if (request.authorityRevision == instance.gameplayPauseRevision)
            return Result<SequenceGameplayPauseResult>::Success({Unchanged, instance.gameplayPauseRevision});

        const bool wasPaused = instance.gameplayPaused;
        instance.gameplayPauseRevision = request.authorityRevision;
        instance.gameplayPaused = request.paused;
        if (wasPaused && !request.paused && instance.coordination.pausePolicy == SequencePausePolicy::FollowGameplay &&
            instance.coordination.clockSource != SequenceClockSource::CommittedSimulation)
            instance.resumeBaselinePending = true;
        if (!request.paused) {
            const SequenceGameplayPauseOutcome outcome =
                wasPaused && instance.coordination.pausePolicy == SequencePausePolicy::FollowGameplay ? Resumed : Unchanged;
            return Result<SequenceGameplayPauseResult>::Success({outcome, instance.gameplayPauseRevision});
        }
        const SequenceGameplayPauseOutcome outcome =
            instance.coordination.pausePolicy == SequencePausePolicy::FollowGameplay ? HeldByGameplayPause : ContinuedDuringGameplayPause;
        return Result<SequenceGameplayPauseResult>::Success({outcome, instance.gameplayPauseRevision});
    }

    /** @copydoc CinematicRuntimeService::CoordinationSnapshot */
    Result<SequencePlaybackCoordinationSnapshot> CinematicRuntimeService::CoordinationSnapshot(const SequencePlayerHandle &handle) const {
        auto slot = ResolveSlot(handle);
        if (slot.HasError())
            return Result<SequencePlaybackCoordinationSnapshot>::Failure(slot.ErrorValue());
        const Instance &instance = *slots_[slot.Value()].instance;
        return Result<SequencePlaybackCoordinationSnapshot>::Success({instance.coordination, instance.gameplayPauseRevision,
                                                                      instance.gameplayPaused, instance.gameplayPauseLease.has_value(),
                                                                      instance.hudSuppressionLease.has_value()});
    }

    /** @copydoc CinematicRuntimeService::OrderActivePlayers */
    Result<std::size_t> CinematicRuntimeService::OrderActivePlayers(const std::span<const SequenceFramePlayerOrder> unordered,
                                                                    const std::span<SequenceFramePlayerOrder> ordered) const {
        for (const SequenceFramePlayerOrder &entry : unordered) {
            auto slot = ResolveSlot(entry.player);
            if (slot.HasError())
                return Result<std::size_t>::Failure(slot.ErrorValue());
        }
        return OrderSequenceFramePlayers(unordered, ordered);
    }

    /** @copydoc CinematicRuntimeService::ResolveGameplayWrite */
    Result<SequenceGameplayWriteResult> CinematicRuntimeService::ResolveGameplayWrite(const SequencePlayerHandle &handle,
                                                                                      const SequenceGameplayWriteRequest &request) const {
        auto slot = ResolveSlot(handle);
        if (slot.HasError())
            return Result<SequenceGameplayWriteResult>::Failure(slot.ErrorValue());
        const Instance &instance = *slots_[slot.Value()].instance;
        if (const SequencePlaybackState state = instance.player.Snapshot().state;
            state == SequencePlaybackState::Closing || IsTerminal(state)) {
            if (!request.target.IsValid() || !IsValidChannel(request.channel))
                return Failed<SequenceGameplayWriteResult>(SequencePlaybackRuntimeErrors::ActivationInvalid);
            return Result<SequenceGameplayWriteResult>::Success({SequenceGameplayWriteOutcome::AcceptedGameplay, std::nullopt});
        }
        if (!instance.authority.has_value()) {
            if (!request.target.IsValid() || !IsValidChannel(request.channel))
                return Failed<SequenceGameplayWriteResult>(SequencePlaybackRuntimeErrors::ActivationInvalid);
            return Result<SequenceGameplayWriteResult>::Success({SequenceGameplayWriteOutcome::AcceptedGameplay, std::nullopt});
        }
        return instance.authority->ResolveGameplayWrite(request);
    }

    /** @copydoc CinematicRuntimeService::Restore */
    Result<SequenceRestoreResult> CinematicRuntimeService::Restore(const SequencePlayerHandle &handle,
                                                                   const std::span<const SequenceRestoreTargetSnapshot> targets,
                                                                   const std::span<SequenceRestoreDiagnostic> diagnostics) {
        auto slot = ResolveSlot(handle);
        if (slot.HasError())
            return Result<SequenceRestoreResult>::Failure(slot.ErrorValue());
        Instance &instance = *slots_[slot.Value()].instance;
        if (!IsTerminal(instance.player.Snapshot().state))
            return Failed<SequenceRestoreResult>(SequencePlaybackRuntimeErrors::PlayerNotTerminal);
        if (instance.restoreApplied)
            return Result<SequenceRestoreResult>::Success({});
        if (instance.blend.restorePolicy == SequenceRestorePolicy::KeepFinalState || !instance.restore.has_value()) {
            instance.restoreApplied = true;
            return Result<SequenceRestoreResult>::Success({});
        }
        auto restored = ApplySequenceRestoreSnapshot(*instance.restore, targets, diagnostics);
        if (restored.HasError())
            return restored;
        instance.restoreApplied = true;
        return restored;
    }

    /** @copydoc CinematicRuntimeService::Release */
    Result<void> CinematicRuntimeService::Release(const SequencePlayerHandle &handle) {
        auto slot = ResolveSlot(handle);
        if (slot.HasError())
            return Result<void>::Failure(slot.ErrorValue());
        Slot &resolved = slots_[slot.Value()];
        Instance &instance = *resolved.instance;
        if (!IsTerminal(instance.player.Snapshot().state))
            return Failed<void>(SequencePlaybackRuntimeErrors::PlayerNotTerminal);
        if (instance.blend.restorePolicy == SequenceRestorePolicy::RestorePrePlayback && !instance.restoreApplied)
            return Failed<void>(SequencePlaybackRuntimeErrors::RestoreRequired);
        const SequenceEvaluationUsage released{1, static_cast<std::uint32_t>(instance.plan.TrackCount()),
                                               static_cast<std::uint32_t>(instance.plan.EventCount() + instance.plan.CameraCutCount()),
                                               instance.retainedBytes, 0};
        usage_.activePlayers -= released.activePlayers;
        usage_.aggregateTracks -= released.aggregateTracks;
        usage_.boundaryOccurrences -= released.boundaryOccurrences;
        usage_.retainedBytes -= released.retainedBytes;
        ReleaseCoordination(instance);
        resolved.retiredHandle = instance.player.Snapshot().handle;
        resolved.hasRetiredHandle = true;
        resolved.instance.reset();
        RecalculateMaximumLoopCrossings();
        return Result<void>::Success();
    }

}  // namespace Horo::Cinematic
