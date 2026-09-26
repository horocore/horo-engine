#include "Horo/Destruction/DestructibleDescriptor.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

namespace Horo::Destruction {
    namespace {
        template <typename Identity> Identity Id(const std::uint64_t value) {
            auto identity = Identity::Create(value);
            REQUIRE(identity.HasValue());
            return identity.Value();
        }

        FractureArtifactContentIdentity Content(const std::uint8_t suffix = 1) {
            std::array<std::uint8_t, 16> assetBytes{};
            assetBytes.back() = suffix;
            auto asset = FractureAssetId::Create(Assets::AssetId::FromBytes(assetBytes));
            REQUIRE(asset.HasValue());
            Sha256Digest digest{};
            digest.bytes.back() = suffix;
            auto content = FractureArtifactContentIdentity::Create(asset.Value(), Id<FractureContentRevision>(suffix), digest);
            REQUIRE(content.HasValue());
            return content.Value();
        }

        DestructionFeatureSet Features(std::initializer_list<DestructionFeature> features) {
            DestructionFeatureSet set{};
            for (const auto feature : features)
                set.bits |= std::uint32_t{1} << static_cast<std::uint32_t>(feature);
            return set;
        }

        DestructibleDescriptorData ValidData() {
            const auto profile = GetDestructionTierProfile(DestructionFeatureTier::Baseline).Value();
            return {.destructible = Id<DestructibleId>(11),
                    .content = Content(3),
                    .configurationRevision = Id<DestructionConfigurationRevision>(5),
                    .features = {.required = Features({DestructionFeature::PreCookedFracture, DestructionFeature::CookedSupport}),
                                 .optional = Features({DestructionFeature::StagedActivation})},
                    .limits = profile.limits};
        }

        DestructionHandle Handle(const std::uint64_t generation = 7) {
            return {Id<DestructionWorldId>(2), Id<DestructibleId>(11), Id<DestructionGeneration>(generation)};
        }

        DestructionArtifactFootprint ValidFootprint() {
            return {.chunkCount = 32,
                    .hierarchyDepth = 1,
                    .peakActiveChunkBodies = 16,
                    .peakEventsPerTransition = 64,
                    .requestedEventJournalEntries = 128,
                    .peakCosmeticDebrisParticles = 0,
                    .artifactBytes = 1024,
                    .peakTransitionBytes = 2048,
                    .peakResidentBytes = 4096,
                    .peakWorkItemsPerTransition = 256};
        }

        template <typename Value> void CheckError(const Result<Value> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            const auto &error = result.ErrorValue();
            CHECK((error.domain.Value() == expected.domain.Value() && error.code.Value() == expected.code.Value()));
        }
    }  // namespace

    TEST_CASE("Destruction tiers expose exact provider-neutral profiles", "[unit][destruction][descriptor]") {
        const auto baseline = GetDestructionTierProfile(DestructionFeatureTier::Baseline).Value();
        const auto standard = GetDestructionTierProfile(DestructionFeatureTier::Standard).Value();
        const auto high = GetDestructionTierProfile(DestructionFeatureTier::High).Value();

        CHECK(baseline.limits.maximumChunksPerDestructible == 64);
        CHECK(standard.limits.maximumChunksPerDestructible == 256);
        CHECK(high.limits.maximumChunksPerDestructible == DestructionHardLimits::ChunksPerDestructible);
        CHECK_FALSE(baseline.supportedFeatures.Contains(DestructionFeature::StagedActivation));
        CHECK(standard.supportedFeatures.Contains(DestructionFeature::StagedActivation));
        CHECK_FALSE(standard.supportedFeatures.Contains(DestructionFeature::HierarchicalFracture));
        CHECK(high.supportedFeatures.Contains(DestructionFeature::HierarchicalFracture));
        CHECK_FALSE(high.supportedFeatures.Contains(DestructionFeature::RuntimeGeometryGeneration));
        CheckError(GetDestructionTierProfile(DestructionFeatureTier::Count), DestructionErrors::TierInvalid);
        CheckError(GetDestructionTierProfile(static_cast<DestructionFeatureTier>(255)), DestructionErrors::TierInvalid);
    }

