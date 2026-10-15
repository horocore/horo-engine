#include "PrefabValueOverridesInternal.h"

namespace Horo::Prefab::ValueDetail {
    namespace {
        /** @brief Extract the common stable object dimension without creating an identity from display data. */
        const PrefabObjectAddress &Object(const PrefabValueOverrideTarget &target) noexcept {
            if (const auto *object = std::get_if<PrefabObjectAddress>(&target))
                return *object;
            return std::get<PrefabPropertyAddress>(target).Object();
        }

        /** @brief Compare scope length first, then persisted slots, as fixed by the canonical contract. */
        std::strong_ordering CompareObjects(const PrefabObjectAddress &left, const PrefabObjectAddress &right) noexcept {
            const auto a = left.NestedInstanceScope();
            const auto b = right.NestedInstanceScope();
            if (auto order = a.size() <=> b.size(); order != 0)
                return order;
            for (std::size_t i = 0; i < a.size(); ++i)
                if (auto order = a[i] <=> b[i]; order != 0)
                    return order;
            return left.SourceObject() <=> right.SourceObject();
        }

        /** @brief Parse stable source slots with explicit width/count admission. */
        Result<PrefabObjectAddress> DecodeObject(const Json &value) {
            if (!value.is_object() || !value.contains("scope") || !value["scope"].is_array() || !value.contains("object") ||
                value["scope"].size() > MaximumPrefabObjectScopeDepth)
                return Invalid<PrefabObjectAddress>();
            std::vector<LocalObjectId> scope;
            scope.reserve(value["scope"].size());
            for (const auto &part : value["scope"]) {
                auto slot = Detail::ReadUnsigned<std::uint32_t>(part);
                if (slot.HasError())
                    return Invalid<PrefabObjectAddress>();
                scope.push_back(LocalObjectId{slot.Value()});
            }
            auto object = Detail::ReadUnsigned<std::uint32_t>(value["object"]);
            if (object.HasError())
                return Invalid<PrefabObjectAddress>();
            return PrefabObjectAddress::Create(scope, LocalObjectId{object.Value()});
        }
    }  // namespace

    /** @brief Define canonical target order independently from record arrival and UI order. */
    bool TargetLess(const PrefabValueOverrideTarget &left, const PrefabValueOverrideTarget &right) noexcept {
        if (auto order = CompareObjects(Object(left), Object(right)); order != 0)
            return order < 0;
        if (left.index() != right.index())
            return left.index() < right.index();
        const auto *a = std::get_if<PrefabPropertyAddress>(&left);
        if (!a)
            return false;
        const auto &b = std::get<PrefabPropertyAddress>(right);
        if (auto order = a->ComponentType() <=> b.ComponentType(); order != 0)
            return order < 0;
        if (auto order = a->ComponentInstance() <=> b.ComponentInstance(); order != 0)
            return order < 0;
        return a->Property() < b.Property();
    }

    /** @brief Encode durable addresses with no names, memory offsets or vector indexes. */
    Wire EncodeTarget(const PrefabValueOverrideTarget &target) {
        const auto &object = Object(target);
        auto scope = Wire::array();
        for (const auto slot : object.NestedInstanceScope())
            scope.push_back(slot.value);
        if (const auto *property = std::get_if<PrefabPropertyAddress>(&target))
            return Wire{{"kind", "property"},
                        {"scope", std::move(scope)},
                        {"object", object.SourceObject().value},
                        {"componentType", property->ComponentType().Value()},
                        {"componentInstance", property->ComponentInstance().Value()},
                        {"property", property->Property().Value()}};
        return Wire{{"kind", "transform"}, {"scope", std::move(scope)}, {"object", object.SourceObject().value}};
    }

    /** @brief Validate complete stable address envelopes before creating typed identity values. */
    Result<PrefabValueOverrideTarget> DecodeTarget(const Json &value) {
        if (!value.is_object() || !value.contains("kind") || !value["kind"].is_string())
            return Invalid<PrefabValueOverrideTarget>();
        auto object = DecodeObject(value);
        if (object.HasError())
            return Invalid<PrefabValueOverrideTarget>();
        if (value["kind"] == "transform" && Foundation::HasAllowedFields(value, {"kind", "scope", "object"}))
            return Result<PrefabValueOverrideTarget>::Success(std::move(object).Value());
        if (value["kind"] != "property" ||
            !Foundation::HasAllowedFields(value, {"kind", "scope", "object", "componentType", "componentInstance", "property"}) ||
            !value["componentType"].is_string())
            return Invalid<PrefabValueOverrideTarget>();
        auto type = Gameplay::ComponentTypeId::Parse(value["componentType"].get_ref<const std::string &>());
        auto instance = Detail::ReadUnsigned<std::uint64_t>(value["componentInstance"]);
        auto property = Detail::ReadUnsigned<std::uint64_t>(value["property"]);
        if (type.HasError() || instance.HasError() || property.HasError())
            return Invalid<PrefabValueOverrideTarget>();
        auto occurrence = PrefabComponentInstanceId::Create(instance.Value());
        auto id = PrefabPropertyId::Create(property.Value());
        if (occurrence.HasError() || id.HasError())
            return Invalid<PrefabValueOverrideTarget>();
        auto target = PrefabPropertyAddress::Create(std::move(object).Value(), std::move(type).Value(), occurrence.Value(), id.Value());
        if (target.HasError())
            return Invalid<PrefabValueOverrideTarget>();
        return Result<PrefabValueOverrideTarget>::Success(std::move(target).Value());
    }

    /** @brief Encode the single unified project version and exact source digest. */
    Wire EncodeRevision(const PrefabSourceRevision &revision) {
        return Wire{{"projectVersion", Application::FormatHoroVersion(revision.projectVersion)},
                    {"contentDigest", FormatSha256(revision.contentDigest)}};
    }

    /** @brief Encode AssignValue with its stable rank zero and exact source/schema precondition. */
    Wire EncodeRecord(const PrefabValueOverrideRecord &record) {
        return Wire{{"operation", 0U},
                    {"target", EncodeTarget(record.target)},
                    {"componentSchemaVersion", record.componentSchemaVersion},
                    {"expectedSourceValue", FormatSha256(record.expectedSourceValue)},
                    {"value", EncodeValue(record.value)}};
    }

    /** @brief Decode one complete closed AssignValue record; unknown operation ranks cannot execute. */
    Result<PrefabValueOverrideRecord> DecodeRecord(const Json &value) {
        if (!Foundation::HasAllowedFields(value, {"operation", "target", "componentSchemaVersion", "expectedSourceValue", "value"}) ||
            !value["expectedSourceValue"].is_string())
            return Invalid<PrefabValueOverrideRecord>();
        auto operation = Detail::ReadUnsigned<std::uint32_t>(value["operation"]);
        auto version = Detail::ReadUnsigned<std::uint32_t>(value["componentSchemaVersion"]);
        auto target = DecodeTarget(value["target"]);
        auto decoded = DecodeValue(value["value"]);
        auto digest = ParseSha256(value["expectedSourceValue"].get_ref<const std::string &>());
        if (operation.HasError() || operation.Value() != 0 || version.HasError() || target.HasError() || decoded.HasError() ||
            digest.HasError())
            return Invalid<PrefabValueOverrideRecord>("Override record operation, target, type or precondition is invalid.");
        return Result<PrefabValueOverrideRecord>::Success(
            {std::move(target).Value(), version.Value(), digest.Value(), std::move(decoded).Value()});
    }
}  // namespace Horo::Prefab::ValueDetail
