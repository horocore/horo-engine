#include "Horo/Prefab/PrefabErrors.h"
#include "PrefabPersistentCodecInternal.h"

#include <algorithm>
#include <array>
#include <new>

namespace Horo::Prefab {
    namespace {
        using Runtime::CanonicalValueWriter;

        /** @brief Emits existing opaque identity bytes without hashing or deriving them from runtime state. */
        template <typename IdentityType> Result<void> Identity(CanonicalValueWriter &writer, const IdentityType &value) {
            return writer.WriteBytes(std::as_bytes(std::span{value.Bytes()}));
        }

        /** @brief Writes exact world-qualified relationship identity and durable incarnation. */
        Result<void> Reference(CanonicalValueWriter &writer, const PrefabPersistentEntityReference &value) {
            const std::array fields{Identity(writer, value.target.entity), writer.WriteUInt64(value.generation.value)};
            for (const auto &field : fields)
                if (field.HasError())
                    return field;
            return Result<void>::Success();
        }

        /** @brief The one closed canonical Save representation of cook-declared initialization scalar values. */
        Result<void> InitializationValue(CanonicalValueWriter &writer, const Gameplay::BehaviorFieldValue &value) {
            return std::visit([&writer]<typename Value>(const Value &input) -> Result<void> {
                if constexpr (std::is_same_v<Value, bool>)
                    return writer.WriteBool(input);
                else if constexpr (std::is_same_v<Value, double>)
                    return writer.WriteFloat64(input);
                else if constexpr (std::is_same_v<Value, std::int64_t>)
                    return writer.WriteInt64(input);
                else if constexpr (std::is_same_v<Value, std::string>)
                    return writer.WriteUtf8(input);
                else if constexpr (std::is_same_v<Value, Math::Vec2>)
                    return writer.WriteVec2(input);
                else if constexpr (std::is_same_v<Value, Math::Vec3>)
                    return writer.WriteVec3(input);
                else if constexpr (std::is_same_v<Value, Math::Quaternion>)
                    return writer.WriteQuaternion(input);
                else
                    return Result<void>::Failure(
                        MakeError(PrefabErrors::AdmissionRejected, "A saved initialization value has no declared kind."));
            }, value);
        }

        /** @brief Stable initialization interface order is independent of request arrival order. */
        Result<void> Initializations(CanonicalValueWriter &writer, std::vector<PrefabInitializationValue> values) {
            std::ranges::sort(values, {}, [](const auto &value) {
                return value.id.value;
            });
            if (auto count = writer.WriteUInt32(static_cast<std::uint32_t>(values.size())); count.HasError())
                return count;
            for (const auto &value : values) {
                const std::array fields{writer.WriteUInt64(value.id.value),
                                        writer.WriteUInt8(static_cast<std::uint8_t>(value.value.index())),
                                        InitializationValue(writer, value.value)};
                for (const auto &field : fields)
                    if (field.HasError())
                        return field;
            }
            return Result<void>::Success();
        }

        /** @brief Encodes only cooked-declared external binding identities, never resolved EntityRefs. */
        Result<void> Bindings(CanonicalValueWriter &writer, std::vector<PrefabPersistentBinding> values) {
            std::ranges::sort(values, {}, &PrefabPersistentBinding::slot);
            if (auto count = writer.WriteUInt32(static_cast<std::uint32_t>(values.size())); count.HasError())
                return count;
            for (const auto &binding : values) {
                const std::array fields{writer.WriteUInt64(binding.slot.Value()), Reference(writer, binding.target)};
                for (const auto &field : fields)
                    if (field.HasError())
                        return field;
            }
            return Result<void>::Success();
        }

