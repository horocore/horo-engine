#include "Horo/Physics/PhysicsCollisionSchema.h"

#include "Horo/Physics/PhysicsErrors.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <new>
#include <utility>

namespace Horo::Physics {
    namespace {
        constexpr std::size_t MaxEnabledLayers = 64;
        constexpr std::size_t MaxEnabledChannels = 64;
        constexpr std::size_t MaxProfiles = 1024;
        constexpr std::size_t MaxRetainedDefinitions = 4096;

        template <typename Id> bool LessId(const Id &left, const Id &right) {
            return left.Bytes() < right.Bytes();
        }

        template <typename Value, typename Id> bool SortUnique(std::vector<Value> &values, Id Value::*member) {
            std::ranges::sort(values, [member](const Value &left, const Value &right) {
                return LessId(left.*member, right.*member);
            });
            return std::ranges::adjacent_find(values, [member](const Value &left, const Value &right) {
                return left.*member == right.*member;
            }) == values.end();
        }

        template <typename Value, typename Id> const Value *Find(const std::vector<Value> &values, const Id id, Id Value::*member) {
            const auto found = std::ranges::lower_bound(values, id, LessId<Id>, member);
            return found != values.end() && (*found).*member == id ? &*found : nullptr;
        }

        void HashByte(std::uint64_t &hash, const std::uint8_t value) noexcept {
            hash ^= value;
            hash *= 1099511628211ULL;
        }

        void HashId(std::uint64_t &hash, const auto &id) noexcept {
            for (const std::uint8_t byte : id.Bytes())
                HashByte(hash, byte);
        }

        void HashBool(std::uint64_t &hash, const bool value) noexcept {
            HashByte(hash, static_cast<std::uint8_t>(value));
        }

        bool PairLess(const CollisionPairDefinition &left, const CollisionPairDefinition &right) {
            if (left.first != right.first)
                return LessId(left.first, right.first);
            return LessId(left.second, right.second);
        }

        Result<void> ValidateCrossTypeIds(const ProjectCollisionSchema &schema) {
            std::map<std::array<std::uint8_t, 16>, std::uint8_t> types;
            const auto admit = [&types](const auto &definitions, const std::uint8_t type) {
                for (const auto &definition : definitions) {
                    const auto [found, inserted] = types.try_emplace(definition.id.Bytes(), type);
                    if (!inserted && found->second != type)
                        return Result<void>::Failure(
                            MakeError(PhysicsErrors::DescriptorInvalid, "Collision identity is reused across types."));
                }
                return Result<void>::Success();
            };
            if (const auto layers = admit(schema.layers, 0); layers.HasError())
                return layers;
            if (const auto channels = admit(schema.queryChannels, 1); channels.HasError())
                return channels;
            if (const auto profiles = admit(schema.profiles, 2); profiles.HasError())
                return profiles;
            return Result<void>::Success();
        }

        Result<void> NormalizeDefinitions(std::vector<CollisionLayerDefinition> &layers,
                                          std::vector<CollisionQueryChannelDefinition> &channels,
                                          std::vector<CollisionProfileDefinition> &profiles) {
            if (!SortUnique(layers, &CollisionLayerDefinition::id) || !SortUnique(channels, &CollisionQueryChannelDefinition::id) ||
                !SortUnique(profiles, &CollisionProfileDefinition::id))
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Duplicate collision identity."));
            const auto enabledLayers = std::ranges::count(layers, true, &CollisionLayerDefinition::enabled);
            if (const auto enabledChannels = std::ranges::count(channels, true, &CollisionQueryChannelDefinition::enabled);
                enabledLayers == 0 || enabledLayers > MaxEnabledLayers || enabledChannels > MaxEnabledChannels)
                return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            for (const auto &layer : layers) {
                if (!layer.id.IsValid() || (layer.enabled && !layer.admitsStatic && !layer.admitsKinematic && !layer.admitsDynamic))
                    return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Invalid collision layer."));
            }
            if (std::ranges::any_of(channels, [](const auto &channel) {
                return !channel.id.IsValid();
            }))
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Invalid query channel."));
            return Result<void>::Success();
        }

