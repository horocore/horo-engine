#include "Horo/Foundation/ModuleDescriptor.h"
#include "Horo/Runtime/Render/VfxGpuKernel.h"
#include "Horo/Vfx/ParticleSystemDescriptor.h"
#include "Horo/Vfx/VfxErrors.h"
#include "support/VfxTestSupport.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Vfx {
    static_assert(!std::is_default_constructible_v<ParticleCookAdmission>);
    static_assert(!std::is_constructible_v<ParticleCookAdmission, ParticleSystemDescriptor, ParticleCookPlan>);
    static_assert(std::is_same_v<decltype(std::declval<const ParticleCookAdmission &>().Descriptor()), const ParticleSystemDescriptor &>);
    static_assert(std::is_same_v<decltype(std::declval<const ParticleCookAdmission &>().Plan()), const ParticleCookPlan &>);

    namespace {
        [[nodiscard]] Assets::AssetId Material(const std::string_view text = "00112233-4455-6677-8899-aabbccddeeff") {
            return Tests::ParticleMaterial(text);
        }

        [[nodiscard]] EmitterId Emitter() {
            return Tests::ParticleEmitter();
        }

        [[nodiscard]] ParticleSystemDescriptorData ValidData() {
            return Tests::ValidParticleDescriptorData();
        }

        [[nodiscard]] ErrorCodeRegistry Registry() {
            return Tests::ParticleErrorRegistry();
        }

        [[nodiscard]] std::string ValidJson() {
            return R"json({
                "schemaVersion":{"major":1,"minor":0},
                "emitterId":{"scope":42,"slot":7,"generation":3},
                "simulationPreference":"preferGpu",
                "maximumParticles":4096,
                "shape":"cone",
                "spawnRate":{"minimum":100.0,"maximum":500.0},
                "lifetime":{"kind":"finite","seconds":{"minimum":0.5,"maximum":4.0},"killCondition":"lifetime"},
                "initialSpeed":{"minimum":-2.0,"maximum":12.0},
                "initialSize":{"minimum":0.1,"maximum":2.0},
                "initialOpacity":{"minimum":0.25,"maximum":1.0},
                "materialId":"00112233-4455-6677-8899-aabbccddeeff",
                "renderMode":"billboard",
                "sortMode":"byDistance",
                "collisionMode":"sceneDepth"
            })json";
        }

        /** @brief Produces one exact authored opt-in, without provider discovery or runtime policy overrides. */
        Render::VfxGpuReadbackDescriptor ReadbackPolicy() {
            return {.enabled = true,
                    .purpose = Render::VfxGpuReadbackPurpose::CosmeticObservation,
                    .requirement = Render::VfxGpuReadbackRequirement::RequiredForVisualEffect,
                    .cadenceSteps = 3,
                    .maximumRecords = 16,
                    .maximumBytesPerResult = 16 * Render::VfxGpuObservationWords::Bytes,
                    .maximumPendingResults = 2};
        }

        /** @brief Builds a closed source object with the same exact public policy vocabulary as the production codec. */
        std::string ReadbackObject(std::string_view purpose = "cosmeticObservation",
                                   std::string_view requirement = "requiredForVisualEffect") {
            return std::format(R"({{"enabled":true,"schemaVersion":1,"purpose":"{}","requirement":"{}","cadenceSteps":3,
                "maximumRecords":16,"maximumBytesPerResult":{},"maximumPendingResults":2}})",
                               purpose, requirement, ReadbackPolicy().maximumBytesPerResult);
        }

        /** @brief Adds only the optional readback field to an otherwise unchanged legacy source. */
        std::string WithReadback(const std::string &object) {
            auto source = ValidJson();
            source.insert(source.rfind('}'), ",\"gpuReadback\":" + object);
            return source;
        }

        /** @brief Changes exactly one known fixture token so malformed-input cases retain every unrelated source field. */
        void ReplaceReadback(std::string &source, std::string_view before, std::string_view after) {
            const auto position = source.find(before);
            REQUIRE(position != std::string::npos);
            source.replace(position, before.size(), after);
        }

        void RequireError(const Error &error, const ErrorCodeDescriptor &descriptor) {
            REQUIRE(error.domain.Value() == descriptor.domain.Value());
            REQUIRE(error.code.Value() == descriptor.code.Value());
        }
    }  // namespace

    TEST_CASE("Particle descriptors decode collision response while preserving legacy bounce", "[unit][vfx][particle-descriptor]") {
        CHECK(ParseParticleSystemDescriptor(ValidJson()).Value().collisionResponse == ParticleCollisionResponse::Bounce);
        auto source = ValidJson();
        source.insert(source.rfind('}'), ",\"collisionResponse\":\"die\"");
        const auto parsed = ParseParticleSystemDescriptor(source);
        REQUIRE(parsed.HasValue());
        CHECK(parsed.Value().collisionResponse == ParticleCollisionResponse::Die);
        REQUIRE(ValidateParticleSystemDescriptor(parsed.Value(), Registry()).Value().Accepted());
        auto invalid = ValidData();
        invalid.collisionResponse = ParticleCollisionResponse::Count;
        CHECK_FALSE(ValidateParticleSystemDescriptor(invalid, Registry()).Value().Accepted());
        source.replace(source.find("\"die\""), 5, "\"unknown\"");
        const auto unknown = ParseParticleSystemDescriptor(source);
        REQUIRE(unknown.HasValue());
        CHECK_FALSE(ValidateParticleSystemDescriptor(unknown.Value(), Registry()).Value().Accepted());
    }

    TEST_CASE("Particle descriptor parses and admits canonical source", "[unit][vfx][particle-descriptor]") {
        auto parsed = ParseParticleSystemDescriptor(ValidJson());
        REQUIRE(parsed.HasValue());
        REQUIRE(parsed.Value() == ValidData());

        auto validation = ValidateParticleSystemDescriptor(std::move(parsed).Value(), Registry(), "assets/fire.particle");
        REQUIRE(validation.HasValue());
        REQUIRE(validation.Value().Accepted());
        REQUIRE(validation.Value().diagnostics.Empty());
        REQUIRE(validation.Value().descriptor->Data() == ValidData());
    }

    TEST_CASE("Particle descriptor parser rejects duplicate malformed versioned and oversized input", "[unit][vfx][particle-descriptor]") {
        auto duplicate = ParseParticleSystemDescriptor(R"({"schemaVersion":{},"schemaVersion":{}})");
        REQUIRE(duplicate.HasError());
        RequireError(duplicate.ErrorValue(), VfxErrors::ParticleDescriptorDuplicate);

        auto malformed = ParseParticleSystemDescriptor("{");
        REQUIRE(malformed.HasError());
        RequireError(malformed.ErrorValue(), VfxErrors::ParticleDescriptorMalformed);

        std::string invalidEmitter = ValidJson();
        invalidEmitter.replace(invalidEmitter.find("\"slot\":7"), std::string{"\"slot\":7"}.size(), "\"slot\":\"seven\"");
        auto emitterFailure = ParseParticleSystemDescriptor(invalidEmitter);
        REQUIRE(emitterFailure.HasError());
        REQUIRE(emitterFailure.ErrorValue().message == "emitterId values are outside their encoded ranges.");

        std::string newer = ValidJson();
        newer.replace(newer.find("\"major\":1"), std::string{"\"major\":1"}.size(), "\"major\":2");
        auto unsupported = ParseParticleSystemDescriptor(newer);
        REQUIRE(unsupported.HasError());
        RequireError(unsupported.ErrorValue(), VfxErrors::ParticleDescriptorVersionUnsupported);

        ParticleDescriptorLimits limits;
        limits.maximumSourceBytes = 32;
        auto oversized = ParseParticleSystemDescriptor(ValidJson(), limits);
        REQUIRE(oversized.HasError());
        RequireError(oversized.ErrorValue(), VfxErrors::ParticleDescriptorLimitExceeded);

        std::string deeplyNested(ParticleDescriptorHardLimits::JsonDepth + 1, '[');
        deeplyNested.append(ParticleDescriptorHardLimits::JsonDepth + 1, ']');
        auto tooDeep = ParseParticleSystemDescriptor(deeplyNested);
        REQUIRE(tooDeep.HasError());
        RequireError(tooDeep.ErrorValue(), VfxErrors::ParticleDescriptorLimitExceeded);
    }

    TEST_CASE("Particle descriptor schema rejects unknown fields and unsupported minor revisions",
              "[unit][vfx][particle-descriptor][qualification]") {
        std::string unknownRootField = ValidJson();
        const auto rootEnd = unknownRootField.rfind('}');
        REQUIRE(rootEnd != std::string::npos);
        unknownRootField.insert(rootEnd, ",\"unknownField\":true");
        const auto unknownRoot = ParseParticleSystemDescriptor(unknownRootField);
        REQUIRE(unknownRoot.HasError());
        RequireError(unknownRoot.ErrorValue(), VfxErrors::ParticleDescriptorMalformed);

        std::string unknownVersionField = ValidJson();
        const std::string version = "\"schemaVersion\":{\"major\":1,\"minor\":0}";
        const auto versionStart = unknownVersionField.find(version);
        REQUIRE(versionStart != std::string::npos);
        unknownVersionField.replace(versionStart, version.size(), "\"schemaVersion\":{\"major\":1,\"minor\":0,\"patch\":0}");
        const auto unknownVersion = ParseParticleSystemDescriptor(unknownVersionField);
        REQUIRE(unknownVersion.HasError());
        RequireError(unknownVersion.ErrorValue(), VfxErrors::ParticleDescriptorMalformed);

        std::string newerMinor = ValidJson();
        const auto minorStart = newerMinor.find("\"minor\":0");
        REQUIRE(minorStart != std::string::npos);
        newerMinor.replace(minorStart, std::string{"\"minor\":0"}.size(), "\"minor\":1");
        const auto unsupportedMinor = ParseParticleSystemDescriptor(newerMinor);
        REQUIRE(unsupportedMinor.HasError());
        RequireError(unsupportedMinor.ErrorValue(), VfxErrors::ParticleDescriptorVersionUnsupported);
    }

    TEST_CASE("Particle descriptor reports every independent semantic finding deterministically", "[unit][vfx][particle-descriptor]") {
        auto candidate = ValidData();
        candidate.maximumParticles = 0;
        candidate.spawnRate = {std::numeric_limits<double>::quiet_NaN(), -1.0};
        candidate.initialSize = {0.0, -1.0};
        candidate.initialOpacity = {-1.0, 2.0};
        candidate.lifetimeKind = ParticleLifetimeKind::Infinite;
        candidate.lifetimeSeconds = {1.0, 2.0};
        candidate.killCondition = ParticleKillCondition::None;
        candidate.renderMode = ParticleRenderMode::Ribbon;
        candidate.sortMode = ParticleSortMode::OldestFirst;

        auto validation = ValidateParticleSystemDescriptor(std::move(candidate), Registry(), "assets/hostile.particle");
        REQUIRE(validation.HasValue());
        REQUIRE_FALSE(validation.Value().Accepted());
        REQUIRE_FALSE(validation.Value().descriptor.has_value());
        REQUIRE(validation.Value().diagnostics.Size() == 7);
        REQUIRE(validation.Value().diagnostics.Count(DiagnosticSeverity::Error) == 7);
        REQUIRE(validation.Value().diagnostics.Diagnostics().front().location.source == "assets/hostile.particle");
        REQUIRE(std::ranges::is_sorted(validation.Value().diagnostics.Diagnostics(), {}, [](const ValidationDiagnostic &finding) {
            return std::pair{finding.domain.Value(), finding.code.Value()};
        }));
    }

    TEST_CASE("Infinite particles require an independent bounded kill condition", "[unit][vfx][particle-descriptor]") {
        auto unbounded = ValidData();
        unbounded.lifetimeKind = ParticleLifetimeKind::Infinite;
        unbounded.lifetimeSeconds = {};
        unbounded.killCondition = ParticleKillCondition::None;
        unbounded.collisionMode = ParticleCollisionMode::None;
        auto rejected = ValidateParticleSystemDescriptor(std::move(unbounded), Registry(), "assets/smoke.particle");
        REQUIRE(rejected.HasValue());
        REQUIRE_FALSE(rejected.Value().Accepted());
        REQUIRE(rejected.Value().diagnostics.Diagnostics().front().code.Value() == VfxErrors::ParticleLifetimeUnbounded.code.Value());

        auto bounded = ValidData();
        bounded.lifetimeKind = ParticleLifetimeKind::Infinite;
        bounded.lifetimeSeconds = {};
        bounded.killCondition = ParticleKillCondition::Collision;
        bounded.collisionMode = ParticleCollisionMode::Planes;
        auto accepted = ValidateParticleSystemDescriptor(std::move(bounded), Registry(), "assets/smoke.particle");
        REQUIRE(accepted.HasValue());
        REQUIRE(accepted.Value().Accepted());
    }

    TEST_CASE("Particle descriptor validates exact boundaries and hostile project limits", "[unit][vfx][particle-descriptor]") {
        auto boundary = ValidData();
        boundary.maximumParticles = ParticleDescriptorHardLimits::Particles;
        boundary.spawnRate.maximum = ParticleDescriptorHardLimits::SpawnRate;
        auto accepted = ValidateParticleSystemDescriptor(boundary, Registry());
        REQUIRE(accepted.HasValue());
        REQUIRE(accepted.Value().Accepted());

        boundary.maximumParticles += 1;
        boundary.spawnRate.maximum += 1.0;
        auto rejected = ValidateParticleSystemDescriptor(std::move(boundary), Registry());
        REQUIRE(rejected.HasValue());
        REQUIRE_FALSE(rejected.Value().Accepted());
        REQUIRE(rejected.Value().diagnostics.Size() == 2);

        ParticleDescriptorLimits invalid;
        invalid.maximumJsonDepth = ParticleDescriptorHardLimits::JsonDepth + 1;
        auto invalidLimits = ValidateParticleSystemDescriptor(ValidData(), Registry(), "particle-system", invalid);
        REQUIRE(invalidLimits.HasError());
        RequireError(invalidLimits.ErrorValue(), VfxErrors::ParticleDescriptorLimitExceeded);
    }

    TEST_CASE("Particle cook checks tier and exact material evidence in one full pass", "[unit][vfx][particle-descriptor]") {
        auto validation = ValidateParticleSystemDescriptor(ValidData(), Registry());
        REQUIRE(validation.HasValue());
        REQUIRE(validation.Value().Accepted());

        const ParticleCookProfile constrained{ParticleCookTier::Compact, 1'024, 10.0};
        const ParticleMaterialEvidence missing{Material(), ParticleMaterialAvailability::Missing};
        auto cook = BuildParticleSystemCookPlan(*validation.Value().descriptor, constrained, missing, Registry(), "assets/fire.particle");
        REQUIRE(cook.HasValue());
        REQUIRE_FALSE(cook.Value().Accepted());
        REQUIRE(cook.Value().diagnostics.Size() == 2);
        REQUIRE_FALSE(cook.Value().admission.has_value());
    }

    TEST_CASE("Particle cook admits stable material identity and canonical tier", "[unit][vfx][particle-descriptor]") {
        auto validation = ValidateParticleSystemDescriptor(ValidData(), Registry());
        REQUIRE(validation.HasValue());
        auto profile = GetParticleCookProfile(ParticleCookTier::Standard);
        REQUIRE(profile.HasValue());

        const ParticleMaterialEvidence available{Material(), ParticleMaterialAvailability::Available};
        auto cook = BuildParticleSystemCookPlan(*validation.Value().descriptor, profile.Value(), available, Registry());
        REQUIRE(cook.HasValue());
        REQUIRE(cook.Value().Accepted());
        REQUIRE(cook.Value().diagnostics.Empty());
        REQUIRE(cook.Value().admission->Plan().material == Material());
        REQUIRE(cook.Value().admission->Plan().emitter == Emitter());
    }

    TEST_CASE("Particle validation fails closed when required diagnostics are not registered", "[unit][vfx][particle-descriptor]") {
        ModuleDescriptor emptyModule{.id = {"horo.empty"}, .version = {1, 0, 0}};
        auto emptyRegistry = BuildErrorCodeRegistry(std::span{&emptyModule, 1});
        REQUIRE(emptyRegistry.HasValue());
        auto invalid = ValidData();
        invalid.maximumParticles = 0;
        auto result = ValidateParticleSystemDescriptor(std::move(invalid), emptyRegistry.Value());
        REQUIRE(result.HasError());
    }

    TEST_CASE("Particle descriptor compatibility policy is explicit", "[unit][vfx][particle-descriptor]") {
        REQUIRE(ClassifyParticleDescriptorCompatibility({1, 0}) == ParticleDescriptorCompatibility::Exact);
        REQUIRE(ClassifyParticleDescriptorCompatibility({1, 1}) == ParticleDescriptorCompatibility::Unsupported);
        REQUIRE(ClassifyParticleDescriptorCompatibility({0, 9}) == ParticleDescriptorCompatibility::Unsupported);
        REQUIRE(ClassifyParticleDescriptorCompatibility({2, 0}) == ParticleDescriptorCompatibility::Unsupported);
        REQUIRE(GetParticleCookProfile(ParticleCookTier::Count).HasError());
    }

    TEST_CASE("Particle authored GPU readback preserves absence and exact policy through real cook admission",
              "[unit][vfx][particle-descriptor][gpu-readback]") {
        const bool enabled = GENERATE(false, true);
        const auto parsed = ParseParticleSystemDescriptor(enabled ? WithReadback(ReadbackObject()) : ValidJson());
        REQUIRE(parsed.HasValue());
        const auto expected = enabled ? ReadbackPolicy() : Render::VfxGpuReadbackDescriptor{};
        CHECK(parsed.Value().gpuReadback == expected);
        const auto validation = ValidateParticleSystemDescriptor(parsed.Value(), Registry());
        REQUIRE(validation.HasValue());
        REQUIRE(validation.Value().Accepted());
        const auto profile = GetParticleCookProfile(ParticleCookTier::Standard);
        REQUIRE(profile.HasValue());
        const auto cooked = BuildParticleSystemCookPlan(*validation.Value().descriptor, profile.Value(),
                                                        {Material(), ParticleMaterialAvailability::Available}, Registry());
        REQUIRE(cooked.HasValue());
        REQUIRE(cooked.Value().Accepted());
        CHECK(cooked.Value().admission->Plan().gpuReadback == expected);
        CHECK(cooked.Value().admission->Descriptor().Data().gpuReadback == expected);
        CHECK(cooked.Value().admission->Plan().maximumParticles == parsed.Value().maximumParticles);
    }

    TEST_CASE("Particle readback source decodes every declared purpose and requirement without substitution",
              "[unit][vfx][particle-descriptor][gpu-readback]") {
        const auto purpose = GENERATE(0U, 1U, 2U);
        const auto requirement = GENERATE(0U, 1U);
        constexpr std::array purposes{"authoringPreview", "diagnosticsCapture", "cosmeticObservation"};
        constexpr std::array requirements{"optional", "requiredForVisualEffect"};
        const auto parsed = ParseParticleSystemDescriptor(WithReadback(ReadbackObject(purposes[purpose], requirements[requirement])));
        REQUIRE(parsed.HasValue());
        CHECK(parsed.Value().gpuReadback.purpose == static_cast<Render::VfxGpuReadbackPurpose>(purpose));
        CHECK(parsed.Value().gpuReadback.requirement == static_cast<Render::VfxGpuReadbackRequirement>(requirement));
        CHECK(ValidateParticleSystemDescriptor(parsed.Value(), Registry()).Value().Accepted());
    }

    TEST_CASE("Particle readback JSON rejects structural errors unsigned overflow duplicate fields and unknown members",
              "[unit][vfx][particle-descriptor][gpu-readback]") {
        auto object = ReadbackObject();
        SECTION("nonobject") {
            object = "[]";
        }
        SECTION("unknown member") {
            object.insert(object.rfind('}'), ",\"extra\":0");
        }
        SECTION("missing member") {
            ReplaceReadback(object, "\"cadenceSteps\":3,", "");
        }
        SECTION("enabled must be boolean") {
            ReplaceReadback(object, "\"enabled\":true", "\"enabled\":1");
        }
        SECTION("negative count") {
            ReplaceReadback(object, "\"maximumRecords\":16", "\"maximumRecords\":-1");
        }
        SECTION("fractional count") {
            ReplaceReadback(object, "\"maximumRecords\":16", "\"maximumRecords\":1.5");
        }
        SECTION("u32 overflow") {
            ReplaceReadback(object, "\"maximumRecords\":16", "\"maximumRecords\":4294967296");
        }
        SECTION("numeric string") {
            ReplaceReadback(object, "\"maximumPendingResults\":2", "\"maximumPendingResults\":\"2\"");
        }
        SECTION("nonfinite JSON") {
            ReplaceReadback(object, "\"maximumRecords\":16", "\"maximumRecords\":NaN");
        }
        const auto parsed = ParseParticleSystemDescriptor(WithReadback(object));
        REQUIRE(parsed.HasError());
        RequireError(parsed.ErrorValue(), VfxErrors::ParticleDescriptorMalformed);
    }

    TEST_CASE("Particle readback nested duplicate detection precedes source decoding", "[unit][vfx][particle-descriptor][gpu-readback]") {
        auto object = ReadbackObject();
        object.insert(1, "\"enabled\":true,");
        const auto parsed = ParseParticleSystemDescriptor(WithReadback(object));
        REQUIRE(parsed.HasError());
        RequireError(parsed.ErrorValue(), VfxErrors::ParticleDescriptorDuplicate);
    }

    TEST_CASE("Particle readback semantic bounds are rejected by the shared canonical policy validator",
              "[unit][vfx][particle-descriptor][gpu-readback]") {
        auto data = ValidData();
        data.gpuReadback = ReadbackPolicy();
        SECTION("unknown schema") {
            data.gpuReadback.schemaVersion = 2;
        }
        SECTION("zero cadence") {
            data.gpuReadback.cadenceSteps = 0;
        }
        SECTION("zero records") {
            data.gpuReadback.maximumRecords = 0;
        }
        SECTION("over emitter capacity") {
            data.gpuReadback.maximumRecords = data.maximumParticles + 1;
        }
        SECTION("undersized result") {
            data.gpuReadback.maximumBytesPerResult -= 1;
        }
        SECTION("zero pending") {
            data.gpuReadback.maximumPendingResults = 0;
        }
        SECTION("over pending ceiling") {
            data.gpuReadback.maximumPendingResults = 65;
        }
        SECTION("disabled cannot retain observation limits") {
            data.gpuReadback.enabled = false;
        }
        const auto result = ValidateParticleSystemDescriptor(data, Registry());
        REQUIRE(result.HasValue());
        CHECK_FALSE(result.Value().Accepted());
        REQUIRE(result.Value().diagnostics.Size() == 1);
        CHECK(result.Value().diagnostics.Diagnostics().front().code.Value() == VfxErrors::ParticleDescriptorMalformed.code.Value());
    }

    TEST_CASE("Particle readback unknown enum strings remain ordered import findings instead of silently disabling",
              "[unit][vfx][particle-descriptor][gpu-readback]") {
        const auto object = GENERATE(ReadbackObject("authoritativeGameplay"), ReadbackObject("cosmeticObservation", "fallbackToCpu"));
        const auto parsed = ParseParticleSystemDescriptor(WithReadback(object));
        REQUIRE(parsed.HasValue());
        const auto result = ValidateParticleSystemDescriptor(parsed.Value(), Registry());
        REQUIRE(result.HasValue());
        CHECK_FALSE(result.Value().Accepted());
        CHECK(result.Value().diagnostics.Diagnostics().front().code.Value() == VfxErrors::ParticleDescriptorMalformed.code.Value());
    }

    TEST_CASE("Particle authored required CPU mode cannot ignore enabled GPU observations",
              "[unit][vfx][particle-descriptor][gpu-readback]") {
        auto data = ValidData();
        data.simulationPreference = SimulationPreference::RequireCPU;
        data.gpuReadback = ReadbackPolicy();
        const auto denied = ValidateParticleSystemDescriptor(data, Registry());
        REQUIRE(denied.HasValue());
        CHECK_FALSE(denied.Value().Accepted());
        REQUIRE(denied.Value().diagnostics.Size() == 1);
        CHECK(denied.Value().diagnostics.Diagnostics().front().code.Value() == VfxErrors::ParticleModeIncompatible.code.Value());
        data.gpuReadback = {};
        const auto disabled = ValidateParticleSystemDescriptor(data, Registry());
        REQUIRE(disabled.HasValue());
        CHECK(disabled.Value().Accepted());
    }

    TEST_CASE("Particle readback full-pass findings preserve existing errors and cook tier failures never erase policy",
              "[unit][vfx][particle-descriptor][gpu-readback]") {
        auto data = ValidData();
        data.maximumParticles = 0;
        data.initialSize = {0, -1};
        data.gpuReadback = ReadbackPolicy();
        const auto rejected = ValidateParticleSystemDescriptor(data, Registry());
        REQUIRE(rejected.HasValue());
        CHECK_FALSE(rejected.Value().Accepted());
        CHECK(rejected.Value().diagnostics.Size() == 3);
        CHECK(std::ranges::is_sorted(rejected.Value().diagnostics.Diagnostics(), {}, [](const ValidationDiagnostic &d) {
            return std::pair{d.domain.Value(), d.code.Value()};
        }));
        data = ValidData();
        data.gpuReadback = ReadbackPolicy();
        const auto admitted = ValidateParticleSystemDescriptor(data, Registry());
        REQUIRE(admitted.HasValue());
        REQUIRE(admitted.Value().Accepted());
        const auto cooked = BuildParticleSystemCookPlan(*admitted.Value().descriptor, {ParticleCookTier::Compact, 1, 1},
                                                        {Material(), ParticleMaterialAvailability::Available}, Registry());
        REQUIRE(cooked.HasValue());
        CHECK_FALSE(cooked.Value().Accepted());
        CHECK_FALSE(cooked.Value().admission.has_value());
        CHECK(admitted.Value().descriptor->Data().gpuReadback == ReadbackPolicy());
    }

    TEST_CASE("Particle explicit disabled readback is canonically equal to omission", "[unit][vfx][particle-descriptor][gpu-readback]") {
        const auto source = WithReadback(R"({"enabled":false,"schemaVersion":1,"purpose":"diagnosticsCapture","requirement":"optional",
            "cadenceSteps":1,"maximumRecords":0,"maximumBytesPerResult":0,"maximumPendingResults":0})");
        const auto parsed = ParseParticleSystemDescriptor(source);
        REQUIRE(parsed.HasValue());
        CHECK(parsed.Value().gpuReadback == Render::VfxGpuReadbackDescriptor{});
        const auto admitted = ValidateParticleSystemDescriptor(parsed.Value(), Registry());
        REQUIRE(admitted.HasValue());
        CHECK(admitted.Value().Accepted());
    }

    TEST_CASE("Particle readback maximum record capacity and pending64 boundary survives cook unchanged",
              "[unit][vfx][particle-descriptor][gpu-readback]") {
        auto data = ValidData();
        data.gpuReadback = ReadbackPolicy();
        data.gpuReadback.maximumRecords = data.maximumParticles;
        data.gpuReadback.maximumBytesPerResult = data.maximumParticles * Render::VfxGpuObservationWords::Bytes;
        data.gpuReadback.maximumPendingResults = 64;
        const auto admitted = ValidateParticleSystemDescriptor(data, Registry());
        REQUIRE(admitted.HasValue());
        REQUIRE(admitted.Value().Accepted());
        const auto profile = GetParticleCookProfile(ParticleCookTier::Compact);
        REQUIRE(profile.HasValue());
        const auto cooked = BuildParticleSystemCookPlan(*admitted.Value().descriptor, profile.Value(),
                                                        {Material(), ParticleMaterialAvailability::Available}, Registry());
        REQUIRE(cooked.HasValue());
        REQUIRE(cooked.Value().Accepted());
        CHECK(cooked.Value().admission->Plan().gpuReadback == data.gpuReadback);
        CHECK(cooked.Value().admission->Descriptor().Data().gpuReadback == data.gpuReadback);
    }

    TEST_CASE("Particle cook admission owns its source and plan after all caller storage changes",
              "[unit][vfx][particle-descriptor][cook-admission]") {
        auto source = ValidData();
        source.gpuReadback = ReadbackPolicy();
        const auto expected = source;
        auto imported = ValidateParticleSystemDescriptor(source, Registry());
        REQUIRE(imported.HasValue());
        REQUIRE(imported.Value().Accepted());
        auto importedPayload = std::move(imported).Value();
        auto profile = GetParticleCookProfile(ParticleCookTier::Standard).Value();
        const auto expectedProfile = profile;
        ParticleMaterialEvidence material{source.material, ParticleMaterialAvailability::Available};
        auto cooked = BuildParticleSystemCookPlan(*importedPayload.descriptor, profile, material, Registry());
        REQUIRE(cooked.HasValue());
        REQUIRE(cooked.Value().Accepted());
        auto cookedPayload = std::move(cooked).Value();
        const auto retained = *cookedPayload.admission;
        auto detachedPlan = retained.Plan();
        detachedPlan.gpuReadback = {};
        detachedPlan.maximumParticles = 0;
        source.gpuReadback = {};
        source.maximumParticles = 1;
        importedPayload.descriptor = Tests::ParticleDescriptor(source);
        profile.maximumParticles = 1;
        material.availability = ParticleMaterialAvailability::Missing;
        cookedPayload.admission.reset();
        CHECK(retained.Descriptor().Data() == expected);
        CHECK(retained.Plan().profile == expectedProfile);
        CHECK(retained.Plan().material == expected.material);
        CHECK(retained.Plan().emitter == expected.emitter);
        CHECK(retained.Plan().maximumParticles == expected.maximumParticles);
        CHECK(retained.Plan().maximumSpawnRate == expected.spawnRate.maximum);
        const auto &policy = retained.Plan().gpuReadback;
        CHECK(policy.enabled == expected.gpuReadback.enabled);
        CHECK(policy.schemaVersion == expected.gpuReadback.schemaVersion);
        CHECK(policy.purpose == expected.gpuReadback.purpose);
        CHECK(policy.requirement == expected.gpuReadback.requirement);
        CHECK(policy.cadenceSteps == expected.gpuReadback.cadenceSteps);
        CHECK(policy.maximumRecords == expected.gpuReadback.maximumRecords);
        CHECK(policy.maximumBytesPerResult == expected.gpuReadback.maximumBytesPerResult);
        CHECK(policy.maximumPendingResults == expected.gpuReadback.maximumPendingResults);
    }

    TEST_CASE("Every canonical particle cook tier admits its exact limits and preserves tighter project ceilings",
              "[unit][vfx][particle-descriptor][cook-admission]") {
        const auto tier = GENERATE(ParticleCookTier::Compact, ParticleCookTier::Standard, ParticleCookTier::Large);
        const bool lowered = GENERATE(false, true);
        auto profile = GetParticleCookProfile(tier).Value();
        if (lowered) {
            profile.maximumParticles /= 2;
            profile.maximumSpawnRate /= 2;
        }
        auto source = ValidData();
        source.maximumParticles = profile.maximumParticles;
        source.spawnRate = {0, profile.maximumSpawnRate};
        const auto imported = ValidateParticleSystemDescriptor(source, Registry());
        REQUIRE(imported.HasValue());
        REQUIRE(imported.Value().Accepted());
        const auto cooked = BuildParticleSystemCookPlan(*imported.Value().descriptor, profile,
                                                        {source.material, ParticleMaterialAvailability::Available}, Registry());
        REQUIRE(cooked.HasValue());
        REQUIRE(cooked.Value().Accepted());
        CHECK(cooked.Value().admission->Plan().profile == profile);
        CHECK(cooked.Value().admission->Descriptor().Data() == source);
    }

    TEST_CASE("Particle cook admission rejects forged tier ceilings one beyond canonical limits",
              "[unit][vfx][particle-descriptor][cook-admission]") {
        const auto tier = GENERATE(ParticleCookTier::Compact, ParticleCookTier::Standard, ParticleCookTier::Large);
        const bool particles = GENERATE(false, true);
        auto profile = GetParticleCookProfile(tier).Value();
        if (particles)
            ++profile.maximumParticles;
        else
            ++profile.maximumSpawnRate;
        const auto source = Tests::ParticleDescriptor(ValidData());
        const auto cooked =
            BuildParticleSystemCookPlan(source, profile, {source.Data().material, ParticleMaterialAvailability::Available}, Registry());
        REQUIRE(cooked.HasValue());
        CHECK_FALSE(cooked.Value().Accepted());
        CHECK_FALSE(cooked.Value().admission.has_value());
        REQUIRE(cooked.Value().diagnostics.Size() == 1);
        CHECK(cooked.Value().diagnostics.Diagnostics().front().code.Value() == VfxErrors::ParticleDescriptorMalformed.code.Value());
    }

    TEST_CASE("Particle source one beyond selected cook capacity or spawn ceiling never receives admission",
              "[unit][vfx][particle-descriptor][cook-admission]") {
        const auto tier = GENERATE(ParticleCookTier::Compact, ParticleCookTier::Standard);
        const bool particles = GENERATE(false, true);
        const auto profile = GetParticleCookProfile(tier).Value();
        auto data = ValidData();
        data.maximumParticles = profile.maximumParticles;
        data.spawnRate = {0, profile.maximumSpawnRate};
        if (particles)
            ++data.maximumParticles;
        else
            ++data.spawnRate.maximum;
        const auto imported = ValidateParticleSystemDescriptor(data, Registry());
        REQUIRE(imported.HasValue());
        REQUIRE(imported.Value().Accepted());
        const auto cooked = BuildParticleSystemCookPlan(*imported.Value().descriptor, profile,
                                                        {data.material, ParticleMaterialAvailability::Available}, Registry());
        REQUIRE(cooked.HasValue());
        CHECK_FALSE(cooked.Value().Accepted());
        CHECK_FALSE(cooked.Value().admission.has_value());
        REQUIRE(cooked.Value().diagnostics.Size() == 1);
        CHECK(cooked.Value().diagnostics.Diagnostics().front().code.Value() == VfxErrors::ParticleCookTierExceeded.code.Value());
    }

    TEST_CASE("Exact available material evidence is mandatory for immutable particle cook admission",
              "[unit][vfx][particle-descriptor][cook-admission]") {
        const auto availability = GENERATE(ParticleMaterialAvailability::Missing, ParticleMaterialAvailability::TypeMismatch,
                                           ParticleMaterialAvailability::Unloadable, ParticleMaterialAvailability::Count,
                                           static_cast<ParticleMaterialAvailability>(5), static_cast<ParticleMaterialAvailability>(255));
        const auto source = Tests::ParticleDescriptor(ValidData());
        const auto profile = GetParticleCookProfile(ParticleCookTier::Standard).Value();
        const auto cooked = BuildParticleSystemCookPlan(source, profile, {source.Data().material, availability}, Registry());
        REQUIRE(cooked.HasValue());
        CHECK_FALSE(cooked.Value().Accepted());
        CHECK_FALSE(cooked.Value().admission.has_value());
        CHECK(cooked.Value().diagnostics.Size() == 1);
        if (availability >= ParticleMaterialAvailability::Count)
            CHECK(cooked.Value().diagnostics.Diagnostics().front().code.Value() == VfxErrors::ParticleDescriptorMalformed.code.Value());
        const auto wrong =
            BuildParticleSystemCookPlan(source, profile,
                                        {Material("10112233-4455-6677-8899-aabbccddeeff"), ParticleMaterialAvailability::Available},
                                        Registry());
        REQUIRE(wrong.HasValue());
        CHECK_FALSE(wrong.Value().admission.has_value());
        REQUIRE(wrong.Value().diagnostics.Size() == 1);
        CHECK(wrong.Value().diagnostics.Diagnostics().front().code.Value() == VfxErrors::ParticleMaterialMissing.code.Value());
    }

    TEST_CASE("Particle cook cannot issue an admission when its rejection diagnostics are unavailable",
              "[unit][vfx][particle-descriptor][cook-admission]") {
        ModuleDescriptor emptyModule{.id = {"horo.empty"}, .version = {1, 0, 0}};
        const auto emptyRegistry = BuildErrorCodeRegistry(std::span{&emptyModule, 1});
        REQUIRE(emptyRegistry.HasValue());
        const auto source = Tests::ParticleDescriptor(ValidData());
        const auto profile = GetParticleCookProfile(ParticleCookTier::Standard).Value();
        const auto cooked = BuildParticleSystemCookPlan(source, profile, {source.Data().material, ParticleMaterialAvailability::Missing},
                                                        emptyRegistry.Value());
        REQUIRE(cooked.HasError());
    }
}  // namespace Horo::Vfx
