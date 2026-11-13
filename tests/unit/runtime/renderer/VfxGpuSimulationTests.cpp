#include "support/TypedIdentityTestSupport.h"
#include "support/VfxGpuQualification.h"
#include "support/VfxGpuTestSupport.h"

#include <algorithm>
#include <bit>
#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    struct Fixture {
        Vfx::Tests::GpuSeedFixture seed;
        std::unique_ptr<Vfx::GpuParticleSimulator> owner{seed.Prepare()};
        VfxGpuSimulationPreparation preparation{owner->Preparation()};
        std::vector<std::uint32_t> initial{preparation.initialWords.begin(), preparation.initialWords.end()};
        std::vector<std::uint32_t> forces{preparation.forceWords.begin(), preparation.forceWords.end()};

        Fixture() {
            preparation.initialWords = initial;
            preparation.forceWords = forces;
        }

        void Seal() {
            preparation.descriptor.initializationDigest = ComputeVfxGpuInitializationDigest(preparation);
        }

        GpuVfxSimulationWork MetadataWork() const {
            // Issued-shaped metadata ONLY: this is deliberately not a native/frontend preparation receipt or execution claim.
            GpuVfxSimulationWork w;
            w.identity = preparation.descriptor.identity;
            w.step = {{{17}, 0, 1}, 1, 0, 1, 2};
            w.deltaSeconds = .25F;
            return w;
        }
    };
}  // namespace

TEST_CASE("GPU qualification policy rejects mismatches and nonfinite observations", "[renderer][vfx-gpu][qualification]") {
    using namespace Horo::Vfx::Tests;
    CHECK(GpuQualificationPolicyVersion == 1);
    for (const auto tolerance : GpuPropertyTolerances) {
        INFO(tolerance.name);
        CHECK(GpuPropertyMatches(1.0F, 1.0F, tolerance));
        CHECK_FALSE(GpuPropertyMatches(1.01F, 1.0F, tolerance));
        CHECK_FALSE(GpuPropertyMatches(std::numeric_limits<float>::quiet_NaN(), 1.0F, tolerance));
        CHECK_FALSE(GpuPropertyMatches(1.0F, std::numeric_limits<float>::infinity(), tolerance));
        CHECK_FALSE(GpuPropertyMatches(std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(), tolerance));
        CHECK_FALSE(GpuPropertyMatches(std::numeric_limits<float>::infinity(), 1.0F, tolerance));
        const bool allowsRelativeError = tolerance.relative > 0.0F;
        CHECK(GpuPropertyMatches(1000.1F, 1000.0F, tolerance) == allowsRelativeError);
        CHECK(GpuPropertyMatches(-1000.1F, -1000.0F, tolerance) == allowsRelativeError);
        CHECK_FALSE(GpuPropertyMatches(1000.4F, 1000.0F, tolerance));
        CHECK_FALSE(GpuPropertyMatches(-1000.4F, -1000.0F, tolerance));
        const float boundary = tolerance.absolute;
        CHECK(GpuPropertyMatches(boundary, 0.0F, tolerance));
        CHECK_FALSE(GpuPropertyMatches(std::nextafter(boundary, std::numeric_limits<float>::infinity()), 0.0F, tolerance));
    }
}

TEST_CASE("GPU initialization identity binds actual seed force metadata and shared schema", "[renderer][vfx-gpu]") {
    Fixture f;
    REQUIRE(ValidateVfxGpuSimulationPreparation(f.preparation).HasValue());
    SECTION("seed contents") {
        f.initial[0] = std::bit_cast<std::uint32_t>(1.0F);
    }
    SECTION("force contents") {
        f.forces[2] = std::bit_cast<std::uint32_t>(2.0F);
    }
    SECTION("semantic key") {
        f.preparation.descriptor.random.effectSeed ^= 1ULL << 40;
    }
    SECTION("origin coordinates") {
        f.preparation.descriptor.simulationOrigin = Math::WorldCoordinate64::FromMillimeters(1, 0, 0);
    }
    SECTION("source logical generation") {
        f.preparation.descriptor.identity.activation.generation++;
    }
    SECTION("cadence") {
        f.preparation.descriptor.readback.cadenceSteps = 2;
    }
    CHECK(ComputeVfxGpuInitializationDigest(f.preparation) != f.preparation.descriptor.initializationDigest);
    CHECK(ValidateVfxGpuSimulationPreparation(f.preparation).HasError());
}

