#pragma once

/** @file AudioRepeatedPlaybackState.h
 * @brief Target-private bounded admission storage and canonical registry projections.
 */
#include "PlaybackVariation.h"

namespace Horo::Audio {
    /** @brief Stable ownership island; playback references this registry until it is destroyed last. */
    struct AudioRepeatedPlayback::State final {
        struct Bucket final {
            AudioConcurrencyGroup group;
            AudioConcurrencyKey key;
            std::optional<std::uint64_t> lastAdmission;
        };

        struct Lane final {
            AudioPlaybackBinding binding;
            AudioRepeatedPlaybackPolicy policy;
            Detail::VariationState variation;
            std::optional<std::size_t> bucket;
            std::optional<AudioRepeatedPlaybackRequest> lastRequest;
            AudioRepeatedPlaybackReceipt lastReceipt;
            bool closed{};
        };

        struct Voice final {
            std::optional<AudioVoicePlayback> playback;
            AudioPlaybackLaneHandle lane;
            AudioSceneContextHandle scene;
            std::optional<std::size_t> bucket;
            AudioRepeatedPlaybackReceipt receipt;
            bool pending{};
        };

        AudioRepeatedPlaybackConfig config;
        AudioVoiceStateMachine registry;
        std::vector<std::optional<Lane>> lanes;
        std::vector<std::uint32_t> generations;
        std::vector<Bucket> buckets;
        std::vector<Voice> voices;
        std::vector<AudioVoiceSnapshot> projection;
        std::uint64_t lastControlFrame{};

        /** @brief All structural capacities are reserved before this owner is published. */
        State(const AudioRepeatedPlaybackConfig &settings, AudioVoiceStateMachine prepared)
            : config(settings), registry(std::move(prepared)), lanes(settings.maximumLanes), generations(settings.maximumLanes),
              voices(settings.maximumVoices) {
            buckets.reserve(settings.maximumLanes);
            projection.reserve(settings.maximumVoices);
        }

        /** @brief Check complete lane identity rather than slot number alone. */
        Lane *Resolve(const AudioPlaybackLaneHandle handle) noexcept {
            if (!handle.IsValid() || handle.owner != config.runtime || handle.slot > lanes.size())
                return nullptr;
            const auto index = handle.slot - 1;
            return generations[index] == handle.generation && lanes[index] && !lanes[index]->closed ? &*lanes[index] : nullptr;
        }

        /** @brief Canonical registry liveness is checked separately; the playback itself retains the exact handle. */
        Voice *Find(const AudioVoiceHandle handle) noexcept {
            if (!handle.IsValid() || handle.slot > voices.size())
                return nullptr;
            auto &voice = voices[handle.slot - 1];
            return voice.playback && voice.playback->Voice() == handle ? &voice : nullptr;
        }

        /** @brief Refresh complete ordered bucket projection from the same canonical registry. */
        void Project(const std::optional<std::size_t> bucket, const AudioPlaybackLaneHandle lane, const AudioVoiceHandle exclude) {
            projection.clear();
            for (const auto &voice : voices) {
                if (!voice.playback || voice.playback->Voice() == exclude ||
                    (bucket.has_value() ? voice.bucket != bucket : voice.lane != lane))
                    continue;
                const auto snapshot = registry.Snapshot(voice.playback->Voice());
                if (snapshot.HasValue())
                    projection.push_back(snapshot.Value());
            }
        }

        /** @brief Select the newest live voice in this exact lane; terminal and recycled handles cannot match. */
        Voice *Latest(const AudioPlaybackLaneHandle lane) noexcept {
            Voice *latest{};
            for (auto &voice : voices) {
                if (voice.lane != lane || !voice.playback)
                    continue;
                if (AudioVoiceState current{}; registry.CheckState(voice.playback->Voice(), current) || IsTerminalAudioVoiceState(current))
                    continue;
                if (!latest || voice.receipt.sequence > latest->receipt.sequence)
                    latest = &voice;
            }
            return latest;
        }

        /** @brief Validate exclusive control time even when returning an already admitted receipt. */
        bool ValidControlTime(const AudioConcurrencyTime time) const noexcept {
            return time.timelineGeneration == config.discontinuityRevision && time.sampleFrame >= lastControlFrame;
        }

