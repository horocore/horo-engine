#include "MixerTestFixture.h"

using namespace Horo::Tests::MixerFixture;

TEST_CASE("Mixer compiler canonicalizes topology and route order independently of authoring arrays", "[audio][mixer]") {
    MixerAssetSchema asset = Asset();
    auto first = Plan(asset);
    const std::array<std::uint64_t, 6> expected{2, 3, 4, 5, 6, 1};
    for (std::size_t i = 0; i < expected.size(); ++i)
        REQUIRE(first->Buses()[i].id.Value() == expected[i]);
    std::ranges::reverse(asset.buses);
    std::ranges::reverse(asset.routes);
    auto second = Plan(asset);
    REQUIRE(std::ranges::equal(first->Routes(), second->Routes()));
    REQUIRE(first->StorageBytes() == second->StorageBytes());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        REQUIRE(first->Buses()[i].id == second->Buses()[i].id);
        REQUIRE(first->Buses()[i].preFaderOffset == second->Buses()[i].preFaderOffset);
    }
    REQUIRE_FALSE(first->ResolveBus(IdOf<AudioBusId>(999)).has_value());
}

TEST_CASE("Mixer compiler rejects invalid topology and unsupported semantic routing before publication", "[audio][mixer]") {
    MixerAssetSchema asset = Asset();
    SECTION("Cycle including disabled send remains structurally invalid") {
        asset.routes.push_back({IdOf<AudioRouteId>(90), IdOf<AudioBusId>(2), IdOf<AudioBusId>(3), MixerRouteKind::Send});
        asset.routes.push_back(
            {IdOf<AudioRouteId>(91), IdOf<AudioBusId>(3), IdOf<AudioBusId>(2), MixerRouteKind::Send, MixerSendTap::PostFader, 0, false});
    }
    SECTION("Missing Master") {
        asset.buses[0].role = MixerBusRole::Bus;
    }
    SECTION("Duplicate identity") {
        asset.buses[2].id = asset.buses[1].id;
    }
    SECTION("Missing endpoint") {
        asset.routes[0].source = IdOf<AudioBusId>(999);
    }
    SECTION("Implicit speaker downmix") {
        asset.buses[1].layout = MakeAudioSpeakerLayout(AudioSpeakerPreset::Mono);
    }
    SECTION("Equal channel count is not semantic compatibility") {
        asset.buses[1].layout = {AudioLayoutKind::Discrete, {AudioDiscreteChannel{0}, AudioDiscreteChannel{1}}, {}};
    }
    SECTION("Primary into Return") {
        asset.buses[0].role = MixerBusRole::Return;
    }
    SECTION("Unrepresentable gain") {
        asset.buses[1].defaults.gainDb = std::numeric_limits<float>::max();
    }
    REQUIRE(CompileMixerGraph(asset, Identity(), Profile()).HasError());
}

TEST_CASE("Mixer compiler charges complete fixed storage scratch fan and work bounds", "[audio][mixer]") {
    MixerCompileProfile profile = Profile();
    SECTION("Memory") {
        profile.maximumStorageBytes = 64;
    }
    SECTION("Fan-in") {
        profile.maximumFanIn = 1;
    }
    SECTION("Work") {
        profile.maximumSampleOperations = 1;
    }
    SECTION("Frame") {
        profile.maximumFrames = 0;
    }
    SECTION("Sample rate") {
        profile.sampleRate = 0;
    }
    SECTION("Master format") {
        profile.outputLayout = MakeAudioSpeakerLayout(AudioSpeakerPreset::Mono);
    }
    REQUIRE(CompileMixerGraph(Asset(), Identity(), profile).HasError());
}

TEST_CASE("Mixer compiler requires an explicit factory for persisted DSP inserts", "[audio][mixer]") {
    REQUIRE(CompileMixerGraph(Asset(true), Identity(), Profile()).HasError());
}

TEST_CASE("Mixer compiler prepares complete owned insert state and scratch", "[audio][mixer]") {
    Probe probe;
    Factory factory(probe);
    auto compiled = CompileMixerGraph(Asset(true), Identity(), Profile(), &factory);
    REQUIRE(compiled.HasValue());
    REQUIRE(compiled.Value()->StorageBytes() > Plan(Asset())->StorageBytes());
}

TEST_CASE("Mixer compiler rejects unavailable DSP capabilities", "[audio][mixer]") {
    Probe probe;
    Factory factory(probe);
    MixerCompileProfile profile = Profile();
    SECTION("Prepare failure") {
        probe.failPrepare = true;
    }
    SECTION("Sidechain") {
        factory.sidechain = true;
    }
    SECTION("Latency compensation") {
        factory.latency = 1;
    }
    SECTION("Callback allocation") {
        factory.allocationFree = false;
    }
    SECTION("Scratch budget") {
        profile.maximumScratchBytes = 0;
    }
    SECTION("DSP work declaration") {
        factory.operations = 0;
    }
    const auto compiled = CompileMixerGraph(Asset(true), Identity(), profile, &factory);
    REQUIRE(compiled.HasError());
    REQUIRE(probe.destroyed.load() == 1);
}

