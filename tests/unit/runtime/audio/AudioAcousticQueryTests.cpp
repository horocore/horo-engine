#include "Horo/Audio/AudioAcousticQuery.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <utility>

namespace Horo::Audio {
    namespace {
        template <typename Id> Id StableId(const std::uint64_t value) {
            auto created = Id::Create(value);
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        AudioAcousticProviderCapabilities Capabilities(const std::uint64_t provider = 1, const std::uint64_t generation = 1) {
            AudioAcousticProviderCapabilities capabilities;
            capabilities.provider = StableId<AudioAcousticProviderId>(provider);
            capabilities.generation = generation;
            capabilities.features[static_cast<std::size_t>(AudioAcousticFeature::Occlusion)] = true;
            capabilities.maximumOutstandingQueries = 2;
            capabilities.minimumUpdateInterval = 3;
            capabilities.maximumSmoothingUpdates = 8;
            return capabilities;
        }

        AudioAcousticQueryInput Input(const AudioRuntimeId owner, const std::uint32_t slot = 1, const std::uint32_t generation = 1,
                                      const std::uint64_t update = 1) {
            return {.source = {owner, slot, generation},
                    .listener = {owner, 1, 1},
                    .sourcePosition = {1.0F, 2.0F, 3.0F},
                    .listenerPosition = {4.0F, 5.0F, 6.0F},
                    .feature = AudioAcousticFeature::Occlusion,
                    .controlUpdate = update,
                    .smoothingUpdates = 4};
        }

        AudioAcousticResult ValueResult(const AudioAcousticQuery &query, const float value = 0.5F) {
            return {.query = query.id,
                    .provider = query.provider,
                    .providerGeneration = query.providerGeneration,
                    .source = query.input.source,
                    .listener = query.input.listener,
                    .feature = query.input.feature,
                    .state = AudioAcousticResultState::Value,
                    .value = value};
        }

        TEST_CASE("Acoustic provider declarations reject invalid versions identities and bounds", "[audio][acoustic]") {
            auto capabilities = Capabilities();
            REQUIRE(ValidateAudioAcousticProviderCapabilities(capabilities));
            AudioAcousticQueryLedger ledger(StableId<AudioRuntimeId>(7));
            REQUIRE(ledger.ConfigureProvider(capabilities) == AudioAcousticStatus::Ok);
            REQUIRE(ledger.ConfigureProvider(capabilities) == AudioAcousticStatus::Ok);
            capabilities.maximumOutstandingQueries = MaximumAudioAcousticSources + 1;
            REQUIRE_FALSE(ValidateAudioAcousticProviderCapabilities(capabilities));
            REQUIRE(ledger.ConfigureProvider(capabilities) == AudioAcousticStatus::InvalidCapabilities);
            capabilities = Capabilities();
            capabilities.contractVersion = 2;
            REQUIRE_FALSE(ValidateAudioAcousticProviderCapabilities(capabilities));
            capabilities = Capabilities();
            capabilities.features.fill(false);
            REQUIRE_FALSE(ValidateAudioAcousticProviderCapabilities(capabilities));
            capabilities = Capabilities();
            capabilities.generation = 0;
            REQUIRE_FALSE(ValidateAudioAcousticProviderCapabilities(capabilities));
            capabilities = Capabilities();
            capabilities.minimumUpdateInterval = 0;
            REQUIRE_FALSE(ValidateAudioAcousticProviderCapabilities(capabilities));
            capabilities = Capabilities();
            capabilities.maximumSmoothingUpdates = MaximumAudioAcousticSmoothingUpdates + 1;
            REQUIRE_FALSE(ValidateAudioAcousticProviderCapabilities(capabilities));
        }

        TEST_CASE("Acoustic queries preserve frequency smoothing and typed terminal failures", "[audio][acoustic]") {
            const AudioRuntimeId owner = StableId<AudioRuntimeId>(7);
            AudioAcousticQueryLedger ledger(owner);
            REQUIRE(ledger.ConfigureProvider(Capabilities()) == AudioAcousticStatus::Ok);
            auto input = Input(owner);
            REQUIRE(ledger.BindSource(input.source) == AudioAcousticStatus::Ok);
            const auto first = ledger.BeginQuery(input);
            REQUIRE(first.status == AudioAcousticStatus::Ok);
            REQUIRE(first.query.id.IsValid());
            REQUIRE(ledger.BeginQuery(input).status == AudioAcousticStatus::QueryPending);
            const auto accepted = ledger.AcceptResult(ValueResult(first.query, 0.25F));
            REQUIRE(accepted.status == AudioAcousticStatus::Ok);
            REQUIRE(accepted.applied.source == input.source);
            REQUIRE(accepted.applied.value == 0.25F);
            REQUIRE(accepted.applied.smoothingUpdates == 4);
            REQUIRE(ledger.AcceptResult(ValueResult(first.query)).status == AudioAcousticStatus::QueryStale);
            input.controlUpdate = 3;
            REQUIRE(ledger.BeginQuery(input).status == AudioAcousticStatus::TooFrequent);
            input.controlUpdate = 4;
            const auto second = ledger.BeginQuery(input);
            REQUIRE(second.status == AudioAcousticStatus::Ok);
            REQUIRE(second.query.id.sequence > first.query.id.sequence);
            auto unavailable = ValueResult(second.query);
            unavailable.state = AudioAcousticResultState::Unavailable;
            unavailable.value = 0.0F;
            REQUIRE(ledger.AcceptResult(unavailable).status == AudioAcousticStatus::ProviderUnavailable);
            input.controlUpdate = 7;
            const auto third = ledger.BeginQuery(input);
            REQUIRE(third.status == AudioAcousticStatus::Ok);
            auto cancelled = ValueResult(third.query);
            cancelled.state = AudioAcousticResultState::Cancelled;
            cancelled.value = 0.0F;
            REQUIRE(ledger.AcceptResult(cancelled).status == AudioAcousticStatus::Cancelled);
            input.controlUpdate = 10;
            const auto fourth = ledger.BeginQuery(input);
            REQUIRE(fourth.status == AudioAcousticStatus::Ok);
            auto timedOut = ValueResult(fourth.query);
            timedOut.state = AudioAcousticResultState::TimedOut;
            timedOut.value = 0.0F;
            REQUIRE(ledger.AcceptResult(timedOut).status == AudioAcousticStatus::TimedOut);
        }

        TEST_CASE("Reused acoustic source slots reject old generations and late results", "[audio][acoustic]") {
            const AudioRuntimeId owner = StableId<AudioRuntimeId>(9);
            AudioAcousticQueryLedger ledger(owner);
            REQUIRE(ledger.ConfigureProvider(Capabilities()) == AudioAcousticStatus::Ok);
            const auto oldInput = Input(owner);
            REQUIRE(ledger.BindSource(oldInput.source) == AudioAcousticStatus::Ok);
            const auto oldQuery = ledger.BeginQuery(oldInput);
            REQUIRE(oldQuery.status == AudioAcousticStatus::Ok);
            REQUIRE(ledger.RetireSource(oldInput.source) == AudioAcousticStatus::Ok);
            REQUIRE(ledger.BindSource(oldInput.source) == AudioAcousticStatus::SourceStale);
            const auto newInput = Input(owner, 1, 2);
            REQUIRE(ledger.BindSource(newInput.source) == AudioAcousticStatus::Ok);
            REQUIRE(ledger.AcceptResult(ValueResult(oldQuery.query)).status == AudioAcousticStatus::SourceStale);
            REQUIRE(ledger.BeginQuery(oldInput).status == AudioAcousticStatus::SourceStale);
            const auto newQuery = ledger.BeginQuery(newInput);
            REQUIRE(newQuery.status == AudioAcousticStatus::Ok);
            REQUIRE(ledger.AcceptResult(ValueResult(newQuery.query)).status == AudioAcousticStatus::Ok);
            REQUIRE(ledger.BindSource(oldInput.source) == AudioAcousticStatus::SourceStale);
            REQUIRE(ledger.RetireSource(oldInput.source) == AudioAcousticStatus::SourceStale);
        }

        TEST_CASE("Provider replacement invalidates pending acoustic results without source resurrection", "[audio][acoustic]") {
            const AudioRuntimeId owner = StableId<AudioRuntimeId>(10);
            AudioAcousticQueryLedger ledger(owner);
            REQUIRE(ledger.ConfigureProvider(Capabilities()) == AudioAcousticStatus::Ok);
            auto input = Input(owner);
            REQUIRE(ledger.BindSource(input.source) == AudioAcousticStatus::Ok);
            const auto oldQuery = ledger.BeginQuery(input);
            REQUIRE(oldQuery.status == AudioAcousticStatus::Ok);
            auto changedSameGeneration = Capabilities();
            changedSameGeneration.minimumUpdateInterval = 4;
            REQUIRE(ledger.ConfigureProvider(changedSameGeneration) == AudioAcousticStatus::ProviderStale);
            REQUIRE(ledger.ConfigureProvider(Capabilities(1, 2)) == AudioAcousticStatus::Ok);
            REQUIRE(ledger.AcceptResult(ValueResult(oldQuery.query)).status == AudioAcousticStatus::ProviderStale);
            REQUIRE(ledger.ConfigureProvider(Capabilities()) == AudioAcousticStatus::ProviderStale);
            input.controlUpdate = 4;
            const auto current = ledger.BeginQuery(input);
            REQUIRE(current.status == AudioAcousticStatus::Ok);
            REQUIRE(current.query.providerGeneration == 2);
            REQUIRE(ledger.AcceptResult(ValueResult(current.query)).status == AudioAcousticStatus::Ok);
        }

        TEST_CASE("Acoustic source generation advance invalidates a pending result without explicit retirement", "[audio][acoustic]") {
            const AudioRuntimeId owner = StableId<AudioRuntimeId>(15);
            AudioAcousticQueryLedger ledger(owner);
            REQUIRE(ledger.ConfigureProvider(Capabilities()) == AudioAcousticStatus::Ok);
            const auto prior = Input(owner);
            REQUIRE(ledger.BindSource(prior.source) == AudioAcousticStatus::Ok);
            const auto query = ledger.BeginQuery(prior);
            REQUIRE(query.status == AudioAcousticStatus::Ok);
            const auto replacement = Input(owner, 1, 2);
            REQUIRE(ledger.BindSource(replacement.source) == AudioAcousticStatus::Ok);
            REQUIRE(ledger.AcceptResult(ValueResult(query.query)).status == AudioAcousticStatus::SourceStale);
            REQUIRE(ledger.BeginQuery(replacement).status == AudioAcousticStatus::Ok);
        }

        TEST_CASE("Malformed acoustic results never consume the valid pending query", "[audio][acoustic]") {
            const AudioRuntimeId owner = StableId<AudioRuntimeId>(11);
            AudioAcousticQueryLedger ledger(owner);
            REQUIRE(ledger.ConfigureProvider(Capabilities()) == AudioAcousticStatus::Ok);
            const auto input = Input(owner);
            REQUIRE(ledger.BindSource(input.source) == AudioAcousticStatus::Ok);
            const auto query = ledger.BeginQuery(input);
            REQUIRE(query.status == AudioAcousticStatus::Ok);
            auto bad = ValueResult(query.query);
            bad.value = std::numeric_limits<float>::quiet_NaN();
            REQUIRE(ledger.AcceptResult(bad).status == AudioAcousticStatus::InvalidResult);
            bad = ValueResult(query.query);
            bad.listener.generation = 2;
            REQUIRE(ledger.AcceptResult(bad).status == AudioAcousticStatus::InvalidResult);
            bad = ValueResult(query.query);
            bad.feature = static_cast<AudioAcousticFeature>(255);
            REQUIRE(ledger.AcceptResult(bad).status == AudioAcousticStatus::InvalidResult);
            bad = ValueResult(query.query);
            bad.value = 1.5F;
            REQUIRE(ledger.AcceptResult(bad).status == AudioAcousticStatus::InvalidResult);
            bad = ValueResult(query.query);
            bad.query.sequence += 1;
            REQUIRE(ledger.AcceptResult(bad).status == AudioAcousticStatus::QueryStale);
            REQUIRE(ledger.AcceptResult(ValueResult(query.query)).status == AudioAcousticStatus::Ok);
        }

        TEST_CASE("Acoustic query admission enforces identities features and fixed outstanding capacity", "[audio][acoustic]") {
            const AudioRuntimeId owner = StableId<AudioRuntimeId>(12);
            AudioAcousticQueryLedger ledger(owner);
            REQUIRE(ledger.BeginQuery(Input(owner)).status == AudioAcousticStatus::ProviderUnavailable);
            REQUIRE(ledger.ConfigureProvider(Capabilities()) == AudioAcousticStatus::Ok);
            auto input = Input(owner);
            REQUIRE(ledger.BindSource(input.source) == AudioAcousticStatus::Ok);
            input.feature = AudioAcousticFeature::EnvironmentSend;
            REQUIRE(ledger.BeginQuery(input).status == AudioAcousticStatus::Unsupported);
            input.feature = AudioAcousticFeature::Occlusion;
            input.listener.owner = StableId<AudioRuntimeId>(13);
            REQUIRE(ledger.BeginQuery(input).status == AudioAcousticStatus::InvalidQuery);
            input.listener.owner = owner;
            input.sourcePosition.x = std::numeric_limits<float>::infinity();
            REQUIRE(ledger.BeginQuery(input).status == AudioAcousticStatus::InvalidQuery);
            input.sourcePosition.x = 1.0F;
            input.smoothingUpdates = 9;
            REQUIRE(ledger.BeginQuery(input).status == AudioAcousticStatus::InvalidQuery);
            input.smoothingUpdates = 4;
            REQUIRE(ledger.BeginQuery(input).status == AudioAcousticStatus::Ok);
            const auto second = Input(owner, 2);
            REQUIRE(ledger.BindSource(second.source) == AudioAcousticStatus::Ok);
            REQUIRE(ledger.BeginQuery(second).status == AudioAcousticStatus::Ok);
            const auto third = Input(owner, 3);
            REQUIRE(ledger.BindSource(third.source) == AudioAcousticStatus::Ok);
            REQUIRE(ledger.BeginQuery(third).status == AudioAcousticStatus::CapacityExceeded);
            REQUIRE(ledger.RetireSource(input.source) == AudioAcousticStatus::Ok);
            REQUIRE(ledger.BeginQuery(third).status == AudioAcousticStatus::Ok);
            REQUIRE(ledger.BindSource(Input(owner, MaximumAudioAcousticSources + 1).source) == AudioAcousticStatus::InvalidSource);
        }

        TEST_CASE("Acoustic ledger preparation never executes a provider callback", "[audio][acoustic]") {
            const AudioRuntimeId owner = StableId<AudioRuntimeId>(14);
            AudioAcousticQueryLedger ledger(owner);
            REQUIRE(ledger.ConfigureProvider(Capabilities()) == AudioAcousticStatus::Ok);
            const auto input = Input(owner);
            REQUIRE(ledger.BindSource(input.source) == AudioAcousticStatus::Ok);
            unsigned providerCalls = 0;
            const auto provider = [&providerCalls](const AudioAcousticQuery &query) {
                ++providerCalls;
                return ValueResult(query);
            };
            const auto prepared = ledger.BeginQuery(input);
            REQUIRE(prepared.status == AudioAcousticStatus::Ok);
            REQUIRE(providerCalls == 0);
            const auto result = provider(prepared.query);  // Host-side stand-in for a non-RT provider dispatch.
            REQUIRE(providerCalls == 1);
            REQUIRE(ledger.AcceptResult(result).status == AudioAcousticStatus::Ok);
        }
    }  // namespace
}  // namespace Horo::Audio