        /** @brief New admissions additionally require a compatible future execution target. */
        bool ValidTime(const AudioRepeatedPlaybackRequest &request, const AudioConcurrencyTime time) const noexcept {
            const auto &target = request.target;
            if (!ValidControlTime(time))
                return false;
            if (target.kind == AudioCommandTargetKind::NextBufferBoundary)
                return target.sampleFrame == 0 && target.clockGeneration == 0 && target.discontinuityRevision == 0;
            return target.kind == AudioCommandTargetKind::ExactSampleFrame && target.sampleFrame >= time.sampleFrame &&
                   target.clockGeneration == config.clockGeneration && target.discontinuityRevision == config.discontinuityRevision;
        }

        /** @brief Admit only implemented resident routing and source-local action semantics. */
        bool Supported(const AudioPlaybackSettings &playback) const noexcept {
            return playback.spatialMode == AudioSpatialMode::TwoD && !playback.bus &&
                   (playback.concurrency.mode == AudioConcurrencyMode::Allow || playback.concurrency.mode == AudioConcurrencyMode::Reject);
        }

        /** @brief Evaluate both constraints using refreshed registry facts; replacement excludes only its own reserved voice. */
        Result<AudioConcurrencyDecision> Evaluate(const Lane &lane, const AudioRepeatedPlaybackRequest &request, const std::uint64_t frame,
                                                  const AudioVoiceHandle exclude) {
            const AudioConcurrencyRequest identities{config.runtime, lane.binding.emitter, lane.binding.owner};
            AudioConcurrencyDecision groupDecision;
            if (lane.bucket.has_value()) {
                auto &bucket = buckets[*lane.bucket];
                Project(lane.bucket, request.lane, exclude);
                auto result = EvaluateAudioConcurrency(bucket.group, identities,
                                                       {bucket.key, config.discontinuityRevision, bucket.lastAdmission, projection},
                                                       {config.discontinuityRevision, frame});
                if (result.HasError())
                    return result;
                groupDecision = result.Value();
            }
            Project(std::nullopt, request.lane, exclude);
            const AudioConcurrencyGroup local{AudioConcurrencyGroupId::Create(1).Value(), AudioConcurrencyScope::Emitter,
                                              request.playback.playback.concurrency.maxInstances};
            auto result = EvaluateAudioConcurrency(local, identities,
                                                   {MakeAudioConcurrencyKey(local, identities).Value(), config.discontinuityRevision,
                                                    std::nullopt, projection},
                                                   {config.discontinuityRevision, frame});
            if (result.HasError())
                return result;
            return Result<AudioConcurrencyDecision>::Success(
                result.Value().eligibility == AudioConcurrencyEligibility::Eligible ? groupDecision : result.Value());
        }

        /** @brief Cache normal outcomes for deduplication without consuming variation or cooldown on rejection. */
        AudioRepeatedPlaybackReceipt Remember(Lane &lane, const AudioRepeatedPlaybackRequest &request,
                                              const AudioRepeatedPlaybackReceipt &receipt, const AudioConcurrencyTime time) noexcept {
            lane.lastRequest = request;
            lane.lastReceipt = receipt;
            lastControlFrame = time.sampleFrame;
            return receipt;
        }

        /** @brief Produce the existing normalized typed command path for the exact retained voice. */
        bool Commands(AudioRepeatedPlaybackReceipt &receipt, const AudioRepeatedPlaybackRequest &request,
                      const AudioVoiceControl control) const noexcept {
            ScheduledAudioCommandBatch batch;
            batch.target = request.target;
            batch.commandCount = 1;
            batch.commands[0] = {{config.runtime, config.epoch, request.playback.sceneContext},
                                 AudioVoiceControlRequest{.voice = receipt.voice,
                                                          .control = control,
                                                          .operationSequence = request.sequence}};
            return NormalizeScheduledAudioCommandBatch(batch, receipt.commands) == ScheduledAudioCommandBatchStatus::Ok;
        }

        /** @brief Validate the complete resolver set before selection to make malformed input order-independent. */
        bool ValidClips(const Lane &lane, const std::span<const AudioResolvedPlaybackClip> clips) const noexcept {
            if (const auto expected = lane.policy.variation ? lane.policy.variation->entries.size() : 1;
                clips.size() != expected || clips.size() > MaximumAudioVariationEntries)
                return false;
            for (std::size_t index = 0; index < clips.size(); ++index) {
                const auto &clip = clips[index];
                if (!ValidClip(clip))
                    return false;
                for (std::size_t prior = 0; prior < index; ++prior) {
                    if (clips[prior].clip == clip.clip)
                        return false;
                }
                if (!ContainsClip(lane, clip.clip))
                    return false;
            }
            return true;
        }