    TEST_CASE("Descriptor creation captures immutable data and intersects optional support", "[unit][destruction][descriptor]") {
        const auto source = ValidData();
        const auto descriptor = DestructibleDescriptor::Create(source);
        REQUIRE(descriptor.HasValue());
        CHECK(descriptor.Value().Data() == source);
        CHECK(descriptor.Value().EffectiveFeatures().Contains(DestructionFeature::PreCookedFracture));
        CHECK(descriptor.Value().EffectiveFeatures().Contains(DestructionFeature::CookedSupport));
        CHECK_FALSE(descriptor.Value().EffectiveFeatures().Contains(DestructionFeature::StagedActivation));
        static_assert(!std::is_default_constructible_v<DestructibleDescriptor>);
        static_assert(std::is_copy_constructible_v<DestructibleDescriptor>);
        static_assert(std::is_trivially_copyable_v<DestructibleDescriptor>);
        static_assert(!std::is_convertible_v<std::uint64_t, DestructionConfigurationRevision>);
    }

    TEST_CASE("Descriptor identity version and feature failures are typed", "[unit][destruction][descriptor]") {
        auto data = ValidData();
        data.contractVersion = 1;
        CheckError(DestructibleDescriptor::Create(data), DestructionErrors::DescriptorInvalid);
        data = ValidData();
        data.contractVersion = CurrentDestructibleDescriptorContractVersion + 1;
        CheckError(DestructibleDescriptor::Create(data), DestructionErrors::DescriptorInvalid);
        data = ValidData();
        data.configurationRevision = {};
        CheckError(DestructibleDescriptor::Create(data), DestructionErrors::DescriptorInvalid);
        data = ValidData();
        data.features.required = {};
        CheckError(DestructibleDescriptor::Create(data), DestructionErrors::DescriptorInvalid);
        data = ValidData();
        data.features.optional = data.features.required;
        CheckError(DestructibleDescriptor::Create(data), DestructionErrors::DescriptorInvalid);
        data = ValidData();
        data.features.required.bits |= DestructionFeatureBit<DestructionFeature::RuntimeGeometryGeneration>;
        CheckError(DestructibleDescriptor::Create(data), DestructionErrors::RuntimeGeometryUnsupported);
        data = ValidData();
        data.features.required.bits |= DestructionFeatureBit<DestructionFeature::HierarchicalFracture>;
        CheckError(DestructibleDescriptor::Create(data), DestructionErrors::FeatureUnsatisfied);
        data = ValidData();
        data.features.optional.bits |= std::uint32_t{1} << 31U;
        CheckError(DestructibleDescriptor::Create(data), DestructionErrors::DescriptorInvalid);
    }

    TEST_CASE("Descriptor health policy is finite and strictly ordered", "[unit][destruction][descriptor]") {
        auto data = ValidData();
        data.health.maximumHealth = std::numeric_limits<float>::infinity();
        CheckError(DestructibleDescriptor::Create(data), DestructionErrors::DescriptorInvalid);
        data = ValidData();
        data.health.fractureHealthThreshold = -1.0F;
        CheckError(DestructibleDescriptor::Create(data), DestructionErrors::DescriptorInvalid);
        data = ValidData();
        data.health.damagedHealthThreshold = data.health.maximumHealth;
        CheckError(DestructibleDescriptor::Create(data), DestructionErrors::DescriptorInvalid);
        data = ValidData();
        data.health.fractureHealthThreshold = data.health.damagedHealthThreshold;
        CheckError(DestructibleDescriptor::Create(data), DestructionErrors::DescriptorInvalid);
    }

    TEST_CASE("Descriptor typed policies require their declared capabilities", "[unit][destruction][descriptor]") {
        auto data = ValidData();
        data.behavior.support = DestructionSupportPolicy::CookedHierarchy;
        CheckError(DestructibleDescriptor::Create(data), DestructionErrors::DescriptorInvalid);
        data = ValidData();
        data.cleanup.chunkRetention = DestructionChunkRetention::DurableDormancy;
        CheckError(DestructibleDescriptor::Create(data), DestructionErrors::DescriptorInvalid);
        data = ValidData();
        data.cleanup.debris = DestructionDebrisPolicy::FiniteLifetime;
        data.cleanup.debrisLifetimeSeconds = 1.0F;
        CheckError(DestructibleDescriptor::Create(data), DestructionErrors::DescriptorInvalid);
        data = ValidData();
        data.replication = DestructionReplicationIntent::ServerAuthoritative;
        CheckError(DestructibleDescriptor::Create(data), DestructionErrors::DescriptorInvalid);
        data = ValidData();
        data.cleanup.debrisLifetimeSeconds = 1.0F;
        CheckError(DestructibleDescriptor::Create(data), DestructionErrors::DescriptorInvalid);
        data = ValidData();
        data.behavior.trigger = DestructionTriggerPolicy::Count;
        CheckError(DestructibleDescriptor::Create(data), DestructionErrors::DescriptorInvalid);

        data = ValidData();
        data.tier = DestructionFeatureTier::High;
        data.features.required = Features({DestructionFeature::PreCookedFracture, DestructionFeature::CookedSupport,
                                           DestructionFeature::HierarchicalFracture, DestructionFeature::CosmeticDebris,
                                           DestructionFeature::DurableDormancy, DestructionFeature::AuthoritativeReplication});
        data.features.optional = {};
        data.behavior.support = DestructionSupportPolicy::CookedHierarchy;
        data.cleanup.chunkRetention = DestructionChunkRetention::DurableDormancy;
        data.cleanup.debris = DestructionDebrisPolicy::FiniteLifetime;
        data.cleanup.debrisLifetimeSeconds = 0.25F;
        data.replication = DestructionReplicationIntent::ServerAuthoritative;
        CHECK(DestructibleDescriptor::Create(data).HasValue());
    }

