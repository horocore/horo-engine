#include "Horo/Vfx/CpuParticleSpawnPipeline.h"
#include "Horo/Vfx/ParticleSystemDescriptor.h"
#include "Horo/Vfx/VfxQualityPolicy.h"
#include "support/AllocationProbe.h"
#include "support/VfxTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <utility>

namespace Horo::Vfx {
    namespace {
        [[nodiscard]] std::string ValidParticleSource() {
            return R"json({
                "schemaVersion":{"major":1,"minor":0},
                "emitterId":{"scope":42,"slot":7,"generation":3},
                "simulationPreference":"preferGpu",
                "maximumParticles":4096,
                "shape":"sphere",
                "spawnRate":{"minimum":24.0,"maximum":24.0},
                "lifetime":{"kind":"finite","seconds":{"minimum":0.5,"maximum":0.5},"killCondition":"lifetime"},
                "initialSpeed":{"minimum":1.0,"maximum":1.0},
                "initialSize":{"minimum":0.25,"maximum":0.25},
                "initialOpacity":{"minimum":0.75,"maximum":0.75},
                "materialId":"00112233-4455-6677-8899-aabbccddeeff",
                "renderMode":"billboard",
                "sortMode":"byDistance",
                "collisionMode":"none"
            })json";
        }

        [[nodiscard]] VfxResourceLimits ResourceLimits() {
            return {.maximumParticles = 65'536,
                    .maximumDecals = 256,
                    .maximumLights = 8,
                    .maximumVolumes = 4,
                    .maximumMemoryBytes = 64U * 1024U * 1024U,
                    .maximumCpuWorkMilliseconds = 4.0,
                    .maximumGpuWorkMilliseconds = 4.0};
        }

        [[nodiscard]] VfxCapabilities Capabilities(const bool gpu, const std::uint64_t revision = 11) {
            std::array<VfxCapabilityFact, VfxCapabilityCount> facts{};
            for (std::size_t index = 0; index < facts.size(); ++index)
                facts[index] = {.capability = static_cast<VfxCapability>(index), .support = VfxCapabilitySupport::Available};

            if (!gpu) {
                for (const auto capability : {VfxCapability::GpuSimulation, VfxCapability::IndirectDraw, VfxCapability::GpuSorting,
                                              VfxCapability::VolumeTextures, VfxCapability::VectorFields})
                    facts[static_cast<std::size_t>(capability)].support = VfxCapabilitySupport::Unsupported;
            }

            return VfxCapabilities::Create(VfxCapabilityRevision::Create(revision).Value(), facts, ResourceLimits()).Value();
        }

        [[nodiscard]] VfxQualityPolicy Policy(const VfxQualityProfile profile, const std::uint64_t revision = 17) {
            return VfxQualityPolicy::Create({.revision = VfxQualityPolicyRevision::Create(revision).Value(),
                                             .profile = profile,
                                             .limits = ResourceLimits(),
                                             .autoGpuParticleThreshold = 2'048})
                .Value();
        }

        [[nodiscard]] VfxEffectRequirements Requirements(const ParticleSystemDescriptor &descriptor) {
            return {.category = VfxEffectCategory::Particle,
                    .preference = descriptor.Data().simulationPreference,
                    .requirementClass = VfxRequirementClass::Cosmetic,
                    .requestedCount = descriptor.Data().maximumParticles,
                    .minimumAuthoredCount = 1,
                    .bytesPerElement = 64,
                    .cpuWorkMilliseconds = 1.0,
                    .gpuWorkMilliseconds = 1.0,
                    .cpuMandatory = false,
                    .hasCpuKernel = true,
                    .hasGpuKernel = true,
                    .requiredGpuCapabilities = [] {
                VfxCapabilityMask required{};
                required[static_cast<std::size_t>(VfxCapability::GpuSimulation)] = true;
                return required;
            }()};
        }

        [[nodiscard]] VfxResolution ResolveForHeadless(const ParticleSystemDescriptor &descriptor, const VfxCapabilities &capabilities,
                                                       const VfxQualityPolicy &policy) {
            const auto requirements = Requirements(descriptor);
            const VfxResolutionRequest request{.hostMode = VfxHostMode::Headless,
                                               .requestedProfile = policy.Profile(),
                                               .expectedCapabilityRevision = capabilities.Revision(),
                                               .expectedPolicyRevision = policy.Revision(),
                                               .requirements = requirements};
            return ResolveSimulationDomain(capabilities, policy, request).Value();
        }

        [[nodiscard]] ParticleBufferId Buffer(const std::uint32_t slot) {
            const auto scope = VfxIdentityScope::Create(42).Value();
            return MakeVfxIdentity<ParticleBufferIdentityTag>(scope, slot, 1).Value();
        }

        [[nodiscard]] EffectSystemId Activation() {
            const auto scope = VfxIdentityScope::Create(42).Value();
            return MakeVfxIdentity<EffectSystemIdentityTag>(scope, 10, 2).Value();
        }
    }  // namespace

    TEST_CASE("VFX foundation composes an authored asset through headless CPU simulation",
              "[unit][vfx][qualification][headless][end-to-end]") {
        const auto parsed = ParseParticleSystemDescriptor(ValidParticleSource());
        REQUIRE(parsed.HasValue());

        const auto imported =
            ValidateParticleSystemDescriptor(std::move(parsed).Value(), Tests::ParticleErrorRegistry(), "assets/effects/火球.particle");
        REQUIRE(imported.HasValue());
        REQUIRE(imported.Value().Accepted());
        REQUIRE(imported.Value().descriptor.has_value());

        const auto cookProfile = GetParticleCookProfile(ParticleCookTier::Standard);
        REQUIRE(cookProfile.HasValue());
        const ParticleMaterialEvidence material{Tests::ParticleMaterial(), ParticleMaterialAvailability::Available};
        const auto cooked = BuildParticleSystemCookPlan(*imported.Value().descriptor, cookProfile.Value(), material,
                                                        Tests::ParticleErrorRegistry(), "assets/effects/火球.particle");
        REQUIRE(cooked.HasValue());
        REQUIRE(cooked.Value().Accepted());

        const auto capabilities = Capabilities(false);
        const auto policy = Policy(VfxQualityProfile::Standard);
        const auto resolution = ResolveForHeadless(*imported.Value().descriptor, capabilities, policy);
        REQUIRE(resolution.domain == ResolvedSimulationDomain::CPU);
        REQUIRE(resolution.degradation == VfxDegradation::None);
        REQUIRE(resolution.selectedCount == imported.Value().descriptor->Data().maximumParticles);
        REQUIRE(ValidateVfxResolutionFreshness(resolution, capabilities.Revision(), policy.Revision()).HasValue());

        auto pipeline = CpuParticleSpawnPipeline::Create(*imported.Value().descriptor,
                                                         {.buffer = Buffer(10),
                                                          .activation = Activation(),
                                                          .effectSeed = 0xA55A'1234ULL,
                                                          .maximumBurstParticles = 128,
                                                          .maximumBufferBytes = capabilities.Limits().maximumMemoryBytes});
        REQUIRE(pipeline.HasValue());

        auto simulation = std::move(pipeline).Value();
        const auto before = ::Horo::Tests::AllocationProbe::Count();
        const auto step = simulation.Advance({.deltaSeconds = 1.0F / 60.0F, .burstCount = 32});
        const auto after = ::Horo::Tests::AllocationProbe::Count();
        REQUIRE(step.HasValue());
        CHECK(step.Value().spawned >= 32);
        CHECK(step.Value().active == step.Value().spawned);
        CHECK(after == before);
        CHECK(simulation.View().Value().positionX.size() == step.Value().active);
        CHECK(simulation.Statistics().nextSpawnOrdinal == step.Value().spawned);
    }

    TEST_CASE("VFX foundation rejects hostile import and cook evidence with typed findings", "[unit][vfx][qualification][hostile]") {
        const auto duplicate = ParseParticleSystemDescriptor(R"({"schemaVersion":{},"schemaVersion":{}})");
        REQUIRE(duplicate.HasError());
        CHECK(duplicate.ErrorValue().code.Value() == VfxErrors::ParticleDescriptorDuplicate.code.Value());

        std::string oversized = ValidParticleSource();
        oversized.append(ParticleDescriptorHardLimits::SourceBytes, 'x');
        const auto oversizedResult = ParseParticleSystemDescriptor(oversized);
        REQUIRE(oversizedResult.HasError());
        CHECK(oversizedResult.ErrorValue().code.Value() == VfxErrors::ParticleDescriptorLimitExceeded.code.Value());

        auto invalid = Tests::ValidParticleDescriptorData();
        invalid.maximumParticles = 0;
        invalid.lifetimeKind = ParticleLifetimeKind::Infinite;
        invalid.lifetimeSeconds = {1.0, 2.0};
        invalid.killCondition = ParticleKillCondition::None;
        const auto import =
            ValidateParticleSystemDescriptor(std::move(invalid), Tests::ParticleErrorRegistry(), "assets/effects/../hostile.particle");
        REQUIRE(import.HasValue());
        REQUIRE_FALSE(import.Value().Accepted());
        CHECK(import.Value().descriptor == std::nullopt);
        CHECK(import.Value().diagnostics.Count(DiagnosticSeverity::Error) == 3);
        CHECK(import.Value().diagnostics.Diagnostics().front().location.source == "assets/effects/../hostile.particle");

        const auto valid = Tests::ParticleDescriptor(Tests::ValidParticleDescriptorData());
        const auto profile = GetParticleCookProfile(ParticleCookTier::Compact).Value();
        const auto missing = BuildParticleSystemCookPlan(valid, profile, {Tests::ParticleMaterial(), ParticleMaterialAvailability::Missing},
                                                         Tests::ParticleErrorRegistry(), "assets/effects/missing-material.particle");
        REQUIRE(missing.HasValue());
        REQUIRE_FALSE(missing.Value().Accepted());
        CHECK(missing.Value().diagnostics.Count(DiagnosticSeverity::Error) == 1);
        CHECK(missing.Value().diagnostics.Diagnostics().front().code.Value() == VfxErrors::ParticleMaterialMissing.code.Value());
    }

    TEST_CASE("VFX foundation keeps cook tiers and quality profiles explicit across the supported matrix",
              "[unit][vfx][qualification][tiers]") {
        const std::array tiers{ParticleCookTier::Compact, ParticleCookTier::Standard, ParticleCookTier::Large};
        for (const auto tier : tiers) {
            const auto profile = GetParticleCookProfile(tier);
            REQUIRE(profile.HasValue());
            auto data = Tests::ValidParticleDescriptorData();
            data.maximumParticles = profile.Value().maximumParticles;
            data.spawnRate = {profile.Value().maximumSpawnRate, profile.Value().maximumSpawnRate};
            const auto descriptor = Tests::ParticleDescriptor(std::move(data));
            const auto cooked = BuildParticleSystemCookPlan(descriptor, profile.Value(),
                                                            {Tests::ParticleMaterial(), ParticleMaterialAvailability::Available},
                                                            Tests::ParticleErrorRegistry());
            REQUIRE(cooked.HasValue());
            REQUIRE(cooked.Value().Accepted());
            CHECK(cooked.Value().plan->maximumParticles == profile.Value().maximumParticles);
            CHECK(cooked.Value().plan->maximumSpawnRate == profile.Value().maximumSpawnRate);
        }

        const std::array qualityProfiles{VfxQualityProfile::Baseline, VfxQualityProfile::Standard, VfxQualityProfile::High,
                                         VfxQualityProfile::Ultra};
        for (const auto profile : qualityProfiles) {
            const auto policy = Policy(profile);
            const auto descriptor = Tests::ParticleDescriptor(Tests::ValidParticleDescriptorData());
            const auto cpuResolution = ResolveForHeadless(descriptor, Capabilities(false), policy);
            REQUIRE(cpuResolution.domain == ResolvedSimulationDomain::CPU);
            CHECK(cpuResolution.selectedProfile == profile);

            const auto interactive = Capabilities(true, 12);
            const auto gpuResolution = ResolveSimulationDomain(interactive, policy,
                                                               {.hostMode = VfxHostMode::Interactive,
                                                                .requestedProfile = profile,
                                                                .expectedCapabilityRevision = interactive.Revision(),
                                                                .expectedPolicyRevision = policy.Revision(),
                                                                .requirements = Requirements(descriptor)});
            REQUIRE(gpuResolution.HasValue());
            CHECK(gpuResolution.Value().domain == ResolvedSimulationDomain::GPU);
            CHECK(gpuResolution.Value().selectedProfile == profile);
        }
    }
}  // namespace Horo::Vfx
