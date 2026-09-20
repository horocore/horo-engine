#include "Horo/Audio/MixerAssetSchema.h"

#include "Horo/Audio/AudioErrors.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_map>
#include <utility>

namespace Horo::Audio {
    namespace {
        template <typename Identity> [[nodiscard]] Identity Stable(const std::uint64_t value) {
            auto identity = Identity::Create(value);
            if (identity.HasError())
                return {};
            return std::move(identity).Value();
        }

        [[nodiscard]] Result<void> Invalid() {
            return Result<void>::Failure(MakeError(AudioErrors::MixerAssetSchemaInvalid));
        }

        [[nodiscard]] Result<void> LimitExceeded() {
            return Result<void>::Failure(MakeError(AudioErrors::MixerAssetSchemaLimitExceeded));
        }

        template <typename T> [[nodiscard]] bool HasDuplicateIdentity(const std::vector<T> &values, const T &candidate) {
            return std::ranges::any_of(values, [&candidate](const T &value) {
                return value == candidate;
            });
        }

        [[nodiscard]] bool IsKnown(const MixerBusRole role) noexcept {
            using enum MixerBusRole;
            switch (role) {
                case MasterOutput:
                case Bus:
                case Return:
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool IsKnown(const MixerEffectKind kind) noexcept {
            using enum MixerEffectKind;
            switch (kind) {
                case Gain:
                case LowPass:
                case HighPass:
                case Reverb:
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool IsKnown(const MixerRouteKind kind) noexcept {
            switch (kind) {
                case MixerRouteKind::Primary:
                case MixerRouteKind::Send:
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool IsKnown(const MixerSendTap tap) noexcept {
            switch (tap) {
                case MixerSendTap::PostInsertPreFader:
                case MixerSendTap::PostFader:
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool IsFinite(const float value) noexcept {
            return std::isfinite(value);
        }

        [[nodiscard]] bool IsValidGainParameters(const MixerEffectParameters &parameters) noexcept {
            const auto *gain = std::get_if<MixerGainEffectParameters>(&parameters);
            return gain != nullptr && IsFinite(gain->gainDb);
        }

        [[nodiscard]] bool IsValidFilterParameters(const MixerEffectParameters &parameters) noexcept {
            const auto *filter = std::get_if<MixerFilterEffectParameters>(&parameters);
            return filter != nullptr && IsFinite(filter->cutoffHz) && filter->cutoffHz > 0.0F && IsFinite(filter->q) && filter->q > 0.0F;
        }

        [[nodiscard]] bool IsValidReverbParameters(const MixerEffectParameters &parameters) noexcept {
            const auto *reverb = std::get_if<MixerReverbEffectParameters>(&parameters);
            return reverb != nullptr && IsFinite(reverb->decaySeconds) && reverb->decaySeconds > 0.0F && IsFinite(reverb->damping) &&
                   reverb->damping >= 0.0F && reverb->damping <= 1.0F && IsFinite(reverb->mix) && reverb->mix >= 0.0F &&
                   reverb->mix <= 1.0F;
        }

        [[nodiscard]] bool IsValidEffectParameters(const MixerEffectDescriptor &effect) noexcept {
            using enum MixerEffectKind;
            switch (effect.kind) {
                case Gain:
                    return IsValidGainParameters(effect.parameters);
                case LowPass:
                case HighPass:
                    return IsValidFilterParameters(effect.parameters);
                case Reverb:
                    return IsValidReverbParameters(effect.parameters);
            }
            return false;
        }

        [[nodiscard]] Result<void> ValidateLimits(const MixerAssetSchemaLimits &limits) {
            if (limits.maximumBuses == 0 || limits.maximumBuses > MaximumMixerAssetBuses || limits.maximumRoutes == 0 ||
                limits.maximumRoutes > MaximumMixerAssetRoutes || limits.maximumEffects == 0 ||
                limits.maximumEffects > MaximumMixerAssetEffects || limits.maximumBusDisplayNameBytes == 0 ||
                limits.maximumBusDisplayNameBytes > MaximumMixerBusDisplayNameBytes)
                return LimitExceeded();
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateEffects(const MixerBusDescriptor &bus, const MixerAssetSchemaLimits &limits,
                                                   std::vector<AudioEffectId> &effectIds, std::size_t &effectCount) {
            if (effectCount > limits.maximumEffects || bus.effects.size() > limits.maximumEffects - effectCount)
                return LimitExceeded();
            for (const MixerEffectDescriptor &effect : bus.effects) {
                if (!effect.id.IsValid() || HasDuplicateIdentity(effectIds, effect.id) || !IsKnown(effect.kind) ||
                    !IsValidEffectParameters(effect))
                    return Invalid();
                effectIds.push_back(effect.id);
            }
            effectCount += bus.effects.size();
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateBusProperties(const MixerBusDescriptor &bus, const MixerAssetSchemaLimits &limits) {
            if (bus.displayName.size() > limits.maximumBusDisplayNameBytes)
                return LimitExceeded();
            if (!IsKnown(bus.role) || !ValidateAudioChannelLayout(ViewAudioChannelLayout(bus.layout)) ||
                bus.displayName.find('\0') != std::string::npos || !IsFinite(bus.defaults.gainDb))
                return Invalid();
            return Result<void>::Success();
        }

        struct BusValidationState final {
            std::vector<AudioBusId> busIds;
            std::vector<AudioEffectId> effectIds;
            std::size_t effectCount{};
            bool foundMaster{};
            std::size_t masterIndex{};
        };

        [[nodiscard]] Result<void> ValidateBus(const MixerBusDescriptor &bus, const std::size_t index, const MixerAssetSchemaLimits &limits,
                                               BusValidationState &state) {
            if (!bus.id.IsValid() || HasDuplicateIdentity(state.busIds, bus.id))
                return Invalid();
            state.busIds.push_back(bus.id);
            if (const Result<void> properties = ValidateBusProperties(bus, limits); properties.HasError())
                return properties;
            if (bus.role == MixerBusRole::MasterOutput) {
                if (state.foundMaster)
                    return Invalid();
                state.foundMaster = true;
                state.masterIndex = index;
            }
            return ValidateEffects(bus, limits, state.effectIds, state.effectCount);
        }

        [[nodiscard]] Result<void> ValidateBuses(const MixerAssetSchema &asset, const MixerAssetSchemaLimits &limits,
                                                 std::size_t &masterIndex) {
            if (asset.buses.empty())
                return Invalid();
            if (asset.buses.size() > limits.maximumBuses)
                return LimitExceeded();

            BusValidationState state;
            state.busIds.reserve(asset.buses.size());
            state.effectIds.reserve(std::min(limits.maximumEffects, asset.buses.size()));
            for (std::size_t index = 0; index < asset.buses.size(); ++index) {
                if (const Result<void> valid = ValidateBus(asset.buses[index], index, limits, state); valid.HasError())
                    return valid;
            }
            if (!state.foundMaster)
                return Invalid();
            masterIndex = state.masterIndex;
            return Result<void>::Success();
        }

        struct RouteValidationState final {
            std::vector<std::size_t> primaryCounts;
            std::vector<std::size_t> returnSendCounts;
            std::vector<std::size_t> primaryDestinations;
            std::vector<std::vector<std::size_t>> adjacency;
            std::vector<std::size_t> indegree;
            std::unordered_map<std::uint64_t, std::size_t> busIndexById;
        };

        [[nodiscard]] bool ResolveRouteEndpoints(const MixerRouteDescriptor &route, const RouteValidationState &state,
                                                 const std::size_t masterIndex, std::size_t &sourceIndex, std::size_t &destinationIndex) {
            if (!route.source.IsValid() || !route.destination.IsValid() || route.source == route.destination || !IsKnown(route.kind) ||
                !IsKnown(route.tap) || !IsFinite(route.gainDb))
                return false;
            const auto source = state.busIndexById.find(route.source.Value());
            const auto destination = state.busIndexById.find(route.destination.Value());
            if (source == state.busIndexById.end() || destination == state.busIndexById.end() || source->second == masterIndex)
                return false;
            sourceIndex = source->second;
            destinationIndex = destination->second;
            return true;
        }

        [[nodiscard]] Result<void> RecordRouteKind(const MixerAssetSchema &asset, const MixerRouteDescriptor &route,
                                                   const std::size_t sourceIndex, const std::size_t destinationIndex,
                                                   RouteValidationState &state) {
            if (route.kind == MixerRouteKind::Primary) {
                if (!route.enabled || route.tap != MixerSendTap::PostFader || asset.buses[destinationIndex].role == MixerBusRole::Return)
                    return Invalid();
                ++state.primaryCounts[sourceIndex];
                state.primaryDestinations[sourceIndex] = destinationIndex;
            } else if (asset.buses[destinationIndex].role == MixerBusRole::Return) {
                ++state.returnSendCounts[destinationIndex];
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateAndRecordRoute(const MixerAssetSchema &asset, const MixerRouteDescriptor &route,
                                                          const std::size_t masterIndex, std::vector<AudioRouteId> &routeIds,
                                                          RouteValidationState &state) {
            std::size_t sourceIndex{};
            std::size_t destinationIndex{};
            if (!route.id.IsValid() || HasDuplicateIdentity(routeIds, route.id) ||
                !ResolveRouteEndpoints(route, state, masterIndex, sourceIndex, destinationIndex))
                return Invalid();
            routeIds.push_back(route.id);
            if (const Result<void> valid = RecordRouteKind(asset, route, sourceIndex, destinationIndex, state); valid.HasError())
                return valid;
            ++state.indegree[destinationIndex];
            state.adjacency[sourceIndex].push_back(destinationIndex);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidatePrimaryCounts(const MixerAssetSchema &asset, const std::size_t masterIndex,
                                                         const RouteValidationState &state) {
            for (std::size_t index = 0; index < asset.buses.size(); ++index) {
                if (const bool isMaster = index == masterIndex;
                    (isMaster && state.primaryCounts[index] != 0) || (!isMaster && state.primaryCounts[index] != 1))
                    return Invalid();
                if (asset.buses[index].role == MixerBusRole::Return && state.returnSendCounts[index] == 0)
                    return Invalid();
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidatePrimaryReachability(const MixerAssetSchema &asset, const std::size_t masterIndex,
                                                               const RouteValidationState &state) {
            std::vector<std::uint8_t> reachedMaster(asset.buses.size());
            for (std::size_t start = 0; start < asset.buses.size(); ++start) {
                if (start == masterIndex)
                    continue;
                std::ranges::fill(reachedMaster, false);
                std::size_t current = start;
                while (current != masterIndex) {
                    if (reachedMaster[current] || state.primaryDestinations[current] == asset.buses.size())
                        return Invalid();
                    reachedMaster[current] = true;
                    current = state.primaryDestinations[current];
                }
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateAcyclic(RouteValidationState state) {
            std::queue<std::size_t> ready;
            for (std::size_t index = 0; index < state.indegree.size(); ++index) {
                if (state.indegree[index] == 0)
                    ready.push(index);
            }
            std::size_t visited{};
            while (!ready.empty()) {
                const std::size_t current = ready.front();
                ready.pop();
                ++visited;
                for (const std::size_t destination : state.adjacency[current]) {
                    --state.indegree[destination];
                    if (state.indegree[destination] == 0)
                        ready.push(destination);
                }
            }
            return visited == state.indegree.size() ? Result<void>::Success() : Invalid();
        }

        [[nodiscard]] Result<void> ValidateRoutes(const MixerAssetSchema &asset, const MixerAssetSchemaLimits &limits,
                                                  const std::size_t masterIndex) {
            if (asset.routes.size() > limits.maximumRoutes)
                return LimitExceeded();
            std::vector<AudioRouteId> routeIds;
            routeIds.reserve(asset.routes.size());
            RouteValidationState state{.primaryCounts = std::vector<std::size_t>(asset.buses.size()),
                                       .returnSendCounts = std::vector<std::size_t>(asset.buses.size()),
                                       .primaryDestinations = std::vector<std::size_t>(asset.buses.size(), asset.buses.size()),
                                       .adjacency = std::vector<std::vector<std::size_t>>(asset.buses.size()),
                                       .indegree = std::vector<std::size_t>(asset.buses.size()),
                                       .busIndexById = {}};
            state.busIndexById.reserve(asset.buses.size());
            for (std::size_t index = 0; index < asset.buses.size(); ++index)
                state.busIndexById.emplace(asset.buses[index].id.Value(), index);
            for (const MixerRouteDescriptor &route : asset.routes) {
                if (const Result<void> valid = ValidateAndRecordRoute(asset, route, masterIndex, routeIds, state); valid.HasError())
                    return valid;
            }
            if (const Result<void> valid = ValidatePrimaryCounts(asset, masterIndex, state); valid.HasError())
                return valid;
            if (const Result<void> valid = ValidatePrimaryReachability(asset, masterIndex, state); valid.HasError())
                return valid;
            return ValidateAcyclic(std::move(state));
        }
    }  // namespace

    /** @copydoc MakeDefaultMixerAsset */
    MixerAssetSchema MakeDefaultMixerAsset() {
        using enum MixerBusRole;
        using enum MixerRouteKind;
        using enum MixerSendTap;
        const AudioChannelLayout stereo = MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo);
        MixerAssetSchema asset;
        asset.buses = {{Stable<AudioBusId>(1), MasterOutput, "Master", stereo, {}, {}},
                       {Stable<AudioBusId>(2), Bus, "Music", stereo, {}, {}},
                       {Stable<AudioBusId>(3), Bus, "SFX", stereo, {}, {}},
                       {Stable<AudioBusId>(4), Bus, "UI", stereo, {}, {}},
                       {Stable<AudioBusId>(5), Bus, "Voice", stereo, {}, {}},
                       {Stable<AudioBusId>(6), Bus, "Ambient", stereo, {}, {}}};
        asset.routes = {{Stable<AudioRouteId>(1), Stable<AudioBusId>(2), Stable<AudioBusId>(1), Primary, PostFader, 0.0F, true},
                        {Stable<AudioRouteId>(2), Stable<AudioBusId>(3), Stable<AudioBusId>(1), Primary, PostFader, 0.0F, true},
                        {Stable<AudioRouteId>(3), Stable<AudioBusId>(4), Stable<AudioBusId>(1), Primary, PostFader, 0.0F, true},
                        {Stable<AudioRouteId>(4), Stable<AudioBusId>(5), Stable<AudioBusId>(1), Primary, PostFader, 0.0F, true},
                        {Stable<AudioRouteId>(5), Stable<AudioBusId>(6), Stable<AudioBusId>(1), Primary, PostFader, 0.0F, true}};
        return asset;
    }

    /** @copydoc ValidateMixerAssetSchemaLimits */
    Result<void> ValidateMixerAssetSchemaLimits(const MixerAssetSchemaLimits &limits) {
        return ValidateLimits(limits);
    }

    /** @copydoc ValidateMixerAssetSchema */
    Result<void> ValidateMixerAssetSchema(const MixerAssetSchema &asset, const MixerAssetSchemaLimits &limits) {
        if (const Result<void> validLimits = ValidateLimits(limits); validLimits.HasError())
            return validLimits;
        if (ClassifyMixerAssetSchemaCompatibility(asset.version) != MixerAssetSchemaCompatibility::Exact)
            return Result<void>::Failure(MakeError(AudioErrors::MixerAssetSchemaVersionUnsupported));
        std::size_t masterIndex{};
        if (const Result<void> buses = ValidateBuses(asset, limits, masterIndex); buses.HasError())
            return buses;
        return ValidateRoutes(asset, limits, masterIndex);
    }

    /** @copydoc MigrateMixerAssetSchema */
    Result<MixerAssetSchema> MigrateMixerAssetSchema(const MixerAssetSchema &source, const MixerAssetSchemaLimits &limits) {
        if (const Result<void> validLimits = ValidateLimits(limits); validLimits.HasError())
            return Result<MixerAssetSchema>::Failure(validLimits.ErrorValue());
        const MixerAssetSchemaCompatibility compatibility = ClassifyMixerAssetSchemaCompatibility(source.version);
        if (compatibility == MixerAssetSchemaCompatibility::Unsupported)
            return Result<MixerAssetSchema>::Failure(MakeError(AudioErrors::MixerAssetSchemaVersionUnsupported));

        MixerAssetSchema migrated = source;
        if (compatibility == MixerAssetSchemaCompatibility::MigrationRequired) {
            for (MixerBusDescriptor &bus : migrated.buses) {
                if (bus.layout.orderedChannels.empty())
                    bus.layout = MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo);
            }
            migrated.version = CurrentMixerAssetSchemaVersion;
            if (const Result<void> valid = ValidateMixerAssetSchema(migrated, limits); valid.HasError())
                return Result<MixerAssetSchema>::Failure(WrapError(AudioErrors::MixerAssetSchemaMigrationFailed, valid.ErrorValue()));
        } else if (const Result<void> valid = ValidateMixerAssetSchema(migrated, limits); valid.HasError()) {
            return Result<MixerAssetSchema>::Failure(valid.ErrorValue());
        }
        return Result<MixerAssetSchema>::Success(std::move(migrated));
    }
}  // namespace Horo::Audio
