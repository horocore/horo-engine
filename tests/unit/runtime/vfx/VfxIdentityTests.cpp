#include "Horo/Vfx/VfxIdentity.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <type_traits>

namespace Horo::Vfx {
    namespace {
        VfxIdentityScope Scope(const std::uint64_t value = 7) {
            return VfxIdentityScope::Create(value).Value();
        }

        template <typename Identity> Identity Id(const std::uint32_t slot = 3, const std::uint32_t generation = 5) {
            return MakeVfxIdentity<typename Identity::IdentityTag>(Scope(), slot, generation).Value();
        }
    }  // namespace

    TEST_CASE("VFX identity domains are distinct and reject reserved dimensions", "[unit][vfx][identity]") {
        static_assert(!std::is_same_v<EffectSystemId, EmitterId>);
        static_assert(!std::is_same_v<EmitterId, ParticleBufferId>);
        static_assert(!std::is_same_v<ParticleBufferId, DecalId>);

        REQUIRE(VfxIdentityScope::Create(0).HasError());
        REQUIRE(MakeVfxIdentity<EffectSystemIdentityTag>({}, 0, 1).HasError());
        REQUIRE(MakeVfxIdentity<EffectSystemIdentityTag>(Scope(), EffectSystemId::InvalidSlot, 1).HasError());
        REQUIRE(MakeVfxIdentity<EffectSystemIdentityTag>(Scope(), 0, 0).HasError());
        REQUIRE(MakeVfxIdentity<EffectSystemIdentityTag>(Scope(), 0, 1).HasValue());
    }

    TEST_CASE("VFX identity access distinguishes stale unknown and malformed handles", "[unit][vfx][identity]") {
        const auto current = MakeVfxIdentity<EffectSystemIdentityTag>(Scope(), 4, 9).Value();
        REQUIRE(ValidateVfxIdentityAccess(current, current).HasValue());
        CHECK(
            ValidateVfxIdentityAccess(MakeVfxIdentity<EffectSystemIdentityTag>(Scope(), 4, 8).Value(), current).ErrorValue().code.Value() ==
            VfxErrors::IdentityStale.code.Value());
        CHECK(
            ValidateVfxIdentityAccess(MakeVfxIdentity<EffectSystemIdentityTag>(Scope(), 5, 9).Value(), current).ErrorValue().code.Value() ==
            VfxErrors::IdentityUnknown.code.Value());
        CHECK(ValidateVfxIdentityAccess(EffectSystemId{}, current).ErrorValue().code.Value() == VfxErrors::IdentityInvalid.code.Value());
    }

    TEST_CASE("VFX generations advance without wrap or identity drift", "[unit][vfx][identity]") {
        const auto current = MakeVfxIdentity<DecalIdentityTag>(Scope(), 12, 41).Value();
        const auto next = AdvanceVfxIdentityGeneration(current).Value();
        CHECK(next.scope == current.scope);
        CHECK(next.slot == current.slot);
        CHECK(next.generation == 42);

        const auto exhausted = MakeVfxIdentity<DecalIdentityTag>(Scope(), 12, std::numeric_limits<std::uint32_t>::max()).Value();
        CHECK(AdvanceVfxIdentityGeneration(exhausted).ErrorValue().code.Value() == VfxErrors::GenerationExhausted.code.Value());
    }

    TEST_CASE("VFX identity decoding follows the canonical network byte order", "[unit][vfx][identity][qualification]") {
        const auto original =
            MakeVfxIdentity<ParticleBufferIdentityTag>(VfxIdentityScope::Create(0x0102030405060708ULL).Value(), 0x11121314U, 0x21222324U)
                .Value();
        constexpr SerializedVfxIdentity canonicalBytes{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                                       0x11, 0x12, 0x13, 0x14, 0x21, 0x22, 0x23, 0x24};
        const SerializedVfxIdentity editorBytes = SerializeVfxIdentity(original);
        const auto runtimeIdentity = DeserializeVfxIdentity<ParticleBufferIdentityTag>(canonicalBytes).Value();
        const SerializedVfxIdentity cookBytes = SerializeVfxIdentity(runtimeIdentity);

        CHECK(runtimeIdentity == original);
        CHECK(editorBytes == canonicalBytes);
        CHECK(cookBytes == canonicalBytes);
    }

    TEST_CASE("VFX identity decoder rejects every reserved canonical representation", "[unit][vfx][identity]") {
        SerializedVfxIdentity bytes{};
        REQUIRE(DeserializeVfxIdentity<EmitterIdentityTag>(bytes).HasError());

        bytes = SerializeVfxIdentity(MakeVfxIdentity<EmitterIdentityTag>(Scope(), 2, 1).Value());
        bytes[12] = 0;
        bytes[13] = 0;
        bytes[14] = 0;
        bytes[15] = 0;
        REQUIRE(DeserializeVfxIdentity<EmitterIdentityTag>(bytes).HasError());

        bytes = SerializeVfxIdentity(MakeVfxIdentity<EmitterIdentityTag>(Scope(), 2, 1).Value());
        bytes[8] = 0xff;
        bytes[9] = 0xff;
        bytes[10] = 0xff;
        bytes[11] = 0xff;
        REQUIRE(DeserializeVfxIdentity<EmitterIdentityTag>(bytes).HasError());
    }

    TEST_CASE("VFX null and deterministic compositions are inert and reproducible", "[unit][vfx][identity]") {
        CHECK(NullVfxIdentityComposition().IsNull());
        CHECK_FALSE(NullVfxIdentityComposition().IsComplete());

        const auto first = MakeDeterministicVfxIdentityComposition(Scope(), 20, 3).Value();
        const auto repeated = MakeDeterministicVfxIdentityComposition(Scope(), 20, 3).Value();
        CHECK(first == repeated);
        CHECK(first.IsComplete());
        CHECK(first.effect.slot == 20);
        CHECK(first.emitter.slot == 21);
        CHECK(first.particles.slot == 22);
        CHECK(first.decal.slot == 23);
        CHECK(MakeDeterministicVfxIdentityComposition(Scope(), EffectSystemId::InvalidSlot - 3U, 3).HasError());
    }
}  // namespace Horo::Vfx
