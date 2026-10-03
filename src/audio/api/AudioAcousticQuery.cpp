#include "Horo/Audio/AudioAcousticQuery.h"

#include <algorithm>
#include <cmath>

namespace Horo::Audio {
    namespace {
        /** @brief Resolve a feature without indexing past the fixed capability table. */
        [[nodiscard]] bool IsKnownFeature(const AudioAcousticFeature feature) noexcept {
            return static_cast<std::size_t>(feature) < static_cast<std::size_t>(AudioAcousticFeature::Count);
        }

        /** @brief Restrict a provider's terminal state to this contract version. */
        [[nodiscard]] bool IsKnownResultState(const AudioAcousticResultState state) noexcept {
            using enum AudioAcousticResultState;
            return state == Value || state == Unsupported || state == Unavailable || state == TimedOut || state == Cancelled ||
                   state == Failed;
        }

        /** @brief Translate an admitted non-value terminal result without treating it as a numeric value. */
        [[nodiscard]] AudioAcousticStatus TerminalStatus(const AudioAcousticResultState state) noexcept {
            switch (state) {
                case AudioAcousticResultState::Value:
                    return AudioAcousticStatus::Ok;
                case AudioAcousticResultState::Unsupported:
                    return AudioAcousticStatus::Unsupported;
                case AudioAcousticResultState::Unavailable:
                    return AudioAcousticStatus::ProviderUnavailable;
                case AudioAcousticResultState::TimedOut:
                    return AudioAcousticStatus::TimedOut;
                case AudioAcousticResultState::Cancelled:
                    return AudioAcousticStatus::Cancelled;
                case AudioAcousticResultState::Failed:
                    return AudioAcousticStatus::ProviderFailed;
            }
            return AudioAcousticStatus::InvalidResult;
        }
    }  // namespace

    /** @copydoc ValidateAudioAcousticProviderCapabilities */
    bool ValidateAudioAcousticProviderCapabilities(const AudioAcousticProviderCapabilities &capabilities) noexcept {
        if (capabilities.contractVersion != 1 || !capabilities.provider.IsValid() || capabilities.generation == 0 ||
            capabilities.maximumOutstandingQueries == 0 || capabilities.maximumOutstandingQueries > MaximumAudioAcousticSources ||
            capabilities.minimumUpdateInterval == 0 || capabilities.maximumSmoothingUpdates > MaximumAudioAcousticSmoothingUpdates)
            return false;

        return std::ranges::any_of(capabilities.features, [](const bool supported) {
            return supported;
        });
    }

    /** @copydoc AudioAcousticQueryLedger::AudioAcousticQueryLedger */
    AudioAcousticQueryLedger::AudioAcousticQueryLedger(const AudioRuntimeId owner) noexcept : owner_(owner) {}

    /** @copydoc AudioAcousticQueryLedger::ConfigureProvider */
    AudioAcousticStatus AudioAcousticQueryLedger::ConfigureProvider(const AudioAcousticProviderCapabilities &capabilities) noexcept {
        using enum AudioAcousticStatus;
        if (!ValidateAudioAcousticProviderCapabilities(capabilities))
            return InvalidCapabilities;
        if (provider_.provider.IsValid() && provider_.provider == capabilities.provider) {
            if (capabilities.generation < provider_.generation)
                return ProviderStale;
            if (capabilities.generation == provider_.generation)
                return capabilities == provider_ ? Ok : ProviderStale;
        }

        for (SourceSlot &slot : sources_) {
            slot.pending = false;
            slot.pendingQuery = {};
        }
        outstanding_ = 0;
        provider_ = capabilities;
        return Ok;
    }

    /** @copydoc AudioAcousticQueryLedger::BindSource */
    AudioAcousticStatus AudioAcousticQueryLedger::BindSource(const AudioAcousticSourceHandle source) noexcept {
        using enum AudioAcousticStatus;
        SourceSlot *const slot = FindSlot(source);
        if (slot == nullptr)
            return InvalidSource;
        if (source.generation < slot->source.generation || (source.generation == slot->source.generation && !slot->active))
            return SourceStale;
        if (source.generation == slot->source.generation)
            return Ok;

        if (slot->pending)
            --outstanding_;
        slot->source = source;
        slot->pendingQuery = {};
        slot->lastIssuedUpdate = 0;
        slot->active = true;
        slot->pending = false;
        return Ok;
    }

