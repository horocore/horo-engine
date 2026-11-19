#include "DecisionSourceWire.h"
#include "Horo/AI/DecisionPlanCodec.h"

#include <bit>
#include <cmath>
#include <new>

namespace Horo::AI::Detail {
    namespace {
        bool ScalarProperty(const BehaviorTreePropertyValue &value) noexcept {
            const auto *scalar = std::get_if<double>(&value);
            return scalar && std::isfinite(*scalar);
        }

        bool ReferenceProperty(const BehaviorTreePropertyValue &value) noexcept {
            const auto *reference = std::get_if<BehaviorTreeNodeId>(&value);
            return reference && reference->IsValid();
        }

        bool OpaqueProperty(const BehaviorTreePropertyValue &value) noexcept {
            const auto *opaque = std::get_if<BehaviorTreeOpaqueProperty>(&value);
            return opaque && opaque->contractVersion != 0 && opaque->bytes.size() <= BehaviorTreeAssetHardLimits::PropertyBytes;
        }

        bool PropertyVariantMatches(const BehaviorTreeProperty &property) noexcept {
            switch (property.kind) {
                using enum Horo::AI::BehaviorTreePropertyKind;
                case Boolean:
                    return std::holds_alternative<bool>(property.value);
                case SignedInteger:
                    return std::holds_alternative<std::int64_t>(property.value);
                case Scalar:
                    return ScalarProperty(property.value);
                case String:
                    return std::holds_alternative<std::string>(property.value);
                case NodeReference:
                    return ReferenceProperty(property.value);
                case Opaque:
                    return OpaqueProperty(property.value);
                case QueryOutput: {
                    const auto *output = std::get_if<BehaviorTreeQueryOutput>(&property.value);
                    return output && output->IsValid();
                }
                default:
                    return false;
            }
        }

        void WriteOpaque(DecisionWriter &writer, const BehaviorTreeOpaqueProperty &value) {
            writer.Unsigned(value.contractVersion, 4);
            writer.Unsigned(value.bytes.size(), 4);
            writer.Bytes(value.bytes);
        }

        void WriteQueryOutput(DecisionWriter &writer, const BehaviorTreeQueryOutput &output) {
            writer.IdentityValue(output.query);
            writer.IdentityValue(output.result);
            writer.Unsigned(output.resultVersion, 4);
            writer.IdentityValue(output.item);
            writer.IdentityValue(output.key);
            writer.Unsigned(output.timeoutTicks);
        }

        void WritePropertyValue(DecisionWriter &writer, const BehaviorTreePropertyValue &value) {
            std::visit([&]<typename VisitedValue>(const VisitedValue &item) {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<T, bool>)
                    writer.Unsigned(item, 1);
                else if constexpr (std::is_same_v<T, std::int64_t>)
                    writer.Unsigned(std::bit_cast<std::uint64_t>(item));
                else if constexpr (std::is_same_v<T, double>)
                    writer.Unsigned(std::bit_cast<std::uint64_t>(item));
                else if constexpr (std::is_same_v<T, std::string>)
                    writer.String(item);
                else if constexpr (std::is_same_v<T, BehaviorTreeNodeId>)
                    writer.IdentityValue(item);
                else if constexpr (std::is_same_v<T, BehaviorTreeOpaqueProperty>)
                    WriteOpaque(writer, item);
                else if constexpr (std::is_same_v<T, BehaviorTreeQueryOutput>)
                    WriteQueryOutput(writer, item);
            }, value);
        }