        /** @brief Resolve variation against candidate state and finite playback contracts, never clamping adjustments. */
        bool Adjust(const Lane &lane, Detail::VariationState &candidate, AudioRepeatedPlaybackReceipt &receipt,
                    const AudioPlaybackSettings &playback) const noexcept {
            if (lane.policy.variation) {
                const auto &variation = *lane.policy.variation;
                std::uint32_t selected{};
                if (!Detail::Select(candidate, variation, selected))
                    return false;
                receipt.clip = variation.entries[selected].clip;
                receipt.pitchDeltaSemitones = Detail::Range(candidate.random, variation.pitchDeltaSemitones);
                receipt.gainDeltaDb = Detail::Range(candidate.random, variation.gainDeltaDb);
            } else {
                receipt.clip = std::get<AudioClipId>(lane.binding.prototype.sound.target);
            }
            receipt.pitch = static_cast<float>(playback.pitch * std::exp2(static_cast<double>(receipt.pitchDeltaSemitones) / 12.0));
            receipt.gain = static_cast<float>(playback.gain * std::pow(10.0, static_cast<double>(receipt.gainDeltaDb) / 20.0));
            return std::isfinite(receipt.pitch) && receipt.pitch > 0.0F && receipt.pitch <= 8.0F && std::isfinite(receipt.gain) &&
                   receipt.gain >= 0.0F;
        }

        /** @brief Validate source shape, supported policy and immutable variation before bucket lookup. */
        Result<void> ValidateBinding(const AudioPlaybackBinding &binding, const AudioRepeatedPlaybackPolicy &policy) const {
            if (const auto valid = ValidateAudioPlaybackRequest(binding.prototype); valid.HasError())
                return Result<void>::Failure(valid.ErrorValue());
            if (!ValidBindingIdentity(binding, policy))
                return Result<void>::Failure(MakeError(AudioErrors::PlaybackRequestInvalid));
            if (!SupportedBinding(binding, policy))
                return Result<void>::Failure(MakeError(AudioErrors::OperationUnsupported));
            if (policy.variation) {
                if (const auto valid = ValidateAudioVariationAssetSchema(*policy.variation); valid.HasError())
                    return Result<void>::Failure(valid.ErrorValue());
            }
            if (binding.prototype.playback.concurrency.group != (policy.group ? std::optional{policy.group->group} : std::nullopt))
                return Result<void>::Failure(MakeError(AudioErrors::ConcurrencyInvalid));
            return Result<void>::Success();
        }

        /** @brief Resolve one authored bucket without publishing new state on failure. */
        struct BucketSelection final {
            std::optional<std::size_t> index;
            std::optional<AudioConcurrencyKey> key;
        };

        /** @brief Require shared-group rule consistency and bounded bucket capacity. */
        Result<BucketSelection> SelectBucket(const AudioPlaybackBinding &binding, const AudioRepeatedPlaybackPolicy &policy) const {
            std::optional<std::size_t> bucketIndex;
            std::optional<AudioConcurrencyKey> key;
            if (policy.group) {
                auto result = MakeAudioConcurrencyKey(*policy.group, {config.runtime, binding.emitter, binding.owner});
                if (result.HasError())
                    return Result<BucketSelection>::Failure(result.ErrorValue());
                key = result.Value();
                for (std::size_t index = 0; index < buckets.size(); ++index) {
                    const auto &bucket = buckets[index];
                    if (bucket.group.group == policy.group->group && !Detail::SameGroup(bucket.group, *policy.group))
                        return Result<BucketSelection>::Failure(MakeError(AudioErrors::ConcurrencyInvalid));
                    if (bucket.key == key)
                        bucketIndex = index;
                }
                if (!bucketIndex.has_value() && buckets.size() == config.maximumLanes)
                    return Result<BucketSelection>::Failure(MakeError(AudioErrors::HandleCapacityExhausted));
            }
            return Result<BucketSelection>::Success({bucketIndex, key});
        }