    /** @copydoc AudioAcousticQueryLedger::RetireSource */
    AudioAcousticStatus AudioAcousticQueryLedger::RetireSource(const AudioAcousticSourceHandle source) noexcept {
        using enum AudioAcousticStatus;
        SourceSlot *const slot = FindSlot(source);
        if (slot == nullptr)
            return InvalidSource;
        if (!slot->active || slot->source != source)
            return SourceStale;

        if (slot->pending)
            --outstanding_;
        slot->active = false;
        slot->pending = false;
        slot->pendingQuery = {};
        return Ok;
    }

    /** @copydoc AudioAcousticQueryLedger::BeginQuery */
    AudioAcousticQueryOutcome AudioAcousticQueryLedger::BeginQuery(const AudioAcousticQueryInput &input) noexcept {
        using enum AudioAcousticStatus;
        if (!provider_.provider.IsValid())
            return {ProviderUnavailable, {}};
        SourceSlot *const slot = FindSlot(input.source);
        if (slot == nullptr)
            return {InvalidSource, {}};
        if (!slot->active || slot->source != input.source)
            return {SourceStale, {}};
        if (!input.listener.IsValid() || input.listener.owner != owner_ || !Math::IsFinite(input.sourcePosition) ||
            !Math::IsFinite(input.listenerPosition) || input.controlUpdate == 0 || !IsKnownFeature(input.feature))
            return {InvalidQuery, {}};
        if (!provider_.features[static_cast<std::size_t>(input.feature)])
            return {Unsupported, {}};
        if (input.smoothingUpdates > provider_.maximumSmoothingUpdates)
            return {InvalidQuery, {}};
        if (slot->pending)
            return {QueryPending, {}};
        if (outstanding_ >= provider_.maximumOutstandingQueries)
            return {CapacityExceeded, {}};
        if (slot->lastIssuedUpdate != 0 && (input.controlUpdate <= slot->lastIssuedUpdate ||
                                            input.controlUpdate - slot->lastIssuedUpdate < provider_.minimumUpdateInterval))
            return {TooFrequent, {}};
        if (nextSequence_ == 0)
            return {SequenceExhausted, {}};

        AudioAcousticQuery query{{owner_, nextSequence_}, provider_.provider, provider_.generation, input};
        ++nextSequence_;  // Wraps only after issuing the maximum sequence; zero permanently disables further queries.
        slot->pendingQuery = query;
        slot->pending = true;
        slot->lastIssuedUpdate = input.controlUpdate;
        ++outstanding_;
        return {Ok, query};
    }

    /** @copydoc AudioAcousticQueryLedger::AcceptResult */
    AudioAcousticApplyOutcome AudioAcousticQueryLedger::AcceptResult(const AudioAcousticResult &result) noexcept {
        using enum AudioAcousticStatus;
        if (!result.query.IsValid() || result.query.owner != owner_ || !result.provider.IsValid() || result.providerGeneration == 0 ||
            !result.source.IsValid() || !result.listener.IsValid() || !IsKnownFeature(result.feature) ||
            !IsKnownResultState(result.state) || !std::isfinite(result.value) ||
            (result.state == AudioAcousticResultState::Value ? (result.value < 0.0F || result.value > 1.0F) : result.value != 0.0F))
            return {InvalidResult, {}};
        if (result.provider != provider_.provider || result.providerGeneration != provider_.generation)
            return {ProviderStale, {}};

        SourceSlot *const slot = FindSlot(result.source);
        if (slot == nullptr)
            return {InvalidSource, {}};
        if (!slot->active || slot->source != result.source)
            return {SourceStale, {}};
        if (!slot->pending || slot->pendingQuery.id != result.query)
            return {QueryStale, {}};
        const AudioAcousticQuery &pending = slot->pendingQuery;
        if (pending.input.listener != result.listener || pending.input.feature != result.feature)
            return {InvalidResult, {}};

        AudioAcousticAppliedValue applied;
        if (result.state == AudioAcousticResultState::Value)
            applied = {result.source, result.listener, result.feature, result.value, pending.input.smoothingUpdates};
        slot->pending = false;
        slot->pendingQuery = {};
        --outstanding_;
        return {TerminalStatus(result.state), applied};
    }

    /** @copydoc AudioAcousticQueryLedger::FindSlot */
    AudioAcousticQueryLedger::SourceSlot *AudioAcousticQueryLedger::FindSlot(const AudioAcousticSourceHandle source) noexcept {
        if (!owner_.IsValid() || !source.IsValid() || source.owner != owner_ || source.slot > MaximumAudioAcousticSources)
            return nullptr;
        return &sources_[source.slot - 1];
    }
}  // namespace Horo::Audio