        BehaviorTreePropertyValue ReadPropertyValue(DecisionReader &reader, BehaviorTreePropertyKind kind) {
            using enum BehaviorTreePropertyKind;
            switch (kind) {
                case Boolean:
                    return reader.Boolean();
                case SignedInteger:
                    return std::bit_cast<std::int64_t>(reader.Unsigned());
                case Scalar: {
                    const auto value = std::bit_cast<double>(reader.Unsigned());
                    if (!std::isfinite(value))
                        reader.Reject();
                    return value;
                }
                case String:
                    return reader.String();
                case NodeReference:
                    return reader.IdentityValue<BehaviorTreeNodeId>();
                case Opaque: {
                    BehaviorTreeOpaqueProperty value;
                    value.contractVersion = static_cast<std::uint32_t>(reader.Unsigned(4));
                    const auto bytes = reader.Bytes(reader.Count(BehaviorTreeAssetHardLimits::PropertyBytes));
                    value.bytes.assign(bytes.begin(), bytes.end());
                    return value;
                }
                case QueryOutput:
                    return BehaviorTreeQueryOutput{reader.IdentityValue<QueryId>(),
                                                   reader.IdentityValue<QueryResultSchemaId>(),
                                                   static_cast<std::uint32_t>(reader.Unsigned(4)),
                                                   reader.IdentityValue<QueryItemTypeId>(),
                                                   reader.IdentityValue<BlackboardKeyId>(),
                                                   reader.Unsigned()};
                default:
                    reader.Reject();
                    return {};
            }
        }

        void WriteReference(DecisionWriter &writer, const BlackboardStoredEntityReference &value) {
            writer.Unsigned(value.sceneIncarnation);
            writer.Unsigned(value.slot, 4);
            writer.Unsigned(value.generation, 4);
        }

        void WriteReference(DecisionWriter &writer, const BlackboardStoredAssetReference &value) {
            writer.Bytes(value.canonicalBytes);
        }

        void WriteCoordinate(DecisionWriter &writer, const Math::WorldCoordinate64 &value) {
            for (const auto coordinate : value.Millimeters())
                writer.Unsigned(std::bit_cast<std::uint64_t>(coordinate));
        }

