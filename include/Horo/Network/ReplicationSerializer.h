#pragma once

/**
 * @file ReplicationSerializer.h
 * @brief Typed bounded replication serializers and immutable adapter registry.
 */

#include "Horo/Foundation/ModuleDescriptor.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Network/ReplicationDescriptorRegistry.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace Horo::Network {
    /** @brief Closed Horo-owned runtime value representations accepted by field serializers. */
    enum class ReplicationValueKind : std::uint8_t {
        Boolean,         /**< Boolean semantic value. */
        SignedInteger,   /**< Signed 64-bit integer semantic value. */
        UnsignedInteger, /**< Unsigned 64-bit integer semantic value. */
        FloatingPoint,   /**< Finite IEEE-754 double semantic value. */
        Utf8Text,        /**< Valid UTF-8 text owned independently of gameplay storage. */
        ByteSequence,    /**< Explicit owned byte-sequence value, never an object-memory view. */
        Count            /**< Closed-set sentinel; never a valid value kind. */
    };

    /** @brief Runtime value copied into a typed adapter without exposing object addresses or layouts. */
    using ReplicationRuntimeValue = std::variant<bool, std::int64_t, std::uint64_t, double, std::string, std::vector<std::byte>>;

    /** @brief Closed quantization policy owned by the wire codec rather than gameplay state. */
    enum class ReplicationQuantizationMode : std::uint8_t {
        Exact,       /**< Preserve the canonical semantic value exactly. */
        NearestStep, /**< Encode a finite floating-point value as the nearest integral step. */
        Count        /**< Closed-set sentinel; never a valid policy. */
    };

    /** @brief Explicit deterministic quantization metadata included in serializer compatibility. */
    struct ReplicationQuantization final {
        ReplicationQuantizationMode mode{ReplicationQuantizationMode::Exact}; /**< Selected codec policy. */
        double step{}; /**< Positive finite step for NearestStep; exactly zero for Exact. */

        constexpr auto operator<=>(const ReplicationQuantization &) const noexcept = default;
    };

    /** @brief Inert serializer metadata snapshotted during host composition. */
    struct ReplicationSerializerDescriptor final {
        ReplicationValueTypeId valueType;                            /**< Exact semantic type resolved by the adapter. */
        ReplicationCodecId codec;                                    /**< Stable canonical wire codec identity. */
        ModuleId owner;                                              /**< Declaring module that owns the semantic contract. */
        ReplicationValueKind valueKind{ReplicationValueKind::Count}; /**< Required Horo runtime representation. */
        ReplicationQuantization quantization;                        /**< Explicit lossy/exact wire policy. */
        std::size_t maximumEncodedBytes{};                           /**< Hard encoded output/input bound. */
        std::size_t maximumElementCount{};                           /**< Hard scalar or container element bound. */

        auto operator<=>(const ReplicationSerializerDescriptor &) const noexcept = default;
    };

    /** @brief Owner-supplied typed codec pinned by an immutable registry generation. */
    class IReplicationFieldSerializer {
    public:
        virtual ~IReplicationFieldSerializer() = default;

        /** @brief Returns inert metadata; the registry copies it and never consults it again after composition. */
        [[nodiscard]] virtual const ReplicationSerializerDescriptor &Descriptor() const noexcept = 0;

        /**
         * @brief Produces deterministic canonical bytes from one typed runtime value.
         * @param value Horo-owned value representation; no native object storage is exposed.
         * @return Canonical bytes or a typed validation/capacity error.
         */
        [[nodiscard]] virtual Result<std::vector<std::byte>> Encode(const ReplicationRuntimeValue &value) const = 0;

        /**
         * @brief Decodes already bounded canonical bytes into a typed runtime value.
         * @param canonicalBytes Complete canonical field payload valid for this call only.
         * @return Owned runtime value or a typed malformed/capacity error.
         */
        [[nodiscard]] virtual Result<ReplicationRuntimeValue> Decode(std::span<const std::byte> canonicalBytes) const = 0;
    };

    /** @brief Finite host limits for one immutable serializer registry generation. */
    struct ReplicationSerializerRegistryLimits final {
        std::size_t maximumSerializers{1024};         /**< Maximum contributed adapter count. */
        std::size_t maximumEncodedBytes{1024 * 1024}; /**< Maximum bytes admitted by any adapter. */
        std::size_t maximumElementCount{65536};       /**< Maximum elements admitted by any adapter. */
    };

    /** @brief Canonical bytes carrying their exact semantic type and codec tags. */
    struct ReplicationEncodedValue final {
        ReplicationValueTypeId valueType;      /**< Semantic value identity copied from the field descriptor. */
        ReplicationCodecId codec;              /**< Codec identity copied from the field descriptor. */
        std::vector<std::byte> canonicalBytes; /**< Owned canonical wire bytes. */

        bool operator==(const ReplicationEncodedValue &) const = default;
    };

    /** @brief Immutable schema-bound serializer generation that pins all adapter lifetimes. */
    class ReplicationSerializerRegistry final {
    public:
        /**
         * @brief Validates, copies metadata, and pins adapters against a schema generation.
         * @param schemas Non-null immutable schema generation retained by the result.
         * @param serializers Complete serializer contributions needed by every declared field.
         * @param limits Finite host-provided registry bounds.
         * @return Immutable registry or a typed malformed, conflict, missing, or capacity error.
         */
        [[nodiscard]] static Result<ReplicationSerializerRegistry> Create(
            ReplicationDescriptorSnapshotPtr schemas, std::span<const std::shared_ptr<const IReplicationFieldSerializer>> serializers,
            const ReplicationSerializerRegistryLimits &limits = {});

        /**
         * @brief Encodes one exact schema field after value-kind and field-bound validation.
         * @param schema Stable schema identity from the pinned generation.
         * @param field Stable field identity scoped by schema.
         * @param value Owned typed runtime value.
         * @return Tagged canonical bytes or a typed error.
         */
        [[nodiscard]] Result<ReplicationEncodedValue> Encode(ReplicationSchemaId schema, FieldId field,
                                                             const ReplicationRuntimeValue &value) const;

        /**
         * @brief Decodes one exact schema field after tag and byte-bound validation.
         * @param schema Stable schema identity from the pinned generation.
         * @param field Stable field identity scoped by schema.
         * @param value Tagged canonical bytes received from bounded record framing.
         * @return Owned typed runtime value or a typed error.
         */
        [[nodiscard]] Result<ReplicationRuntimeValue> Decode(ReplicationSchemaId schema, FieldId field,
                                                             const ReplicationEncodedValue &value) const;

        /**
         * @brief Compares two runtime values by their canonical encoded representation.
         * @param schema Stable schema identity from the pinned generation.
         * @param field Stable field identity scoped by schema.
         * @param left First runtime value.
         * @param right Second runtime value.
         * @return Whether both values produce identical canonical bytes, or a typed encode error.
         */
        [[nodiscard]] Result<bool> CanonicallyEqual(ReplicationSchemaId schema, FieldId field, const ReplicationRuntimeValue &left,
                                                    const ReplicationRuntimeValue &right) const;

        /** @brief Returns the pinned schema generation used by every lookup and codec call. */
        [[nodiscard]] const ReplicationDescriptorSnapshotPtr &Schemas() const noexcept;

    private:
        struct Entry final {
            ReplicationSerializerDescriptor descriptor;
            std::shared_ptr<const IReplicationFieldSerializer> serializer;
        };

        struct Binding final {
            const ReplicationSchemaDescriptor *schema{};
            const ReplicationFieldDescriptor *field{};
            const Entry *entry{};
        };

        /** @brief Resolves an exact adapter entry without fallback. */
        [[nodiscard]] static const Entry *FindEntry(std::span<const Entry> entries, const ModuleId &owner,
                                                    const ReplicationFieldDescriptor &field) noexcept;
        /** @brief Verifies complete schema coverage and rejects unbound contributions. */
        [[nodiscard]] static Result<void> ValidateBindings(const ReplicationDescriptorSnapshotPtr &schemas, std::span<const Entry> entries);
        /** @brief Decodes and re-encodes one optional default to prove its canonical typed form. */
        [[nodiscard]] static Result<void> ValidateDefault(const ReplicationFieldDescriptor &field, const Entry &entry);
        /** @brief Validates, copies, and canonicalizes adapter contributions. */
        [[nodiscard]] static Result<std::vector<Entry>> BuildEntries(
            std::span<const std::shared_ptr<const IReplicationFieldSerializer>> serializers,
            const ReplicationSerializerRegistryLimits &limits);
        /** @brief Resolves one exact schema field and its pinned adapter. */
        [[nodiscard]] Result<Binding> Resolve(ReplicationSchemaId schema, FieldId field) const;

        ReplicationDescriptorSnapshotPtr schemas_;
        std::vector<Entry> entries_;
    };

    /** @brief Horo-owned deterministic scalar codec supporting exact and stepped floating-point fields. */
    class CanonicalScalarReplicationSerializer final : public IReplicationFieldSerializer {
    public:
        /**
         * @brief Creates a validated scalar serializer.
         * @param descriptor Scalar serializer metadata and quantization policy.
         * @return Pinned adapter or a typed malformed/capacity error.
         */
        [[nodiscard]] static Result<std::shared_ptr<const CanonicalScalarReplicationSerializer>> Create(
            const ReplicationSerializerDescriptor &descriptor);

        /** @copydoc IReplicationFieldSerializer::Descriptor */
        [[nodiscard]] const ReplicationSerializerDescriptor &Descriptor() const noexcept override;
        /** @copydoc IReplicationFieldSerializer::Encode */
        [[nodiscard]] Result<std::vector<std::byte>> Encode(const ReplicationRuntimeValue &value) const override;
        /** @copydoc IReplicationFieldSerializer::Decode */
        [[nodiscard]] Result<ReplicationRuntimeValue> Decode(std::span<const std::byte> canonicalBytes) const override;

    private:
        /** @brief Stores already validated inert metadata for one scalar adapter. */
        explicit CanonicalScalarReplicationSerializer(ReplicationSerializerDescriptor descriptor);

        ReplicationSerializerDescriptor descriptor_;
    };
}  // namespace Horo::Network
