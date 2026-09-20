#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/MixerAssetSchema.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

namespace Horo::Audio {
    namespace {
        template <typename Id> Id Stable(const std::uint64_t value) {
            auto result = Id::Create(value);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        template <typename T> void RequireError(const Result<T> &result, const ErrorCodeDescriptor &descriptor) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == descriptor.code.Value());
        }

        MixerBusDescriptor &Bus(MixerAssetSchema &asset, const std::uint64_t id) {
            for (MixerBusDescriptor &bus : asset.buses) {
                if (bus.id.Value() == id)
                    return bus;
            }
            FAIL("expected mixer bus was not found");
            return asset.buses.front();
        }

        MixerRouteDescriptor Primary(const std::uint64_t id, const std::uint64_t source, const std::uint64_t destination) {
            return {.id = Stable<AudioRouteId>(id),
                    .source = Stable<AudioBusId>(source),
                    .destination = Stable<AudioBusId>(destination),
                    .kind = MixerRouteKind::Primary,
                    .tap = MixerSendTap::PostFader,
                    .gainDb = 0.0F,
                    .enabled = true};
        }

        MixerRouteDescriptor Send(const std::uint64_t id, const std::uint64_t source, const std::uint64_t destination,
                                  const MixerSendTap tap = MixerSendTap::PostFader, const float gainDb = 0.0F, const bool enabled = true) {
            return {.id = Stable<AudioRouteId>(id),
                    .source = Stable<AudioBusId>(source),
                    .destination = Stable<AudioBusId>(destination),
                    .kind = MixerRouteKind::Send,
                    .tap = tap,
                    .gainDb = gainDb,
                    .enabled = enabled};
        }