        Result<void> NormalizePairs(std::vector<CollisionPairDefinition> &pairs, const std::vector<CollisionLayerDefinition> &layers) {
            for (auto &pair : pairs) {
                if (!pair.first.IsValid() || !pair.second.IsValid() || pair.response >= SimulationPairResponse::Count)
                    return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Invalid collision pair."));
                if (LessId(pair.second, pair.first))
                    std::swap(pair.first, pair.second);
                const auto *first = Find(layers, pair.first, &CollisionLayerDefinition::id);
                const auto *second = Find(layers, pair.second, &CollisionLayerDefinition::id);
                if (first == nullptr || second == nullptr || !first->enabled || !second->enabled)
                    return Result<void>::Failure(
                        MakeError(PhysicsErrors::DescriptorInvalid, "Collision pair references an unavailable layer."));
                if (pair.response == SimulationPairResponse::Overlap && (!first->admitsOverlap || !second->admitsOverlap))
                    return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Overlap is not admitted by both layers."));
            }
            std::ranges::sort(pairs, PairLess);
            if (std::ranges::adjacent_find(pairs, [](const auto &left, const auto &right) {
                return left.first == right.first && left.second == right.second;
            }) != pairs.end())
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Duplicate collision pair."));
            if (const auto layerCount = static_cast<std::size_t>(std::ranges::count(layers, true, &CollisionLayerDefinition::enabled));
                pairs.size() != layerCount * (layerCount + 1) / 2)
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Collision matrix is incomplete."));
            return Result<void>::Success();
        }

        Result<void> NormalizeProfiles(std::vector<CollisionProfileDefinition> &profiles,
                                       const std::vector<CollisionLayerDefinition> &layers,
                                       const std::vector<CollisionQueryChannelDefinition> &channels,
                                       const CollisionProfileId defaultProfile) {
            const auto enabledChannels = std::ranges::count(channels, true, &CollisionQueryChannelDefinition::enabled);
            for (auto &profile : profiles) {
                if (const auto *layer = Find(layers, profile.layer, &CollisionLayerDefinition::id);
                    !profile.id.IsValid() || layer == nullptr || !layer->enabled ||
                    (profile.lifecycleEventsEnabled && !profile.simulationEnabled))
                    return Result<void>::Failure(
                        MakeError(PhysicsErrors::DescriptorInvalid, "Collision profile has an invalid layer or participation policy."));
                if (!SortUnique(profile.queryResponses, &CollisionProfileQueryResponse::channel))
                    return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Duplicate profile query response."));
                if (profile.queryResponses.size() != static_cast<std::size_t>(enabledChannels))
                    return Result<void>::Failure(
                        MakeError(PhysicsErrors::DescriptorInvalid, "Profile query response table is incomplete."));
                for (const auto &response : profile.queryResponses) {
                    const auto *channel = Find(channels, response.channel, &CollisionQueryChannelDefinition::id);
                    if (channel == nullptr || !channel->enabled || response.response >= CollisionQueryResponse::Count ||
                        (!profile.queryEnabled && response.response != CollisionQueryResponse::Ignore))
                        return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Invalid profile query response."));
                }
            }
            if (Find(profiles, defaultProfile, &CollisionProfileDefinition::id) == nullptr)
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Default collision profile is missing."));
            return Result<void>::Success();
        }

