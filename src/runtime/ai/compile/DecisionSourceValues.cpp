#include "DecisionSourceWire.h"

#include <bit>
#include <cmath>

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
                default:
                    return false;
            }
        }

        void WriteOpaque(DecisionWriter &writer, const BehaviorTreeOpaqueProperty &value) {
            writer.Unsigned(value.contractVersion, 4);
            writer.Unsigned(value.bytes.size(), 4);
            writer.Bytes(value.bytes);
        }

        void WritePropertyValue(DecisionWriter &writer, const BehaviorTreePropertyValue &value) {
            std::visit([&](const auto &item) {
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

    void WriteScalar(DecisionWriter &writer, const BlackboardScalarValue &value) {
        if (const auto *scalar = std::get_if<double>(&value); scalar && !std::isfinite(*scalar)) {
            writer.Reject();
            return;
        }
        writer.Unsigned(value.index(), 1);
        std::visit([&](const auto &item) {
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

    BlackboardScalarValue ReadScalar(DecisionReader &reader) {
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
