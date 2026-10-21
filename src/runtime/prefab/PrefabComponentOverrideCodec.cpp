#include "PrefabValueOverridesInternal.h"

namespace Horo::Prefab::ValueDetail {
    namespace {
        /** @brief Borrow exact target dimensions without copying component IDs during canonical encoding. */
        struct TargetView {
            const PrefabObjectAddress &object;
            const Gameplay::ComponentTypeId &type;
            PrefabComponentInstanceId instance;
        };

        /** @brief Derive an add target from its sole payload authority; remove targets retain their captured source identity. */
        TargetView Target(const PrefabComponentOverrideRecord &record) {
            if (const auto *add = std::get_if<PrefabAddComponentOverride>(&record))
                return {add->object, add->payload.component.typeId, add->payload.instance};
            const auto &remove = std::get<PrefabRemoveComponentOverride>(record);
            return {remove.target.object, remove.target.type, remove.target.instance};
        }

        /** @brief Retain opaque component payload bytes exactly, including data whose provider is currently unavailable. */
        Wire EncodeBytes(const std::vector<std::byte> &payload) {
            auto bytes = Wire::array();
            bytes.get_ref<Wire::array_t &>().reserve(payload.size());
            for (const auto byte : payload)
                bytes.push_back(std::to_integer<unsigned int>(byte));
            return Wire{{"bytes", std::move(bytes)}};
        }

        /** @brief Extend the shared object address with stable component dimensions, never names or vector positions. */
        Wire EncodeComponentTarget(const TargetView &target) {
            auto encoded = EncodeTarget(PrefabValueOverrideTarget{target.object});
            encoded["kind"] = "component";
            encoded["componentType"] = target.type.Value();
            encoded["componentInstance"] = target.instance.Value();
            return encoded;
        }

        /** @brief Admit complete component address fields before constructing any occurrence identity. */
        Result<PrefabComponentOverrideTarget> DecodeComponentTarget(const Json &value) {
            if (!Foundation::HasAllowedFields(value, {"kind", "scope", "object", "componentType", "componentInstance"}) ||
                value["kind"] != "component" || !value["componentType"].is_string())
                return Invalid<PrefabComponentOverrideTarget>();
            auto object = DecodeObjectAddress(value);
            auto type = Gameplay::ComponentTypeId::Parse(value["componentType"].get_ref<const std::string &>());
            auto instance = Detail::ReadUnsigned<std::uint64_t>(value["componentInstance"]);
            if (object.HasError() || type.HasError() || instance.HasError())
                return Invalid<PrefabComponentOverrideTarget>();
            auto identity = PrefabComponentInstanceId::Create(instance.Value());
            if (identity.HasError())
                return Invalid<PrefabComponentOverrideTarget>();
            return Result<PrefabComponentOverrideTarget>::Success({std::move(object).Value(), std::move(type).Value(), identity.Value()});
        }
    }  // namespace

    /** @brief Copy one stable target for operation admission and explicit selected-record authoring commands. */
    PrefabComponentOverrideTarget ComponentTarget(const PrefabComponentOverrideRecord &record) {
        const auto target = Target(record);
        return {target.object, target.type, target.instance};
    }

    /** @brief Preserve ADR-093 wire ranks 5/6 and one authoritative type/instance/schema/payload representation. */
    Wire EncodeComponentRecord(const PrefabComponentOverrideRecord &record) {
        auto target = EncodeComponentTarget(Target(record));
        if (const auto *add = std::get_if<PrefabAddComponentOverride>(&record))
            return Wire{{"operation", 5U},
                        {"target", std::move(target)},
                        {"componentSchemaVersion", add->payload.component.schemaVersion},
                        {"payload", EncodeBytes(add->payload.component.payload)}};
        const auto &remove = std::get<PrefabRemoveComponentOverride>(record);
        return Wire{{"operation", 6U},
                    {"target", std::move(target)},
                    {"componentSchemaVersion", remove.componentSchemaVersion},
                    {"expectedSourceValue", FormatSha256(remove.expectedSourceComponent)}};
    }

    /** @brief Dispatch only recognized topology ranks; all other operation admission remains with the value decoder. */
    bool IsComponentRecord(const Json &value) {
        if (!value.is_object())
            return false;
        const auto found = value.find("operation");
        if (found == value.end() || !found->is_number_unsigned())
            return false;
        const auto rank = found->get<std::uint64_t>();
        return rank == 5 || rank == 6;
    }