        std::uint64_t ComputeFingerprint(const CollisionProfileId defaultProfile, const std::vector<CollisionLayerDefinition> &layers,
                                         const std::vector<CollisionPairDefinition> &pairs,
                                         const std::vector<CollisionQueryChannelDefinition> &channels,
                                         const std::vector<CollisionProfileDefinition> &profiles) noexcept {
            std::uint64_t fingerprint = 14695981039346656037ULL;
            HashId(fingerprint, defaultProfile);
            for (const auto &layer : layers) {
                HashId(fingerprint, layer.id);
                for (const bool value :
                     {layer.enabled, layer.admitsStatic, layer.admitsKinematic, layer.admitsDynamic, layer.admitsOverlap})
                    HashBool(fingerprint, value);
            }
            for (const auto &pair : pairs) {
                HashId(fingerprint, pair.first);
                HashId(fingerprint, pair.second);
                HashByte(fingerprint, static_cast<std::uint8_t>(pair.response));
            }
            for (const auto &channel : channels) {
                HashId(fingerprint, channel.id);
                HashBool(fingerprint, channel.enabled);
            }
            for (const auto &profile : profiles) {
                HashId(fingerprint, profile.id);
                HashId(fingerprint, profile.layer);
                for (const bool value : {profile.simulationEnabled, profile.queryEnabled, profile.lifecycleEventsEnabled})
                    HashBool(fingerprint, value);
                for (const auto &response : profile.queryResponses) {
                    HashId(fingerprint, response.channel);
                    HashByte(fingerprint, static_cast<std::uint8_t>(response.response));
                }
            }
            return fingerprint;
        }
    }  // namespace

    /** @copydoc NormalizedCollisionSchema::Create */
    Result<NormalizedCollisionSchema> NormalizedCollisionSchema::Create(const ProjectCollisionSchema &authored) {
        try {
            if (authored.schemaVersion != 1 || authored.requiredFeatureBits != 0 || !authored.defaultProfile.IsValid())
                return Result<NormalizedCollisionSchema>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Unsupported collision schema metadata."));
            if (authored.layers.size() > MaxRetainedDefinitions || authored.queryChannels.size() > MaxRetainedDefinitions ||
                authored.profiles.size() > MaxProfiles || authored.pairs.size() > MaxEnabledLayers * (MaxEnabledLayers + 1) / 2 ||
                std::ranges::any_of(authored.profiles, [](const auto &profile) {
                return profile.queryResponses.size() > MaxEnabledChannels;
            }))
                return Result<NormalizedCollisionSchema>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            if (const auto ids = ValidateCrossTypeIds(authored); ids.HasError())
                return Result<NormalizedCollisionSchema>::Failure(ids.ErrorValue());

            auto layers = authored.layers;
            auto channels = authored.queryChannels;
            auto profiles = authored.profiles;
            auto pairs = authored.pairs;
            if (const auto definitions = NormalizeDefinitions(layers, channels, profiles); definitions.HasError())
                return Result<NormalizedCollisionSchema>::Failure(definitions.ErrorValue());
            if (const auto matrix = NormalizePairs(pairs, layers); matrix.HasError())
                return Result<NormalizedCollisionSchema>::Failure(matrix.ErrorValue());
            if (const auto profileTable = NormalizeProfiles(profiles, layers, channels, authored.defaultProfile); profileTable.HasError())
                return Result<NormalizedCollisionSchema>::Failure(profileTable.ErrorValue());
            const auto fingerprint = ComputeFingerprint(authored.defaultProfile, layers, pairs, channels, profiles);
            return Result<NormalizedCollisionSchema>::Success(NormalizedCollisionSchema{authored.defaultProfile, std::move(layers),
                                                                                        std::move(pairs), std::move(channels),
                                                                                        std::move(profiles), fingerprint});
        } catch (const std::bad_alloc &) {
            return Result<NormalizedCollisionSchema>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        }
    }

    /** @copydoc NormalizedCollisionSchema::Layers */
    std::span<const CollisionLayerDefinition> NormalizedCollisionSchema::Layers() const noexcept {
        return layers_;
    }

    /** @copydoc NormalizedCollisionSchema::Pairs */
    std::span<const CollisionPairDefinition> NormalizedCollisionSchema::Pairs() const noexcept {
        return pairs_;
    }

    /** @copydoc NormalizedCollisionSchema::QueryChannels */
    std::span<const CollisionQueryChannelDefinition> NormalizedCollisionSchema::QueryChannels() const noexcept {
        return channels_;
    }