        /** @brief Reject complete source-identity reuse independently of mutable playback defaults. */
        bool DuplicateBinding(const AudioPlaybackBinding &binding) const noexcept {
            return std::ranges::any_of(lanes, [&binding](const auto &lane) {
                return lane && lane->binding.prototype.sound == binding.prototype.sound &&
                       lane->binding.prototype.sceneContext == binding.prototype.sceneContext && lane->binding.emitter == binding.emitter &&
                       lane->binding.owner == binding.owner && lane->binding.incarnation == binding.incarnation;
            });
        }

        /** @brief Copy policy and seed into a prepared lane before committing its bucket and generation. */
        Result<AudioPlaybackLaneHandle> StoreLane(const AudioPlaybackBinding &binding, const AudioRepeatedPlaybackPolicy &policy,
                                                  const BucketSelection &selection) {
            const auto &bucketIndex = selection.index;
            const auto &key = selection.key;
            for (std::size_t index = 0; index < lanes.size(); ++index) {
                if (lanes[index] || generations[index] == std::numeric_limits<std::uint32_t>::max())
                    continue;
                try {
                    Lane prepared{binding, policy, {}, bucketIndex, std::nullopt, {}};
                    if (prepared.policy.variation) {
                        auto &variation = *prepared.policy.variation;
                        if (variation.selection != AudioVariationSelection::RoundRobin)
                            std::ranges::sort(variation.entries, {}, &AudioVariationEntry::clip);
                        prepared.variation.random = variation.deterministicSeed;
                        if (variation.selection == AudioVariationSelection::Shuffle)
                            prepared.variation.position = static_cast<std::uint32_t>(variation.entries.size());
                    }
                    if (key && !bucketIndex.has_value()) {
                        prepared.bucket = buckets.size();
                        buckets.emplace_back(*policy.group, *key, std::nullopt);
                    }
                    lanes[index] = std::move(prepared);
                    return Result<AudioPlaybackLaneHandle>::Success(
                        {config.runtime, static_cast<std::uint32_t>(index + 1), ++generations[index]});
                } catch (const std::bad_alloc &) {
                    return Result<AudioPlaybackLaneHandle>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
                }
            }
            return Result<AudioPlaybackLaneHandle>::Failure(MakeError(AudioErrors::HandleCapacityExhausted));
        }

        /** @brief Validate bound source, producer sequence and timeline before deduplication or policy evaluation. */
        Result<void> ValidateRequest(const Lane &lane, const AudioRepeatedPlaybackRequest &request, const AudioConcurrencyTime time) const {
            if (request.sequence == 0 || request.playback.sound != lane.binding.prototype.sound ||
                request.playback.sceneContext != lane.binding.prototype.sceneContext ||
                request.playback.sceneLifecycle != lane.binding.prototype.sceneLifecycle)
                return Result<void>::Failure(MakeError(AudioErrors::PlaybackRequestInvalid));
            if (!ValidTime(request, time))
                return Result<void>::Failure(MakeError(AudioErrors::ConcurrencyTimelineStale));
            if (const auto valid = ValidateAudioPlaybackRequest(request.playback); valid.HasError())
                return Result<void>::Failure(valid.ErrorValue());
            if (!Supported(request.playback.playback))
                return Result<void>::Failure(MakeError(AudioErrors::OperationUnsupported));
            if (request.playback.playback.concurrency.group != lane.binding.prototype.playback.concurrency.group)
                return Result<void>::Failure(MakeError(AudioErrors::ConcurrencyInvalid));
            return Result<void>::Success();
        }

