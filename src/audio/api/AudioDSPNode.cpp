#include "Horo/Audio/AudioDSPNode.h"

#include "Horo/Audio/AudioErrors.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ranges>

namespace Horo::Audio {
    namespace {
        [[nodiscard]] Result<void> Invalid() {
            return Result<void>::Failure(MakeError(AudioErrors::DspContractInvalid));
        }

        [[nodiscard]] Result<void> Limits() {
            return Result<void>::Failure(MakeError(AudioErrors::DspContractLimitExceeded));
        }

        [[nodiscard]] Result<void> Storage() {
            return Result<void>::Failure(MakeError(AudioErrors::DspPreparationStorageInsufficient));
        }

        [[nodiscard]] bool Known(const AudioDSPPortKind kind) noexcept {
            using enum AudioDSPPortKind;
            switch (kind) {
                case Main:
                case Sidechain:
                case Auxiliary:
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool Known(const AudioDSPBypassMode mode) noexcept {
            using enum AudioDSPBypassMode;
            switch (mode) {
                case Process:
                case CopyMainInput:
                case Silence:
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool Aligned(const std::span<const std::byte> bytes, const std::size_t alignment) noexcept {
            return bytes.empty() || (reinterpret_cast<std::uintptr_t>(bytes.data()) % alignment) == 0;
        }

        [[nodiscard]] bool ValidParameter(const AudioDSPParameterDescriptor &parameter) noexcept {
            return parameter.identity.IsValid() && std::isfinite(parameter.minimum) && std::isfinite(parameter.maximum) &&
                   std::isfinite(parameter.defaultValue) && parameter.minimum <= parameter.maximum &&
                   parameter.defaultValue >= parameter.minimum && parameter.defaultValue <= parameter.maximum;
        }

        [[nodiscard]] bool ValidPort(const AudioDSPPortDescriptor &port, const bool output) noexcept {
            return Known(port.kind) && ValidateAudioProcessingFormat(port.format) && port.maximumFrames != 0 &&
                   port.maximumFrames <= MaximumAudioCallbackFrames && (!output || !port.optional);
        }

        [[nodiscard]] bool ValidBuffer(const AudioDSPPortBuffer &binding, const AudioDSPPortDescriptor &port, const std::uint32_t frames,
                                       const bool output) noexcept {
            if (!binding.connected)
                return !output && port.optional;
            if (!ValidateAudioPlanarBlock(binding.block, port.format) || binding.block.capacityFrames < frames ||
                binding.block.capacityFrames > port.maximumFrames)
                return false;
            return output || binding.block.validFrames >= frames;
        }

        [[nodiscard]] bool ValidParameterValue(const AudioDSPParameterValue &value,
                                               const AudioDSPParameterDescriptor &descriptor) noexcept {
            return value.identity == descriptor.identity && std::isfinite(value.value) && std::isfinite(value.target) &&
                   value.value >= descriptor.minimum && value.value <= descriptor.maximum && value.target >= descriptor.minimum &&
                   value.target <= descriptor.maximum;
        }

        enum class DescriptorValidity : std::uint8_t {
            Valid,
            Invalid,
            Limits
        };

        [[nodiscard]] DescriptorValidity CheckDescriptor(const AudioDSPNodeDescriptor &descriptor) noexcept {
            if (descriptor.inputs.empty() || descriptor.inputs.size() > MaximumAudioDSPPorts || descriptor.outputs.empty() ||
                descriptor.outputs.size() > MaximumAudioDSPPorts || descriptor.parameters.size() > MaximumAudioDSPParameters ||
                descriptor.maximumFrames == 0 || descriptor.maximumFrames > MaximumAudioCallbackFrames ||
                descriptor.latencyFrames > MaximumAudioDSPLatencyFrames || descriptor.tailFrames > MaximumAudioDSPTailFrames ||
                descriptor.memory.alignment != AudioDSPMemoryAlignment || descriptor.memory.stateBytes > MaximumAudioDSPStateBytes ||
                descriptor.memory.scratchBytes > MaximumAudioDSPScratchBytes || !descriptor.allocationFree || !descriptor.blockingFree ||
                !descriptor.loggingFree || !descriptor.externalCallbackFree)
                return DescriptorValidity::Limits;

            if (const auto validPorts = [](const auto &ports, const bool output) {
                return std::ranges::all_of(ports, [output](const auto &port) {
                    return ValidPort(port, output);
                });
            }; !validPorts(descriptor.inputs, false) || !validPorts(descriptor.outputs, true))
                return DescriptorValidity::Invalid;

            for (std::size_t index = 0; index < descriptor.parameters.size(); ++index) {
                if (!ValidParameter(descriptor.parameters[index]))
                    return DescriptorValidity::Invalid;
                for (std::size_t previous = 0; previous < index; ++previous) {
                    if (descriptor.parameters[previous].identity == descriptor.parameters[index].identity)
                        return DescriptorValidity::Invalid;
                }
            }
            return DescriptorValidity::Valid;
        }
    }  // namespace

    /** @copydoc ValidateAudioDSPNodeDescriptor */
    Result<void> ValidateAudioDSPNodeDescriptor(const AudioDSPNodeDescriptor &descriptor) {
        switch (CheckDescriptor(descriptor)) {
            case DescriptorValidity::Valid:
                return Result<void>::Success();
            case DescriptorValidity::Invalid:
                return Invalid();
            case DescriptorValidity::Limits:
                return Limits();
        }
        return Invalid();
    }

    /** @copydoc ValidateAudioDSPPreparation */
    Result<void> ValidateAudioDSPPreparation(const AudioDSPNodeDescriptor &descriptor, const AudioDSPPrepareContext &preparation) {
        if (const auto validation = ValidateAudioDSPNodeDescriptor(descriptor); validation.HasError())
            return validation;
        if (preparation.maximumFrames == 0 || preparation.maximumFrames > descriptor.maximumFrames ||
            preparation.stateStorage.size() < descriptor.memory.stateBytes ||
            preparation.scratchStorage.size() < descriptor.memory.scratchBytes ||
            !Aligned(preparation.stateStorage, AudioDSPMemoryAlignment) || !Aligned(preparation.scratchStorage, AudioDSPMemoryAlignment))
            return Storage();
        return Result<void>::Success();
    }

    /** @copydoc ValidateAudioDSPProcess */
    bool ValidateAudioDSPProcess(const AudioDSPNodeDescriptor &descriptor, const AudioDSPProcessContext &process) noexcept {
        if (process.frames == 0 || process.frames > descriptor.maximumFrames || process.inputs.size() != descriptor.inputs.size() ||
            process.outputs.size() != descriptor.outputs.size() || process.parameters.size() != descriptor.parameters.size() ||
            process.stateStorage.size() < descriptor.memory.stateBytes || process.scratchStorage.size() < descriptor.memory.scratchBytes ||
            !Aligned(process.stateStorage, AudioDSPMemoryAlignment) || !Aligned(process.scratchStorage, AudioDSPMemoryAlignment) ||
            !Known(process.bypass) || (process.bypass != AudioDSPBypassMode::Process && !descriptor.supportsBypass))
            return false;

        for (std::size_t index = 0; index < process.inputs.size(); ++index) {
            if (!ValidBuffer(process.inputs[index], descriptor.inputs[index], process.frames, false))
                return false;
        }
        for (std::size_t index = 0; index < process.outputs.size(); ++index) {
            if (!ValidBuffer(process.outputs[index], descriptor.outputs[index], process.frames, true))
                return false;
        }
        for (std::size_t index = 0; index < process.parameters.size(); ++index) {
            if (!ValidParameterValue(process.parameters[index], descriptor.parameters[index]))
                return false;
        }
        return true;
    }
}  // namespace Horo::Audio
