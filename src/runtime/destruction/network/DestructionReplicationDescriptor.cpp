#include "DestructionReplicationFields.h"
#include "Horo/Destruction/DestructionErrors.h"
#include "Horo/Destruction/DestructionReplication.h"
#include "Horo/Network/NetworkErrors.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace Horo::Destruction {
    namespace {
        [[nodiscard]] Network::ReplicationValueTypeId ValueType(const std::size_t index) {
            return Network::ReplicationValueTypeId::Create(0xDF000100U + static_cast<std::uint32_t>(index)).Value();
        }

        [[nodiscard]] Network::ReplicationCodecId Codec(const std::size_t index) {
            return Network::ReplicationCodecId::Create(0xDF000200U + static_cast<std::uint32_t>(index)).Value();
        }

        /** @brief Copies already canonical DFR field bytes inside the exact Network registry bounds. */
        class CanonicalDestructionFieldSerializer final : public Network::IReplicationFieldSerializer {
        public:
            explicit CanonicalDestructionFieldSerializer(const std::size_t index)
                : descriptor_{.valueType = ValueType(index),
                              .codec = Codec(index),
                              .owner = {.value = "horo.destruction"},
                              .valueKind = Network::ReplicationValueKind::ByteSequence,
                              .quantization = {},
                              .maximumEncodedBytes = Detail::MaximumFieldBytes[index],
                              .maximumElementCount = Detail::MaximumFieldBytes[index]} {}

            [[nodiscard]] const Network::ReplicationSerializerDescriptor &Descriptor() const noexcept override {
                return descriptor_;
            }

            [[nodiscard]] Result<std::vector<std::byte>> Encode(const Network::ReplicationRuntimeValue &value) const override {
                const auto *bytes = std::get_if<std::vector<std::byte>>(&value);
                if (bytes == nullptr)
                    return Result<std::vector<std::byte>>::Failure(MakeError(Network::NetworkErrors::ReplicationSerializerValueInvalid));
                if (bytes->size() > descriptor_.maximumEncodedBytes)
                    return Result<std::vector<std::byte>>::Failure(
                        MakeError(Network::NetworkErrors::ReplicationSerializerCapacityExceeded));
                try {
                    return Result<std::vector<std::byte>>::Success(*bytes);
                } catch (const std::bad_alloc &) {
                    return Result<std::vector<std::byte>>::Failure(
                        MakeError(Network::NetworkErrors::ReplicationSerializerCapacityExceeded));
                }
            }

            [[nodiscard]] Result<Network::ReplicationRuntimeValue> Decode(const std::span<const std::byte> bytes) const override {
                if (bytes.size() > descriptor_.maximumEncodedBytes)
                    return Result<Network::ReplicationRuntimeValue>::Failure(
                        MakeError(Network::NetworkErrors::ReplicationSerializerCapacityExceeded));
                try {
                    return Result<Network::ReplicationRuntimeValue>::Success(std::vector<std::byte>{bytes.begin(), bytes.end()});
                } catch (const std::bad_alloc &) {
                    return Result<Network::ReplicationRuntimeValue>::Failure(
                        MakeError(Network::NetworkErrors::ReplicationSerializerCapacityExceeded));
                }
            }

            [[nodiscard]] Result<bool> CanonicallyEqual(const Network::ReplicationRuntimeValue &left,
                                                        const Network::ReplicationRuntimeValue &right) const override {
                const auto *leftBytes = std::get_if<std::vector<std::byte>>(&left);
                const auto *rightBytes = std::get_if<std::vector<std::byte>>(&right);
                if (leftBytes == nullptr || rightBytes == nullptr)
                    return Result<bool>::Failure(MakeError(Network::NetworkErrors::ReplicationSerializerValueInvalid));
                if (leftBytes->size() > descriptor_.maximumEncodedBytes || rightBytes->size() > descriptor_.maximumEncodedBytes)
                    return Result<bool>::Failure(MakeError(Network::NetworkErrors::ReplicationSerializerCapacityExceeded));
                return Result<bool>::Success(*leftBytes == *rightBytes);
            }

        private:
            Network::ReplicationSerializerDescriptor descriptor_;
        };
    }  // namespace

    /** @copydoc MakeDestructionReplicationDescriptor */
    Result<Network::ReplicationSchemaDescriptor> MakeDestructionReplicationDescriptor() {
        try {
            Network::ReplicationSchemaDescriptor schema{.id = Detail::SchemaId(),
                                                        .version = DestructionReplicationVersion,
                                                        .compatibility = {.minimum = DestructionReplicationVersion,
                                                                          .maximum = DestructionReplicationVersion},
                                                        .owner = {.value = "horo.destruction"}};
            schema.fields.reserve(Detail::FieldValues.size());
            for (std::size_t index = 0; index < Detail::FieldValues.size(); ++index) {
                schema.fields.push_back({.id = Detail::FieldId(index),
                                         .valueType = ValueType(index),
                                         .codec = Codec(index),
                                         .introducedVersion = DestructionReplicationVersion,
                                         .condition = Network::ReplicationCondition::Always,
                                         .requirement = Network::ReplicationFieldRequirement::Required,
                                         .writePolicy = Network::ReplicationWritePolicy::AuthorityServerOnly,
                                         .limits = {.maximumEncodedBytes = Detail::MaximumFieldBytes[index],
                                                    .maximumElementCount = Detail::MaximumFieldBytes[index]}});
            }
            return Result<Network::ReplicationSchemaDescriptor>::Success(std::move(schema));
        } catch (const std::bad_alloc &) {
            return Result<Network::ReplicationSchemaDescriptor>::Failure(MakeError(DestructionErrors::ReplicationLimitExceeded));
        }
    }

    /** @copydoc MakeDestructionReplicationSerializers */
    Result<std::vector<std::shared_ptr<const Network::IReplicationFieldSerializer>>> MakeDestructionReplicationSerializers() {
        try {
            std::vector<std::shared_ptr<const Network::IReplicationFieldSerializer>> serializers;
            serializers.reserve(Detail::FieldValues.size());
            for (std::size_t index = 0; index < Detail::FieldValues.size(); ++index)
                serializers.push_back(std::make_shared<CanonicalDestructionFieldSerializer>(index));
            return Result<std::vector<std::shared_ptr<const Network::IReplicationFieldSerializer>>>::Success(std::move(serializers));
        } catch (const std::bad_alloc &) {
            return Result<std::vector<std::shared_ptr<const Network::IReplicationFieldSerializer>>>::Failure(
                MakeError(DestructionErrors::ReplicationLimitExceeded));
        }
    }
}  // namespace Horo::Destruction