        /** @brief Prepare and reserve a real new voice, then atomically publish its candidate replay state. */
        Result<void> PrepareStart(Lane &lane, const AudioRepeatedPlaybackRequest &request,
                                  const std::span<const AudioResolvedPlaybackClip> clips, AudioRepeatedPlaybackReceipt &receipt) {
            if (!ValidClips(lane, clips))
                return Result<void>::Failure(MakeError(AudioErrors::PlaybackRequestInvalid));
            auto candidate = lane.variation;
            if (!Adjust(lane, candidate, receipt, request.playback.playback))
                return Result<void>::Failure(MakeError(AudioErrors::PlaybackRequestInvalid));
            const auto resolved = std::ranges::find(clips, receipt.clip, &AudioResolvedPlaybackClip::clip);
            auto descriptor = config.conversion;
            descriptor.inputRate = resolved->sampleRate;
            descriptor.pitch = receipt.pitch;
            auto plan = AudioResamplerPlan::Prepare(descriptor, config.conversionBudget);
            if (plan.HasError())
                return Result<void>::Failure(plan.ErrorValue());
            const AudioVoiceLoop loop = request.playback.playback.loop ? AudioVoiceLoop{true, 0, resolved->pcm.frames} : AudioVoiceLoop{};
            auto prepared = AudioVoicePlayback::Create(registry, resolved->pcm,
                                                       {plan.Value(), loop, config.rampFrames, config.maximumVoiceStorageBytes,
                                                        config.maximumCoefficientBytes, receipt.gain});
            if (prepared.HasError())
                return Result<void>::Failure(prepared.ErrorValue());
            receipt.voice = prepared.Value().Voice();
            receipt.disposition = AudioRepeatedPlaybackDisposition::Admitted;
            if (!Commands(receipt, request, AudioVoiceControl::Start))
                return Result<void>::Failure(MakeError(AudioErrors::PlaybackRequestInvalid));
            auto &voice = voices[receipt.voice.slot - 1];
            voice.playback = std::move(prepared).Value();
            voice.lane = request.lane;
            voice.scene = request.playback.sceneContext;
            voice.bucket = lane.bucket;
            voice.receipt = receipt;
            voice.pending = true;
            lane.variation = candidate;
            return Result<void>::Success();
        }

        /** @brief Preserve prior outcomes for one exact producer request; conflicting reuse is an error. */
        std::optional<Result<AudioRepeatedPlaybackReceipt>> Replay(const Lane &lane, const AudioRepeatedPlaybackRequest &request) const {
            if (!lane.lastRequest || request.sequence > lane.lastRequest->sequence)
                return std::nullopt;
            if (const auto &prior = *lane.lastRequest; request.sequence != prior.sequence || request.playback != prior.playback ||
                                                       request.cancelled != prior.cancelled ||
                                                       !Detail::SameTarget(request.target, prior.target))
                return Result<AudioRepeatedPlaybackReceipt>::Failure(MakeError(AudioErrors::PlaybackRequestInvalid));
            auto receipt = lane.lastReceipt;
            receipt.disposition = AudioRepeatedPlaybackDisposition::Replay;
            receipt.commands.commandCount = 0;
            return Result<AudioRepeatedPlaybackReceipt>::Success(receipt);
        }

        /** @brief Direct controls share the owned scope and cannot overlap a pending admission except cancellation. */
        const ErrorCodeDescriptor *ApplyDirect(Voice &voice, const AudioCommand &command, const AudioVoiceControlRequest &control,
                                               const AudioConcurrencyTime time) const noexcept {
            if (command.scope != AudioCommandScope{config.runtime, config.epoch, voice.scene} ||
                time.timelineGeneration != config.discontinuityRevision)
                return &AudioErrors::ConcurrencyTimelineStale;
            if (voice.pending && control.control != AudioVoiceControl::Cancel)
                return &AudioErrors::VoiceInvalidTransition;
            const auto *error = voice.playback->Apply(control);
            if (!error && control.control == AudioVoiceControl::Cancel)
                voice.pending = false;
            return error;
        }

        /** @brief Consume only the retained command identity at its promised execution time. */
        const ErrorCodeDescriptor *ApplyPending(Voice &voice, const AudioCommand &command, const AudioVoiceControlRequest &control,
                                                const AudioConcurrencyTime time) const noexcept {
            if (!voice.pending)
                return &AudioErrors::HandleStale;
            const auto &batch = voice.receipt.commands;
            const auto &expected = batch.commands[0];
            if (const auto &operation = std::get<AudioVoiceControlRequest>(expected.payload);
                command.scope != expected.scope || control.control != operation.control || control.seekFrame != operation.seekFrame ||
                control.operationSequence != operation.operationSequence)
                return &AudioErrors::PlaybackRequestInvalid;
            if (time.timelineGeneration != config.discontinuityRevision ||
                (batch.target.kind == AudioCommandTargetKind::ExactSampleFrame && time.sampleFrame != batch.target.sampleFrame))
                return &AudioErrors::ConcurrencyTimelineStale;
            if (const auto *error = voice.playback->Apply(control))
                return error;
            voice.pending = false;
            return nullptr;
        }

