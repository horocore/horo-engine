#include "DecisionCompileDetail.h"
#include "DecisionSourceWire.h"

#include <algorithm>
#include <new>
#include <tuple>

namespace Horo::AI::Detail {
    namespace {
        void WriteValue(DecisionWriter &writer, const BlackboardValue &value) {
            writer.Unsigned(value.index(), 1);
            std::visit([&](const auto &item) {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<T, BlackboardScalarValue>)
                    WriteScalar(writer, item);
                else if constexpr (std::is_same_v<T, BlackboardCollectionValue>) {
                    writer.Enumeration(item.elementKind);
                    if (!writer.Count(item.count, MaximumBlackboardCollectionElements))
                        return;
                    for (std::size_t index = 0; index < item.count; ++index)
                        WriteScalar(writer, item.elements[index]);
                } else {
                    writer.Unsigned(item.schemaVersion, 4);
                    writer.Unsigned(item.serializedType, 4);
                    if (!writer.Count(item.size, MaximumBlackboardOpaqueBytes))
                        return;
                    writer.Bytes({reinterpret_cast<const std::uint8_t *>(item.bytes.data()), item.size});
                }
            }, value);
        }

        void WriteKey(DecisionWriter &writer, const BlackboardKeyDescriptor &key) {
            writer.IdentityValue(key.key);
            writer.Enumeration(key.kind);
            writer.Enumeration(key.cardinality);
            writer.Unsigned(key.maximumCollectionElements, 4);
            writer.Enumeration(key.presence);
            writer.Enumeration(key.access);
            writer.Unsigned(key.defaultValue.has_value(), 1);
            if (key.defaultValue)
                WriteValue(writer, *key.defaultValue);
        }

        void WriteSchema(DecisionWriter &writer, const BlackboardSchema &schema) {
            writer.IdentityValue(schema.Identity());
            writer.Unsigned(schema.Version(), 4);
            writer.Enumeration(schema.UnknownValuePolicy());
            WriteSequence<BlackboardKeyDescriptor>(writer, schema.Keys(), MaximumBlackboardKeys, WriteKey);
        }

        void WritePort(DecisionWriter &writer, const DecisionNodePort &port) {
            writer.Unsigned(port.id);
            writer.Enumeration(port.direction);
            writer.Enumeration(port.kind);
            writer.Enumeration(port.cardinality);
            writer.Unsigned(port.required, 1);
        }

        void WriteContractPorts(DecisionWriter &writer, std::span<const DecisionNodePort> values) {
            if (!writer.Count(values.size(), 128))
                return;
            std::vector<const DecisionNodePort *> ordered;
            for (const auto &value : values)
                ordered.push_back(&value);
            std::ranges::sort(ordered, {}, [](const auto *value) {
                return value->id;
            });
            for (const auto *value : ordered)
                WritePort(writer, *value);
        }

        void WriteContractProperties(DecisionWriter &writer, std::span<const BehaviorTreeProperty> values) {
            if (!writer.Count(values.size(), 64))
                return;
            std::vector<const BehaviorTreeProperty *> ordered;
            for (const auto &value : values)
                ordered.push_back(&value);
            std::ranges::sort(ordered, {}, [](const auto *value) {
                return value->id;
            });
            for (const auto *value : ordered)
                WriteProperty(writer, *value);
        }

        void WriteContract(DecisionWriter &writer, const DecisionNodeContract &contract) {
            const auto &descriptor = contract.descriptor;
            writer.IdentityValue(descriptor.type);
            writer.Enumeration(descriptor.origin.kind);
            writer.IdentityValue(descriptor.origin.provider);
            writer.Unsigned(descriptor.origin.version, 4);
            WriteLocation(writer, descriptor.source);
            WriteRequirements(writer, descriptor.requirements);
            writer.Enumeration(contract.role);
            writer.Unsigned(contract.capabilities.bits, 4);
            WriteContractPorts(writer, contract.ports);
            WriteContractProperties(writer, contract.properties);
            writer.Enumeration(contract.memory.kind);
            writer.Unsigned(contract.memory.stateBytes);
            writer.Unsigned(contract.memory.scratchBytes);
            writer.Unsigned(contract.memory.enforcedVmBytes);
        }

        Result<void> WriteSchemas(DecisionWriter &writer, const DecisionCompilationEnvironment &environment) {
            if (!writer.Count(environment.schemas.size(), DecisionAssetValidationHardLimits::Schemas))
                return Result<void>::Failure(MakeError(AIErrors::DecisionAssetLimitExceeded));
            std::vector<const BlackboardSchema *> schemas;
            for (const auto &schema : environment.schemas) {
                if (!schema)
                    return Result<void>::Failure(MakeError(AIErrors::DecisionAssetSchemaMissing));
                schemas.push_back(schema.get());
            }
            std::ranges::sort(schemas, {}, [](const auto *schema) {
                return std::pair{schema->Identity(), schema->Version()};
            });
            for (const auto *schema : schemas)
                WriteSchema(writer, *schema);
            return Result<void>::Success();
        }
    }  // namespace

    Result<Sha256Digest> DecisionEnvironmentIdentity(const DecisionCompilationEnvironment &environment) {
        if (!environment.nodes || !environment.nodes->CanUse() || !environment.capabilities.IsValid() ||
            environment.runtimeLayoutVersion != 1)
            return Result<Sha256Digest>::Failure(MakeError(AIErrors::RuntimeUnavailable));
        try {
            DecisionWriter writer(16U * 1024U * 1024U);
            writer.String("horo.decision.environment.1");
            writer.Unsigned(environment.runtimeLayoutVersion, 4);
            writer.Unsigned(environment.capabilities.bits, 4);
            // Logical kernel layout is platform-sensitive; this identity is not a portable raw-object snapshot.
            writer.Unsigned(sizeof(void *), 1);
            writer.Unsigned(sizeof(BlackboardValue), 4);
            auto schemas = WriteSchemas(writer, environment);
            if (schemas.HasError())
                return Result<Sha256Digest>::Failure(schemas.ErrorValue());
            WriteSequence<DecisionNodeContract>(writer, environment.nodes->Contracts(), DecisionNodeGeneration::MaximumNodes,
                                                WriteContract);
            if (!writer.Valid())
                return Result<Sha256Digest>::Failure(MakeError(AIErrors::DecisionAssetLimitExceeded));
            return Result<Sha256Digest>::Success(ComputeSha256(std::as_bytes(writer.Data())));
        } catch (const std::bad_alloc &) {
            return Result<Sha256Digest>::Failure(MakeError(AIErrors::DecisionAssetStorageUnavailable));
        }
    }
}  // namespace Horo::AI::Detail
