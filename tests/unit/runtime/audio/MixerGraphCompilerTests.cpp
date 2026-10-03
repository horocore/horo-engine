#include "Horo/Platform/ExternalProcess.h"
#include "MixerAllocationFailureOutcome.h"
#include "MixerTestFixture.h"

#include <cstdio>

using namespace Horo::Tests::MixerFixture;

namespace {
    std::size_t observedFailureBytes{};

    /** @brief Observe the attempted request without changing its countdown or allocating. */
    void ObserveFailureBytes(const std::size_t bytes) noexcept {
        observedFailureBytes = bytes;
    }

    /** @brief Exercise every prefix without allowing checked-STL termination to strand the test process. */
    void CheckAllocationFailurePrefixes(const char *mode) {
        NativeExternalProcessRunner runner;
        std::size_t caught{};
        std::size_t proxyTerminations{};
        constexpr std::size_t maximumPrefixes = 1024;
        for (std::size_t prefix = 0; prefix < maximumPrefixes; ++prefix) {
            std::string diagnostic;
            ExternalProcessRequest request;
            request.executable = HORO_MIXER_ALLOCATION_WORKER;
            request.arguments = {mode, std::to_string(prefix)};
            request.timeout = std::chrono::seconds{5};
            request.gracefulTermination = std::chrono::milliseconds{0};
            request.maximumOutputBytes = 16U * 1024U;
            request.onOutput = [&](ProcessOutputLine line) {
                diagnostic += line.text + '\n';
            };
            const auto child = runner.Run(request, {});
            INFO("mode: " << mode << ", prefix: " << prefix << ", child output: " << diagnostic);
            REQUIRE(child.HasValue());
            REQUIRE(child.Value().reason == ProcessTerminationReason::Exited);
            INFO("worker exit code: " << child.Value().exitCode);
            const auto outcome = static_cast<AllocationFailureOutcome>(child.Value().exitCode);
            if (outcome == AllocationFailureOutcome::Complete) {
                REQUIRE(caught > 0);
                if (std::string_view{mode} != "runtime")
                    REQUIRE(caught > 10);
                std::printf("%s: %zu caught allocation failures, %zu proven MSVC proxy terminations, complete at prefix %zu\n", mode,
                            caught, proxyTerminations, prefix);
                return;
            }
            REQUIRE((outcome == AllocationFailureOutcome::Caught || outcome == AllocationFailureOutcome::MsvcDebugProxyTerminated));
            if (outcome == AllocationFailureOutcome::Caught) {
                ++caught;
            } else {
#if !defined(_MSC_VER) || _ITERATOR_DEBUG_LEVEL == 0
                FAIL("Checked-STL termination is only expected in MSVC iterator-debug builds");
#endif
                if (proxyTerminations == 0)
                    std::fputs(diagnostic.c_str(), stdout);
                ++proxyTerminations;
            }
        }
        FAIL("Mixer preparation did not succeed after every bounded allocation prefix");
    }

    /** @brief Check one explicit request family independently of Catch section selection. */
    void CheckAllocationObserver(const bool aligned) {
        constexpr std::size_t bytes = 64;
        observedFailureBytes = 0;
        bool caught{};
        const std::size_t allocations = Horo::Tests::AllocationProbe::Count();
        const std::size_t frees = Horo::Tests::AllocationProbe::FreeCount();
        {
            Horo::Tests::AllocationProbe::ScopedFailure injected(0, ObserveFailureBytes);
            try {
                void *unexpected = aligned ? ::operator new(bytes, std::align_val_t{64}) : ::operator new(bytes);
                if (aligned)
                    ::operator delete(unexpected, std::align_val_t{64});
                else
                    ::operator delete(unexpected);
            } catch (const std::bad_alloc &) {
                caught = true;
            }
            void *recovered = ::operator new(bytes);
            ::operator delete(recovered);
        }
        const std::size_t acquired = Horo::Tests::AllocationProbe::Count() - allocations;
        const std::size_t released = Horo::Tests::AllocationProbe::FreeCount() - frees;
        REQUIRE(caught);
        REQUIRE(observedFailureBytes == bytes);
        REQUIRE(acquired == 2);
        REQUIRE(released == 1);
    }
}  // namespace

TEST_CASE("Mixer allocation probe observes ordinary and aligned failures and permits one-shot recovery", "[audio][mixer]") {
    SECTION("Ordinary allocation") {
        CheckAllocationObserver(false);
    }
    SECTION("Aligned allocation") {
        CheckAllocationObserver(true);
    }
}

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
    SECTION("Complete topology and aligned storage") {
        CheckAllocationFailurePrefixes("compiler");
    }
    SECTION("Owned DSP descriptors nodes parameters and scratch") {
        CheckAllocationFailurePrefixes("inserts");
    }
}

TEST_CASE("Mixer compilation requires exact nonzero revision identity", "[audio][mixer]") {
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
}

TEST_CASE("Mixer runtime preparation rolls back allocation failures", "[audio][mixer]") {
    CheckAllocationFailurePrefixes("runtime");
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