TEST_CASE("GPU cold validation rejects forged bounds raw truncation and hidden particle data", "[renderer][vfx-gpu]") {
    Fixture f;
    SECTION("forged tiny envelope with fresh hash") {
        f.preparation.descriptor.arithmetic.maximumPositionMagnitude = 0;
    }
    SECTION("raw kind truncation") {
        f.forces[0] = 256;
    }
    SECTION("raw channel truncation") {
        f.forces[0] = 3;
        f.forces[1] = 256;
    }
    SECTION("reserved force word") {
        f.forces[11] = 1;
    }
    SECTION("invalid alive flag") {
        f.initial[16 * f.preparation.descriptor.layout.streamStrideWords] = 2;
    }
    SECTION("nonzero padding") {
        f.initial[2] = 1;
    }
    SECTION("duplicate stable identity") {
        f.initial[14 * f.preparation.descriptor.layout.streamStrideWords + 1] = 1001;
    }
    SECTION("nonfinite position") {
        f.initial[0] = 0x7f800000;
    }
    SECTION("missing force word") {
        f.preparation.forceWords = f.preparation.forceWords.first(11);
    }
    f.Seal();
    CHECK(ValidateVfxGpuSimulationPreparation(f.preparation).HasError());
}

TEST_CASE("GPU arithmetic admission rejects finite inputs whose trajectory or attenuation cannot remain finite", "[renderer][vfx-gpu]") {
    Fixture f;
    SECTION("large position dot intermediate") {
        f.initial[0] = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::max());
    }
    SECTION("large velocity integration") {
        f.initial[3 * f.preparation.descriptor.layout.streamStrideWords] = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::max());
    }
    SECTION("large rotation integration") {
        f.initial[9 * f.preparation.descriptor.layout.streamStrideWords] = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::max());
    }
    SECTION("attraction falloff intermediate") {
        f.forces[0] = 2;
        f.forces[3] = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::max());
        f.forces[8] = std::bit_cast<std::uint32_t>(1.0F);
    }
    Horo::Tests::RequireActionableError(ComputeVfxGpuArithmeticEnvelope(f.preparation), VfxGpuSimulationErrors::ArithmeticUnsafe);
}

TEST_CASE("GPU canonical state inventory and renderer slice include the immutable upload seed", "[renderer][vfx-gpu]") {
    Fixture f;
    auto &d = f.preparation.descriptor;
    d.maximumPendingSteps = 1;
    CHECK(VfxGpuStateBufferCount(d) == 3);
    d.maximumPendingSteps = 64;
    CHECK(VfxGpuStateBufferCount(d) == 66);
    d.maximumPendingSteps = 0;
    CHECK(VfxGpuStateBufferCount(d) == 0);
    d.maximumPendingSteps = 65;
    CHECK(VfxGpuStateBufferCount(d) == 0);
    d.maximumPendingSteps = 2;
    d.maximumRetainedBytes = 4 * d.layout.byteCount + 48 + 4;
    f.Seal();
    REQUIRE(ValidateVfxGpuSimulationPreparation(f.preparation).HasValue());
    --d.maximumRetainedBytes;
    f.Seal();
    CHECK(ValidateVfxGpuSimulationPreparation(f.preparation).HasError());
}