        /** @brief Uses persisted stable source scopes rather than dense cooked offsets or runtime slots. */
        Result<void> Address(CanonicalValueWriter &writer, const PrefabObjectAddress &address) {
            if (auto count = writer.WriteUInt32(static_cast<std::uint32_t>(address.NestedInstanceScope().size())); count.HasError())
                return count;
            for (const auto part : address.NestedInstanceScope())
                if (auto field = writer.WriteUInt32(part.value); field.HasError())
                    return field;
            return writer.WriteUInt32(address.SourceObject().value);
        }

        /** @brief Stable source occurrence order preserves exact saved runtime attachment IDs without reissuing them. */
        Result<void> Behaviors(CanonicalValueWriter &writer, std::vector<PrefabPersistentBehaviorIdentity> behaviors) {
            std::ranges::sort(behaviors, {}, [](const auto &behavior) {
                return behavior.source.value;
            });
            if (auto count = writer.WriteUInt32(static_cast<std::uint32_t>(behaviors.size())); count.HasError())
                return count;
            for (const auto &behavior : behaviors) {
                const std::array fields{writer.WriteUtf8(behavior.type.Value()), writer.WriteUInt64(behavior.source.value),
                                        writer.WriteUInt64(behavior.runtime.value), writer.WriteUInt32(behavior.schemaVersion)};
                for (const auto &field : fields)
                    if (field.HasError())
                        return field;
            }
            return Result<void>::Success();
        }

        /** @brief Stores one member's exact provenance and deletion; occurrence/definition come from its already-validated recipe. */
        Result<void> Member(CanonicalValueWriter &writer, const PrefabPersistentMember &member) {
            const std::array fields{Identity(writer, member.record.identity),
                                    writer.WriteUInt64(member.record.generation.value),
                                    writer.WriteUInt8(static_cast<std::uint8_t>(member.record.disposition)),
                                    Identity(writer, member.source.sourceAsset),
                                    Address(writer, member.source.sourceObject),
                                    writer.WriteBytes(std::as_bytes(std::span{member.source.sourceDigest.bytes}))};
            for (const auto &field : fields)
                if (field.HasError())
                    return field;
            return Behaviors(writer, member.behaviors);
        }

        /** @brief Canonical member wire ordering does not become a reconstruction-time dense index. */
        Result<void> Members(CanonicalValueWriter &writer, const std::vector<PrefabPersistentMember> &members) {
            std::vector<const PrefabPersistentMember *> ordered;
            ordered.reserve(members.size());
            for (const auto &member : members)
                ordered.push_back(&member);
            std::ranges::sort(ordered, {}, [](const auto *member) {
                return member->source.sourceObject;
            });
            if (auto count = writer.WriteUInt32(static_cast<std::uint32_t>(ordered.size())); count.HasError())
                return count;
            for (const auto *member : ordered)
                if (auto encoded = Member(writer, *member); encoded.HasError())
                    return encoded;
            return Result<void>::Success();
        }

        /** @brief Exact metadata identifies actual cooked content and authentic root source revision separately. */
        Result<void> Source(CanonicalValueWriter &writer, const PrefabPersistentInstance &instance) {
            const std::array fields{Identity(writer, instance.identity),
                                    writer.WriteUtf8(instance.creator),
                                    Identity(writer, instance.source.asset),
                                    writer.WriteBytes(std::as_bytes(std::span{instance.source.artifactDigest.bytes})),
                                    writer.WriteUtf8(instance.source.target.Value()),
                                    writer.WriteUtf8(Application::FormatHoroVersion(instance.rootSourceRevision.projectVersion)),
                                    writer.WriteBytes(std::as_bytes(std::span{instance.rootSourceRevision.contentDigest.bytes})),
                                    writer.WriteUInt8(static_cast<std::uint8_t>(instance.disposition))};
            for (const auto &field : fields)
                if (field.HasError())
                    return field;
            return Result<void>::Success();
        }

