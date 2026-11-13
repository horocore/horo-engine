#include "Horo/Vfx/CpuParticleSimulator.h"
#include "Horo/Vfx/VfxQualityPolicy.h"
#include "support/VfxTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <initializer_list>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>

namespace Horo::Vfx {
    namespace {
        VfxCapabilityRevision CapabilityRevision(const std::uint64_t value = 11) {
            return VfxCapabilityRevision::Create(value).Value();
        }

        VfxQualityPolicyRevision PolicyRevision(const std::uint64_t value = 17) {
            return VfxQualityPolicyRevision::Create(value).Value();
        }

        VfxResourceLimits Limits() {
            return {.maximumParticles = 4096,
                    .maximumDecals = 256,
                    .maximumLights = 4,
                    .maximumVolumes = 2,
                    .maximumMemoryBytes = 16 * 1024 * 1024,
                    .maximumCpuWorkMilliseconds = 4.0,
                    .maximumGpuWorkMilliseconds = 3.0};
        }

        std::array<VfxCapabilityFact, VfxCapabilityCount> Facts(const bool gpu = true) {
            std::array<VfxCapabilityFact, VfxCapabilityCount> facts{};
            for (std::size_t index = 0; index < facts.size(); ++index) {
                facts[index] = {.capability = static_cast<VfxCapability>(index), .support = VfxCapabilitySupport::Available};
            }
            if (!gpu) {
                for (const auto capability : {VfxCapability::GpuSimulation, VfxCapability::IndirectDraw, VfxCapability::GpuSorting,
                                              VfxCapability::VolumeTextures, VfxCapability::VectorFields})
                    facts[static_cast<std::size_t>(capability)].support = VfxCapabilitySupport::Unsupported;
            }
            return facts;
        }

        VfxCapabilities Capabilities(const bool gpu = true, const std::uint64_t revision = 11, const VfxResourceLimits limits = Limits()) {
            const auto facts = Facts(gpu);
            return VfxCapabilities::Create(CapabilityRevision(revision), facts, limits).Value();
        }

        VfxQualityPolicy Policy(const VfxResourceLimits limits = Limits(), const std::uint64_t revision = 17,
                                const VfxQualityProfile profile = VfxQualityProfile::High, const std::uint32_t threshold = 2048) {
            return VfxQualityPolicy::Create(
                       {.revision = PolicyRevision(revision), .profile = profile, .limits = limits, .autoGpuParticleThreshold = threshold})
                .Value();
        }

        VfxCapabilityMask Required(const std::initializer_list<VfxCapability> capabilities) {
            VfxCapabilityMask required{};
            for (const auto capability : capabilities)
                required[static_cast<std::size_t>(capability)] = true;
            return required;
        }

        VfxEffectRequirements Requirements(const SimulationPreference preference = SimulationPreference::Automatic,
                                           const std::uint32_t count = 128,
                                           const VfxEffectCategory category = VfxEffectCategory::Particle) {
            return {.category = category,
                    .preference = preference,
                    .requirementClass = VfxRequirementClass::Cosmetic,
                    .requestedCount = count,
                    .minimumAuthoredCount = 1,
                    .bytesPerElement = 64,
                    .cpuWorkMilliseconds = 1.0,
                    .gpuWorkMilliseconds = 1.0,
                    .cpuMandatory = false,
                    .hasCpuKernel = true,
                    .hasGpuKernel = true,
                    .requiredGpuCapabilities = Required({VfxCapability::GpuSimulation})};
        }

        VfxResolutionRequest Request(const VfxCapabilities &capabilities, const VfxQualityPolicy &policy,
                                     const VfxEffectRequirements requirements, const std::span<const VfxFallbackVariant> fallbacks = {},
                                     const VfxHostMode mode = VfxHostMode::Interactive) {
            return {.hostMode = mode,
                    .requestedProfile = policy.Profile(),
                    .expectedCapabilityRevision = capabilities.Revision(),
                    .expectedPolicyRevision = policy.Revision(),
                    .requirements = requirements,
                    .fallbackVariants = fallbacks};
        }

