#include "AiTestSupport.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/AI/BlackboardSchema.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::AI {
    namespace {
        using TestSupport::ExpectError;
        using TestSupport::MakeIdentity;

        [[nodiscard]] BlackboardValue Scalar(const BlackboardScalarValue &value) {
            return BlackboardValue{value};
        }

        [[nodiscard]] BlackboardKeyDescriptor Key(const std::uint64_t identity, const BlackboardValueKind kind,
                                                  BlackboardValue defaultValue = Scalar(BlackboardScalarValue{false})) {
            return {
                .key = MakeIdentity<BlackboardKeyId>(identity),
                .kind = kind,
                .defaultValue = std::move(defaultValue),
            };
        }

        TEST_CASE("Blackboard schema capture owns and key-sorts typed defaults", "[unit][ai][blackboard]") {
            std::array keys{
                Key(9, BlackboardValueKind::SignedInteger, Scalar(BlackboardScalarValue{std::int64_t{42}})),
                Key(3, BlackboardValueKind::Boolean),
            };
            const BlackboardSchemaDescriptor source{
                .identity = MakeIdentity<BlackboardSchemaId>(11),
                .version = 4,
                .unknownValuePolicy = BlackboardUnknownValuePolicy::PreserveOpaque,
                .keys = keys,
            };
            auto captured = BlackboardSchema::Capture(source);
            REQUIRE(captured.HasValue());
            keys[0].key = MakeIdentity<BlackboardKeyId>(1);
            keys[0].defaultValue = Scalar(BlackboardScalarValue{std::int64_t{7}});

            CHECK(captured.Value().Identity() == source.identity);
            CHECK(captured.Value().Version() == 4);
            CHECK(captured.Value().UnknownValuePolicy() == BlackboardUnknownValuePolicy::PreserveOpaque);
            REQUIRE(captured.Value().Keys().size() == 2);
            CHECK(captured.Value().Keys()[0].key.Value() == 3);
            CHECK(captured.Value().Keys()[1].key.Value() == 9);
            CHECK(std::get<std::int64_t>(std::get<BlackboardScalarValue>(*captured.Value().Keys()[1].defaultValue)) == 42);

            const auto *stableStorage = captured.Value().Keys().data();
            BlackboardSchema moved = std::move(captured).Value();
            CHECK(moved.Keys().data() == stableStorage);
            CHECK(moved.Keys()[1].key.Value() == 9);
            CHECK(captured.Value().Keys().empty());

            static_assert(!std::is_default_constructible_v<BlackboardSchema>);
            static_assert(std::is_nothrow_move_constructible_v<BlackboardSchema>);
            static_assert(!std::is_copy_constructible_v<BlackboardSchema>);
            static_assert(!std::is_copy_assignable_v<BlackboardSchema>);
            static_assert(!std::is_move_assignable_v<BlackboardSchema>);
            static_assert(sizeof(BlackboardSchema) <= 64);
        }

        TEST_CASE("Blackboard schema rejects invalid bounds enums and duplicate identities transactionally", "[unit][ai][blackboard]") {
            const auto validKey = Key(1, BlackboardValueKind::Boolean);
            std::array keys{validKey};
            const BlackboardSchemaDescriptor valid{MakeIdentity<BlackboardSchemaId>(1), 1, BlackboardUnknownValuePolicy::Reject, keys};

            ExpectError(BlackboardSchema::Capture({{}, 1, BlackboardUnknownValuePolicy::Reject, keys}), AIErrors::BlackboardSchemaInvalid);
            ExpectError(BlackboardSchema::Capture({valid.identity, 0, BlackboardUnknownValuePolicy::Reject, keys}),
                        AIErrors::BlackboardSchemaInvalid);
            ExpectError(BlackboardSchema::Capture({valid.identity, 1, BlackboardUnknownValuePolicy::Reject, {}}),
                        AIErrors::BlackboardSchemaInvalid);

            std::vector<BlackboardKeyDescriptor> oversized(MaximumBlackboardKeys + 1, validKey);
            ExpectError(BlackboardSchema::Capture({valid.identity, 1, BlackboardUnknownValuePolicy::Reject, oversized}),
                        AIErrors::BlackboardLimitExceeded);

            std::array duplicates{validKey, validKey};
            ExpectError(BlackboardSchema::Capture({valid.identity, 1, BlackboardUnknownValuePolicy::Reject, duplicates}),
                        AIErrors::DescriptorConflict);
            CHECK(keys[0] == validKey);
        }

        TEST_CASE("Blackboard schema rejects enum sentinels and unknown values transactionally", "[unit][ai][blackboard]") {
            const auto validKey = Key(1, BlackboardValueKind::Boolean);
            std::array keys{validKey};
            const BlackboardSchemaDescriptor valid{MakeIdentity<BlackboardSchemaId>(1), 1, BlackboardUnknownValuePolicy::Reject, keys};
            auto invalid = validKey;
            invalid.kind = BlackboardValueKind::Count;
            ExpectError(BlackboardSchema::Capture({valid.identity, 1, BlackboardUnknownValuePolicy::Reject, std::span{&invalid, 1}}),
                        AIErrors::BlackboardSchemaInvalid);
            invalid = validKey;
            invalid.kind = static_cast<BlackboardValueKind>(255);
            ExpectError(BlackboardSchema::Capture({valid.identity, 1, BlackboardUnknownValuePolicy::Reject, std::span{&invalid, 1}}),
                        AIErrors::BlackboardSchemaInvalid);
            invalid = validKey;
            invalid.cardinality = BlackboardValueCardinality::Count;
            ExpectError(BlackboardSchema::Capture({valid.identity, 1, BlackboardUnknownValuePolicy::Reject, std::span{&invalid, 1}}),
                        AIErrors::BlackboardSchemaInvalid);
            invalid = validKey;
            invalid.cardinality = static_cast<BlackboardValueCardinality>(255);
            ExpectError(BlackboardSchema::Capture({valid.identity, 1, BlackboardUnknownValuePolicy::Reject, std::span{&invalid, 1}}),
                        AIErrors::BlackboardSchemaInvalid);
            invalid = validKey;
            invalid.access = BlackboardKeyAccess::Count;
            ExpectError(BlackboardSchema::Capture({valid.identity, 1, BlackboardUnknownValuePolicy::Reject, std::span{&invalid, 1}}),
                        AIErrors::BlackboardSchemaInvalid);
            invalid = validKey;
            invalid.access = static_cast<BlackboardKeyAccess>(255);
            ExpectError(BlackboardSchema::Capture({valid.identity, 1, BlackboardUnknownValuePolicy::Reject, std::span{&invalid, 1}}),
                        AIErrors::BlackboardSchemaInvalid);
            invalid = validKey;
            invalid.presence = BlackboardKeyPresence::Count;
            ExpectError(BlackboardSchema::Capture({valid.identity, 1, BlackboardUnknownValuePolicy::Reject, std::span{&invalid, 1}}),
                        AIErrors::BlackboardSchemaInvalid);
            invalid = validKey;
            invalid.presence = static_cast<BlackboardKeyPresence>(255);
            ExpectError(BlackboardSchema::Capture({valid.identity, 1, BlackboardUnknownValuePolicy::Reject, std::span{&invalid, 1}}),
                        AIErrors::BlackboardSchemaInvalid);
            ExpectError(BlackboardSchema::Capture({valid.identity, 1, BlackboardUnknownValuePolicy::Count, keys}),
                        AIErrors::BlackboardSchemaInvalid);
            ExpectError(BlackboardSchema::Capture({valid.identity, 1, static_cast<BlackboardUnknownValuePolicy>(255), keys}),
                        AIErrors::BlackboardSchemaInvalid);
            CHECK(keys[0] == validKey);
        }

        TEST_CASE("Blackboard scalar values are finite type checked and reference shaped", "[unit][ai][blackboard]") {
            const auto scalarKey = Key(1, BlackboardValueKind::Scalar, Scalar(BlackboardScalarValue{1.0}));
            CHECK(ValidateBlackboardValue(Scalar(BlackboardScalarValue{1.0}), &scalarKey, BlackboardUnknownValuePolicy::Reject).HasValue());
            ExpectError(ValidateBlackboardValue(Scalar(BlackboardScalarValue{true}), &scalarKey, BlackboardUnknownValuePolicy::Reject),
                        AIErrors::BlackboardValueTypeMismatch);
            ExpectError(ValidateBlackboardValue(Scalar(BlackboardScalarValue{std::numeric_limits<double>::infinity()}), &scalarKey,
                                                BlackboardUnknownValuePolicy::Reject),
                        AIErrors::BlackboardValueInvalid);
            ExpectError(ValidateBlackboardValue(Scalar(BlackboardScalarValue{std::nan("")}), &scalarKey,
                                                BlackboardUnknownValuePolicy::Reject),
                        AIErrors::BlackboardValueInvalid);

            const auto entityKey =
                Key(2, BlackboardValueKind::EntityReference, Scalar(BlackboardScalarValue{BlackboardStoredEntityReference{7, 3, 2}}));
            CHECK(ValidateBlackboardValue(*entityKey.defaultValue, &entityKey, BlackboardUnknownValuePolicy::Reject).HasValue());
            ExpectError(ValidateBlackboardValue(Scalar(BlackboardScalarValue{BlackboardStoredEntityReference{}}), &entityKey,
                                                BlackboardUnknownValuePolicy::Reject),
                        AIErrors::BlackboardValueInvalid);

            BlackboardStoredAssetReference asset;
            asset.canonicalBytes.back() = 1;
            const auto assetKey = Key(3, BlackboardValueKind::AssetReference, Scalar(BlackboardScalarValue{asset}));
            CHECK(ValidateBlackboardValue(*assetKey.defaultValue, &assetKey, BlackboardUnknownValuePolicy::Reject).HasValue());
            ExpectError(ValidateBlackboardValue(Scalar(BlackboardScalarValue{BlackboardStoredAssetReference{}}), &assetKey,
                                                BlackboardUnknownValuePolicy::Reject),
                        AIErrors::BlackboardValueInvalid);

            const auto coordinateKey = Key(4, BlackboardValueKind::WorldCoordinate,
                                           Scalar(BlackboardScalarValue{Math::WorldCoordinate64::FromMillimeters(-1, 2, 3)}));
            CHECK(ValidateBlackboardValue(*coordinateKey.defaultValue, &coordinateKey, BlackboardUnknownValuePolicy::Reject).HasValue());
        }

        TEST_CASE("Stored entity references use explicit fixed-width network encoding", "[unit][ai][blackboard]") {
            const BlackboardStoredEntityReference reference{0x0102030405060708ULL, 0x11121314U, 0x21222324U};
            const SerializedBlackboardEntityReference expected{
                0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x11, 0x12, 0x13, 0x14, 0x21, 0x22, 0x23, 0x24,
            };
            const auto encoded = SerializeBlackboardEntityReference(reference);
            REQUIRE(encoded.HasValue());
            CHECK(encoded.Value() == expected);
            const auto decoded = DeserializeBlackboardEntityReference(expected);
            REQUIRE(decoded.HasValue());
            CHECK(decoded.Value() == reference);
            ExpectError(SerializeBlackboardEntityReference({}), AIErrors::BlackboardValueInvalid);
            ExpectError(DeserializeBlackboardEntityReference({}), AIErrors::BlackboardValueInvalid);
        }

        TEST_CASE("Blackboard collections are flat homogeneous and bounded by their key", "[unit][ai][blackboard]") {
            BlackboardKeyDescriptor key = Key(1, BlackboardValueKind::SignedInteger, Scalar(BlackboardScalarValue{std::int64_t{0}}));
            key.cardinality = BlackboardValueCardinality::Collection;
            key.maximumCollectionElements = 2;

            BlackboardCollectionValue values;
            values.elementKind = BlackboardValueKind::SignedInteger;
            values.elements[0] = std::int64_t{1};
            values.elements[1] = std::int64_t{2};
            values.count = 2;
            const BlackboardValue valid{values};
            CHECK(ValidateBlackboardValue(valid, &key, BlackboardUnknownValuePolicy::Reject).HasValue());

            values.count = 3;
            ExpectError(ValidateBlackboardValue(BlackboardValue{values}, &key, BlackboardUnknownValuePolicy::Reject),
                        AIErrors::BlackboardLimitExceeded);
            values.count = 1;
            values.elements[0] = true;
            ExpectError(ValidateBlackboardValue(BlackboardValue{values}, &key, BlackboardUnknownValuePolicy::Reject),
                        AIErrors::BlackboardValueTypeMismatch);
            values.elementKind = BlackboardValueKind::Boolean;
            ExpectError(ValidateBlackboardValue(BlackboardValue{values}, &key, BlackboardUnknownValuePolicy::Reject),
                        AIErrors::BlackboardValueTypeMismatch);

            BlackboardCollectionValue equalLeft;
            equalLeft.elementKind = BlackboardValueKind::Boolean;
            equalLeft.elements[0] = true;
            equalLeft.elements[1] = false;
            equalLeft.count = 1;
            auto equalRight = equalLeft;
            equalRight.elements[1] = true;
            CHECK(equalLeft == equalRight);
            equalLeft.count = MaximumBlackboardCollectionElements + 1;
            equalRight.count = equalLeft.count;
            CHECK_FALSE(equalLeft == equalRight);

            key.maximumCollectionElements = 0;
            ExpectError(BlackboardSchema::Capture(
                            {MakeIdentity<BlackboardSchemaId>(1), 1, BlackboardUnknownValuePolicy::Reject, std::span{&key, 1}}),
                        AIErrors::BlackboardSchemaInvalid);
            key.maximumCollectionElements = MaximumBlackboardCollectionElements + 1;
            ExpectError(BlackboardSchema::Capture(
                            {MakeIdentity<BlackboardSchemaId>(1), 1, BlackboardUnknownValuePolicy::Reject, std::span{&key, 1}}),
                        AIErrors::BlackboardSchemaInvalid);
        }

        TEST_CASE("Unknown blackboard values follow explicit reject or byte-preserve policy", "[unit][ai][blackboard]") {
            BlackboardOpaqueValue source;
            source.schemaVersion = 8;
            source.serializedType = 77;
            source.bytes[0] = std::byte{0xab};
            source.bytes[1] = std::byte{0xcd};
            source.size = 2;
            const BlackboardValue value{source};

            ExpectError(ValidateBlackboardValue(value, nullptr, BlackboardUnknownValuePolicy::Reject),
                        AIErrors::BlackboardUnknownValueRejected);
            CHECK(ValidateBlackboardValue(value, nullptr, BlackboardUnknownValuePolicy::PreserveOpaque).HasValue());
            CHECK(std::get<BlackboardOpaqueValue>(value) == source);

            auto equalTail = source;
            equalTail.bytes[2] = std::byte{0xef};
            CHECK(source == equalTail);

            source.schemaVersion = 0;
            ExpectError(ValidateBlackboardValue(BlackboardValue{source}, nullptr, BlackboardUnknownValuePolicy::PreserveOpaque),
                        AIErrors::BlackboardValueInvalid);
            source.schemaVersion = 1;
            source.serializedType = 0;
            ExpectError(ValidateBlackboardValue(BlackboardValue{source}, nullptr, BlackboardUnknownValuePolicy::PreserveOpaque),
                        AIErrors::BlackboardValueInvalid);
            source.serializedType = 1;
            source.size = MaximumBlackboardOpaqueBytes + 1;
            ExpectError(ValidateBlackboardValue(BlackboardValue{source}, nullptr, BlackboardUnknownValuePolicy::PreserveOpaque),
                        AIErrors::BlackboardValueInvalid);
            equalTail = source;
            CHECK_FALSE(source == equalTail);
            ExpectError(ValidateBlackboardValue(Scalar(BlackboardScalarValue{true}), nullptr, BlackboardUnknownValuePolicy::PreserveOpaque),
                        AIErrors::BlackboardValueTypeMismatch);
        }
    }  // namespace
}  // namespace Horo::AI
