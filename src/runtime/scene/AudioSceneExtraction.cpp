#include "Horo/Runtime/Scene/AudioSceneExtraction.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Horo::Runtime {
    namespace AudioSceneErrors {
        const ErrorCodeDescriptor InvalidInput{ErrorDomainId{"horo.scene"}, ErrorCode{"scene.audio.invalid"}, ErrorSeverity::Error,
                                               "Spatial audio extraction input is invalid.",
                                               "Supply a current scene, finite motion and a declared listener policy."};
        const ErrorCodeDescriptor Capacity{ErrorDomainId{"horo.scene"}, ErrorCode{"scene.audio.capacity"}, ErrorSeverity::Error,
                                           "Spatial audio extraction exceeds a bounded capacity.",
                                           "Reduce scene slots, spatial objects, overrides or hierarchy depth."};
        const ErrorCodeDescriptor Stale{ErrorDomainId{"horo.scene"}, ErrorCode{"scene.audio.stale"}, ErrorSeverity::Error,
                                        "Spatial audio scene, sequence or context evidence is stale.",
                                        "Reacquire the scene view, advance the sequence and replace retired Audio contexts."};
        const ErrorCodeDescriptor Closed{ErrorDomainId{"horo.scene"}, ErrorCode{"scene.audio.closed"}, ErrorSeverity::Error,
                                         "Spatial audio extraction has shut down.", "Compose a new extraction owner."};
    }  // namespace AudioSceneErrors

    namespace {
        using namespace Audio;
        constexpr std::size_t MaximumSceneSlots = 4096;
        constexpr std::size_t MaximumAncestors = 64;

        /** @brief Reject unknown selection policies instead of silently selecting primary. */
        [[nodiscard]] bool KnownPolicy(const AudioListenerPolicy policy) noexcept {
            using enum AudioListenerPolicy;
            return policy == Primary || policy == PerView || policy == WeightedAll;
        }

        /** @brief Validate the bounded override set before reading or updating history. */
        [[nodiscard]] Result<void> ValidateInput(const RuntimeSceneView scene, const AudioSceneExtractionInput &input) {
            if (!input.context.IsValid() || input.sequence == 0 || !std::isfinite(input.elapsedSeconds) || input.elapsedSeconds <= 0 ||
                !KnownPolicy(input.policy))
                return Result<void>::Failure(MakeError(AudioSceneErrors::InvalidInput));
            if (scene.SlotCount() > MaximumSceneSlots || input.motion.size() > MaximumSpatialAudioSources + MaximumSpatialAudioListeners)
                return Result<void>::Failure(MakeError(AudioSceneErrors::Capacity));
            for (std::size_t index = 0; index < input.motion.size(); ++index) {
                const auto &motionInput = input.motion[index];
                auto entity = scene.Get(motionInput.entity);
                if (entity.HasError())
                    return Result<void>::Failure(entity.ErrorValue());
                if ((!entity.Value().components->audioSource && !entity.Value().components->audioListener) ||
                    (motionInput.velocity && !Math::IsFinite(*motionInput.velocity)))
                    return Result<void>::Failure(MakeError(AudioSceneErrors::InvalidInput));
                for (std::size_t previous = 0; previous < index; ++previous)
                    if (input.motion[previous].entity == motionInput.entity)
                        return Result<void>::Failure(MakeError(AudioSceneErrors::InvalidInput));
            }
            return Result<void>::Success();
        }

        /** @brief Compose position with full ancestor scale and orientation with proper rotations only. */
        [[nodiscard]] Result<AudioSpatialPose> WorldPose(const RuntimeSceneView scene, RuntimeEntityView entity) {
            Math::Mat4 matrix = Math::Mat4::Identity();
            Math::Quaternion orientation;
            for (std::size_t depth = 0; depth < MaximumAncestors; ++depth) {
                auto local = entity.localTransform->TryToMatrix();
                if (local.HasError())
                    return Result<AudioSpatialPose>::Failure(local.ErrorValue());
                matrix = Math::Multiply(local.Value(), matrix);
                orientation = entity.localTransform->rotation.Normalized() * orientation;
                if (!entity.parent) {
                    auto position = Math::TryTransformPoint(matrix, {});
                    auto rotation = orientation.TryNormalized();
                    if (position.HasError())
                        return Result<AudioSpatialPose>::Failure(position.ErrorValue());
                    if (rotation.HasError())
                        return Result<AudioSpatialPose>::Failure(rotation.ErrorValue());
                    return Result<AudioSpatialPose>::Success({position.Value(), rotation.Value()});
                }
                auto parent = scene.Get(*entity.parent);
                if (parent.HasError())
                    return Result<AudioSpatialPose>::Failure(parent.ErrorValue());
                entity = parent.Value();
            }
            return Result<AudioSpatialPose>::Failure(MakeError(AudioSceneErrors::Capacity));
        }

        /** @brief Derive motion from exact identity history, suppressing velocity across discontinuities. */
        template <typename Record>
        [[nodiscard]] Result<AudioSpatialMotion> Motion(const AudioSpatialIdentity identity, const AudioSpatialPose pose,
                                                        const EntityRef entity, const AudioSceneExtractionInput &input,
                                                        const std::span<const Record> history) {
            const auto previous = std::ranges::find(history, identity, &Record::identity);
            const auto motionInput = std::ranges::find(input.motion, entity, &AudioSceneMotionInput::entity);
            const auto revision = motionInput == input.motion.end() ? 0 : motionInput->discontinuityRevision;
            AudioSpatialMotion motion{.previous = pose, .current = pose, .discontinuityRevision = revision};
            motion.discontinuous = input.discontinuous || previous == history.end() || previous->motion.discontinuityRevision != revision;
            if (!motion.discontinuous) {
                motion.previous = previous->motion.current;
                motion.velocity = motionInput != input.motion.end() && motionInput->velocity
                                      ? *motionInput->velocity
                                      : (pose.position - motion.previous.position) / input.elapsedSeconds;
                if (!Math::IsFinite(motion.velocity))
                    return Result<AudioSpatialMotion>::Failure(MakeError(AudioSceneErrors::InvalidInput));
            }
            return Result<AudioSpatialMotion>::Success(motion);
        }

        /** @brief Compare priority first and complete identity second for reproducible selection. */
        [[nodiscard]] bool Preferred(const AudioSpatialListener &lhs, const AudioSpatialListener &rhs) noexcept {
            return lhs.priority > rhs.priority || (lhs.priority == rhs.priority && lhs.identity < rhs.identity);
        }

        /** @brief Select a bounded listener set and normalize its authored weights without fallback discovery. */
        void SelectListeners(AudioSpatialFrame &frame) noexcept {
            auto begin = frame.listeners.begin();
            if (auto end = begin + static_cast<std::ptrdiff_t>(frame.listenerCount);
                frame.policy == AudioListenerPolicy::Primary && begin != end) {
                const auto preferred = std::min_element(begin, end, Preferred);
                frame.listeners[0] = *preferred;
                frame.listenerCount = 1;
            } else if (frame.policy == AudioListenerPolicy::PerView) {
                std::sort(begin, end, [](const auto &lhs, const auto &rhs) {
                    return lhs.view < rhs.view || (lhs.view == rhs.view && Preferred(lhs, rhs));
                });
                std::size_t count{};
                for (auto cursor = begin; cursor != end; ++cursor)
                    if (count == 0 || frame.listeners[count - 1].view != cursor->view)
                        frame.listeners[count++] = *cursor;
                frame.listenerCount = count;
            }
            double total{};
            for (const auto &listener : frame.Listeners())
                total += listener.weight;
            for (std::size_t index = 0; index < frame.listenerCount; ++index)
                frame.listeners[index].weight = static_cast<float>(frame.listeners[index].weight / total);
        }

        /** @brief Append enabled typed components only after world-pose and motion validation. */
        [[nodiscard]] Result<void> Append(const RuntimeSceneView scene, const RuntimeEntityView &entity,
                                          const AudioSceneExtractionInput &input, const AudioSpatialFrame &history,
                                          AudioSpatialFrame &frame) {
            const auto &components = *entity.components;
            const bool source = components.audioSource && components.audioSource->enabled;
            const bool listener = components.audioListener && components.audioListener->enabled;
            if (!source && !listener)
                return Result<void>::Success();
            if ((source && frame.sourceCount == MaximumSpatialAudioSources) ||
                (listener && frame.listenerCount == MaximumSpatialAudioListeners))
                return Result<void>::Failure(MakeError(AudioSceneErrors::Capacity));
            auto pose = WorldPose(scene, entity);
            if (pose.HasError())
                return Result<void>::Failure(pose.ErrorValue());
            const AudioSpatialIdentity identity{input.context, entity.entity.entity.index + 1, entity.entity.entity.generation};
            if (source) {
                auto motion = Motion(identity, pose.Value(), entity.entity, input, history.Sources());
                if (motion.HasError())
                    return Result<void>::Failure(motion.ErrorValue());
                frame.sources[frame.sourceCount++] = {identity, motion.Value(), components.audioSource->sound,
                                                      components.audioSource->playback};
            }
            if (listener) {
                auto motion = Motion(identity, pose.Value(), entity.entity, input, history.Listeners());
                if (motion.HasError())
                    return Result<void>::Failure(motion.ErrorValue());
                const auto &authored = *components.audioListener;
                frame.listeners[frame.listenerCount++] = {identity, motion.Value(), authored.view, authored.priority, authored.weight};
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc AudioSceneExtractor::Capture */
    Result<Audio::AudioSpatialFrame> AudioSceneExtractor::Capture(const RuntimeSceneView scene, const AudioSceneExtractionInput &input) {
        if (closed_)
            return Result<Audio::AudioSpatialFrame>::Failure(MakeError(AudioSceneErrors::Closed));
        if (!scene.IsCurrent() || input.sequence <= history_.sequence ||
            (scene_.IsValid() && scene_ != scene.RuntimeId() && input.context == history_.context))
            return Result<Audio::AudioSpatialFrame>::Failure(MakeError(AudioSceneErrors::Stale));
        if (const auto valid = ValidateInput(scene, input); valid.HasError())
            return Result<Audio::AudioSpatialFrame>::Failure(valid.ErrorValue());
        Audio::AudioSpatialFrame frame;
        frame.context = input.context;
        frame.sequence = input.sequence;
        frame.policy = input.policy;
        for (std::size_t slot = 0; slot < scene.SlotCount(); ++slot) {
            const auto entity = scene.EntityAt(slot);
            if (entity)
                if (auto result = Append(scene, *entity, input, history_, frame); result.HasError())
                    return Result<Audio::AudioSpatialFrame>::Failure(result.ErrorValue());
        }
        // Keep every authored listener in history; selection must not erase motion for an unselected listener.
        history_ = frame;
        scene_ = scene.RuntimeId();
        SelectListeners(frame);
        return Result<Audio::AudioSpatialFrame>::Success(std::move(frame));
    }

    /** @copydoc AudioSceneExtractor::Shutdown */
    void AudioSceneExtractor::Shutdown() noexcept {
        closed_ = true;
        history_ = {};
        scene_ = {};
    }
}  // namespace Horo::Runtime