        /** @brief Executes this fixture host's authored variant 73 through the genuine CPU simulator after resolution. */
        void CheckCpuSubstitute(const VfxResolution &selected) {
            REQUIRE(selected.domain == ResolvedSimulationDomain::CPU);
            REQUIRE(selected.selectedVariantId == 73);
            auto data = Tests::ValidParticleDescriptorData();
            data.simulationPreference = SimulationPreference::RequireCPU;
            data.maximumParticles = 512;
            data.shape = ParticleEmitterShape::Point;
            data.spawnRate = {0, 0};
            data.initialSpeed = {0, 0};
            data.lifetimeSeconds = {10, 10};
            data.collisionMode = ParticleCollisionMode::None;
            const auto descriptor = Tests::ParticleDescriptor(data);
            REQUIRE(selected.selectedCount == descriptor.Data().maximumParticles);
            const auto scope = VfxIdentityScope::Create(73).Value();
            const std::array forces{CpuParticleForceModule{.kind = CpuParticleForceKind::Gravity, .vector = {0, -1, 0}}};
            auto created = CpuParticleSimulator::Create(descriptor, {.buffer = {scope, 1, 1},
                                                                     .activation = {scope, 2, 1},
                                                                     .effectSeed = 73,
                                                                     .maximumBurstParticles = 512,
                                                                     .forces = forces});
            REQUIRE(created.HasValue());
            auto simulator = std::move(created).Value();
            REQUIRE(simulator.Advance({.deltaSeconds = 0, .burstCount = 512, .tick = 0}).HasValue());
            for (std::uint64_t tick = 1; tick <= 2; ++tick)
                REQUIRE(simulator.Advance({.deltaSeconds = .125F, .tick = tick}).HasValue());
            const auto extracted = simulator.Extract();
            REQUIRE(extracted.HasValue());
            const auto &view = extracted.Value();
            REQUIRE(view.positionY.size() == selected.selectedCount);
            for (std::size_t i = 0; i < view.positionY.size(); ++i) {
                CHECK(view.positionY[i] == -.046875F);
                CHECK(view.velocityY[i] == -.25F);
                CHECK(view.age[i] == .25F);
            }
            CHECK(simulator.Statistics().active == selected.selectedCount);
        }