        /** @brief Root TRS is persisted once as placement; typed child/property/topology intent uses its sole canonical codec. */
        Result<void> Placement(CanonicalValueWriter &writer, const PrefabPersistentInstance &instance) {
            const std::array fields{writer.WriteBool(instance.parent.has_value()), writer.WriteVec3(instance.placement.translation),
                                    writer.WriteQuaternion(instance.placement.rotation), writer.WriteVec3(instance.placement.scale)};
            for (const auto &field : fields)
                if (field.HasError())
                    return field;
            return instance.parent ? Reference(writer, *instance.parent) : Result<void>::Success();
        }

        /** @brief Exact saved runtime ancestry is not conflated with source nesting or parent relationships. */
        Result<void> Lineage(CanonicalValueWriter &writer, const PrefabPersistentInstance &instance) {
            if (auto count = writer.WriteUInt32(static_cast<std::uint32_t>(instance.creationLineage.size())); count.HasError())
                return count;
            for (const auto asset : instance.creationLineage)
                if (auto field = Identity(writer, asset); field.HasError())
                    return field;
            return Result<void>::Success();
        }

        /** @brief One recipe contains data-only creation provenance; no runtime component memory or native handles enter the record. */
        Result<void> Instance(CanonicalValueWriter &writer, const PrefabPersistentInstance &instance, const PrefabLimitProfile &profile) {
            if (auto source = Source(writer, instance); source.HasError())
                return source;
            if (auto placement = Placement(writer, instance); placement.HasError())
                return placement;
            if (auto lineage = Lineage(writer, instance); lineage.HasError())
                return lineage;
            if (auto initial = Initializations(writer, instance.initialization); initial.HasError())
                return initial;
            if (auto bindings = Bindings(writer, instance.bindings); bindings.HasError())
                return bindings;
            auto delta = SerializePrefabValueOverrides(instance.overrides, profile);
            if (delta.HasError())
                return Result<void>::Failure(delta.ErrorValue());
            if (auto encoded = writer.WriteBytes(std::as_bytes(std::span{delta.Value().data(), delta.Value().size()})); encoded.HasError())
                return encoded;
            return Members(writer, instance.members);
        }

        /** @brief Finite canonical whole-state output is sealed only after every recipe succeeds. */
        Result<Runtime::CanonicalEncodedValue> Encode(const PrefabPersistentState &state, const PrefabPersistenceLimits &limits,
                                                      const PrefabLimitProfile &profile) {
            using Outcome = Result<Runtime::CanonicalEncodedValue>;
            if (auto valid = ValidatePrefabPersistentState(state, limits, profile); valid.HasError())
                return Outcome::Failure(valid.ErrorValue());
            CanonicalValueWriter writer{Detail::PersistentCodecLimits(limits)};
            const std::array header{writer.WriteUInt32(1), Identity(writer, state.world),
                                    writer.WriteUInt32(static_cast<std::uint32_t>(state.instances.size()))};
            for (const auto &field : header)
                if (field.HasError())
                    return Outcome::Failure(field.ErrorValue());
            std::vector<const PrefabPersistentInstance *> ordered;
            ordered.reserve(state.instances.size());
            for (const auto &instance : state.instances)
                ordered.push_back(&instance);
            std::ranges::sort(ordered, {}, [](const auto *instance) {
                return instance->identity;
            });
            for (const auto *instance : ordered)
                if (auto encoded = Instance(writer, *instance, profile); encoded.HasError())
                    return Outcome::Failure(encoded.ErrorValue());
            return std::move(writer).Finalize();
        }
    }  // namespace

    /** @copydoc EncodePrefabPersistentState */
    Result<Runtime::CanonicalEncodedValue> EncodePrefabPersistentState(const PrefabPersistentState &state,
                                                                       const PrefabPersistenceLimits &limits,
                                                                       const PrefabLimitProfile &profile) {
        try {
            return Encode(state, limits, profile);
        } catch (const std::bad_alloc &) {
            return Result<Runtime::CanonicalEncodedValue>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }
}  // namespace Horo::Prefab