        BlackboardScalarValue ReadReference(DecisionReader &reader, BlackboardValueKind kind) {
            if (kind == BlackboardValueKind::EntityReference) {
                BlackboardStoredEntityReference value{reader.Unsigned(), static_cast<std::uint32_t>(reader.Unsigned(4)),
                                                      static_cast<std::uint32_t>(reader.Unsigned(4))};
                if (!value.IsValid())
                    reader.Reject();
                return value;
            }
            BlackboardStoredAssetReference value;
            const auto bytes = reader.Bytes(value.canonicalBytes.size());
            std::ranges::copy(bytes, value.canonicalBytes.begin());
            if (!value.IsValid())
                reader.Reject();
            return value;
        }
    }  // namespace

    void WriteLocation(DecisionWriter &writer, const SourceLocation &value) {
        writer.String(value.source);
        writer.Unsigned(value.line, 4);
        writer.Unsigned(value.column, 4);
    }

    SourceLocation ReadLocation(DecisionReader &reader) {
        SourceLocation value{reader.String(), static_cast<std::uint32_t>(reader.Unsigned(4)),
                             static_cast<std::uint32_t>(reader.Unsigned(4))};
        if (value.line == 0 && value.column != 0)
            reader.Reject();
        return value;
    }

    void WriteRequirement(DecisionWriter &writer, const DecisionBlackboardRequirement &value) {
        writer.IdentityValue(value.key);
        writer.Enumeration(value.kind);
        writer.Enumeration(value.cardinality);
        writer.Enumeration(value.access);
        writer.Enumeration(value.presence);
        writer.Unsigned(value.requireDefault, 1);
        WriteLocation(writer, value.source);
    }

    void WriteRequirements(DecisionWriter &writer, std::span<const DecisionBlackboardRequirement> values) {
        if (!writer.Count(values.size(), DecisionAssetValidationHardLimits::RequirementsPerNode))
            return;
        std::vector<const DecisionBlackboardRequirement *> ordered;
        for (const auto &value : values)
            ordered.push_back(&value);
        std::ranges::sort(ordered, {}, [](const auto *value) {
            return value->key;
        });
        for (const auto *value : ordered)
            WriteRequirement(writer, *value);
    }

    DecisionBlackboardRequirement ReadRequirement(DecisionReader &reader) {
        DecisionBlackboardRequirement value;
        value.key = reader.IdentityValue<BlackboardKeyId>();
        value.kind = reader.Enumeration(BlackboardValueKind::Count);
        value.cardinality = reader.Enumeration(BlackboardValueCardinality::Count);
        value.access = reader.Enumeration(BlackboardKeyAccess::Count);
        value.presence = reader.Enumeration(BlackboardKeyPresence::Count);
        value.requireDefault = reader.Boolean();
        value.source = ReadLocation(reader);
        return value;
    }

    void WriteProperty(DecisionWriter &writer, const BehaviorTreeProperty &value) {
        if (!value.id.IsValid() || !PropertyVariantMatches(value)) {
            writer.Reject();
            return;
        }
        writer.IdentityValue(value.id);
        writer.String(value.displayName, BehaviorTreeAssetHardLimits::DisplayNameBytes);
        writer.Enumeration(value.kind);
        WritePropertyValue(writer, value.value);
    }

    BehaviorTreeProperty ReadProperty(DecisionReader &reader) {
        BehaviorTreeProperty value;
        value.id = reader.IdentityValue<BehaviorTreePropertyId>();
        value.displayName = reader.String(BehaviorTreeAssetHardLimits::DisplayNameBytes);
        value.kind = reader.Enumeration(BehaviorTreePropertyKind::Count);
        value.value = ReadPropertyValue(reader, value.kind);
        return value;
    }

    void WriteScalar(DecisionWriter &writer, const BlackboardScalarValue &value, const WriteBlackboardEntity &entity) {
        if (const auto *scalar = std::get_if<double>(&value); scalar && !std::isfinite(*scalar)) {
            writer.Reject();
            return;
        }
        writer.Unsigned(value.index(), 1);
        if (const auto *reference = std::get_if<BlackboardStoredEntityReference>(&value); reference && entity) {
            entity(writer, *reference);
            return;
        }
        std::visit([&]<typename VisitedValue>(const VisitedValue &item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, bool>)
                writer.Unsigned(item, 1);
            else if constexpr (std::is_same_v<T, std::int64_t>)
                writer.Unsigned(std::bit_cast<std::uint64_t>(item));
            else if constexpr (std::is_same_v<T, double>)
                writer.Unsigned(std::bit_cast<std::uint64_t>(item));
            else if constexpr (std::is_same_v<T, Math::WorldCoordinate64>)
                WriteCoordinate(writer, item);
            else
                WriteReference(writer, item);
        }, value);
    }

    BlackboardScalarValue ReadScalar(DecisionReader &reader, const ReadBlackboardEntity &entity) {
        const auto kind = reader.Enumeration(BlackboardValueKind::Count);
        using enum BlackboardValueKind;
        switch (kind) {
            case Boolean:
                return reader.Boolean();
            case SignedInteger:
                return std::bit_cast<std::int64_t>(reader.Unsigned());
            case Scalar: {
                const auto value = std::bit_cast<double>(reader.Unsigned());
                if (!std::isfinite(value))
                    reader.Reject();
                return value;
            }
            case EntityReference:
                return entity ? BlackboardScalarValue{entity(reader)} : ReadReference(reader, kind);
            case AssetReference:
                return ReadReference(reader, kind);
            case WorldCoordinate: {
                const auto x = std::bit_cast<std::int64_t>(reader.Unsigned());
                const auto y = std::bit_cast<std::int64_t>(reader.Unsigned());
                const auto z = std::bit_cast<std::int64_t>(reader.Unsigned());
                return Math::WorldCoordinate64::FromMillimeters(x, y, z);
            }
            default:
                reader.Reject();
                return false;
        }
    }

    /** @copydoc WriteBlackboardValue */
    void WriteBlackboardValue(DecisionWriter &writer, const BlackboardValue &value, const WriteBlackboardEntity &entity) {
        writer.Unsigned(value.index(), 1);
        std::visit([&]<typename VisitedValue>(const VisitedValue &item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, BlackboardScalarValue>)
                WriteScalar(writer, item, entity);
            else if constexpr (std::is_same_v<T, BlackboardCollectionValue>) {
                writer.Enumeration(item.elementKind);
                if (!writer.Count(item.count, MaximumBlackboardCollectionElements))
                    return;
                for (std::size_t index = 0; index < item.count; ++index)
                    WriteScalar(writer, item.elements[index], entity);
            } else {
                writer.Unsigned(item.schemaVersion, 4);
                writer.Unsigned(item.serializedType, 4);
                if (!writer.Count(item.size, MaximumBlackboardOpaqueBytes))
                    return;
                writer.Bytes({reinterpret_cast<const std::uint8_t *>(item.bytes.data()), item.size});
            }
        }, value);
    }

    /** @copydoc ReadBlackboardValue */
    BlackboardValue ReadBlackboardValue(DecisionReader &reader, const ReadBlackboardEntity &entity) {
        switch (reader.Unsigned(1)) {
            case 0:
                return ReadScalar(reader, entity);
            case 1: {
                BlackboardCollectionValue value;
                value.elementKind = reader.Enumeration(BlackboardValueKind::Count);
                value.count = reader.Count(MaximumBlackboardCollectionElements);
                for (std::size_t index = 0; index < value.count && reader.Valid(); ++index)
                    value.elements[index] = ReadScalar(reader, entity);
                return value;
            }
            case 2: {
                BlackboardOpaqueValue value;
                value.schemaVersion = static_cast<std::uint32_t>(reader.Unsigned(4));
                value.serializedType = static_cast<std::uint32_t>(reader.Unsigned(4));
                value.size = reader.Count(MaximumBlackboardOpaqueBytes);
                const auto bytes = reader.Bytes(value.size);
                std::ranges::transform(bytes, value.bytes.begin(), [](std::uint8_t byte) {
                    return static_cast<std::byte>(byte);
                });
                return value;
            }
            default:
                reader.Reject();
                return BlackboardScalarValue{false};
        }
    }

    namespace {
        /** @brief Writes the stable schema declaration and its optional default. */
        void WriteKey(DecisionWriter &writer, const BlackboardKeyDescriptor &key) {
            writer.IdentityValue(key.key);
            writer.Enumeration(key.kind);
            writer.Enumeration(key.cardinality);
            writer.Unsigned(key.maximumCollectionElements, 4);
            writer.Enumeration(key.presence);
            writer.Enumeration(key.access);
            writer.Unsigned(key.defaultValue.has_value(), 1);
            if (key.defaultValue)
                WriteBlackboardValue(writer, *key.defaultValue);
        }

        /** @brief Reads one key without bypassing BlackboardSchema::Capture's semantic checks. */
        BlackboardKeyDescriptor ReadBlackboardKey(DecisionReader &reader) {
            BlackboardKeyDescriptor key;
            key.key = reader.IdentityValue<BlackboardKeyId>();
            key.kind = reader.Enumeration(BlackboardValueKind::Count);
            key.cardinality = reader.Enumeration(BlackboardValueCardinality::Count);
            key.maximumCollectionElements = static_cast<std::size_t>(reader.Unsigned(4));
            key.presence = reader.Enumeration(BlackboardKeyPresence::Count);
            key.access = reader.Enumeration(BlackboardKeyAccess::Count);
            if (reader.Boolean())
                key.defaultValue = ReadBlackboardValue(reader);
            return key;
        }
    }  // namespace

    /** @copydoc WriteBlackboardSchema */
    void WriteBlackboardSchema(DecisionWriter &writer, const BlackboardSchema &schema) {
        writer.IdentityValue(schema.Identity());
        writer.Unsigned(schema.Version(), 4);
        writer.Enumeration(schema.UnknownValuePolicy());
        WriteSequence<BlackboardKeyDescriptor>(writer, schema.Keys(), MaximumBlackboardKeys, WriteKey);
    }

    void WriteCall(DecisionWriter &writer, const std::optional<DecisionSubplanCall> &value) {
        writer.Unsigned(value.has_value(), 1);
        if (!value)
            return;
        writer.IdentityValue(value->asset);
        writer.Enumeration(value->kind);
        writer.Unsigned(value->schema.minimum, 4);
        writer.Unsigned(value->schema.maximum, 4);
    }

    std::optional<DecisionSubplanCall> ReadCall(DecisionReader &reader) {
        if (!reader.Boolean())
            return {};
        DecisionSubplanCall value;
        value.asset = reader.IdentityValue<DecisionGraphAssetId>();
        value.kind = reader.Enumeration(DecisionPlanKind::Count);
        value.schema = {static_cast<std::uint32_t>(reader.Unsigned(4)), static_cast<std::uint32_t>(reader.Unsigned(4))};
        if (!value.schema.IsValid())
            reader.Reject();
        return value;
    }
}  // namespace Horo::AI::Detail