        void CheckError(const Result<VfxResolution> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == expected.code.Value());
        }
    }  // namespace

    TEST_CASE("VFX capabilities reject incomplete contradictory and nonfinite evidence", "[unit][vfx][quality]") {
        auto facts = Facts();
        CHECK(VfxCapabilities::Create(CapabilityRevision(), std::span{facts}.first(facts.size() - 1), Limits()).HasError());

        facts[1].capability = facts[0].capability;
        CHECK(VfxCapabilities::Create(CapabilityRevision(), facts, Limits()).HasError());

        facts = Facts(false);
        facts[static_cast<std::size_t>(VfxCapability::GpuSorting)].support = VfxCapabilitySupport::Available;
        CHECK(VfxCapabilities::Create(CapabilityRevision(), facts, Limits()).HasError());

        auto invalidLimits = Limits();
        invalidLimits.maximumCpuWorkMilliseconds = std::numeric_limits<double>::infinity();
        CHECK(VfxCapabilities::Create(CapabilityRevision(), Facts(), invalidLimits).HasError());
    }

    TEST_CASE("VFX policy candidates are atomic and retain the previous valid revision on failure", "[unit][vfx][quality]") {
        const auto active = Policy();
        auto invalid = Limits();
        invalid.maximumMemoryBytes = 0;
        const auto rejected = VfxQualityPolicy::Create(
            {.revision = PolicyRevision(18), .profile = VfxQualityProfile::Ultra, .limits = invalid, .autoGpuParticleThreshold = 2048});
        REQUIRE(rejected.HasError());
        CHECK(active.Revision() == PolicyRevision(17));
        CHECK(active.Profile() == VfxQualityProfile::High);

        invalid = Limits();
        invalid.maximumGpuWorkMilliseconds = std::numeric_limits<double>::quiet_NaN();
        CHECK(VfxQualityPolicy::Create(
                  {.revision = PolicyRevision(18), .profile = VfxQualityProfile::High, .limits = invalid, .autoGpuParticleThreshold = 2048})
                  .HasError());
        CHECK(
            VfxQualityPolicy::Create(
                {.revision = PolicyRevision(18), .profile = VfxQualityProfile::Count, .limits = Limits(), .autoGpuParticleThreshold = 2048})
                .HasError());
        CHECK(VfxQualityPolicy::Create(
                  {.revision = PolicyRevision(18), .profile = VfxQualityProfile::High, .limits = Limits(), .autoGpuParticleThreshold = 0})
                  .HasError());
    }

    TEST_CASE("Simulation preference matrix follows explicit CPU GPU and Automatic order", "[unit][vfx][quality]") {
        const auto capabilities = Capabilities();
        const auto policy = Policy();
        const std::array cases{
            std::pair{SimulationPreference::RequireCPU, ResolvedSimulationDomain::CPU},
            std::pair{SimulationPreference::PreferCPU, ResolvedSimulationDomain::CPU},
            std::pair{SimulationPreference::PreferGPU, ResolvedSimulationDomain::GPU},
            std::pair{SimulationPreference::RequireGPU, ResolvedSimulationDomain::GPU},
        };
        for (const auto &[preference, expected] : cases) {
            const auto resolved = ResolveSimulationDomain(capabilities, policy, Request(capabilities, policy, Requirements(preference)));
            REQUIRE(resolved.HasValue());
            CHECK(resolved.Value().domain == expected);
            CHECK(resolved.Value().degradation == VfxDegradation::None);
        }

        CHECK(ResolveSimulationDomain(capabilities, policy, Request(capabilities, policy, Requirements({}, 2048))).Value().domain ==
              ResolvedSimulationDomain::CPU);
        CHECK(ResolveSimulationDomain(capabilities, policy, Request(capabilities, policy, Requirements({}, 2049))).Value().domain ==
              ResolvedSimulationDomain::GPU);
    }

    TEST_CASE("Every profile category and domain preference resolves from the same typed matrix", "[unit][vfx][quality]") {
        static_assert(std::is_trivially_copyable_v<VfxResolution>);
        const auto capabilities = Capabilities();
        const std::array profiles{VfxQualityProfile::Baseline, VfxQualityProfile::Standard, VfxQualityProfile::High,
                                  VfxQualityProfile::Ultra};
        const std::array categories{VfxEffectCategory::Particle, VfxEffectCategory::Decal, VfxEffectCategory::Light,
                                    VfxEffectCategory::Volumetric};
        const std::array preferences{SimulationPreference::Automatic, SimulationPreference::RequireCPU, SimulationPreference::PreferCPU,
                                     SimulationPreference::PreferGPU, SimulationPreference::RequireGPU};
        for (const auto profile : profiles) {
            const auto policy = Policy(Limits(), 17, profile);
            for (const auto category : categories) {
                for (const auto preference : preferences) {
                    const auto resolved =
                        ResolveSimulationDomain(capabilities, policy, Request(capabilities, policy, Requirements(preference, 1, category)));
                    REQUIRE(resolved.HasValue());
                    CHECK(resolved.Value().selectedProfile == profile);
                    CHECK(resolved.Value().domain ==
                          (preference == SimulationPreference::PreferGPU || preference == SimulationPreference::RequireGPU
                               ? ResolvedSimulationDomain::GPU
                               : ResolvedSimulationDomain::CPU));
                }
            }
        }
    }

    TEST_CASE("Actual capability facts override identical product profile labels", "[unit][vfx][quality]") {
        const auto gpuCapabilities = Capabilities(true);
        const auto cpuCapabilities = Capabilities(false);
        const auto policy = Policy();
        auto requirements = Requirements(SimulationPreference::PreferGPU);

        CHECK(ResolveSimulationDomain(gpuCapabilities, policy, Request(gpuCapabilities, policy, requirements)).Value().domain ==
              ResolvedSimulationDomain::GPU);
        CHECK(ResolveSimulationDomain(cpuCapabilities, policy, Request(cpuCapabilities, policy, requirements)).Value().domain ==
              ResolvedSimulationDomain::CPU);

        requirements.preference = SimulationPreference::RequireGPU;
        CheckError(ResolveSimulationDomain(cpuCapabilities, policy, Request(cpuCapabilities, policy, requirements)),
                   VfxErrors::UnsupportedCapability);
    }

    TEST_CASE("Gameplay and CPU-mandatory requirements never silently change authority", "[unit][vfx][quality]") {
        const auto capabilities = Capabilities();
        const auto policy = Policy();
        auto requirements = Requirements(SimulationPreference::RequireGPU, 3000);
        requirements.requirementClass = VfxRequirementClass::GameplayRequired;
        requirements.cpuMandatory = true;
        CheckError(ResolveSimulationDomain(capabilities, policy, Request(capabilities, policy, requirements)), VfxErrors::DomainConflict);

        requirements.preference = SimulationPreference::Automatic;
        REQUIRE(ResolveSimulationDomain(capabilities, policy, Request(capabilities, policy, requirements)).Value().domain ==
                ResolvedSimulationDomain::CPU);
        requirements.hasCpuKernel = false;
        CheckError(ResolveSimulationDomain(capabilities, policy, Request(capabilities, policy, requirements)), VfxErrors::MissingKernel);
    }

    TEST_CASE("Every product tier records the authored CPU substitute when compute is absent or unqualified",
              "[unit][vfx][quality][gpu-qualification]") {
        constexpr std::array profiles{VfxQualityProfile::Baseline, VfxQualityProfile::Standard, VfxQualityProfile::High,
                                      VfxQualityProfile::Ultra};
        constexpr std::array supportStates{VfxCapabilitySupport::Unsupported, VfxCapabilitySupport::Unknown};
        for (const auto profile : profiles) {
            for (const auto support : supportStates) {
                INFO("profile=" << static_cast<unsigned>(profile) << " compute=" << static_cast<unsigned>(support));
                auto facts = Facts(false);
                facts[static_cast<std::size_t>(VfxCapability::GpuSimulation)].support = support;
                const auto capabilities = VfxCapabilities::Create(CapabilityRevision(), facts, Limits()).Value();
                const auto policy = Policy(Limits(), 17, profile);
                auto requirements = Requirements(SimulationPreference::PreferGPU, 2048);
                requirements.hasCpuKernel = false;
                requirements.minimumAuthoredCount = 512;
                VfxFallbackVariant fallback{.stableId = 73,
                                            .category = VfxEffectCategory::Particle,
                                            .degradation = VfxDegradation::CompatibleCpu,
                                            .domain = ResolvedSimulationDomain::CPU,
                                            .selectedCount = 512,
                                            .bytesPerElement = 64,
                                            .workMilliseconds = 4.0};
                const auto resolve = [&] {
                    return ResolveSimulationDomain(capabilities, policy, Request(capabilities, policy, requirements, {&fallback, 1}));
                };
                const auto selected = resolve();
                REQUIRE(selected.HasValue());
                CHECK(selected.Value().domain == ResolvedSimulationDomain::CPU);
                CHECK(selected.Value().degradation == VfxDegradation::CompatibleCpu);
                CHECK(selected.Value().selectedVariantId == 73);
                CHECK(selected.Value().selectedCount == 512);
                CHECK(selected.Value().selectedMemoryBytes == 512 * 64);
                CHECK(selected.Value().capabilityRevision == capabilities.Revision());
                CHECK(selected.Value().policyRevision == policy.Revision());
                CheckCpuSubstitute(selected.Value());

                fallback.workMilliseconds = 4.0001;
                CheckError(resolve(), VfxErrors::MissingVariant);
                fallback.workMilliseconds = 4.0;
                requirements.preference = SimulationPreference::RequireGPU;
                CheckError(resolve(), VfxErrors::MissingVariant);
            }
        }
    }

    TEST_CASE("Every effect category accepts its exact limit and rejects one beyond", "[unit][vfx][quality]") {
        const auto capabilities = Capabilities();
        const auto policy = Policy();
        const std::array cases{
            std::pair{VfxEffectCategory::Particle, Limits().maximumParticles},
            std::pair{VfxEffectCategory::Decal, Limits().maximumDecals},
            std::pair{VfxEffectCategory::Light, Limits().maximumLights},
            std::pair{VfxEffectCategory::Volumetric, Limits().maximumVolumes},
        };
        for (const auto &[category, limit] : cases) {
            const auto exact = Requirements(SimulationPreference::RequireCPU, limit, category);
            REQUIRE(ResolveSimulationDomain(capabilities, policy, Request(capabilities, policy, exact)).HasValue());
            const auto over = Requirements(SimulationPreference::RequireCPU, limit + 1U, category);
            CheckError(ResolveSimulationDomain(capabilities, policy, Request(capabilities, policy, over)), VfxErrors::LimitExceeded);
        }
    }

    TEST_CASE("Memory and CPU GPU work limits reject exact overflow without clamping", "[unit][vfx][quality]") {
        auto limits = Limits();
        limits.maximumMemoryBytes = 128;
        limits.maximumCpuWorkMilliseconds = 1.0;
        limits.maximumGpuWorkMilliseconds = 1.0;
        const auto capabilities = Capabilities(true, 11, limits);
        const auto policy = Policy(limits);

        auto requirements = Requirements(SimulationPreference::RequireCPU, 2);
        requirements.bytesPerElement = 64;
        requirements.cpuWorkMilliseconds = 1.0;
        REQUIRE(ResolveSimulationDomain(capabilities, policy, Request(capabilities, policy, requirements)).HasValue());
        requirements.bytesPerElement = 65;
        CheckError(ResolveSimulationDomain(capabilities, policy, Request(capabilities, policy, requirements)), VfxErrors::LimitExceeded);
        requirements.bytesPerElement = 64;
        requirements.cpuWorkMilliseconds = 1.0001;
        CheckError(ResolveSimulationDomain(capabilities, policy, Request(capabilities, policy, requirements)), VfxErrors::LimitExceeded);

        requirements = Requirements(SimulationPreference::RequireGPU, 2);
        requirements.bytesPerElement = std::numeric_limits<std::uint64_t>::max();
        CheckError(ResolveSimulationDomain(capabilities, policy, Request(capabilities, policy, requirements)), VfxErrors::LimitExceeded);
    }

    TEST_CASE("Authored degradations use fixed kind and stable identity ordering", "[unit][vfx][quality]") {
        auto limits = Limits();
        limits.maximumParticles = 100;
        const auto capabilities = Capabilities(true, 11, limits);
        const auto policy = Policy(limits, 17, VfxQualityProfile::Baseline, 50);
        const auto requirements = Requirements(SimulationPreference::RequireGPU, 101);
        const std::array variants{
            VfxFallbackVariant{.stableId = 9,
                               .category = VfxEffectCategory::Particle,
                               .degradation = VfxDegradation::ReducedCount,
                               .domain = ResolvedSimulationDomain::GPU,
                               .selectedCount = 100,
                               .bytesPerElement = 64,
                               .workMilliseconds = 1.0},
            VfxFallbackVariant{.stableId = 3,
                               .category = VfxEffectCategory::Particle,
                               .degradation = VfxDegradation::ReducedCount,
                               .domain = ResolvedSimulationDomain::GPU,
                               .selectedCount = 80,
                               .bytesPerElement = 64,
                               .workMilliseconds = 1.0},
            VfxFallbackVariant{.stableId = 1,
                               .category = VfxEffectCategory::Particle,
                               .degradation = VfxDegradation::SimpleCookedVariant,
                               .domain = ResolvedSimulationDomain::GPU,
                               .selectedCount = 70,
                               .bytesPerElement = 32,
                               .workMilliseconds = 0.5},
        };
        const auto resolved = ResolveSimulationDomain(capabilities, policy, Request(capabilities, policy, requirements, variants));
        REQUIRE(resolved.HasValue());
        CHECK(resolved.Value().degradation == VfxDegradation::ReducedCount);
        CHECK(resolved.Value().selectedVariantId == 3);
        CHECK(resolved.Value().selectedCount == 80);

        auto boundedRequirements = requirements;
        boundedRequirements.minimumAuthoredCount = 90;
        CheckError(ResolveSimulationDomain(capabilities, policy, Request(capabilities, policy, boundedRequirements, variants)),
                   VfxErrors::RequirementInvalid);
    }

    TEST_CASE("Particle decal light and volumetric effects select explicitly authored variant paths", "[unit][vfx][quality]") {
        auto limits = Limits();
        limits.maximumParticles = limits.maximumDecals = limits.maximumLights = limits.maximumVolumes = 1;
        const auto capabilities = Capabilities(true, 11, limits);
        const auto policy = Policy(limits, 17, VfxQualityProfile::Baseline, 1);
        const std::array categories{VfxEffectCategory::Particle, VfxEffectCategory::Decal, VfxEffectCategory::Light,
                                    VfxEffectCategory::Volumetric};
        const std::array kinds{VfxDegradation::ReducedCount, VfxDegradation::SimpleCookedVariant, VfxDegradation::CompatibleCpu,
                               VfxDegradation::Substitute};
        const auto headlessCapabilities = Capabilities(false, 11, limits);
        for (const auto category : categories) {
            for (const auto kind : kinds) {
                auto requirements = Requirements(SimulationPreference::PreferGPU, 2, category);
                const VfxFallbackVariant fallback{.stableId = static_cast<std::uint32_t>(kind),
                                                  .category = category,
                                                  .degradation = kind,
                                                  .domain = kind == VfxDegradation::CompatibleCpu ? ResolvedSimulationDomain::CPU
                                                                                                  : ResolvedSimulationDomain::GPU,
                                                  .selectedCount = 1,
                                                  .bytesPerElement = 32,
                                                  .workMilliseconds = 0.5};
                const auto resolved =
                    ResolveSimulationDomain(capabilities, policy, Request(capabilities, policy, requirements, {&fallback, 1}));
                REQUIRE(resolved.HasValue());
                CHECK(resolved.Value().degradation == kind);
            }

            const auto requirements = Requirements(SimulationPreference::PreferGPU, 2, category);
            const VfxFallbackVariant nullFallback{.stableId = 9,
                                                  .category = category,
                                                  .degradation = VfxDegradation::NullSuppression,
                                                  .domain = ResolvedSimulationDomain::Null};
            const auto suppressed =
                ResolveSimulationDomain(headlessCapabilities, policy,
                                        Request(headlessCapabilities, policy, requirements, {&nullFallback, 1}, VfxHostMode::Headless));
            REQUIRE(suppressed.HasValue());
            CHECK(suppressed.Value().degradation == VfxDegradation::NullSuppression);
        }
    }

    TEST_CASE("Headless gameplay uses real CPU while cosmetic Null requires explicit authorization", "[unit][vfx][quality]") {
        const auto capabilities = Capabilities(false);
        const auto policy = Policy();
        auto gameplay = Requirements(SimulationPreference::PreferGPU, 64);
        gameplay.requirementClass = VfxRequirementClass::GameplayRequired;
        gameplay.cpuMandatory = true;
        CHECK(ResolveSimulationDomain(capabilities, policy, Request(capabilities, policy, gameplay, {}, VfxHostMode::Headless))
                  .Value()
                  .domain == ResolvedSimulationDomain::CPU);

        auto cosmetic = Requirements(SimulationPreference::PreferGPU, 64);
        cosmetic.hasCpuKernel = false;
        CheckError(ResolveSimulationDomain(capabilities, policy, Request(capabilities, policy, cosmetic, {}, VfxHostMode::Headless)),
                   VfxErrors::MissingKernel);
        const VfxFallbackVariant nullFallback{.stableId = 4,
                                              .category = VfxEffectCategory::Particle,
                                              .degradation = VfxDegradation::NullSuppression,
                                              .domain = ResolvedSimulationDomain::Null};
        const auto suppressed = ResolveSimulationDomain(capabilities, policy,
                                                        Request(capabilities, policy, cosmetic, {&nullFallback, 1}, VfxHostMode::Headless));
        REQUIRE(suppressed.HasValue());
        CHECK(suppressed.Value().domain == ResolvedSimulationDomain::Null);
        CHECK(suppressed.Value().degradation == VfxDegradation::NullSuppression);

        auto requireGpu = cosmetic;
        requireGpu.preference = SimulationPreference::RequireGPU;
        CheckError(ResolveSimulationDomain(capabilities, policy,
                                           Request(capabilities, policy, requireGpu, {&nullFallback, 1}, VfxHostMode::Headless)),
                   VfxErrors::MissingVariant);

        gameplay.hasCpuKernel = false;
        CheckError(ResolveSimulationDomain(capabilities, policy,
                                           Request(capabilities, policy, gameplay, {&nullFallback, 1}, VfxHostMode::Headless)),
                   VfxErrors::MissingVariant);
    }

    TEST_CASE("Missing capabilities and invalid authored variants fail closed", "[unit][vfx][quality]") {
        const auto capabilities = Capabilities(false);
        const auto policy = Policy();
        auto requirements = Requirements(SimulationPreference::RequireGPU);
        requirements.hasGpuKernel = false;
        CheckError(ResolveSimulationDomain(capabilities, policy, Request(capabilities, policy, requirements)), VfxErrors::MissingKernel);

        const std::array duplicates{
            VfxFallbackVariant{.stableId = 1,
                               .category = VfxEffectCategory::Particle,
                               .degradation = VfxDegradation::CompatibleCpu,
                               .domain = ResolvedSimulationDomain::CPU,
                               .selectedCount = 64,
                               .bytesPerElement = 32,
                               .workMilliseconds = 0.5},
            VfxFallbackVariant{.stableId = 1,
                               .category = VfxEffectCategory::Particle,
                               .degradation = VfxDegradation::Substitute,
                               .domain = ResolvedSimulationDomain::CPU,
                               .selectedCount = 64,
                               .bytesPerElement = 32,
                               .workMilliseconds = 0.5},
        };
        CheckError(ResolveSimulationDomain(capabilities, policy, Request(capabilities, policy, requirements, duplicates)),
                   VfxErrors::RequirementInvalid);

        auto missingVolumeFacts = Facts();
        missingVolumeFacts[static_cast<std::size_t>(VfxCapability::VolumeTextures)].support = VfxCapabilitySupport::Unsupported;
        const auto missingVolume = VfxCapabilities::Create(CapabilityRevision(), missingVolumeFacts, Limits()).Value();
        requirements = Requirements(SimulationPreference::RequireGPU, 1, VfxEffectCategory::Volumetric);
        requirements.requiredGpuCapabilities = Required({VfxCapability::GpuSimulation, VfxCapability::VolumeTextures});
        CheckError(ResolveSimulationDomain(missingVolume, policy, Request(missingVolume, policy, requirements)),
                   VfxErrors::UnsupportedCapability);
    }

    TEST_CASE("Resolution rejects stale preparation and remains deterministic without mutation", "[unit][vfx][quality]") {
        const auto capabilities = Capabilities();
        const auto policy = Policy();
        auto request = Request(capabilities, policy, Requirements(SimulationPreference::PreferGPU));
        request.expectedCapabilityRevision = CapabilityRevision(12);
        CheckError(ResolveSimulationDomain(capabilities, policy, request), VfxErrors::CapabilityRevisionStale);
        request.expectedCapabilityRevision = capabilities.Revision();
        request.expectedPolicyRevision = PolicyRevision(18);
        CheckError(ResolveSimulationDomain(capabilities, policy, request), VfxErrors::QualityPolicyRevisionStale);

        request.expectedPolicyRevision = policy.Revision();
        request.requestedProfile = VfxQualityProfile::Baseline;
        CheckError(ResolveSimulationDomain(capabilities, policy, request), VfxErrors::QualityPolicyInvalid);
        request.requestedProfile = policy.Profile();
        const auto first = ResolveSimulationDomain(capabilities, policy, request).Value();
        for (std::size_t iteration = 0; iteration < 1024; ++iteration)
            CHECK(ResolveSimulationDomain(capabilities, policy, request).Value() == first);
        CHECK(ValidateVfxResolutionFreshness(first, capabilities.Revision(), policy.Revision()).HasValue());
        CHECK(ValidateVfxResolutionFreshness(first, CapabilityRevision(12), policy.Revision()).ErrorValue().code.Value() ==
              VfxErrors::CapabilityRevisionStale.code.Value());
        CHECK(ValidateVfxResolutionFreshness(first, capabilities.Revision(), PolicyRevision(18)).ErrorValue().code.Value() ==
              VfxErrors::QualityPolicyRevisionStale.code.Value());
    }
}  // namespace Horo::Vfx