        /** @brief Teardown preserves terminal snapshots while retiring any live reservation. */
        void CancelVoice(Voice &voice) const noexcept {
            if (AudioVoiceState current{};
                registry.CheckState(voice.playback->Voice(), current) == nullptr && !IsTerminalAudioVoiceState(current))
                (void)registry.TryCancel(voice.playback->Voice());
            voice.pending = false;
        }

        /** @brief Validate owner identity independently of conversion and storage bounds. */
        static bool ValidConfigIdentity(const AudioRepeatedPlaybackConfig &config) noexcept {
            return config.runtime.IsValid() && config.epoch != 0 && config.clockGeneration != 0 && config.discontinuityRevision != 0;
        }

        /** @brief Bound every structural capacity and supported conversion shape before allocating ownership. */
        static bool ValidConfig(const AudioRepeatedPlaybackConfig &config) noexcept {
            return ValidConfigIdentity(config) && config.maximumLanes != 0 && config.maximumLanes <= MaximumAudioPlaybackLanes &&
                   config.maximumVoices != 0 && config.maximumVoices <= MaximumAudioPlaybackLanes &&
                   config.conversion.stage == AudioResamplerStage::ClipToMix && config.conversion.pitch == 1.0 && config.rampFrames != 0 &&
                   config.rampFrames <= 16384;
        }

        /** @brief Validate complete runtime-owned source identity before retaining a lane. */
        bool ValidBindingIdentity(const AudioPlaybackBinding &binding, const AudioRepeatedPlaybackPolicy &policy) const noexcept {
            return binding.emitter.IsValid() && binding.owner.IsValid() && binding.incarnation != 0 &&
                   binding.emitter.owner == config.runtime && binding.owner.owner == config.runtime &&
                   binding.prototype.sceneContext.owner == config.runtime && policy.retrigger <= AudioRetriggerPolicy::Restart;
        }

        /** @brief Require a complete resident PCM view and supported sample format before candidate selection. */
        bool ValidClip(const AudioResolvedPlaybackClip &clip) const noexcept {
            return clip.clip.IsValid() && clip.pcm.planes.size() == config.conversion.channels && clip.pcm.frames != 0 &&
                   clip.sampleRate >= MinimumAudioSampleRate && clip.sampleRate <= MaximumAudioSampleRate &&
                   std::ranges::none_of(clip.pcm.planes, [&clip](const auto plane) {
                return plane.size() < clip.pcm.frames || plane.data() == nullptr;
            });
        }

        /** @brief A normal policy rejection carries a receipt but no reservation; errors carry no committed history. */
        struct Admission final {
            AudioRepeatedPlaybackReceipt receipt;
            Voice *restart{};
            bool proceed{};
        };

        /** @brief Restart feasibility is checked before cooldown or variation state can be committed. */
        const ErrorCodeDescriptor *CheckRestart(const Voice &voice) const noexcept {
            return voice.pending ? &AudioErrors::VoiceInvalidTransition : voice.playback->CheckRestart(voice.playback->Voice());
        }

        /** @brief Resolve retrigger and both concurrency constraints using only current canonical registry state. */
        Result<Admission> CheckAdmission(const Lane &lane, const AudioRepeatedPlaybackRequest &request, const AudioConcurrencyTime time) {
            Admission admission{{.lane = request.lane, .sequence = request.sequence}};
            auto *latest = Latest(request.lane);
            using enum AudioRepeatedPlaybackDisposition;
            if (request.cancelled || (latest && lane.policy.retrigger == AudioRetriggerPolicy::Ignore)) {
                admission.receipt.disposition = request.cancelled ? Cancelled : Ignored;
                return Result<Admission>::Success(admission);
            }
            admission.restart = latest && lane.policy.retrigger == AudioRetriggerPolicy::Restart ? latest : nullptr;
            if (admission.restart) {
                if (const auto *error = CheckRestart(*admission.restart))
                    return Result<Admission>::Failure(MakeError(*error));
            }
            if (const auto valid = EvaluateAdmission(admission, lane, request, time); valid.HasError())
                return Result<Admission>::Failure(valid.ErrorValue());
            return Result<Admission>::Success(admission);
        }