        MixerAssetSchema AssetWithReturnAndSend() {
            MixerAssetSchema asset = MakeDefaultMixerAsset();
            MixerBusDescriptor reverb;
            reverb.id = Stable<AudioBusId>(100);
            reverb.role = MixerBusRole::Return;
            reverb.displayName = "Environment Return";
            reverb.layout = MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo);
            reverb.effects.push_back({.id = Stable<AudioEffectId>(200),
                                      .kind = MixerEffectKind::Reverb,
                                      .bypassed = false,
                                      .parameters = MixerReverbEffectParameters{.decaySeconds = 1.5F, .damping = 0.4F, .mix = 1.0F}});
            asset.buses.push_back(std::move(reverb));
            asset.routes.push_back(Primary(100, 100, 1));
            asset.routes.push_back({.id = Stable<AudioRouteId>(101),
                                    .source = Stable<AudioBusId>(2),
                                    .destination = Stable<AudioBusId>(100),
                                    .kind = MixerRouteKind::Send,
                                    .tap = MixerSendTap::PostInsertPreFader,
                                    .gainDb = -3.0F,
                                    .enabled = true});
            return asset;
        }
    }  // namespace

    TEST_CASE("Default mixer asset is versioned and uses stable bus identity", "[unit][audio][mixer-schema]") {
        const MixerAssetSchema asset = MakeDefaultMixerAsset();
        REQUIRE(ValidateMixerAssetSchema(asset).HasValue());
        CHECK(asset.version == CurrentMixerAssetSchemaVersion);
        CHECK(asset.buses.size() == 6);
        CHECK(asset.routes.size() == 5);
        CHECK(asset.buses.front().role == MixerBusRole::MasterOutput);
        CHECK(asset.buses.front().displayName == "Master");
        CHECK(asset.buses.front().id.Value() == 1);
        CHECK(asset.buses[1].id.Value() == 2);
        static_assert(!std::is_same_v<AudioBusId, AudioRouteId>);
        static_assert(!std::is_same_v<AudioRouteId, AudioEffectId>);
    }

    TEST_CASE("Mixer asset persists return buses sends and ordered effect chains", "[unit][audio][mixer-schema]") {
        MixerAssetSchema asset = AssetWithReturnAndSend();
        REQUIRE(ValidateMixerAssetSchema(asset).HasValue());
        CHECK(asset.buses.back().role == MixerBusRole::Return);
        CHECK(asset.buses.back().effects.front().kind == MixerEffectKind::Reverb);
        CHECK(asset.routes.back().tap == MixerSendTap::PostInsertPreFader);
        CHECK(asset.routes.back().source.Value() == 2);
        CHECK(asset.routes.back().destination.Value() == 100);

        asset.buses[1].displayName = "Renamed presentation label";
        REQUIRE(ValidateMixerAssetSchema(asset).HasValue());
        CHECK(asset.buses[1].id.Value() == 2);
        CHECK(asset.routes.back().source.Value() == 2);

        asset.buses.back().effects.push_back({.id = Stable<AudioEffectId>(201),
                                              .kind = MixerEffectKind::Gain,
                                              .bypassed = true,
                                              .parameters = MixerGainEffectParameters{.gainDb = -1.0F}});
        REQUIRE(ValidateMixerAssetSchema(asset).HasValue());
        CHECK(asset.buses.back().effects[0].id.Value() == 200);
        CHECK(asset.buses.back().effects[1].id.Value() == 201);

        std::swap(asset.buses[1], asset.buses[2]);
        std::swap(asset.routes[0], asset.routes[1]);
        REQUIRE(ValidateMixerAssetSchema(asset).HasValue());
        CHECK(asset.buses[1].id.Value() == 3);
        CHECK(asset.routes[0].source.Value() == 3);
    }

    TEST_CASE("Mixer validation rejects missing and duplicate stable identities", "[unit][audio][mixer-schema]") {
        MixerAssetSchema asset = MakeDefaultMixerAsset();
        asset.buses.front().id = {};
        RequireError(ValidateMixerAssetSchema(asset), AudioErrors::MixerAssetSchemaInvalid);

        asset = MakeDefaultMixerAsset();
        asset.buses[1].id = asset.buses[2].id;
        RequireError(ValidateMixerAssetSchema(asset), AudioErrors::MixerAssetSchemaInvalid);

        asset = MakeDefaultMixerAsset();
        asset.routes.push_back(asset.routes.front());
        RequireError(ValidateMixerAssetSchema(asset), AudioErrors::MixerAssetSchemaInvalid);
    }

    TEST_CASE("Mixer validation enforces one rooted primary hierarchy", "[unit][audio][mixer-schema]") {
        MixerAssetSchema asset = MakeDefaultMixerAsset();
        asset.routes.erase(asset.routes.begin());
        RequireError(ValidateMixerAssetSchema(asset), AudioErrors::MixerAssetSchemaInvalid);

        asset = MakeDefaultMixerAsset();
        asset.routes.front().source = Stable<AudioBusId>(1);
        RequireError(ValidateMixerAssetSchema(asset), AudioErrors::MixerAssetSchemaInvalid);

        asset = MakeDefaultMixerAsset();
        asset.routes.front().destination = Stable<AudioBusId>(2);
        RequireError(ValidateMixerAssetSchema(asset), AudioErrors::MixerAssetSchemaInvalid);

        asset = MakeDefaultMixerAsset();
        asset.buses.front().role = MixerBusRole::Bus;
        RequireError(ValidateMixerAssetSchema(asset), AudioErrors::MixerAssetSchemaInvalid);

        asset = MakeDefaultMixerAsset();
        asset.buses[1].role = MixerBusRole::MasterOutput;
        RequireError(ValidateMixerAssetSchema(asset), AudioErrors::MixerAssetSchemaInvalid);

        asset = MakeDefaultMixerAsset();
        asset.routes.front().enabled = false;
        RequireError(ValidateMixerAssetSchema(asset), AudioErrors::MixerAssetSchemaInvalid);

        asset = AssetWithReturnAndSend();
        asset.routes.erase(asset.routes.begin() + 5);
        RequireError(ValidateMixerAssetSchema(asset), AudioErrors::MixerAssetSchemaInvalid);

        asset = AssetWithReturnAndSend();
        asset.routes.back().kind = MixerRouteKind::Primary;
        RequireError(ValidateMixerAssetSchema(asset), AudioErrors::MixerAssetSchemaInvalid);
    }

    TEST_CASE("Mixer validation rejects cycles and malformed cross-tree edges", "[unit][audio][mixer-schema]") {
        MixerAssetSchema asset = MakeDefaultMixerAsset();
        asset.routes.push_back(Send(100, 2, 3));
        asset.routes.push_back(Send(101, 3, 2));
        RequireError(ValidateMixerAssetSchema(asset), AudioErrors::MixerAssetSchemaInvalid);

        asset = MakeDefaultMixerAsset();
        asset.buses.push_back({.id = Stable<AudioBusId>(7),
                               .role = MixerBusRole::Bus,
                               .displayName = "Child",
                               .layout = MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo),
                               .defaults = {},
                               .effects = {}});
        asset.routes.push_back(Primary(100, 7, 2));
        asset.routes.push_back(Send(101, 2, 7));
        RequireError(ValidateMixerAssetSchema(asset), AudioErrors::MixerAssetSchemaInvalid);

        asset = MakeDefaultMixerAsset();
        asset.routes.front().destination = Stable<AudioBusId>(999);
        RequireError(ValidateMixerAssetSchema(asset), AudioErrors::MixerAssetSchemaInvalid);

        asset = MakeDefaultMixerAsset();
        asset.routes.back().tap = static_cast<MixerSendTap>(255);
        RequireError(ValidateMixerAssetSchema(asset), AudioErrors::MixerAssetSchemaInvalid);

        asset = MakeDefaultMixerAsset();
        asset.routes.front().gainDb = std::numeric_limits<float>::quiet_NaN();
        RequireError(ValidateMixerAssetSchema(asset), AudioErrors::MixerAssetSchemaInvalid);

        asset = MakeDefaultMixerAsset();
        Bus(asset, 2).defaults.gainDb = std::numeric_limits<float>::infinity();
        RequireError(ValidateMixerAssetSchema(asset), AudioErrors::MixerAssetSchemaInvalid);
    }

    TEST_CASE("Mixer effect descriptors reject mismatched kinds and nonfinite parameters", "[unit][audio][mixer-schema]") {
        MixerAssetSchema asset = MakeDefaultMixerAsset();
        auto &effectBus = Bus(asset, 2);
        effectBus.effects.push_back({.id = Stable<AudioEffectId>(200),
                                     .kind = MixerEffectKind::LowPass,
                                     .bypassed = false,
                                     .parameters = MixerGainEffectParameters{.gainDb = 0.0F}});
        RequireError(ValidateMixerAssetSchema(asset), AudioErrors::MixerAssetSchemaInvalid);

        asset = MakeDefaultMixerAsset();
        auto &filterBus = Bus(asset, 2);
        filterBus.effects.push_back({.id = Stable<AudioEffectId>(200),
                                     .kind = MixerEffectKind::LowPass,
                                     .bypassed = false,
                                     .parameters = MixerFilterEffectParameters{.cutoffHz = 0.0F, .q = 0.7F}});
        RequireError(ValidateMixerAssetSchema(asset), AudioErrors::MixerAssetSchemaInvalid);

        asset = MakeDefaultMixerAsset();
        auto &gainBus = Bus(asset, 2);
        gainBus.effects.push_back({.id = Stable<AudioEffectId>(200),
                                   .kind = MixerEffectKind::Gain,
                                   .bypassed = false,
                                   .parameters = MixerGainEffectParameters{.gainDb = std::numeric_limits<float>::infinity()}});
        RequireError(ValidateMixerAssetSchema(asset), AudioErrors::MixerAssetSchemaInvalid);

        asset = MakeDefaultMixerAsset();
        Bus(asset, 2).effects.push_back(
            {.id = {}, .kind = MixerEffectKind::Gain, .bypassed = false, .parameters = MixerGainEffectParameters{.gainDb = 0.0F}});
        RequireError(ValidateMixerAssetSchema(asset), AudioErrors::MixerAssetSchemaInvalid);

        asset = MakeDefaultMixerAsset();
        Bus(asset, 2).effects.push_back({.id = Stable<AudioEffectId>(200),
                                         .kind = MixerEffectKind::Gain,
                                         .bypassed = false,
                                         .parameters = MixerGainEffectParameters{.gainDb = 0.0F}});
        Bus(asset, 3).effects.push_back({.id = Stable<AudioEffectId>(200),
                                         .kind = MixerEffectKind::Gain,
                                         .bypassed = false,
                                         .parameters = MixerGainEffectParameters{.gainDb = 0.0F}});
        RequireError(ValidateMixerAssetSchema(asset), AudioErrors::MixerAssetSchemaInvalid);

        asset = MakeDefaultMixerAsset();
        auto &effectLimitBus = Bus(asset, 2);
        effectLimitBus.effects.push_back({.id = Stable<AudioEffectId>(200),
                                          .kind = MixerEffectKind::Gain,
                                          .bypassed = false,
                                          .parameters = MixerGainEffectParameters{.gainDb = 1.0F}});
        effectLimitBus.effects.push_back({.id = Stable<AudioEffectId>(201),
                                          .kind = MixerEffectKind::Gain,
                                          .bypassed = false,
                                          .parameters = MixerGainEffectParameters{.gainDb = 2.0F}});
        MixerAssetSchemaLimits effectLimits;
        effectLimits.maximumEffects = 1;
        RequireError(ValidateMixerAssetSchema(asset, effectLimits), AudioErrors::MixerAssetSchemaLimitExceeded);
    }

    TEST_CASE("Mixer schema limits lower but never raise compiled ceilings", "[unit][audio][mixer-schema]") {
        MixerAssetSchemaLimits limits;
        limits.maximumBuses = 6;
        limits.maximumRoutes = 5;
        REQUIRE(ValidateMixerAssetSchema(MakeDefaultMixerAsset(), limits).HasValue());

        limits.maximumBuses = 5;
        RequireError(ValidateMixerAssetSchema(MakeDefaultMixerAsset(), limits), AudioErrors::MixerAssetSchemaLimitExceeded);
        limits = {};
        limits.maximumRoutes = 4;
        RequireError(ValidateMixerAssetSchema(MakeDefaultMixerAsset(), limits), AudioErrors::MixerAssetSchemaLimitExceeded);
        limits = {};
        limits.maximumBusDisplayNameBytes = 4;
        RequireError(ValidateMixerAssetSchema(MakeDefaultMixerAsset(), limits), AudioErrors::MixerAssetSchemaLimitExceeded);
        limits = {};
        limits.maximumEffects = 0;
        RequireError(ValidateMixerAssetSchemaLimits(limits), AudioErrors::MixerAssetSchemaLimitExceeded);
        limits = {};
        limits.maximumBuses = MaximumMixerAssetBuses + 1;
        RequireError(ValidateMixerAssetSchemaLimits(limits), AudioErrors::MixerAssetSchemaLimitExceeded);
    }

    TEST_CASE("Mixer validation handles the maximum dense acyclic topology", "[unit][audio][mixer-schema]") {
        MixerAssetSchema asset;
        asset.buses.reserve(MaximumMixerAssetBuses);
        asset.routes.reserve(MaximumMixerAssetRoutes);
        const AudioChannelLayout stereo = MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo);
        asset.buses.push_back({Stable<AudioBusId>(1), MixerBusRole::MasterOutput, "Master", stereo, {}, {}});
        for (std::size_t index = 1; index < MaximumMixerAssetBuses; ++index) {
            const std::uint64_t busId = static_cast<std::uint64_t>(index + 1);
            asset.buses.push_back({Stable<AudioBusId>(busId), MixerBusRole::Bus, "Bus", stereo, {}, {}});
            asset.routes.push_back(Primary(static_cast<std::uint64_t>(index), busId, 1));
        }
        for (std::uint64_t routeId = static_cast<std::uint64_t>(asset.routes.size() + 1); asset.routes.size() < MaximumMixerAssetRoutes;
             ++routeId)
            asset.routes.push_back(Send(routeId, 2, 1));

        REQUIRE(asset.buses.size() == MaximumMixerAssetBuses);
        REQUIRE(asset.routes.size() == MaximumMixerAssetRoutes);
        REQUIRE(ValidateMixerAssetSchema(asset).HasValue());
    }

    TEST_CASE("Mixer schema compatibility distinguishes exact migration and unknown versions", "[unit][audio][mixer-schema]") {
        CHECK(ClassifyMixerAssetSchemaCompatibility(CurrentMixerAssetSchemaVersion) == MixerAssetSchemaCompatibility::Exact);
        CHECK(ClassifyMixerAssetSchemaCompatibility({1, 0}) == MixerAssetSchemaCompatibility::MigrationRequired);
        CHECK(ClassifyMixerAssetSchemaCompatibility({0, 9}) == MixerAssetSchemaCompatibility::Unsupported);
        CHECK(ClassifyMixerAssetSchemaCompatibility({1, 2}) == MixerAssetSchemaCompatibility::Unsupported);

        MixerAssetSchema legacy = MakeDefaultMixerAsset();
        legacy.version = {1, 0};
        for (MixerBusDescriptor &bus : legacy.buses)
            bus.layout = {};
        const MixerAssetSchema original = legacy;
        auto migrated = MigrateMixerAssetSchema(legacy);
        REQUIRE(migrated.HasValue());
        CHECK(migrated.Value().version == CurrentMixerAssetSchemaVersion);
        CHECK(ValidateMixerAssetSchema(migrated.Value()).HasValue());
        CHECK(legacy == original);

        MixerAssetSchema current = MakeDefaultMixerAsset();
        current.buses.front().id = {};
        RequireError(MigrateMixerAssetSchema(current), AudioErrors::MixerAssetSchemaInvalid);

        legacy.version = {2, 0};
        RequireError(MigrateMixerAssetSchema(legacy), AudioErrors::MixerAssetSchemaVersionUnsupported);
    }
}  // namespace Horo::Audio
