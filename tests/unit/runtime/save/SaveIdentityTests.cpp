#include "../../../support/AllocationProbe.h"
#include "Horo/Runtime/Save/SaveIdentity.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace Horo::Runtime {
    namespace {
        constexpr std::string_view kFirstUuid = "00112233-4455-6677-8899-aabbccddeeff";
        constexpr std::string_view kSecondUuid = "10112233-4455-6677-8899-aabbccddeeff";

        template <typename Identity> Identity ParseIdentity(const std::string_view text) {
            auto result = Identity::Parse(text);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        template <typename Version> Version MakeVersion(const std::uint32_t value) {
            auto result = Version::Create(value);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        TEST_CASE("Persistent save identities round-trip canonically and remain strongly typed", "[unit][save][identity]") {
            const auto slot = ParseIdentity<SaveGameSlotId>(kFirstUuid);
            REQUIRE(slot.ToString() == kFirstUuid);
            const auto fromBytes = SaveGameSlotId::FromBytes(slot.Bytes());
            REQUIRE(fromBytes.HasValue());
            REQUIRE(fromBytes.Value() == slot);
            REQUIRE(ParseIdentity<SaveGameSlotId>(kSecondUuid) > slot);

            static_assert(!std::is_same_v<SaveGameSlotId, GameProfileId>);
            static_assert(!std::is_same_v<SlotGenerationId, CapturedStateId>);
            static_assert(!std::is_convertible_v<std::string_view, SaveGameSlotId>);
            static_assert(std::is_trivially_copyable_v<SaveGameSlotId>);
        }

        TEST_CASE("Save identity formatting propagates every allocation failure without terminating",
                  "[unit][save][identity][allocation]") {
            const auto identity = ParseIdentity<SlotGenerationId>(kFirstUuid);
            // A default string is noexcept even when MSVC Debug allocates a container proxy inside it.
            static_assert(noexcept(std::string{}));
            Horo::Tests::AllocationProbe::Measurement measurement;
            {
                Horo::Tests::AllocationProbe::ScopedMeasurement measured;
                const auto formatted = identity.ToString();
                measurement = measured.Snapshot();
            }
            REQUIRE(measurement.requests > 0);
            for (std::size_t offset = 0; offset < measurement.requests; ++offset) {
                bool caught{};
                bool recovered{};
                {
                    Horo::Tests::AllocationProbe::ScopedFailure failure{offset};
                    try {
                        const auto formatted = identity.ToString();
                    } catch (const std::bad_alloc &) {
                        caught = true;
                    }
                    // The injection is one-shot; recovery remains possible before the scope leaves.
                    recovered = identity.ToString() == kFirstUuid;
                }
                CHECK(caught);
                CHECK(recovered);
            }
            CHECK(identity.ToString() == kFirstUuid);
        }

        TEST_CASE("Persistent save identities reject missing reserved and malformed values", "[unit][save][identity]") {
            REQUIRE_FALSE(SaveGameSlotId{}.IsValid());
            REQUIRE(SaveGameSlotId::FromBytes({}).HasError());
            REQUIRE(SaveGameSlotId::Parse("").HasError());
            REQUIRE(SaveGameSlotId::Parse("00112233445566778899aabbccddeeff").HasError());
            REQUIRE(SaveGameSlotId::Parse("00112233-4455-6677-8899-AABBCCDDEEFF").HasError());
            REQUIRE(SaveGameSlotId::Parse("00000000-0000-0000-0000-000000000000").HasError());
            REQUIRE(SaveGameSlotId::Parse("g0112233-4455-6677-8899-aabbccddeeff").HasError());
        }

        TEST_CASE("Typed hashes use canonical bytes and preserve semantic separation", "[unit][save][identity]") {
            using SlotSet = std::unordered_set<SaveGameSlotId, PersistentSaveIdentityHash<SaveGameSlotIdentityTag>>;
            SlotSet values;
            const auto first = ParseIdentity<SaveGameSlotId>(kFirstUuid);
            const auto second = ParseIdentity<SaveGameSlotId>(kSecondUuid);
            REQUIRE(values.insert(first).second);
            REQUIRE_FALSE(values.insert(first).second);
            REQUIRE(values.insert(second).second);
            REQUIRE(values.size() == 2);
            REQUIRE(SaveIdentityDetail::HashUuid(first.Bytes()) == 0x52a39b8a741b3ef5ULL);
            REQUIRE(PersistentSaveIdentityHash<SaveGameSlotIdentityTag>{}(first) == static_cast<std::size_t>(0x52a39b8a741b3ef5ULL));
        }

        TEST_CASE("Identity collection validation rejects missing and duplicate records", "[unit][save][identity]") {
            const auto first = ParseIdentity<SaveRecordId>(kFirstUuid);
            const auto second = ParseIdentity<SaveRecordId>(kSecondUuid);
            const std::array unique{first, second};
            const std::array duplicate{first, second, first};
            const std::array withMissing{first, SaveRecordId{}};
            REQUIRE(ValidateUniqueSaveIdentities<SaveRecordIdentityTag>(unique).HasValue());
            REQUIRE(ValidateUniqueSaveIdentities<SaveRecordIdentityTag>(duplicate).HasError());
            REQUIRE(ValidateUniqueSaveIdentities<SaveRecordIdentityTag>(withMissing).HasError());
            REQUIRE(ValidateUniqueSaveIdentities<SaveRecordIdentityTag>({}).HasValue());
        }

        TEST_CASE("Identity collection validation scales to large unique inputs", "[unit][save][identity]") {
            std::vector<SaveRecordId> identities;
            identities.reserve(4096);
            for (std::uint32_t index = 1; index <= 4096; ++index) {
                SaveIdentityDetail::Bytes bytes{};
                bytes[12] = static_cast<std::uint8_t>(index >> 24U);
                bytes[13] = static_cast<std::uint8_t>(index >> 16U);
                bytes[14] = static_cast<std::uint8_t>(index >> 8U);
                bytes[15] = static_cast<std::uint8_t>(index);
                auto identity = SaveRecordId::FromBytes(bytes);
                REQUIRE(identity.HasValue());
                identities.push_back(identity.Value());
            }

            REQUIRE(ValidateUniqueSaveIdentities<SaveRecordIdentityTag>(identities).HasValue());
            identities.back() = identities.front();
            REQUIRE(ValidateUniqueSaveIdentities<SaveRecordIdentityTag>(identities).HasError());
        }

        TEST_CASE("Participant identities reject ambiguous missing and oversized input", "[unit][save][identity]") {
            const auto parsed = SaveParticipantId::Parse("horo.scene.core_v1");
            REQUIRE(parsed.HasValue());
            REQUIRE(parsed.Value().Value() == "horo.scene.core_v1");
            REQUIRE(SaveParticipantId::Parse("").HasError());
            REQUIRE(SaveParticipantId::Parse("scene").HasError());
            REQUIRE(SaveParticipantId::Parse("Horo.scene").HasError());
            REQUIRE(SaveParticipantId::Parse("horo..scene").HasError());
            REQUIRE(SaveParticipantId::Parse("horo.1scene").HasError());
            REQUIRE(SaveParticipantId::Parse(std::string(MaximumSaveParticipantIdBytes + 1, 'a')).HasError());
        }

        TEST_CASE("Independent save versions use explicit canonical bytes", "[unit][save][version]") {
            const auto archive = MakeVersion<ArchiveFormatVersion>(0x01020304U);
            const SerializedSaveVersion expected{0x04, 0x03, 0x02, 0x01};
            REQUIRE(SerializeSaveVersion(archive) == expected);
            const auto decoded = DeserializeSaveVersion<ArchiveFormatVersionTag>(expected);
            REQUIRE(decoded.HasValue());
            REQUIRE(decoded.Value() == archive);
            static_assert(!std::is_same_v<ArchiveFormatVersion, SaveSchemaVersion>);
            static_assert(!std::is_same_v<SaveSchemaVersion, ParticipantSchemaVersion>);
            static_assert(!std::is_convertible_v<std::uint32_t, SaveSchemaVersion>);
        }

        TEST_CASE("Version values reject missing and unsupported newer inputs", "[unit][save][version]") {
            REQUIRE(SaveSchemaVersion::Create(0).HasError());
            REQUIRE(DeserializeSaveVersion<SaveSchemaVersionTag>({}).HasError());
            const auto current = MakeVersion<SaveSchemaVersion>(7);
            REQUIRE(ValidateReadableSaveVersion(MakeVersion<SaveSchemaVersion>(1), current).HasValue());
            REQUIRE(ValidateReadableSaveVersion(current, current).HasValue());
            REQUIRE(ValidateReadableSaveVersion(MakeVersion<SaveSchemaVersion>(8), current).HasError());
            REQUIRE(ValidateReadableSaveVersion(SaveSchemaVersion{}, current).HasError());
            REQUIRE(ValidateReadableSaveVersion(current, SaveSchemaVersion{}).HasError());
            REQUIRE(MakeVersion<SaveSchemaVersion>(std::numeric_limits<std::uint32_t>::max()).IsValid());
        }
    }  // namespace
}  // namespace Horo::Runtime
