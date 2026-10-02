#include "Horo/Audio/AudioCommands.h"

#include <cmath>
#include <type_traits>

namespace Horo::Audio {
    namespace {
        static_assert(std::is_nothrow_copy_assignable_v<AudioCommand>);
        static_assert(std::is_nothrow_move_assignable_v<AudioCommand>);
        static_assert(std::is_nothrow_copy_constructible_v<AudioCommand>);
        static_assert(std::is_trivially_destructible_v<AudioCommand>);

        /** @brief Validate the shared runtime/scene generation before visiting any payload identity. */
        bool ValidScope(const AudioCommand &command) noexcept {
            const auto &scope = command.scope;
            if (!scope.owner.IsValid() || scope.epoch == 0) {
                return false;
            }
            if (std::holds_alternative<AudioResetCommand>(command.payload)) {
                return scope.scene == AudioSceneContextHandle{};
            }
            return scope.scene.IsValid() && scope.scene.owner == scope.owner;
        }

        /** @brief Bound payload identity checks to the already validated runtime owner without registry access. */
        struct PayloadValidator {
            AudioRuntimeId owner;

            /** @brief Check a typed runtime handle's shape and exact owner. */
            template <typename Tag> bool Handle(const AudioHandle<Tag> &handle) const noexcept {
                return handle.IsValid() && handle.owner == owner;
            }

            /** @brief Check every pool handle identity field; pool liveness is a separate control fact. */
            bool Storage(const AudioMemoryHandle &handle) const noexcept {
                return handle.owner == owner && handle.pool.IsValid() && handle.slot != 0 && handle.generation != 0;
            }

            /** @brief Validate both already admitted voice and clip handles. */
            bool operator()(const AudioCreateVoiceCommand &command) const noexcept {
                return Handle(command.voice) && Handle(command.clip);
            }

            /** @brief Validate the target voice without starting playback. */
            bool operator()(const AudioStartVoiceCommand &command) const noexcept {
                return Handle(command.voice);
            }

            /** @brief Validate the target voice without performing a stop. */
            bool operator()(const AudioStopVoiceCommand &command) const noexcept {
                return Handle(command.voice);
            }

            /** @brief Validate typed playback intent while leaving state/PCM bounds to the playback owner. */
            bool operator()(const AudioVoiceControlRequest &command) const noexcept {
                return Handle(command.voice) && ValidateAudioVoiceControlRequest(command);
            }

            /** @brief Reject malformed parameter targets and nonfinite model values. */
            bool operator()(const AudioSetParameterCommand &command) const noexcept {
                return Handle(command.voice) && command.parameter.IsValid() && std::isfinite(command.value);
            }

            /** @brief Validate prepared graph storage without resolving or adopting it. */
            bool operator()(const AudioSwapGraphCommand &command) const noexcept {
                return Storage(command.storage);
            }

            /** @brief Validate the requested logical-release identity without reclaiming storage. */
            bool operator()(const AudioReleaseResourceCommand &command) const noexcept {
                return Storage(command.storage);
            }

            /** @brief The already validated scope fully identifies a scene-unload barrier. */
            bool operator()(const AudioSceneUnloadCommand &) const noexcept {
                return true;
            }

            /** @brief The already validated runtime epoch fully identifies a reset barrier. */
            bool operator()(const AudioResetCommand &) const noexcept {
                return true;
            }

            /** @brief Validate one retained atomic-batch reference without resolving callback-visible storage. */
            bool operator()(const AudioScheduledBatchCommand &command) const noexcept {
                using enum AudioCommandTargetKind;
                const bool bufferTarget = command.target.kind == NextBufferBoundary && command.target.sampleFrame == 0 &&
                                          command.target.clockGeneration == 0 && command.target.discontinuityRevision == 0;
                const bool sampleTarget = command.target.kind == ExactSampleFrame && command.target.clockGeneration != 0 &&
                                          command.target.discontinuityRevision != 0;
                return Storage(command.storage) && command.commandCount != 0 && command.commandCount <= MaximumScheduledAudioCommands &&
                       (bufferTarget || sampleTarget);
            }
        };
    }  // namespace

    /** @copydoc NormalizeAudioCommand */
    AudioCommandStatus NormalizeAudioCommand(const AudioCommand &command, AudioCommand &normalized) noexcept {
        using enum AudioCommandStatus;
        if (!ValidScope(command)) {
            return InvalidScope;
        }
        if (!std::visit(PayloadValidator{command.scope.owner}, command.payload)) {
            return InvalidPayload;
        }
        normalized = command;
        if (auto *parameter = std::get_if<AudioSetParameterCommand>(&normalized.payload)) {
            if (parameter->value == 0.0F || std::fpclassify(parameter->value) == FP_SUBNORMAL) {
                parameter->value = 0.0F;
            }
        }
        return Ok;
    }

    /** @copydoc ClassifyAudioCommand */
    AudioCommandClass ClassifyAudioCommand(const AudioCommand &command) noexcept {
        const bool critical = std::holds_alternative<AudioStopVoiceCommand>(command.payload) ||
                              (std::holds_alternative<AudioVoiceControlRequest>(command.payload) &&
                               (std::get<AudioVoiceControlRequest>(command.payload).control == AudioVoiceControl::Stop ||
                                std::get<AudioVoiceControlRequest>(command.payload).control == AudioVoiceControl::Cancel)) ||
                              std::holds_alternative<AudioReleaseResourceCommand>(command.payload) ||
                              std::holds_alternative<AudioSceneUnloadCommand>(command.payload) ||
                              std::holds_alternative<AudioResetCommand>(command.payload) ||
                              (std::holds_alternative<AudioScheduledBatchCommand>(command.payload) &&
                               std::get<AudioScheduledBatchCommand>(command.payload).containsCriticalCommand);
        return critical ? AudioCommandClass::Critical : AudioCommandClass::Ordinary;
    }

    /** @copydoc CanCoalesceAudioCommands */
    bool CanCoalesceAudioCommands(const AudioCommand &earlier, const AudioCommand &later) noexcept {
        const auto *first = std::get_if<AudioSetParameterCommand>(&earlier.payload);
        const auto *second = std::get_if<AudioSetParameterCommand>(&later.payload);
        return first && second && earlier.scope == later.scope && first->voice == second->voice && first->parameter == second->parameter;
    }
}  // namespace Horo::Audio
