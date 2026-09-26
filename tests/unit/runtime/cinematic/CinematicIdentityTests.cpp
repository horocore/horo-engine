#include "Horo/Cinematic/CinematicIdentity.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <set>
#include <string_view>
#include <type_traits>

namespace Horo::Cinematic {
    namespace {
        template <typename Identity> Identity Id(const std::uint64_t value = 7, const std::uint32_t generation = 3) {
            const auto identity = MakeCinematicIdentity<typename Identity::IdentityTag>(value, generation);
            REQUIRE(identity.HasValue());
            return identity.Value();
        }
    }  // namespace

    TEST_CASE("Cinematic identity domains are distinct and reserve zero dimensions", "[unit][cinematic][identity]") {
        static_assert(!std::is_same_v<SequenceId, TrackId>);
        static_assert(!std::is_same_v<TrackId, KeyframeId>);
        static_assert(!std::is_same_v<KeyframeId, PropertyBindingId>);

        CHECK(MakeCinematicIdentity<SequenceIdentityTag>(0, 1).HasError());
        CHECK(MakeCinematicIdentity<TrackIdentityTag>(1, 0).HasError());
        CHECK(MakeCinematicIdentity<KeyframeIdentityTag>(1, 1).HasValue());
        CHECK(Id<PropertyBindingId>().IsValid());
        static_assert(std::is_trivially_copyable_v<SequenceId>);
    }

    TEST_CASE("Cinematic identity access distinguishes stale unknown and malformed values", "[unit][cinematic][identity]") {
        const auto current = Id<TrackId>(41, 9);
        CHECK(ValidateCinematicIdentityAccess(current, current).HasValue());
        CHECK(ValidateCinematicIdentityAccess(Id<TrackId>(41, 8), current).ErrorValue().code.Value() ==
              CinematicErrors::IdentityStale.code.Value());
        CHECK(ValidateCinematicIdentityAccess(Id<TrackId>(42, 9), current).ErrorValue().code.Value() ==
              CinematicErrors::IdentityUnknown.code.Value());
        CHECK(ValidateCinematicIdentityAccess(TrackId{}, current).ErrorValue().code.Value() ==
              CinematicErrors::IdentityInvalid.code.Value());
    }

    TEST_CASE("Cinematic generations advance without wrap or stable-value drift", "[unit][cinematic][identity]") {
        const auto current = Id<KeyframeId>(52, 11);
        const auto next = AdvanceCinematicIdentityGeneration(current).Value();
        CHECK(next.stableValue == current.stableValue);
        CHECK(next.generation == 12);
        CHECK(AdvanceCinematicIdentityGeneration(KeyframeId{}).ErrorValue().code.Value() == CinematicErrors::IdentityInvalid.code.Value());

        const auto exhausted = Id<KeyframeId>(52, std::numeric_limits<std::uint32_t>::max());
        CHECK(AdvanceCinematicIdentityGeneration(exhausted).ErrorValue().code.Value() == CinematicErrors::GenerationExhausted.code.Value());
    }

    TEST_CASE("Cinematic identities retain canonical bytes through save load and cook", "[unit][cinematic][identity]") {
        const auto authored = Id<PropertyBindingId>(0x0102030405060708ULL, 0x11121314U);
        const auto saveBytes = SerializeCinematicIdentity(authored);
        const auto loaded = DeserializeCinematicIdentity<PropertyBindingIdentityTag>(saveBytes).Value();
        const auto cookedBytes = SerializeCinematicIdentity(loaded);

        CHECK(loaded == authored);
        CHECK(cookedBytes == saveBytes);
        CHECK((saveBytes == SerializedCinematicIdentity{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x11, 0x12, 0x13, 0x14}));
        static_assert(SerializedCinematicIdentity{}.size() == 12);
    }

    TEST_CASE("Cinematic identity decoding rejects reserved representations", "[unit][cinematic][identity]") {
        CHECK(DeserializeCinematicIdentity<SequenceIdentityTag>({}).HasError());

        auto bytes = SerializeCinematicIdentity(Id<SequenceId>(5, 1));
        bytes[8] = 0;
        bytes[9] = 0;
        bytes[10] = 0;
        bytes[11] = 0;
        CHECK(DeserializeCinematicIdentity<SequenceIdentityTag>(bytes).HasError());

        bytes = SerializeCinematicIdentity(Id<SequenceId>(5, 1));
        for (std::size_t index = 0; index < sizeof(std::uint64_t); ++index)
            bytes[index] = 0;
        CHECK(DeserializeCinematicIdentity<SequenceIdentityTag>(bytes).HasError());
    }

    TEST_CASE("Cinematic identity wire order is independent of host byte order", "[unit][cinematic][identity][qualification]") {
        constexpr SerializedCinematicIdentity wire{0x80, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0xff, 0x81, 0x02, 0x03, 0xfe};
        const auto decoded = DeserializeCinematicIdentity<SequenceIdentityTag>(wire);
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().stableValue == 0x80010203040506ffULL);
        CHECK(decoded.Value().generation == 0x810203feU);
        CHECK(SerializeCinematicIdentity(decoded.Value()) == wire);

        auto reversed = wire;
        std::reverse(reversed.begin(), reversed.begin() + 8);
        std::reverse(reversed.begin() + 8, reversed.end());
        const auto wrongOrder = DeserializeCinematicIdentity<SequenceIdentityTag>(reversed);
        REQUIRE(wrongOrder.HasValue());
        CHECK(wrongOrder.Value() != decoded.Value());
        CHECK(SerializeCinematicIdentity(wrongOrder.Value()) == reversed);
    }

    TEST_CASE("Cinematic null and deterministic compositions are inert and reproducible", "[unit][cinematic][identity]") {
        CHECK(NullCinematicIdentityComposition().IsNull());
        CHECK_FALSE(NullCinematicIdentityComposition().IsComplete());

        const auto first = MakeDeterministicCinematicIdentityComposition(20, 3).Value();
        const auto repeated = MakeDeterministicCinematicIdentityComposition(20, 3).Value();
        CHECK(first == repeated);
        CHECK(first.IsComplete());
        CHECK(first.sequence.stableValue == 20);
        CHECK(first.track.stableValue == 21);
        CHECK(first.keyframe.stableValue == 22);
        CHECK(first.binding.stableValue == 23);
        CHECK(MakeDeterministicCinematicIdentityComposition(0, 3).HasError());
        CHECK(MakeDeterministicCinematicIdentityComposition(std::numeric_limits<std::uint64_t>::max() - 3U, 3).HasValue());
        CHECK(MakeDeterministicCinematicIdentityComposition(std::numeric_limits<std::uint64_t>::max() - 2U, 3).HasError());
    }

    TEST_CASE("Cinematic identity errors expose unique stable descriptors", "[unit][cinematic][errors]") {
        const std::array descriptors{&CinematicErrors::IdentityInvalid, &CinematicErrors::IdentityUnknown, &CinematicErrors::IdentityStale,
                                     &CinematicErrors::GenerationExhausted, &CinematicErrors::SerializedIdentityInvalid};
        std::set<std::string_view> uniqueCodes;
        for (const auto *descriptor : descriptors) {
            CHECK(descriptor->domain.Value() == "horo.cinematic");
            CHECK(uniqueCodes.insert(descriptor->code.Value()).second);
            CHECK_FALSE(descriptor->summary.empty());
            CHECK_FALSE(descriptor->remediationHint.empty());
        }
        CHECK(CinematicErrors::GenerationExhausted.defaultSeverity == ErrorSeverity::Critical);
    }
}  // namespace Horo::Cinematic