TEST_CASE("GPU portable work checks exact step lineage cancellation and opted-in cadence without native authority", "[renderer][vfx-gpu]") {
    Fixture f;
    auto work = f.MetadataWork();
    REQUIRE(ValidateVfxGpuSimulationWork(f.preparation.descriptor, work));
    SECTION("wrong tick") {
        work.step.tick = 1;
    }
    SECTION("wrong source generation") {
        work.step.sourceGeneration = 2;
    }
    SECTION("wrong target generation") {
        work.step.targetGeneration = 1;
    }
    SECTION("invalid pool slot sentinel") {
        work.step.simulation.slot = std::numeric_limits<std::uint32_t>::max();
    }
    SECTION("unsafe delta") {
        work.deltaSeconds = 1;
    }
    SECTION("stale semantic identity") {
        work.identity.emitter.generation++;
    }
    SECTION("undeclared readback") {
        work.observe = true;
    }
    SECTION("caller cancellation") {
        CancellationSource source;
        work.callerCancellation = source.Token();
        source.RequestCancellation();
    }
    SECTION("lifetime cancellation") {
        CancellationSource source;
        work.lifetimeCancellation = source.Token();
        source.RequestCancellation();
    }
    CHECK_FALSE(ValidateVfxGpuSimulationWork(f.preparation.descriptor, work));
}

TEST_CASE("GPU canonical uniform word packing preserves ABI and failed destinations", "[renderer][vfx-gpu]") {
    Fixture f;
    auto work = f.MetadataWork();
    std::array<std::uint32_t, VfxGpuDispatchWords::Count + 1> output;
    output.fill(0xdeadbeef);
    REQUIRE(PackVfxGpuDispatchParameters(f.preparation.descriptor, work, output));
    CHECK(output[VfxGpuDispatchWords::Capacity] == 2);
    CHECK(output[VfxGpuDispatchWords::StreamStride] == 4);
    CHECK(output[VfxGpuDispatchWords::Delta] == std::bit_cast<std::uint32_t>(.25F));
    CHECK(output.back() == 0xdeadbeef);
    output.fill(0xdeadbeef);
    work.step.targetGeneration = 1;
    CHECK_FALSE(PackVfxGpuDispatchParameters(f.preparation.descriptor, work, output));
    CHECK(std::ranges::all_of(output, [](auto value) {
        return value == 0xdeadbeef;
    }));
    VfxGpuDrawView view;
    view.extent = {128, 128};
    view.viewProjection.values[12] = 3;
    std::array<std::uint32_t, VfxGpuDrawWords::Count> draw{};
    REQUIRE(PackVfxGpuDrawParameters(f.preparation.descriptor, view, draw));
    CHECK(draw[12] == std::bit_cast<std::uint32_t>(3.0F));
    CHECK(draw[VfxGpuDrawWords::Layout] == 2);
}

TEST_CASE("GPU descriptor equality compares metadata independently of retained content digest", "[renderer][vfx-gpu]") {
    Fixture f;
    const auto original = f.preparation.descriptor;
    auto changed = original;
    CHECK(SameVfxGpuDescriptor(original, changed));
    SECTION("semantic identity") {
        ++changed.identity.emitter.generation;
    }
    SECTION("random identity") {
        ++changed.random.activation.slot;
    }
    SECTION("layout") {
        ++changed.layout.streamStrideWords;
    }
    SECTION("origin") {
        changed.simulationOrigin = Math::WorldCoordinate64::FromMillimeters(1, 0, 0);
    }
    SECTION("counts") {
        ++changed.forceCount;
    }
    SECTION("ledger") {
        ++changed.maximumRetainedBytes;
    }
    SECTION("revisions") {
        ++changed.policyRevision;
    }
    SECTION("arithmetic") {
        changed.arithmetic.maximumVelocityMagnitude += 1;
    }
    SECTION("observation") {
        ++changed.readback.cadenceSteps;
    }
    SECTION("digest") {
        changed.initializationDigest.bytes[0] ^= 1U;
    }
    CHECK_FALSE(SameVfxGpuDescriptor(original, changed));
}