        /** @brief Publish a new operation on the same prepared voice without selecting or preparing media again. */
        Result<void> PrepareRestart(Voice &voice, const AudioRepeatedPlaybackRequest &request,
                                    AudioRepeatedPlaybackReceipt &receipt) const {
            const auto decision = receipt.concurrency;
            receipt = voice.receipt;
            receipt.sequence = request.sequence;
            receipt.disposition = AudioRepeatedPlaybackDisposition::Restarted;
            receipt.concurrency = decision;
            if (!Commands(receipt, request, AudioVoiceControl::Restart))
                return Result<void>::Failure(MakeError(AudioErrors::PlaybackRequestInvalid));
            voice.receipt = receipt;
            voice.pending = true;
            return Result<void>::Success();
        }

        /** @brief Cooldown measures the successful execution target rather than earlier control preparation time. */
        static std::uint64_t AdmissionFrame(const AudioRepeatedPlaybackRequest &request, const AudioConcurrencyTime time) noexcept {
            return request.target.kind == AudioCommandTargetKind::ExactSampleFrame ? request.target.sampleFrame : time.sampleFrame;
        }

        /** @brief Commit history only for a normal outcome or completely prepared successful reservation. */
        Result<AudioRepeatedPlaybackReceipt> Admit(Lane &lane, const AudioRepeatedPlaybackRequest &request,
                                                   const std::span<const AudioResolvedPlaybackClip> clips,
                                                   const AudioConcurrencyTime time) {
            auto checked = CheckAdmission(lane, request, time);
            if (checked.HasError())
                return Result<AudioRepeatedPlaybackReceipt>::Failure(checked.ErrorValue());
            auto admission = checked.Value();
            if (admission.proceed) {
                if (const auto prepared = admission.restart ? PrepareRestart(*admission.restart, request, admission.receipt)
                                                            : PrepareStart(lane, request, clips, admission.receipt);
                    prepared.HasError())
                    return Result<AudioRepeatedPlaybackReceipt>::Failure(prepared.ErrorValue());
                if (lane.bucket.has_value())
                    buckets[*lane.bucket].lastAdmission = AdmissionFrame(request, time);
            }
            return Result<AudioRepeatedPlaybackReceipt>::Success(Remember(lane, request, admission.receipt, time));
        }

        /** @brief Match each resolver to the authored source after duplicate and PCM validation. */
        static bool ContainsClip(const Lane &lane, const AudioClipId clip) noexcept {
            return lane.policy.variation ? std::ranges::any_of(lane.policy.variation->entries,
                                                               [clip](const auto &entry) {
                return entry.clip == clip;
            })
                                         : clip == std::get<AudioClipId>(lane.binding.prototype.sound.target);
        }

        /** @brief Match the resident adapter to the authored reference and scene lifecycle policy. */
        bool SupportedBinding(const AudioPlaybackBinding &binding, const AudioRepeatedPlaybackPolicy &policy) const noexcept {
            const auto kind = binding.prototype.sound.kind;
            return (policy.variation ? kind == AudioSoundReferenceKind::Variation : kind == AudioSoundReferenceKind::Clip) &&
                   Supported(binding.prototype.playback) && binding.prototype.sceneLifecycle == AudioSceneLifecyclePolicy::StopOnUnload;
        }

        /** @brief Attach the authoritative eligibility decision before any real playback reservation. */
        Result<void> EvaluateAdmission(Admission &admission, const Lane &lane, const AudioRepeatedPlaybackRequest &request,
                                       const AudioConcurrencyTime time) {
            const auto frame = AdmissionFrame(request, time);
            auto decision = Evaluate(lane, request, frame, admission.restart ? admission.restart->playback->Voice() : AudioVoiceHandle{});
            if (decision.HasError())
                return Result<void>::Failure(decision.ErrorValue());
            admission.receipt.concurrency = decision.Value();
            admission.proceed = decision.Value().eligibility == AudioConcurrencyEligibility::Eligible;
            if (!admission.proceed)
                admission.receipt.disposition = decision.Value().eligibility == AudioConcurrencyEligibility::InstanceLimit
                                                    ? AudioRepeatedPlaybackDisposition::InstanceLimit
                                                    : AudioRepeatedPlaybackDisposition::RetriggerWindow;
            return Result<void>::Success();
        }
    };

}  // namespace Horo::Audio
