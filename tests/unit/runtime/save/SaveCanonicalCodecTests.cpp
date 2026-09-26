#include "Horo/Runtime/Save/SaveCanonicalCodec.h"
#include "Horo/Runtime/Save/SaveDiagnostics.h"
#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;

    enum class CompositeKind : std::uint8_t {
        Sequence,
        Optional,
        Variant,
        Map,
        Set,
        Record
    };

    CanonicalFieldId Field(const std::uint32_t value) {
        return CanonicalFieldId::Create(value).Value();
    }

    CanonicalEncodedValue Finish(CanonicalValueWriter writer) {
        auto value = std::move(writer).Finalize();
        REQUIRE(value.HasValue());
        return std::move(value).Value();
    }

    CanonicalEncodedValue EncodeUInt32(const std::uint32_t value, const CanonicalCodecLimits limits = {}) {
        CanonicalValueWriter writer{limits};
        REQUIRE(writer.WriteUInt32(value).HasValue());
        return Finish(std::move(writer));
    }

    CanonicalValueReader ReaderFor(const CanonicalEncodedValue &value, const CanonicalCodecLimits limits = {}) {
        auto reader = CanonicalValueReader::Create(value.Bytes(), limits);
        REQUIRE(reader.HasValue());
        return std::move(reader).Value();
    }

    Result<void> WriteComposite(CanonicalValueWriter &writer, const CompositeKind kind, const CanonicalEncodedValue &child,
                                const CanonicalCodecLimits limits) {
        switch (kind) {
            case CompositeKind::Sequence: {
                const std::array values{child};
                return writer.WriteSequence(values);
            }
            case CompositeKind::Optional:
                return writer.WriteOptional(std::optional{child});
            case CompositeKind::Variant:
                return writer.WriteVariant(1, 2, child);
            case CompositeKind::Map: {
                const std::array entries{CanonicalMapEntry{EncodeUInt32(1, limits), child}};
                return writer.WriteMap(entries);
            }
            case CompositeKind::Set: {
                const std::array values{child};
                return writer.WriteSet(values);
            }
            case CompositeKind::Record: {
                const std::array fields{CanonicalRecordField{Field(1), child}};
                return writer.WriteRecord(fields);
            }
        }
        return Result<void>::Failure(MakeError(SaveErrors::CanonicalCodecInvalid));
    }

    CanonicalEncodedValue Wrap(const CompositeKind kind, const CanonicalEncodedValue &child, const CanonicalCodecLimits limits = {}) {
        CanonicalValueWriter writer{limits};
        REQUIRE(WriteComposite(writer, kind, child, limits).HasValue());
        return Finish(std::move(writer));
    }

    Result<CanonicalValueReader> ReadCompositeChild(const CompositeKind kind, CanonicalValueReader &reader) {
        switch (kind) {
            case CompositeKind::Sequence: {
                auto values = reader.ReadSequence();
                if (values.HasError())
                    return Result<CanonicalValueReader>::Failure(values.ErrorValue());
                return values.Value().front().OpenReader();
            }
            case CompositeKind::Optional: {
                auto value = reader.ReadOptional();
                if (value.HasError())
                    return Result<CanonicalValueReader>::Failure(value.ErrorValue());
                return value.Value()->OpenReader();
            }
            case CompositeKind::Variant: {
                auto value = reader.ReadVariant(2);
                if (value.HasError())
                    return Result<CanonicalValueReader>::Failure(value.ErrorValue());
                return value.Value().second.OpenReader();
            }
            case CompositeKind::Map: {
                auto entries = reader.ReadMap();
                if (entries.HasError())
                    return Result<CanonicalValueReader>::Failure(entries.ErrorValue());
                return entries.Value().front().value.OpenReader();
            }
            case CompositeKind::Set: {
                auto values = reader.ReadSet();
                if (values.HasError())
                    return Result<CanonicalValueReader>::Failure(values.ErrorValue());
                return values.Value().front().OpenReader();
            }
            case CompositeKind::Record: {
                auto fields = reader.ReadRecord();
                if (fields.HasError())
                    return Result<CanonicalValueReader>::Failure(fields.ErrorValue());
                return fields.Value().front().value.OpenReader();
            }
        }
        return Result<CanonicalValueReader>::Failure(MakeError(SaveErrors::CanonicalCodecInvalid));
    }

    TEST_CASE("Canonical scalar codec has byte-exact little-endian golden output", "[runtime][save][canonical-codec]") {
        CanonicalValueWriter writer;
        REQUIRE(writer.WriteBool(true).HasValue());
        REQUIRE(writer.WriteUInt16(0x1234U).HasValue());
        REQUIRE(writer.WriteInt32(-2).HasValue());
        REQUIRE(writer.WriteFloat32(-0.0F).HasValue());
        REQUIRE(writer.WriteUtf8("Horo \xF0\x9F\x8C\x8D").HasValue());
        const auto encoded = Finish(std::move(writer));

        const std::vector<std::byte> expected{std::byte{0x01}, std::byte{0x34}, std::byte{0x12}, std::byte{0xfe}, std::byte{0xff},
                                              std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                                              std::byte{0x00}, std::byte{0x09}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                                              std::byte{'H'},  std::byte{'o'},  std::byte{'r'},  std::byte{'o'},  std::byte{' '},
                                              std::byte{0xf0}, std::byte{0x9f}, std::byte{0x8c}, std::byte{0x8d}};
        CHECK(std::ranges::equal(encoded.Bytes(), expected));

        auto reader = ReaderFor(encoded);
        CHECK(reader.ReadBool().Value());
        CHECK(reader.ReadUInt16().Value() == 0x1234U);
        CHECK(reader.ReadInt32().Value() == -2);
        CHECK(std::bit_cast<std::uint32_t>(reader.ReadFloat32().Value()) == 0U);
        CHECK(reader.ReadUtf8().Value() == "Horo \xF0\x9F\x8C\x8D");
        CHECK(reader.RequireFinished().HasValue());
    }

    TEST_CASE("Canonical child readers share a cumulative read work budget", "[runtime][save][canonical-codec]") {
        const auto child = EncodeUInt32(42);
        const auto parent = Wrap(CompositeKind::Sequence, child);
        CanonicalCodecLimits limits;
        limits.maximumReadWorkBytes = parent.Bytes().size() + child.Bytes().size();
        auto root = ReaderFor(parent, limits);
        auto values = root.ReadSequence();
        REQUIRE(values.HasValue());
        REQUIRE(values.Value().size() == 1);
        auto first = values.Value().front().OpenReader();
        REQUIRE(first.HasValue());
        auto firstReader = std::move(first).Value();
        CHECK(firstReader.ReadUInt32().Value() == 42);
        auto reopened = values.Value().front().OpenReader();
        REQUIRE(reopened.HasValue());
        auto reopenedReader = std::move(reopened).Value();
        const auto exhausted = reopenedReader.ReadUInt32();
        REQUIRE(exhausted.HasError());
        CHECK(exhausted.ErrorValue().code.Value() == SaveErrors::CanonicalCodecLimitExceeded.code.Value());
        REQUIRE(exhausted.ErrorValue().diagnostics.size() == 1);
        CHECK(exhausted.ErrorValue().diagnostics.front().code.Value() == "save.canonical_codec.limit.read_work");

        limits.maximumReadWorkBytes = std::numeric_limits<std::size_t>::max();
        const auto unbounded = CanonicalValueReader::Create(parent.Bytes(), limits);
        REQUIRE(unbounded.HasError());
        CHECK(unbounded.ErrorValue().code.Value() == SaveErrors::CanonicalCodecConfigurationInvalid.code.Value());
    }

    TEST_CASE("Canonical fixed-width and math values round trip", "[runtime][save][canonical-codec]") {
        CanonicalValueWriter writer;
        REQUIRE(writer.WriteUInt8(std::numeric_limits<std::uint8_t>::max()).HasValue());
        REQUIRE(writer.WriteUInt16(std::numeric_limits<std::uint16_t>::max()).HasValue());
        REQUIRE(writer.WriteUInt32(std::numeric_limits<std::uint32_t>::max()).HasValue());
        REQUIRE(writer.WriteUInt64(std::numeric_limits<std::uint64_t>::max()).HasValue());
        REQUIRE(writer.WriteInt8(std::numeric_limits<std::int8_t>::min()).HasValue());
        REQUIRE(writer.WriteInt16(std::numeric_limits<std::int16_t>::min()).HasValue());
        REQUIRE(writer.WriteInt32(std::numeric_limits<std::int32_t>::min()).HasValue());
        REQUIRE(writer.WriteInt64(std::numeric_limits<std::int64_t>::min()).HasValue());
        REQUIRE(writer.WriteVec3({1.0F, -2.0F, 3.5F}).HasValue());
        REQUIRE(writer.WriteQuaternion({0.0F, 0.5F, 0.0F, 0.5F}).HasValue());
        const auto encoded = Finish(std::move(writer));

        auto reader = ReaderFor(encoded);
        CHECK(reader.ReadUInt8().Value() == std::numeric_limits<std::uint8_t>::max());
        CHECK(reader.ReadUInt16().Value() == std::numeric_limits<std::uint16_t>::max());
        CHECK(reader.ReadUInt32().Value() == std::numeric_limits<std::uint32_t>::max());
        CHECK(reader.ReadUInt64().Value() == std::numeric_limits<std::uint64_t>::max());
        CHECK(reader.ReadInt8().Value() == std::numeric_limits<std::int8_t>::min());
        CHECK(reader.ReadInt16().Value() == std::numeric_limits<std::int16_t>::min());
        CHECK(reader.ReadInt32().Value() == std::numeric_limits<std::int32_t>::min());
        CHECK(reader.ReadInt64().Value() == std::numeric_limits<std::int64_t>::min());
        const Math::Vec3 expectedVector{1.0F, -2.0F, 3.5F};
        const Math::Quaternion expectedRotation{0.0F, 0.5F, 0.0F, 0.5F};
        CHECK(reader.ReadVec3().Value() == expectedVector);
        CHECK(reader.ReadQuaternion().Value() == expectedRotation);
        CHECK(reader.RequireFinished().HasValue());
    }

    TEST_CASE("Canonical maps sets and records ignore input ordering", "[runtime][save][canonical-codec]") {
        const std::vector<CanonicalMapEntry> mapA{{EncodeUInt32(9), EncodeUInt32(90)}, {EncodeUInt32(1), EncodeUInt32(10)}};
        const std::vector<CanonicalMapEntry> mapB{mapA[1], mapA[0]};
        CanonicalValueWriter first;
        CanonicalValueWriter second;
        REQUIRE(first.WriteMap(mapA).HasValue());
        REQUIRE(second.WriteMap(mapB).HasValue());
        const auto firstMap = Finish(std::move(first));
        const auto secondMap = Finish(std::move(second));
        CHECK(std::ranges::equal(firstMap.Bytes(), secondMap.Bytes()));

        const std::vector<CanonicalEncodedValue> setA{EncodeUInt32(8), EncodeUInt32(2)};
        const std::vector<CanonicalEncodedValue> setB{setA.rbegin(), setA.rend()};
        CanonicalValueWriter setFirst;
        CanonicalValueWriter setSecond;
        REQUIRE(setFirst.WriteSet(setA).HasValue());
        REQUIRE(setSecond.WriteSet(setB).HasValue());
        const auto firstSet = Finish(std::move(setFirst));
        const auto secondSet = Finish(std::move(setSecond));
        CHECK(std::ranges::equal(firstSet.Bytes(), secondSet.Bytes()));

        const std::vector<CanonicalRecordField> recordA{{Field(9), EncodeUInt32(90)}, {Field(1), EncodeUInt32(10)}};
        const std::vector<CanonicalRecordField> recordB{recordA[1], recordA[0]};
        CanonicalValueWriter recordFirst;
        CanonicalValueWriter recordSecond;
        REQUIRE(recordFirst.WriteRecord(recordA).HasValue());
        REQUIRE(recordSecond.WriteRecord(recordB).HasValue());
        const auto firstRecord = Finish(std::move(recordFirst));
        const auto secondRecord = Finish(std::move(recordSecond));
        CHECK(std::ranges::equal(firstRecord.Bytes(), secondRecord.Bytes()));

        auto reader = ReaderFor(firstRecord);
        const auto decoded = reader.ReadRecord();
        REQUIRE(decoded.HasValue());
        REQUIRE(decoded.Value().size() == 2);
        CHECK(decoded.Value()[0].id == Field(1));
        CHECK(decoded.Value()[1].id == Field(9));
    }

    TEST_CASE("Canonical codec enforces exact structural depth for every composite", "[runtime][save][canonical-codec]") {
        constexpr std::array kinds{CompositeKind::Sequence, CompositeKind::Optional, CompositeKind::Variant,
                                   CompositeKind::Map,      CompositeKind::Set,      CompositeKind::Record};
        for (const auto kind : kinds) {
            DYNAMIC_SECTION("kind " << static_cast<int>(kind)) {
                CanonicalCodecLimits limits;
                limits.maximumNestingDepth = 2;
                const auto leaf = EncodeUInt32(7, limits);
                const auto levelOne = Wrap(kind, leaf, limits);
                const auto levelTwo = Wrap(kind, levelOne, limits);

                CanonicalValueWriter rejected{limits};
                CHECK(WriteComposite(rejected, kind, levelTwo, limits).HasError());

                auto reader = ReaderFor(levelTwo, limits);
                auto child = ReadCompositeChild(kind, reader);
                REQUIRE(child.HasValue());
                auto childReader = std::move(child).Value();
                auto grandchild = ReadCompositeChild(kind, childReader);
                REQUIRE(grandchild.HasValue());
                auto grandchildReader = std::move(grandchild).Value();
                CHECK(grandchildReader.ReadUInt32().Value() == 7);

                auto shallowLimits = limits;
                shallowLimits.maximumNestingDepth = 1;
                auto shallowReader = ReaderFor(levelTwo, shallowLimits);
                auto firstChild = ReadCompositeChild(kind, shallowReader);
                REQUIRE(firstChild.HasValue());
                auto firstChildReader = std::move(firstChild).Value();
                CHECK(ReadCompositeChild(kind, firstChildReader).HasError());
            }
        }
    }

    TEST_CASE("Canonical decoder rejects decoded-memory amplification", "[runtime][save][canonical-codec]") {
        CanonicalValueWriter writer;
        REQUIRE(writer.WriteUtf8("12345678").HasValue());
        const auto encoded = Finish(std::move(writer));
        CanonicalCodecLimits limits;
        limits.maximumDecodedBytes = 12;
        auto reader = ReaderFor(encoded, limits);
        const auto decoded = reader.ReadUtf8();
        REQUIRE(decoded.HasError());
        CHECK(decoded.ErrorValue().code.Value() == SaveErrors::CanonicalCodecLimitExceeded.code.Value());
    }

    TEST_CASE("Canonical decoded-memory budget is shared across child readers", "[runtime][save][canonical-codec]") {
        CanonicalValueWriter textWriter;
        REQUIRE(textWriter.WriteUtf8("four").HasValue());
        const auto text = Finish(std::move(textWriter));
        const std::array children{text, text};
        CanonicalValueWriter sequenceWriter;
        REQUIRE(sequenceWriter.WriteSequence(children).HasValue());
        const auto encoded = Finish(std::move(sequenceWriter));

        CanonicalCodecLimits limits;
        limits.maximumDecodedBytes = 2 * sizeof(CanonicalDecodedValue) + 8;
        auto reader = ReaderFor(encoded, limits);
        auto decoded = reader.ReadSequence();
        REQUIRE(decoded.HasValue());
        auto first = decoded.Value()[0].OpenReader();
        REQUIRE(first.HasValue());
        auto firstReader = std::move(first).Value();
        CHECK(firstReader.ReadUtf8().HasValue());
        auto second = decoded.Value()[1].OpenReader();
        REQUIRE(second.HasValue());
        auto secondReader = std::move(second).Value();
        const auto exhausted = secondReader.ReadUtf8();
        REQUIRE(exhausted.HasError());
        CHECK(exhausted.ErrorValue().code.Value() == SaveErrors::CanonicalCodecLimitExceeded.code.Value());
    }

    TEST_CASE("Canonical collection readers reject forged counts before decoded allocation", "[runtime][save][canonical-codec]") {
        const std::array forgedCount{std::byte{1}, std::byte{}, std::byte{}, std::byte{}};
        CanonicalCodecLimits limits;
        limits.maximumDecodedBytes = 1;
        auto sequence = CanonicalValueReader::Create(forgedCount, limits);
        auto set = CanonicalValueReader::Create(forgedCount, limits);
        auto map = CanonicalValueReader::Create(forgedCount, limits);
        auto record = CanonicalValueReader::Create(forgedCount, limits);
        REQUIRE(sequence.HasValue());
        REQUIRE(set.HasValue());
        REQUIRE(map.HasValue());
        REQUIRE(record.HasValue());
        CHECK(std::move(sequence).Value().ReadSequence().ErrorValue().code.Value() == SaveErrors::CanonicalCodecCorrupt.code.Value());
        CHECK(std::move(set).Value().ReadSet().ErrorValue().code.Value() == SaveErrors::CanonicalCodecCorrupt.code.Value());
        CHECK(std::move(map).Value().ReadMap().ErrorValue().code.Value() == SaveErrors::CanonicalCodecCorrupt.code.Value());
        CHECK(std::move(record).Value().ReadRecord().ErrorValue().code.Value() == SaveErrors::CanonicalCodecCorrupt.code.Value());
    }

    TEST_CASE("Canonical opaque bytes round trip with an explicit caller bound", "[runtime][save][canonical-codec]") {
        const std::array bytes{std::byte{0x00}, std::byte{0x7f}, std::byte{0xff}};
        CanonicalValueWriter writer;
        REQUIRE(writer.WriteBytes(bytes).HasValue());
        const auto encoded = Finish(std::move(writer));
        auto reader = ReaderFor(encoded);
        const auto decoded = reader.ReadBytes(bytes.size());
        REQUIRE(decoded.HasValue());
        CHECK(std::ranges::equal(decoded.Value(), bytes));
    }

    TEST_CASE("Canonical malformed UTF-8 and floating special values are corrupt wire", "[runtime][save][canonical-codec]") {
        const std::array malformedUtf8{std::byte{2}, std::byte{}, std::byte{}, std::byte{}, std::byte{0xc0}, std::byte{0x80}};
        auto utf8Reader = CanonicalValueReader::Create(malformedUtf8);
        REQUIRE(utf8Reader.HasValue());
        CHECK(std::move(utf8Reader).Value().ReadUtf8().ErrorValue().code.Value() == SaveErrors::CanonicalCodecCorrupt.code.Value());

        const std::array negativeZero{std::byte{}, std::byte{}, std::byte{}, std::byte{0x80}};
        auto negativeZeroReader = CanonicalValueReader::Create(negativeZero);
        REQUIRE(negativeZeroReader.HasValue());
        CHECK(std::move(negativeZeroReader).Value().ReadFloat32().ErrorValue().code.Value() ==
              SaveErrors::CanonicalCodecCorrupt.code.Value());

        CanonicalValueWriter writer;
        CHECK(writer.WriteFloat64(std::numeric_limits<double>::infinity()).HasError());
    }

    TEST_CASE("Canonical field context is fallible and shared by nested diagnostics", "[runtime][save][canonical-codec]") {
        auto outerResult = CanonicalValueWriter{}.ForField(Field(3));
        REQUIRE(outerResult.HasValue());
        auto outer = std::move(outerResult).Value();
        auto innerResult = outer.ForField(Field(7));
        REQUIRE(innerResult.HasValue());
        auto inner = std::move(innerResult).Value();
        const auto failure = inner.WriteFloat32(std::numeric_limits<float>::quiet_NaN());
        REQUIRE(failure.HasError());
        REQUIRE(failure.ErrorValue().diagnostics.size() == 1);
        CHECK(failure.ErrorValue().diagnostics[0].location.source == "canonical/field:3/field:7");

        const std::array fields{CanonicalRecordField{Field(1), EncodeUInt32(7)}};
        CanonicalValueWriter recordWriter;
        REQUIRE(recordWriter.WriteRecord(fields).HasValue());
        const auto encoded = Finish(std::move(recordWriter));
        CanonicalCodecLimits limits;
        limits.maximumDecodedBytes = sizeof(CanonicalDecodedField);
        auto reader = ReaderFor(encoded, limits);
        const auto decoded = reader.ReadRecord();
        REQUIRE(decoded.HasError());
        CHECK(decoded.ErrorValue().code.Value() == SaveErrors::CanonicalCodecLimitExceeded.code.Value());
    }

    TEST_CASE("Canonical reader reports exact byte offsets and corruption category", "[runtime][save][canonical-codec]") {
        const std::array truncated{std::byte{2}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{'a'}};
        auto created = CanonicalValueReader::Create(truncated);
        REQUIRE(created.HasValue());
        auto reader = std::move(created).Value();
        const auto decoded = reader.ReadUtf8();
        REQUIRE(decoded.HasError());
        CHECK(reader.ByteOffset() == sizeof(std::uint32_t));
        CHECK(decoded.ErrorValue().code.Value() == SaveErrors::CanonicalCodecCorrupt.code.Value());
        REQUIRE(decoded.ErrorValue().diagnostics.size() == 1);
        CHECK(decoded.ErrorValue().diagnostics[0].location.column == sizeof(std::uint32_t));
        const auto diagnostic = MakeSaveDiagnosticRecord(decoded.ErrorValue());
        REQUIRE(diagnostic.HasValue());
        CHECK(diagnostic.Value().Category() == SaveFailureCategory::Corruption);
    }

    TEST_CASE("Canonical writer failure is sticky and exposes no partial value", "[runtime][save][canonical-codec]") {
        CanonicalCodecLimits limits;
        limits.maximumBytes = 8;
        limits.maximumStringBytes = 8;
        CanonicalValueWriter writer{limits};
        REQUIRE(writer.WriteUInt8(1).HasValue());
        const std::array child{EncodeUInt32(7)};
        const auto failed = writer.WriteSequence(child);
        REQUIRE(failed.HasError());
        CHECK(failed.ErrorValue().code.Value() == SaveErrors::CanonicalCodecLimitExceeded.code.Value());
        CHECK(writer.WriteUInt8(2).HasError());
        const auto finalized = std::move(writer).Finalize();
        REQUIRE(finalized.HasError());
        CHECK(finalized.ErrorValue().code.Value() == SaveErrors::CanonicalCodecLimitExceeded.code.Value());
    }

    TEST_CASE("Canonical codec rejects duplicate canonical identities", "[runtime][save][canonical-codec]") {
        const auto key = EncodeUInt32(1);
        const std::vector<CanonicalMapEntry> duplicateMap{{key, EncodeUInt32(10)}, {key, EncodeUInt32(11)}};
        CanonicalValueWriter writer;
        CHECK(writer.WriteMap(duplicateMap).HasError());
        CHECK(std::move(writer).Finalize().HasError());

        const std::vector<CanonicalEncodedValue> duplicateSet{key, key};
        CanonicalValueWriter setWriter;
        CHECK(setWriter.WriteSet(duplicateSet).HasError());

        const std::vector<CanonicalRecordField> duplicateRecord{{Field(1), key}, {Field(1), EncodeUInt32(2)}};
        CanonicalValueWriter recordWriter;
        CHECK(recordWriter.WriteRecord(duplicateRecord).HasError());

        CanonicalValueWriter oneEntry;
        REQUIRE(oneEntry.WriteMap(std::span{duplicateMap.data(), 1}).HasValue());
        const auto canonical = Finish(std::move(oneEntry));
        std::vector<std::byte> malformed{canonical.Bytes().begin(), canonical.Bytes().end()};
        const std::vector<std::byte> repeatedEntry{malformed.begin() + 4, malformed.end()};
        malformed.insert(malformed.end(), repeatedEntry.begin(), repeatedEntry.end());
        malformed[0] = std::byte{2};
        auto malformedReader = CanonicalValueReader::Create(malformed);
        REQUIRE(malformedReader.HasValue());
        auto malformedValueReader = std::move(malformedReader).Value();
        const auto decoded = malformedValueReader.ReadMap();
        REQUIRE(decoded.HasError());
        CHECK(decoded.ErrorValue().code.Value() == SaveErrors::CanonicalCodecCorrupt.code.Value());
    }

    TEST_CASE("Canonical codec separates caller validation, configuration, and corrupt wire", "[runtime][save][canonical-codec]") {
        CanonicalValueWriter writer;
        const auto nonFinite = writer.WriteFloat32(std::numeric_limits<float>::quiet_NaN());
        REQUIRE(nonFinite.HasError());
        CHECK(nonFinite.ErrorValue().code.Value() == SaveErrors::CanonicalCodecNonFinite.code.Value());

        CanonicalCodecLimits invalidLimits;
        invalidLimits.maximumBytes = 0;
        const auto invalid = CanonicalValueReader::Create({}, invalidLimits);
        REQUIRE(invalid.HasError());
        CHECK(invalid.ErrorValue().code.Value() == SaveErrors::CanonicalCodecConfigurationInvalid.code.Value());

        CanonicalValueWriter invalidWriter{invalidLimits};
        const auto invalidFloat = invalidWriter.WriteFloat32(std::numeric_limits<float>::quiet_NaN());
        REQUIRE(invalidFloat.HasError());
        CHECK(invalidFloat.ErrorValue().code.Value() == SaveErrors::CanonicalCodecConfigurationInvalid.code.Value());
        CanonicalValueWriter invalidFieldWriter{invalidLimits};
        CHECK(invalidFieldWriter.ForField(Field(1)).ErrorValue().code.Value() ==
              SaveErrors::CanonicalCodecConfigurationInvalid.code.Value());

        const std::array invalidBool{std::byte{2}};
        auto boolReader = CanonicalValueReader::Create(invalidBool);
        REQUIRE(boolReader.HasValue());
        auto invalidBoolReader = std::move(boolReader).Value();
        const auto corrupt = invalidBoolReader.ReadBool();
        REQUIRE(corrupt.HasError());
        CHECK(corrupt.ErrorValue().code.Value() == SaveErrors::CanonicalCodecCorrupt.code.Value());
    }
}  // namespace