    TEST_CASE("Descriptor limits are positive internally consistent and bounded by the exact tier", "[unit][destruction][descriptor]") {
        auto data = ValidData();
        data.limits.maximumChunksPerDestructible = 0;
        CheckError(DestructibleDescriptor::Create(data), DestructionErrors::LimitProfileInvalid);
        data = ValidData();
        data.limits.maximumActiveChunkBodies = data.limits.maximumChunksPerDestructible + 1;
        CheckError(DestructibleDescriptor::Create(data), DestructionErrors::LimitProfileInvalid);
        data = ValidData();
        data.limits.maximumEventsPerTransition = data.limits.maximumEventJournalEntries + 1;
        CheckError(DestructibleDescriptor::Create(data), DestructionErrors::LimitProfileInvalid);
        data = ValidData();
        data.limits.maximumArtifactBytes = data.limits.maximumTransitionBytes + 1;
        CheckError(DestructibleDescriptor::Create(data), DestructionErrors::LimitProfileInvalid);
        data = ValidData();
        data.limits.maximumChunksPerDestructible++;
        CheckError(DestructibleDescriptor::Create(data), DestructionErrors::LimitProfileInvalid);
    }

    TEST_CASE("Admission rejects stale identity configuration content and oversized input before work", "[unit][destruction][descriptor]") {
        const auto descriptor = DestructibleDescriptor::Create(ValidData()).Value();
        const auto target = Handle();
        const auto revision = descriptor.Data().configurationRevision;
        const auto content = descriptor.Data().content;
        auto footprint = ValidFootprint();

        CHECK(AdmitDestructibleDescriptor(descriptor, target, target, revision, content, footprint).HasValue());
        CheckError(AdmitDestructibleDescriptor(descriptor, Handle(6), target, revision, content, footprint),
                   DestructionErrors::StaleGeneration);
        CheckError(AdmitDestructibleDescriptor(descriptor, target, target, Id<DestructionConfigurationRevision>(6), content, footprint),
                   DestructionErrors::StaleConfiguration);
        CheckError(AdmitDestructibleDescriptor(descriptor, target, target, revision, Content(4), footprint),
                   DestructionErrors::IdentityUnknown);
        Sha256Digest newerDigest = content.SemanticDigest();
        newerDigest.bytes.back()++;
        const auto newerContent =
            FractureArtifactContentIdentity::Create(content.Asset(), Id<FractureContentRevision>(content.Revision().Value() + 1),
                                                    newerDigest);
        REQUIRE(newerContent.HasValue());
        CheckError(AdmitDestructibleDescriptor(descriptor, target, target, revision, newerContent.Value(), footprint),
                   DestructionErrors::StaleContent);
        footprint.chunkCount = descriptor.Data().limits.maximumChunksPerDestructible + 1;
        CheckError(AdmitDestructibleDescriptor(descriptor, target, target, revision, content, footprint), DestructionErrors::LimitExceeded);
        footprint = ValidFootprint();
        footprint.peakCosmeticDebrisParticles = 1;
        CheckError(AdmitDestructibleDescriptor(descriptor, target, target, revision, content, footprint),
                   DestructionErrors::DescriptorInvalid);
        footprint = ValidFootprint();
        footprint.artifactBytes = 0;
        CheckError(AdmitDestructibleDescriptor(descriptor, target, target, revision, content, footprint),
                   DestructionErrors::DescriptorInvalid);
    }
}  // namespace Horo::Destruction