    /** @copydoc NormalizedCollisionSchema::Profiles */
    std::span<const CollisionProfileDefinition> NormalizedCollisionSchema::Profiles() const noexcept {
        return profiles_;
    }

    /** @copydoc NormalizedCollisionSchema::DefaultProfile */
    CollisionProfileId NormalizedCollisionSchema::DefaultProfile() const noexcept {
        return defaultProfile_;
    }

    /** @copydoc NormalizedCollisionSchema::SemanticFingerprint */
    std::uint64_t NormalizedCollisionSchema::SemanticFingerprint() const noexcept {
        return fingerprint_;
    }

    /** @copydoc NormalizedCollisionSchema::ResolvePair */
    Result<SimulationPairResponse> NormalizedCollisionSchema::ResolvePair(CollisionLayerId first, CollisionLayerId second) const {
        if (LessId(second, first))
            std::swap(first, second);
        const auto found = std::ranges::lower_bound(pairs_, CollisionPairDefinition{first, second}, PairLess);
        if (found == pairs_.end() || found->first != first || found->second != second)
            return Result<SimulationPairResponse>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Collision pair is unknown."));
        return Result<SimulationPairResponse>::Success(found->response);
    }

    /** @copydoc NormalizedCollisionSchema::ResolveProfile */
    Result<const CollisionProfileDefinition *> NormalizedCollisionSchema::ResolveProfile(const CollisionProfileId id) const {
        const auto *profile = Find(profiles_, id, &CollisionProfileDefinition::id);
        if (profile == nullptr)
            return Result<const CollisionProfileDefinition *>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "Collision profile is unknown."));
        return Result<const CollisionProfileDefinition *>::Success(profile);
    }

    /** @copydoc NormalizedCollisionSchema::ResolveQuery */
    Result<CollisionQueryResponse> NormalizedCollisionSchema::ResolveQuery(const CollisionProfileId profile,
                                                                           const PhysicsQueryChannelId channel) const {
        const auto foundProfile = ResolveProfile(profile);
        if (foundProfile.HasError())
            return Result<CollisionQueryResponse>::Failure(foundProfile.ErrorValue());
        const auto &responses = foundProfile.Value()->queryResponses;
        const auto *response = Find(responses, channel, &CollisionProfileQueryResponse::channel);
        if (response == nullptr)
            return Result<CollisionQueryResponse>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Query channel is unknown."));
        return Result<CollisionQueryResponse>::Success(response->response);
    }

    /** @brief Owns validated canonical filter data. */
    NormalizedCollisionSchema::NormalizedCollisionSchema(CollisionProfileId defaultProfile, std::vector<CollisionLayerDefinition> layers,
                                                         std::vector<CollisionPairDefinition> pairs,
                                                         std::vector<CollisionQueryChannelDefinition> channels,
                                                         std::vector<CollisionProfileDefinition> profiles, const std::uint64_t fingerprint)
        : defaultProfile_(defaultProfile), layers_(std::move(layers)), pairs_(std::move(pairs)), channels_(std::move(channels)),
          profiles_(std::move(profiles)), fingerprint_(fingerprint) {}

    /** @copydoc NormalizedCollisionSchema::ResolveStaticSensorProfile */
    Result<const CollisionProfileDefinition *> NormalizedCollisionSchema::ResolveStaticSensorProfile(const CollisionProfileId id) const {
        const auto profile = ResolveProfile(id);
        if (profile.HasError())
            return profile;
        const auto layer = std::ranges::find(layers_, profile.Value()->layer, &CollisionLayerDefinition::id);
        if (!profile.Value()->simulationEnabled || !profile.Value()->lifecycleEventsEnabled || layer == layers_.end() || !layer->enabled ||
            !layer->admitsStatic || !layer->admitsOverlap)
            return Result<const CollisionProfileDefinition *>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid,
                          "The selected project profile/layer must admit static sensors and lifecycle events."));
        return profile;
    }
}  // namespace Horo::Physics