TEST_CASE("Mixer preparation allocation failures never publish partially constructed plans", "[audio][mixer]") {
    const MixerAssetSchema asset = Asset();
    const MixerCompileProfile profile = Profile();
    std::uint32_t failures{};
    for (std::uint32_t fail = 0; fail < 256; ++fail) {
        std::optional<Result<std::unique_ptr<MixerRenderPlan>>> result;
        {
            Horo::Tests::AllocationProbe::ScopedFailure injected(fail);
            result.emplace(CompileMixerGraph(asset, Identity(), profile));
        }
        if (result->HasValue())
            break;
        ++failures;
    }
    REQUIRE(failures > 10);
    REQUIRE(failures < 256);
}

TEST_CASE("Mixer compilation requires exact nonzero revision identity and runtime preparation rolls back allocation failures",
          "[audio][mixer]") {
    MixerPlanIdentity invalid = Identity();
    SECTION("Owner") {
        invalid.owner = {};
    }
    SECTION("Epoch") {
        invalid.epoch = 0;
    }
    SECTION("Generation") {
        invalid.generation = 0;
    }
    SECTION("Asset revision") {
        invalid.assetRevision = 0;
    }
    SECTION("Catalog revision") {
        invalid.catalogRevision = 0;
    }
    SECTION("Profile revision") {
        invalid.profileRevision = 0;
    }
    REQUIRE(CompileMixerGraph(Asset(), invalid, Profile()).HasError());
    const MixerRuntimeDescriptor descriptor{Scope, IdOf<AudioMemoryPoolId>(33), Profile()};
    bool succeeded{};
    for (std::size_t fail = 0; fail < 32; ++fail) {
        std::optional<Result<std::unique_ptr<MixerGraphRuntime>>> result;
        {
            Horo::Tests::AllocationProbe::ScopedFailure injected(fail);
            result.emplace(MixerGraphRuntime::Create(descriptor));
        }
        if (result->HasValue()) {
            succeeded = true;
            break;
        }
    }
    REQUIRE(succeeded);
}

TEST_CASE("Mixer cycle diagnostics are canonical across source order and retain typed schema evidence", "[audio][mixer]") {
    MixerAssetSchema asset = Asset();
    asset.routes.push_back({IdOf<AudioRouteId>(90), IdOf<AudioBusId>(3), IdOf<AudioBusId>(2), MixerRouteKind::Send});
    asset.routes.push_back({IdOf<AudioRouteId>(91), IdOf<AudioBusId>(2), IdOf<AudioBusId>(3), MixerRouteKind::Send});
    const auto first = CompileMixerGraph(asset, Identity(), Profile());
    REQUIRE(first.HasError());
    REQUIRE(first.ErrorValue().message == "Mixer asset revision 1, generation 1");
    REQUIRE(first.ErrorValue().cause.Get() != nullptr);
    REQUIRE(first.ErrorValue().cause.Get()->message == "Cycle buses: 2 -> 3 -> 2");
    std::ranges::reverse(asset.buses);
    std::ranges::reverse(asset.routes);
    const auto second = CompileMixerGraph(asset, Identity(), Profile());
    REQUIRE(second.HasError());
    REQUIRE(second.ErrorValue().cause.Get()->message == first.ErrorValue().cause.Get()->message);
}

TEST_CASE("Mixer rejects factory absence incompatible DSP output and schema ceilings without publishing", "[audio][mixer]") {
    Probe probe;
    Factory factory(probe);
    SECTION("Unavailable provider") {
        factory.failCreate = true;
    }
    SECTION("Missing owned strategy") {
        factory.noNode = true;
    }
    SECTION("Explicitly incompatible output") {
        factory.mismatchedFormat = true;
    }
    REQUIRE(CompileMixerGraph(Asset(true), Identity(), Profile(), &factory).HasError());
    MixerAssetSchema oversized = Asset();
    oversized.buses.resize(MaximumMixerAssetBuses + 1);
    REQUIRE(CompileMixerGraph(oversized, Identity(), Profile()).HasError());
}

TEST_CASE("Mixer insert state scratch work reset bypass and selected frame budgets fail before adoption", "[audio][mixer]") {
    Probe probe;
    Factory factory(probe);
    MixerAssetSchema asset = Asset(true);
    MixerCompileProfile profile = Profile();
    SECTION("State allocation bound") {
        profile.maximumStorageBytes = 384;
    }
    SECTION("Scratch allocation bound") {
        profile.maximumStorageBytes = 2368;
    }
    SECTION("DSP work plus host work") {
        factory.operations = profile.maximumSampleOperations / profile.maximumFrames;
    }
    SECTION("Missing reset") {
        factory.reset = false;
    }
    SECTION("Missing bypass") {
        factory.bypass = false;
        asset.buses[1].effects.front().bypassed = true;
    }
    SECTION("Selected frame mismatch") {
        factory.maximumFrames = 8;
    }
    REQUIRE(CompileMixerGraph(asset, Identity(), profile, &factory).HasError());
    REQUIRE(probe.destroyed.load() == 1);
}

TEST_CASE("Mixer unrepresentable route gain reports failure with caller revision intact", "[audio][mixer]") {
    MixerAssetSchema asset = Asset();
    asset.routes.front().gainDb = std::numeric_limits<float>::max();
    const auto result = CompileMixerGraph(asset, Identity(5), Profile());
    REQUIRE(result.HasError());
    REQUIRE(result.ErrorValue().message == "Mixer asset revision 5, generation 5");
}