    /** @brief Decode bounded opaque addition bytes or an exact removal precondition without interpreting provider data. */
    Result<PrefabComponentOverrideRecord> DecodeComponentRecord(const Json &value) {
        if (!IsComponentRecord(value))
            return Invalid<PrefabComponentOverrideRecord>();
        const bool addition = value["operation"] == 5U;
        if (!(addition ? Foundation::HasAllowedFields(value, {"operation", "target", "componentSchemaVersion", "payload"})
                       : Foundation::HasAllowedFields(value, {"operation", "target", "componentSchemaVersion", "expectedSourceValue"})))
            return Invalid<PrefabComponentOverrideRecord>();
        auto target = DecodeComponentTarget(value["target"]);
        auto version = Detail::ReadUnsigned<std::uint32_t>(value["componentSchemaVersion"]);
        if (target.HasError() || version.HasError() || version.Value() == 0)
            return Invalid<PrefabComponentOverrideRecord>();
        auto address = std::move(target).Value();
        if (addition) {
            auto bytes = Detail::ParseComponentBytes(value["payload"]);
            if (bytes.HasError())
                return Result<PrefabComponentOverrideRecord>::Failure(bytes.ErrorValue());
            RawComponentPayload payload{address.instance,
                                        {std::move(address.type), version.Value(), Gameplay::ComponentPayloadEncoding::CanonicalJson,
                                         std::move(bytes).Value()}};
            return Result<PrefabComponentOverrideRecord>::Success(
                PrefabAddComponentOverride{std::move(address.object), std::move(payload)});
        }
        if (!value["expectedSourceValue"].is_string())
            return Invalid<PrefabComponentOverrideRecord>();
        auto digest = ParseSha256(value["expectedSourceValue"].get_ref<const std::string &>());
        if (digest.HasError())
            return Invalid<PrefabComponentOverrideRecord>();
        return Result<PrefabComponentOverrideRecord>::Success(
            PrefabRemoveComponentOverride{std::move(address), version.Value(), digest.Value()});
    }

    /** @brief Hash exact opaque source content and its stable envelope; payload formatting is not silently normalized. */
    Sha256Digest ComponentDigest(const RawComponentPayload &component) {
        const auto bytes = Wire{{"instanceId", component.instance.Value()},
                                {"typeId", component.component.typeId.Value()},
                                {"schemaVersion", component.component.schemaVersion},
                                {"encoding", "canonicalJson"},
                                {"payload", EncodeBytes(component.component.payload)}}
                               .dump(0);
        return ComputeSha256(std::as_bytes(std::span(bytes.data(), bytes.size())));
    }

    /** @brief Preserve component conflict bytes together with both exact source revisions and schema evidence. */
    Wire EncodeComponentConflict(const PrefabComponentOverrideConflict &conflict) {
        return Wire{{"record", EncodeComponentRecord(conflict.original)},
                    {"authoredAgainst", EncodeRevision(conflict.authoredAgainst)},
                    {"schemaRevision", FormatSha256(conflict.schemaRevision)},
                    {"actualSourceValue", FormatSha256(conflict.actualSourceComponent)},
                    {"observedSource", EncodeRevision(conflict.observedSource)}};
    }

    /** @brief Preserve original topology intent while its target or validating provider is unavailable. */
    Wire EncodeComponentOrphan(const PrefabComponentOverrideOrphan &orphan) {
        return Wire{{"record", EncodeComponentRecord(orphan.original)},
                    {"authoredAgainst", EncodeRevision(orphan.authoredAgainst)},
                    {"schemaRevision", FormatSha256(orphan.schemaRevision)},
                    {"reason", static_cast<unsigned>(orphan.reason)}};
    }

    /** @brief Admit the complete preserved conflict envelope before copying source or component bytes. */
    Result<PrefabComponentOverrideConflict> DecodeComponentConflict(const Json &value) {
        if (!Foundation::HasAllowedFields(value, {"record", "authoredAgainst", "schemaRevision", "actualSourceValue", "observedSource"}) ||
            !value["schemaRevision"].is_string() || !value["actualSourceValue"].is_string())
            return Invalid<PrefabComponentOverrideConflict>();
        auto record = DecodeComponentRecord(value["record"]);
        auto authored = Detail::ParseRevision(value["authoredAgainst"]);
        auto schema = ParseSha256(value["schemaRevision"].get_ref<const std::string &>());
        auto digest = ParseSha256(value["actualSourceValue"].get_ref<const std::string &>());
        auto observed = Detail::ParseRevision(value["observedSource"]);
        if (record.HasError() || authored.HasError() || schema.HasError() || digest.HasError() || observed.HasError())
            return Invalid<PrefabComponentOverrideConflict>();
        return Result<PrefabComponentOverrideConflict>::Success(
            {std::move(record).Value(), std::move(authored).Value(), schema.Value(), digest.Value(), std::move(observed).Value()});
    }

    /** @brief Admit closed orphan reasons without discarding a component record whose provider is unavailable. */
    Result<PrefabComponentOverrideOrphan> DecodeComponentOrphan(const Json &value) {
        if (!Foundation::HasAllowedFields(value, {"record", "authoredAgainst", "schemaRevision", "reason"}) ||
            !value["schemaRevision"].is_string())
            return Invalid<PrefabComponentOverrideOrphan>();
        auto record = DecodeComponentRecord(value["record"]);
        auto authored = Detail::ParseRevision(value["authoredAgainst"]);
        auto schema = ParseSha256(value["schemaRevision"].get_ref<const std::string &>());
        auto reason = Detail::ReadUnsigned<std::uint32_t>(value["reason"]);
        if (record.HasError() || authored.HasError() || schema.HasError() || reason.HasError() ||
            reason.Value() > static_cast<unsigned>(PrefabComponentOverrideIssue::SchemaChanged))
            return Invalid<PrefabComponentOverrideOrphan>();
        return Result<PrefabComponentOverrideOrphan>::Success({std::move(record).Value(), std::move(authored).Value(), schema.Value(),
                                                               static_cast<PrefabComponentOverrideIssue>(reason.Value())});
    }
}  // namespace Horo::Prefab::ValueDetail
