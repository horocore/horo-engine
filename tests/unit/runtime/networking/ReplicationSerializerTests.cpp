#include "Horo/Network/ReplicationSerializer.h"
#include "ReplicationDescriptorTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace Horo::Network {
    using namespace TestSupport;

    namespace {
        ReplicationSerializerDescriptor SerializerDescriptor(const ReplicationValueKind kind = ReplicationValueKind::FloatingPoint,
                                                             const ReplicationQuantization quantization = {}) {
            return {.valueType = ValueType(1),
                    .codec = Codec(1),
                    .owner = {.value = "game.replication"},
                    .valueKind = kind,
                    .quantization = quantization,
                    .maximumEncodedBytes = 8,
                    .maximumElementCount = 1};
        }

        ReplicationDescriptorSnapshotPtr Schemas(const ReplicationFieldDescriptor &field = Field()) {
            const std::array descriptors{Schema(10, {field})};
            return BuildReplicationDescriptorSnapshot(descriptors, Limits).Value();
        }

        std::shared_ptr<const CanonicalScalarReplicationSerializer> Scalar(
            const ReplicationValueKind kind = ReplicationValueKind::FloatingPoint, const ReplicationQuantization quantization = {}) {
            return CanonicalScalarReplicationSerializer::Create(SerializerDescriptor(kind, quantization)).Value();
        }

        ReplicationSerializerRegistry Registry(const ReplicationValueKind kind = ReplicationValueKind::FloatingPoint,
                                               const ReplicationQuantization quantization = {}) {
            const std::array<std::shared_ptr<const IReplicationFieldSerializer>, 1> serializers{Scalar(kind, quantization)};
            return ReplicationSerializerRegistry::Create(Schemas(), serializers).Value();
        }

        class OversizedSerializer final : public IReplicationFieldSerializer {
        public:
            explicit OversizedSerializer(ReplicationSerializerDescriptor descriptor) : descriptor_(std::move(descriptor)) {}

            const ReplicationSerializerDescriptor &Descriptor() const noexcept override {
                return descriptor_;
            }

            Result<std::vector<std::byte>> Encode(const ReplicationRuntimeValue &) const override {
                return Result<std::vector<std::byte>>::Success(std::vector<std::byte>(descriptor_.maximumEncodedBytes + 1));
            }

            Result<ReplicationRuntimeValue> Decode(const std::span<const std::byte>) const override {
                return Result<ReplicationRuntimeValue>::Success(0.0);
            }

        private:
            ReplicationSerializerDescriptor descriptor_;
        };
    }  // namespace

    TEST_CASE("Replication serializer registry requires exact complete typed bindings", "[unit][network][replication][serializer]") {
        const auto schemas = Schemas();
        const std::array<std::shared_ptr<const IReplicationFieldSerializer>, 1> serializers{Scalar()};
        REQUIRE(ReplicationSerializerRegistry::Create(schemas, serializers).HasValue());

        RequireError(ReplicationSerializerRegistry::Create(schemas, {}), NetworkErrors::ReplicationSerializerUnknown);

        const std::array duplicates{serializers.front(), serializers.front()};
        RequireError(ReplicationSerializerRegistry::Create(schemas, duplicates), NetworkErrors::ReplicationSerializerConflict);

        auto foreignDescriptor = SerializerDescriptor();
        foreignDescriptor.owner.value = "other.module";
        const auto foreignAdapter = CanonicalScalarReplicationSerializer::Create(foreignDescriptor).Value();
        const std::array<std::shared_ptr<const IReplicationFieldSerializer>, 1> foreignContribution{foreignAdapter};
        RequireError(ReplicationSerializerRegistry::Create(schemas, foreignContribution), NetworkErrors::ReplicationSerializerUnknown);

        ReplicationSerializerRegistryLimits noCapacity;
        noCapacity.maximumSerializers = 0;
        RequireError(ReplicationSerializerRegistry::Create(schemas, serializers, noCapacity), NetworkErrors::ReplicationSerializerInvalid);
    }

    TEST_CASE("Canonical scalar serializers use deterministic big endian bytes", "[unit][network][replication][serializer]") {
        const auto signedRegistry = Registry(ReplicationValueKind::SignedInteger);
        const auto encoded = signedRegistry.Encode(SchemaId(10), FieldIdValue(1), std::int64_t{0x0102030405060708});
        REQUIRE(encoded.HasValue());
        REQUIRE(encoded.Value().canonicalBytes == std::vector<std::byte>{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
                                                                         std::byte{0x05}, std::byte{0x06}, std::byte{0x07},
                                                                         std::byte{0x08}});
        REQUIRE(std::get<std::int64_t>(signedRegistry.Decode(SchemaId(10), FieldIdValue(1), encoded.Value()).Value()) ==
                std::int64_t{0x0102030405060708});

        const auto floatingRegistry = Registry();
        REQUIRE(floatingRegistry.CanonicallyEqual(SchemaId(10), FieldIdValue(1), 0.0, -0.0).Value());
        const auto first = floatingRegistry.Encode(SchemaId(10), FieldIdValue(1), 12.5).Value();
        const auto second = floatingRegistry.Encode(SchemaId(10), FieldIdValue(1), 12.5).Value();
        REQUIRE(first == second);
    }

    TEST_CASE("Canonical scalar serializers round trip every admitted primitive kind", "[unit][network][replication][serializer]") {
        const auto booleanRegistry = Registry(ReplicationValueKind::Boolean);
        const auto encodedTrue = booleanRegistry.Encode(SchemaId(10), FieldIdValue(1), true).Value();
        REQUIRE(encodedTrue.canonicalBytes == std::vector<std::byte>{std::byte{1}});
        REQUIRE(std::get<bool>(booleanRegistry.Decode(SchemaId(10), FieldIdValue(1), encodedTrue).Value()));
        auto invalidBoolean = encodedTrue;
        invalidBoolean.canonicalBytes.front() = std::byte{2};
        RequireError(booleanRegistry.Decode(SchemaId(10), FieldIdValue(1), invalidBoolean),
                     NetworkErrors::ReplicationSerializerValueInvalid);

        const auto unsignedRegistry = Registry(ReplicationValueKind::UnsignedInteger);
        const auto encodedMaximum =
            unsignedRegistry.Encode(SchemaId(10), FieldIdValue(1), std::numeric_limits<std::uint64_t>::max()).Value();
        REQUIRE(std::get<std::uint64_t>(unsignedRegistry.Decode(SchemaId(10), FieldIdValue(1), encodedMaximum).Value()) ==
                std::numeric_limits<std::uint64_t>::max());

        const auto signedRegistry = Registry(ReplicationValueKind::SignedInteger);
        const auto encodedMinimum = signedRegistry.Encode(SchemaId(10), FieldIdValue(1), std::numeric_limits<std::int64_t>::min()).Value();
        REQUIRE(std::get<std::int64_t>(signedRegistry.Decode(SchemaId(10), FieldIdValue(1), encodedMinimum).Value()) ==
                std::numeric_limits<std::int64_t>::min());
    }

    TEST_CASE("Floating quantization defines canonical comparison without mutating gameplay values",
              "[unit][network][replication][serializer]") {
        const ReplicationQuantization quantization{ReplicationQuantizationMode::NearestStep, 0.25};
        const auto registry = Registry(ReplicationValueKind::FloatingPoint, quantization);
        REQUIRE(registry.CanonicallyEqual(SchemaId(10), FieldIdValue(1), 1.12, 1.10).Value());
        REQUIRE_FALSE(registry.CanonicallyEqual(SchemaId(10), FieldIdValue(1), 1.12, 1.14).Value());

        const auto encoded = registry.Encode(SchemaId(10), FieldIdValue(1), 1.12).Value();
        REQUIRE(std::get<double>(registry.Decode(SchemaId(10), FieldIdValue(1), encoded).Value()) == 1.0);
        REQUIRE(std::get<double>(ReplicationRuntimeValue{1.12}) == 1.12);
    }

    TEST_CASE("Replication serializers reject malformed quantization and hostile scalar payloads",
              "[unit][network][replication][serializer]") {
        auto descriptor = SerializerDescriptor();
        descriptor.quantization = {ReplicationQuantizationMode::NearestStep, 0.0};
        RequireError(CanonicalScalarReplicationSerializer::Create(descriptor), NetworkErrors::ReplicationSerializerInvalid);

        descriptor.quantization = {ReplicationQuantizationMode::Exact, 1.0};
        RequireError(CanonicalScalarReplicationSerializer::Create(descriptor), NetworkErrors::ReplicationSerializerInvalid);

        descriptor = SerializerDescriptor(ReplicationValueKind::Utf8Text);
        RequireError(CanonicalScalarReplicationSerializer::Create(descriptor), NetworkErrors::ReplicationSerializerInvalid);

        const auto registry = Registry();
        RequireError(registry.Encode(SchemaId(10), FieldIdValue(1), std::numeric_limits<double>::infinity()),
                     NetworkErrors::ReplicationSerializerValueInvalid);
        RequireError(registry.Encode(SchemaId(10), FieldIdValue(1), std::uint64_t{2}), NetworkErrors::ReplicationSerializerValueInvalid);

        auto encoded = registry.Encode(SchemaId(10), FieldIdValue(1), 2.0).Value();
        encoded.canonicalBytes.pop_back();
        RequireError(registry.Decode(SchemaId(10), FieldIdValue(1), encoded), NetworkErrors::ReplicationSerializerValueInvalid);

        encoded = registry.Encode(SchemaId(10), FieldIdValue(1), 0.0).Value();
        encoded.canonicalBytes.front() = std::byte{0x80};
        RequireError(registry.Decode(SchemaId(10), FieldIdValue(1), encoded), NetworkErrors::ReplicationSerializerValueInvalid);

        const auto quantized =
            Registry(ReplicationValueKind::FloatingPoint, {ReplicationQuantizationMode::NearestStep, std::numeric_limits<double>::min()});
        RequireError(quantized.Encode(SchemaId(10), FieldIdValue(1), std::numeric_limits<double>::max()),
                     NetworkErrors::ReplicationSerializerCapacityExceeded);
    }

    TEST_CASE("Replication decode rejects incompatible tags and unknown fields without adapter calls",
              "[unit][network][replication][serializer]") {
        const auto registry = Registry();
        auto encoded = registry.Encode(SchemaId(10), FieldIdValue(1), 3.0).Value();
        encoded.codec = Codec(99);
        RequireError(registry.Decode(SchemaId(10), FieldIdValue(1), encoded), NetworkErrors::ReplicationSerializerIncompatible);
        RequireError(registry.Encode(SchemaId(10), FieldIdValue(99), 3.0), NetworkErrors::ReplicationSerializerUnknown);
        RequireError(registry.Encode(SchemaId(99), FieldIdValue(1), 3.0), NetworkErrors::ReplicationSchemaUnknown);
    }

    TEST_CASE("Compatible minor schemas bind added fields only through their exact codec identity",
              "[unit][network][replication][serializer]") {
        const auto previous = Schemas();
        auto compatible = Schema();
        compatible.version = {1, 1};
        compatible.compatibility.maximum = compatible.version;
        auto added = Field(2, {1, 1});
        added.requirement = ReplicationFieldRequirement::Optional;
        added.canonicalDefault = ReplicationFieldDefault{std::vector<std::byte>(8)};
        compatible.fields.push_back(added);
        const std::array descriptors{compatible};
        const auto replacement = BuildReplicationDescriptorReplacement(previous, descriptors, Limits).Value();
        const std::array<std::shared_ptr<const IReplicationFieldSerializer>, 1> serializers{Scalar()};
        const auto registry = ReplicationSerializerRegistry::Create(replacement, serializers);
        REQUIRE(registry.HasValue());
        REQUIRE(registry.Value().Encode(SchemaId(10), FieldIdValue(2), 5.0).HasValue());

        compatible.fields.back().canonicalDefault = ReplicationFieldDefault{{std::byte{0}}};
        const std::array malformedDefaultDescriptors{compatible};
        const auto malformedDefault = BuildReplicationDescriptorSnapshot(malformedDefaultDescriptors, Limits).Value();
        RequireError(ReplicationSerializerRegistry::Create(malformedDefault, serializers),
                     NetworkErrors::ReplicationSerializerValueInvalid);

        compatible.fields.back().canonicalDefault = ReplicationFieldDefault{std::vector<std::byte>(8)};
        compatible.fields.back().codec = Codec(2);
        const std::array incompatibleDescriptors{compatible};
        const auto independentlyValidated = BuildReplicationDescriptorSnapshot(incompatibleDescriptors, Limits).Value();
        RequireError(ReplicationSerializerRegistry::Create(independentlyValidated, serializers),
                     NetworkErrors::ReplicationSerializerUnknown);
    }

    TEST_CASE("Schema field limits fence adapter output and registry composition", "[unit][network][replication][serializer]") {
        auto narrow = Field();
        narrow.limits.maximumEncodedBytes = 4;
        const std::array<std::shared_ptr<const IReplicationFieldSerializer>, 1> serializers{Scalar()};
        const auto registry = ReplicationSerializerRegistry::Create(Schemas(narrow), serializers).Value();
        RequireError(registry.Encode(SchemaId(10), FieldIdValue(1), 3.0), NetworkErrors::ReplicationSerializerCapacityExceeded);

        auto insufficient = SerializerDescriptor();
        insufficient.maximumEncodedBytes = 4;
        const auto oversized = std::make_shared<OversizedSerializer>(insufficient);
        const std::array<std::shared_ptr<const IReplicationFieldSerializer>, 1> contributions{oversized};
        RequireError(ReplicationSerializerRegistry::Create(Schemas(), contributions), NetworkErrors::ReplicationSerializerCapacityExceeded);
    }

    TEST_CASE("Immutable serializer registry pins schema and adapter lifetimes", "[unit][network][replication][serializer]") {
        std::weak_ptr<const IReplicationFieldSerializer> adapterLifetime;
        std::weak_ptr<const ReplicationDescriptorSnapshot> schemaLifetime;
        std::optional<ReplicationSerializerRegistry> registry;
        {
            const auto schemas = Schemas();
            const std::shared_ptr<const IReplicationFieldSerializer> serializer = Scalar();
            adapterLifetime = serializer;
            schemaLifetime = schemas;
            const std::array contributions{serializer};
            registry = ReplicationSerializerRegistry::Create(schemas, contributions).Value();
        }
        REQUIRE_FALSE(adapterLifetime.expired());
        REQUIRE_FALSE(schemaLifetime.expired());
        REQUIRE(registry->Encode(SchemaId(10), FieldIdValue(1), 4.0).HasValue());
        registry.reset();
        REQUIRE(adapterLifetime.expired());
        REQUIRE(schemaLifetime.expired());
    }
}  // namespace Horo::Network
