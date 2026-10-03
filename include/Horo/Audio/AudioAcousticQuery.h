#pragma once

/**
 * @file AudioAcousticQuery.h
 * @brief Bounded off-callback acoustic query and generation-checked result contract.
 */

#include "Horo/Audio/AudioIdentity.h"
#include "Horo/Math/SceneMath.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace Horo::Audio {
    /** @brief Maximum distinct acoustic source slots in one control-owned query ledger. */
    inline constexpr std::size_t MaximumAudioAcousticSources = 128;
    /** @brief Maximum smoothing interval admitted by this version of the contract. */
    inline constexpr std::uint32_t MaximumAudioAcousticSmoothingUpdates = 1'024;

    /** @brief Semantic acoustic value requested outside the audio callback. */
    enum class AudioAcousticFeature : std::uint8_t {
        Occlusion,
        Obstruction,
        EnvironmentSend,
        Count
    };

    /**
     * @brief Immutable capability facts for one selected provider generation.
     *
     * This describes off-callback query production only, not a callback DSP provider or an
     * active Physics service. A host admits the provider and owns its lifetime. The minimum
     * interval is measured in monotonically increasing non-RT audio-control updates.
     * At most one query may be pending per source; features are sampled on separate updates.
     */
    struct AudioAcousticProviderCapabilities final {
        std::uint32_t contractVersion{1};
        AudioAcousticProviderId provider;
        std::uint64_t generation{};
        std::array<bool, static_cast<std::size_t>(AudioAcousticFeature::Count)> features{};
        std::uint32_t maximumOutstandingQueries{};
        std::uint32_t minimumUpdateInterval{1};
        std::uint32_t maximumSmoothingUpdates{};

        [[nodiscard]] auto operator<=>(const AudioAcousticProviderCapabilities &) const noexcept = default;
    };

    /** @brief Non-wrapping query identity scoped to one audio runtime owner. */
    struct AudioAcousticQueryId final {
        AudioRuntimeId owner;
        std::uint64_t sequence{};

        /** @brief Reports whether both owner and sequence are present. @return True for a usable query identity. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return owner.IsValid() && sequence != 0;
        }

        [[nodiscard]] auto operator<=>(const AudioAcousticQueryId &) const noexcept = default;
    };

    /** @brief Caller-owned input sampled by scene extraction or a non-real-time audio update. */
    struct AudioAcousticQueryInput final {
        AudioAcousticSourceHandle source;
        AudioAcousticListenerHandle listener;
        Math::Vec3 sourcePosition;
        Math::Vec3 listenerPosition;
        AudioAcousticFeature feature{AudioAcousticFeature::Occlusion};
        std::uint64_t controlUpdate{}; /**< Non-zero and monotonic for this bound source. */
        std::uint32_t smoothingUpdates{};
    };

    /** @brief Complete bounded request passed to a host-selected provider only outside the callback. */
    struct AudioAcousticQuery final {
        AudioAcousticQueryId id;
        AudioAcousticProviderId provider;
        std::uint64_t providerGeneration{};
        AudioAcousticQueryInput input;
    };

    /** @brief Provider terminal outcome; only Value carries a normalized numeric result. */
    enum class AudioAcousticResultState : std::uint8_t {
        Value,
        Unsupported,
        Unavailable,
        TimedOut,
        Cancelled,
        Failed
    };

    /** @brief Owned provider result with exact query, source and provider generation evidence. */
    struct AudioAcousticResult final {
        AudioAcousticQueryId query;
        AudioAcousticProviderId provider;
        std::uint64_t providerGeneration{};
        AudioAcousticSourceHandle source;
        AudioAcousticListenerHandle listener;
        AudioAcousticFeature feature{AudioAcousticFeature::Occlusion};
        AudioAcousticResultState state{AudioAcousticResultState::Value};
        float value{}; /**< Finite normalized [0, 1] for Value; zero for failures. */
    };

    /** @brief Typed validation, admission and provider failure categories with no silent fallback. */
    enum class AudioAcousticStatus : std::uint8_t {
        Ok,
        InvalidCapabilities,
        InvalidSource,
        InvalidQuery,
        InvalidResult,
        CapacityExceeded,
        ProviderUnavailable,
        ProviderStale,
        SourceStale,
        QueryStale,
        QueryPending,
        TooFrequent,
        SequenceExhausted,
        Unsupported,
        TimedOut,
        Cancelled,
        ProviderFailed
    };

    /** @brief Prepared provider query or a typed rejection with an empty query. */
    struct AudioAcousticQueryOutcome final {
        AudioAcousticStatus status{AudioAcousticStatus::InvalidQuery};
        AudioAcousticQuery query;
    };

    /** @brief Control-side numeric result; only Ok may be staged as an ordinary audio command. */
    struct AudioAcousticAppliedValue final {
        AudioAcousticSourceHandle source;
        AudioAcousticListenerHandle listener;
        AudioAcousticFeature feature{AudioAcousticFeature::Occlusion};
        float value{};
        std::uint32_t smoothingUpdates{};
    };

    /** @brief Accepted value or a typed rejection/failure with no callback-visible mutation. */
    struct AudioAcousticApplyOutcome final {
        AudioAcousticStatus status{AudioAcousticStatus::InvalidResult};
        AudioAcousticAppliedValue applied;
    };

    /**
     * @brief Fixed-capacity correlation gate owned by one non-real-time audio control thread.
     *
     * This type stores no provider callback and has no RenderPort dependency. Host adapters
     * dispatch returned queries and collect results outside the audio callback. Bind/retire,
     * provider replacement, and result acceptance are serialized by the control owner. A source
     * slot retains its highest generation after retirement, so a late result or same-generation
     * resurrection cannot update a reused slot. No method applies values to callback state;
     * the control owner must stage an accepted value through normal bounded audio commands.
     */
    class AudioAcousticQueryLedger final {
    public:
        /** @brief Construct an inert ledger for one runtime owner. @param owner Non-zero runtime generation. */
        explicit AudioAcousticQueryLedger(AudioRuntimeId owner) noexcept;

        /**
         * @brief Admit or replace one off-callback provider generation, invalidating pending queries on change.
         * @param capabilities Bounded complete provider declaration.
         * @return Ok, InvalidCapabilities, or ProviderStale for a changed same-generation declaration.
         */
        [[nodiscard]] AudioAcousticStatus ConfigureProvider(const AudioAcousticProviderCapabilities &capabilities) noexcept;

        /**
         * @brief Bind one source slot, allowing reuse only at a higher non-wrapping generation.
         * @param source Exact runtime-owned source handle; slot must be within MaximumAudioAcousticSources.
         * @return Ok, InvalidSource, or SourceStale.
         */
        [[nodiscard]] AudioAcousticStatus BindSource(AudioAcousticSourceHandle source) noexcept;

        /**
         * @brief Retire an exact source generation and invalidate its pending result.
         * @param source Currently bound source handle.
         * @return Ok, InvalidSource, or SourceStale.
         */
        [[nodiscard]] AudioAcousticStatus RetireSource(AudioAcousticSourceHandle source) noexcept;

        /**
         * @brief Prepare one bounded query without invoking a provider or touching callback state.
         * @param input Finite positions, active identities, feature and control-update cadence.
         * @return Complete query or precise typed rejection.
         */
        [[nodiscard]] AudioAcousticQueryOutcome BeginQuery(const AudioAcousticQueryInput &input) noexcept;

        /**
         * @brief Admit one terminal provider result against exact pending and active generations.
         * @param result Owned off-callback result; malformed results do not consume the pending query.
         * @return Numeric value only for Ok, otherwise a typed rejection or provider failure.
         */
        [[nodiscard]] AudioAcousticApplyOutcome AcceptResult(const AudioAcousticResult &result) noexcept;

    private:
        struct SourceSlot final {
            AudioAcousticSourceHandle source;
            AudioAcousticQuery pendingQuery;
            std::uint64_t lastIssuedUpdate{};
            bool active{};
            bool pending{};
        };

        /** @brief Resolve an owned, in-range slot without assuming its generation is active. */
        [[nodiscard]] SourceSlot *FindSlot(AudioAcousticSourceHandle source) noexcept;

        AudioRuntimeId owner_;
        AudioAcousticProviderCapabilities provider_;
        std::array<SourceSlot, MaximumAudioAcousticSources> sources_{};
        std::uint64_t nextSequence_{1};
        std::uint32_t outstanding_{};
    };

    /**
     * @brief Validate provider shape without installing it or calling provider code.
     * @param capabilities Candidate bounded declaration.
     * @return True only for a supported complete version, identity, feature set and limits.
     */
    [[nodiscard]] bool ValidateAudioAcousticProviderCapabilities(const AudioAcousticProviderCapabilities &capabilities) noexcept;
}  // namespace Horo::Audio
