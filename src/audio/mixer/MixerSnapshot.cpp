#include "Horo/Audio/MixerSnapshot.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace Horo::Audio {
    namespace {
        /** @brief Compare the persistent part of a prepared physical binding. */
        bool Matches(const MixerSnapshotParameter &parameter, const AudioParameterAddress &address) noexcept {
            return parameter.kind == address.kind && parameter.bus == address.bus && parameter.send == address.send &&
                   parameter.effect == address.effect && parameter.parameter == address.parameter;
        }

        /** @brief Bounded display-name length; zero means invalid or empty. */
        std::size_t NameLength(const MixerSnapshotAsset &asset) noexcept {
            for (std::size_t index = 0; index < asset.name.size(); ++index) {
                if (asset.name[index] == '\0')
                    return index;
                if (static_cast<unsigned char>(asset.name[index]) < 32U)
                    return 0;
            }
            return 0;
        }

        /** @brief Encode one bounded unsigned value little endian. */
        void Write(std::span<std::byte> bytes, std::size_t &offset, std::uint64_t value, const std::size_t size) noexcept {
            for (std::size_t index = 0; index < size; ++index) {
                bytes[offset++] = static_cast<std::byte>(value & 255U);
                value >>= 8U;
            }
        }

        /** @brief Read one unsigned value after complete frame-size admission. */
        std::uint64_t Read(const std::span<const std::byte> bytes, std::size_t &offset, const std::size_t size) noexcept {
            std::uint64_t value{};
            for (std::size_t index = 0; index < size; ++index)
                value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[offset++])) << (index * 8U);
            return value;
        }

        /** @brief Construct optional stable identity after shape validation. */
        template <typename Identity> Identity IdentityValue(const std::uint64_t value) {
            return value == 0 ? Identity{} : Identity::Create(value).Value();
        }

        /** @brief Resolve one unambiguous immutable exact binding before preparing owned intent. */
        const AudioAutomationParameter *FindBinding(const MixerSnapshotParameter &parameter,
                                                    const std::span<const AudioAutomationParameter> bindings,
                                                    const AudioCommandScope &scope) noexcept {
            const AudioAutomationParameter *binding{};
            for (const auto &entry : bindings) {
                if (!Matches(parameter, entry.address))
                    continue;
                if (binding || !IsValidAudioParameterAddress(entry.address) || entry.address.owner != scope.owner ||
                    parameter.value < entry.minimum || parameter.value > entry.maximum)
                    return nullptr;
                binding = &entry;
            }
            return binding;
        }

        /** @brief Validate one persistent target shape independently from asset ordering and duplicate checks. */
        bool ValidSnapshotParameter(const MixerSnapshotParameter &parameter) noexcept {
            using enum AudioParameterTargetKind;
            if (!parameter.bus.IsValid() || !parameter.parameter.IsValid() || !std::isfinite(parameter.value))
                return false;
            switch (parameter.kind) {
                case Bus:
                    return !parameter.send.IsValid() && !parameter.effect.IsValid();
                case Send:
                    return parameter.send.IsValid() && !parameter.effect.IsValid();
                case DSP:
                    return parameter.effect.IsValid() && !parameter.send.IsValid();
                default:
                    return false;
            }
        }

        /** @brief Reject malformed retained metadata before it can influence precedence. */
        bool ValidPrepared(const PreparedMixerSnapshot &prepared) noexcept {
            if (prepared.transitionId == 0 || prepared.batch.commandCount == 0 ||
                prepared.batch.commandCount > MaximumMixerSnapshotParameters ||
                prepared.batch.target.kind != AudioCommandTargetKind::ExactSampleFrame)
                return false;
            if (ScheduledAudioCommandBatch normalized;
                NormalizeScheduledAudioCommandBatch(prepared.batch, normalized) != ScheduledAudioCommandBatchStatus::Ok)
                return false;
            for (std::uint32_t index = 0; index < prepared.batch.commandCount; ++index) {
                const auto *parameter = std::get_if<AudioAutomateParameterCommand>(&prepared.batch.commands[index].payload);
                if (!parameter || parameter->request.address.kind == AudioParameterTargetKind::Voice ||
                    parameter->request.startFrame != prepared.batch.target.sampleFrame ||
                    parameter->request.clockGeneration != prepared.batch.target.clockGeneration ||
                    parameter->request.discontinuityRevision != prepared.batch.target.discontinuityRevision)
                    return false;
                for (std::uint32_t previous = 0; previous < index; ++previous)
                    if (std::get<AudioAutomateParameterCommand>(prepared.batch.commands[previous].payload).request.address ==
                        parameter->request.address)
                        return false;
            }
            return true;
        }
    }  // namespace

    /** @copydoc ValidateMixerSnapshot */
    MixerSnapshotStatus ValidateMixerSnapshot(const MixerSnapshotAsset &asset) noexcept {
        using enum MixerSnapshotStatus;
        if (!asset.mixer.IsValid() || NameLength(asset) == 0 || asset.parameterCount == 0 ||
            asset.parameterCount > MaximumMixerSnapshotParameters)
            return InvalidAsset;
        const auto nameLength = NameLength(asset);
        for (std::size_t index = nameLength; index < asset.name.size(); ++index)
            if (asset.name[index] != '\0')
                return InvalidAsset;
        for (std::uint32_t index = 0; index < asset.parameterCount; ++index) {
            const auto &parameter = asset.parameters[index];
            if (!ValidSnapshotParameter(parameter))
                return InvalidAsset;
            for (std::uint32_t previous = 0; previous < index; ++previous) {
                auto target = asset.parameters[previous];
                target.value = parameter.value;
                if (target == parameter)
                    return InvalidAsset;
            }
        }
        for (std::size_t index = asset.parameterCount; index < asset.parameters.size(); ++index)
            if (asset.parameters[index] != MixerSnapshotParameter{})
                return InvalidAsset;
        return Ok;
    }

    /** @copydoc SerializeMixerSnapshot */
    MixerSnapshotStatus SerializeMixerSnapshot(const MixerSnapshotAsset &asset, const std::span<std::byte> bytes,
                                               std::size_t &written) noexcept {
        if (const auto status = ValidateMixerSnapshot(asset); status != MixerSnapshotStatus::Ok)
            return status;
        const auto nameLength = NameLength(asset);
        if (const auto size = 23U + nameLength + asset.parameterCount * 37U; bytes.size() < size)
            return MixerSnapshotStatus::Capacity;
        std::size_t offset{};
        Write(bytes, offset, 0x504e534dU, 4);  // MSNP
        Write(bytes, offset, 1, 1);
        Write(bytes, offset, nameLength, 1);
        for (const auto value : asset.mixer.Bytes())
            Write(bytes, offset, value, 1);
        Write(bytes, offset, asset.parameterCount, 1);
        for (std::size_t index = 0; index < nameLength; ++index)
            Write(bytes, offset, static_cast<unsigned char>(asset.name[index]), 1);
        for (std::uint32_t index = 0; index < asset.parameterCount; ++index) {
            const auto &parameter = asset.parameters[index];
            Write(bytes, offset, static_cast<std::uint8_t>(parameter.kind), 1);
            Write(bytes, offset, parameter.bus.Value(), 8);
            Write(bytes, offset, parameter.send.Value(), 8);
            Write(bytes, offset, parameter.effect.Value(), 8);
            Write(bytes, offset, parameter.parameter.Value(), 8);
            Write(bytes, offset, std::bit_cast<std::uint32_t>(parameter.value), 4);
        }
        written = offset;
        return MixerSnapshotStatus::Ok;
    }

    /** @copydoc DeserializeMixerSnapshot */
    MixerSnapshotStatus DeserializeMixerSnapshot(const std::span<const std::byte> bytes, MixerSnapshotAsset &asset) noexcept {
        using enum MixerSnapshotStatus;
        if (bytes.size() < 23 || bytes.size() > MaximumMixerSnapshotSerializedBytes)
            return InvalidSerialization;
        std::size_t offset{};
        if (Read(bytes, offset, 4) != 0x504e534dU)
            return InvalidSerialization;
        if (Read(bytes, offset, 1) != 1)
            return UnsupportedVersion;
        const auto nameLength = Read(bytes, offset, 1);
        std::array<std::uint8_t, 16> mixer{};
        for (auto &value : mixer)
            value = static_cast<std::uint8_t>(Read(bytes, offset, 1));
        MixerSnapshotAsset candidate{.mixer = Assets::AssetId::FromBytes(mixer)};
        candidate.parameterCount = static_cast<std::uint32_t>(Read(bytes, offset, 1));
        if (nameLength == 0 || nameLength > MaximumMixerSnapshotNameBytes || candidate.parameterCount == 0 ||
            candidate.parameterCount > MaximumMixerSnapshotParameters || bytes.size() != 23U + nameLength + candidate.parameterCount * 37U)
            return InvalidSerialization;
        for (std::size_t index = 0; index < nameLength; ++index)
            candidate.name[index] = static_cast<char>(Read(bytes, offset, 1));
        for (std::uint32_t index = 0; index < candidate.parameterCount; ++index) {
            auto &parameter = candidate.parameters[index];
            parameter.kind = static_cast<AudioParameterTargetKind>(Read(bytes, offset, 1));
            parameter.bus = IdentityValue<AudioBusId>(Read(bytes, offset, 8));
            parameter.send = IdentityValue<AudioRouteId>(Read(bytes, offset, 8));
            parameter.effect = IdentityValue<AudioEffectId>(Read(bytes, offset, 8));
            parameter.parameter = IdentityValue<AudioParameterId>(Read(bytes, offset, 8));
            parameter.value = std::bit_cast<float>(static_cast<std::uint32_t>(Read(bytes, offset, 4)));
        }
        if (const auto status = ValidateMixerSnapshot(candidate); status != Ok)
            return status;
        asset = candidate;
        return Ok;
    }

    /** @copydoc PrepareMixerSnapshot */
    MixerSnapshotStatus PrepareMixerSnapshot(const MixerSnapshotAsset &asset, const Assets::AssetId &mixer,
                                             const std::span<const AudioAutomationParameter> bindings,
                                             const MixerSnapshotTransitionRequest &transition, PreparedMixerSnapshot &prepared) noexcept {
        using enum MixerSnapshotStatus;
        if (const auto status = ValidateMixerSnapshot(asset); status != Ok)
            return status;
        if (asset.mixer != mixer || bindings.size() > MaximumAudioAutomationParameters)
            return InvalidBinding;
        if (transition.transitionId == 0 || transition.firstRequestId == 0 ||
            transition.firstRequestId > std::numeric_limits<std::uint64_t>::max() - (asset.parameterCount - 1U) ||
            transition.target.kind != AudioCommandTargetKind::ExactSampleFrame)
            return InvalidTransition;
        PreparedMixerSnapshot candidate{.transitionId = transition.transitionId,
                                        .priority = transition.priority,
                                        .batch = {.target = transition.target}};
        for (std::uint32_t index = 0; index < asset.parameterCount; ++index) {
            const auto &parameter = asset.parameters[index];
            const auto *binding = FindBinding(parameter, bindings, transition.scope);
            if (!binding)
                return InvalidBinding;
            const AudioParameterAutomationRequest request{.address = binding->address,
                                                          .requestId = transition.firstRequestId + index,
                                                          .clockGeneration = transition.target.clockGeneration,
                                                          .discontinuityRevision = transition.target.discontinuityRevision,
                                                          .startFrame = transition.target.sampleFrame,
                                                          .durationFrames = transition.durationFrames,
                                                          .targetValue = parameter.value,
                                                          .curve = transition.curve};
            candidate.batch.commands[candidate.batch.commandCount++] = {.scope = transition.scope,
                                                                        .payload = AudioAutomateParameterCommand{request}};
        }
        if (!ValidPrepared(candidate))
            return InvalidTransition;
        prepared = candidate;
        return Ok;
    }

    /** @copydoc MixerSnapshotTransitions::Active */
    bool MixerSnapshotTransitions::Active() const noexcept {
        for (std::uint32_t index = 0; index < count_; ++index)
            if (automation_->HasRequest(requests_[index]))
                return true;
        return false;
    }

    /** @copydoc MixerSnapshotTransitions::AppendCancellation */
    void MixerSnapshotTransitions::AppendCancellation(ScheduledAudioCommandBatch &batch) const noexcept {
        for (std::uint32_t index = 0; index < count_; ++index)
            if (automation_->HasRequest(requests_[index]))
                batch.commands[batch.commandCount++] = {.scope = scope_,
                                                        .payload =
                                                            AudioCancelAutomationCommand{.requestId = requests_[index],
                                                                                         .clockGeneration = batch.target.clockGeneration,
                                                                                         .discontinuityRevision =
                                                                                             batch.target.discontinuityRevision}};
    }

    /** @copydoc MixerSnapshotTransitions::Apply */
    MixerSnapshotStatus MixerSnapshotTransitions::Apply(const PreparedMixerSnapshot &prepared) noexcept {
        using enum MixerSnapshotStatus;
        if (!ValidPrepared(prepared))
            return InvalidTransition;
        if (prepared.transitionId <= transitionId_)
            return Duplicate;
        if (Active() && prepared.priority < priority_)
            return Precedence;
        ScheduledAudioCommandBatch batch{.target = prepared.batch.target};
        AppendCancellation(batch);
        for (std::uint32_t index = 0; index < prepared.batch.commandCount; ++index)
            batch.commands[batch.commandCount++] = prepared.batch.commands[index];
        status_ = automation_->ApplyBatch(batch);
        if (status_ != AudioAutomationStatus::Ok)
            return AutomationRejected;
        scope_ = prepared.batch.commands[0].scope;
        count_ = prepared.batch.commandCount;
        for (std::uint32_t index = 0; index < count_; ++index)
            requests_[index] = std::get<AudioAutomateParameterCommand>(prepared.batch.commands[index].payload).request.requestId;
        transitionId_ = prepared.transitionId;
        priority_ = prepared.priority;
        return Ok;
    }

    /** @copydoc MixerSnapshotTransitions::Cancel */
    MixerSnapshotStatus MixerSnapshotTransitions::Cancel(const std::uint64_t transitionId, const AudioCommandTarget &target) noexcept {
        using enum MixerSnapshotStatus;
        if (transitionId != transitionId_ || !Active())
            return NotFound;
        if (target.kind != AudioCommandTargetKind::ExactSampleFrame)
            return InvalidTransition;
        ScheduledAudioCommandBatch batch{.target = target};
        AppendCancellation(batch);
        status_ = automation_->ApplyBatch(batch);
        if (status_ != AudioAutomationStatus::Ok)
            return AutomationRejected;
        count_ = 0;
        return Ok;
    }
}  // namespace Horo::Audio