namespace Horo::AI {
    namespace {
        constexpr std::uint64_t SchemaMagic = 0x314142424f524f48ULL;  // HOROBBA1
    }

    /** @copydoc BlackboardSchemaCodec::Encode */
    Result<std::vector<std::uint8_t>> BlackboardSchemaCodec::Encode(const BlackboardSchema &schema) {
        using Encoded = Result<std::vector<std::uint8_t>>;
        if (schema.Keys().empty())
            return Encoded::Failure(MakeError(AIErrors::BlackboardSchemaInvalid));
        try {
            Detail::DecisionWriter writer(MaximumBytes);
            writer.Unsigned(SchemaMagic);
            writer.Unsigned(Version, 4);
            Detail::WriteBlackboardSchema(writer, schema);
            if (!writer.Valid())
                return Encoded::Failure(MakeError(AIErrors::BlackboardLimitExceeded));
            return Encoded::Success(std::move(writer).Take());
        } catch (const std::bad_alloc &) {
            return Encoded::Failure(MakeError(AIErrors::BlackboardStorageUnavailable));
        }
    }

    /** @copydoc BlackboardSchemaCodec::Decode */
    Result<BlackboardSchema> BlackboardSchemaCodec::Decode(const std::span<const std::uint8_t> bytes) {
        if (bytes.size() > MaximumBytes)
            return Result<BlackboardSchema>::Failure(MakeError(AIErrors::BlackboardLimitExceeded));
        try {
            Detail::DecisionReader reader(bytes);
            if (reader.Unsigned() != SchemaMagic || reader.Unsigned(4) != Version)
                return Result<BlackboardSchema>::Failure(MakeError(AIErrors::BlackboardSchemaInvalid));
            const auto identity = reader.IdentityValue<BlackboardSchemaId>();
            const auto version = static_cast<std::uint32_t>(reader.Unsigned(4));
            const auto policy = reader.Enumeration(BlackboardUnknownValuePolicy::Count);
            const auto keys = Detail::ReadSequence<BlackboardKeyDescriptor>(reader, MaximumBlackboardKeys, Detail::ReadBlackboardKey);
            if (!reader.Complete())
                return Result<BlackboardSchema>::Failure(MakeError(AIErrors::BlackboardSchemaInvalid));
            return BlackboardSchema::Capture({identity, version, policy, keys});
        } catch (const std::bad_alloc &) {
            return Result<BlackboardSchema>::Failure(MakeError(AIErrors::BlackboardStorageUnavailable));
        }
    }
}  // namespace Horo::AI
