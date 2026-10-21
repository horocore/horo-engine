#include "PrefabValueOverridesInternal.h"

#include <algorithm>
#include <new>

namespace Horo::Prefab {
    namespace {
        using namespace ValueDetail;

        /** @brief Encode metadata independently so exact array lengths can be admitted before the complete writer is allocated. */
        Wire EncodeSet(const PrefabValueOverrideSet &set, Wire records = Wire::array(), Wire conflicts = Wire::array(),
                       Wire orphans = Wire::array()) {
            return Wire{{"schema", 1U},
                        {"sourcePrefab", set.sourcePrefab.ToString()},
                        {"authoredAgainst", EncodeRevision(set.authoredAgainst)},
                        {"schemaRevision", FormatSha256(set.schemaRevision)},
                        {"records", std::move(records)},
                        {"conflicts", std::move(conflicts)},
                        {"orphans", std::move(orphans)}};
        }

        /** @brief Reject target/type confusion even when no live component schema is available. */
        Result<void> ValidateRecord(const PrefabValueOverrideRecord &record) {
            if (const bool transform = std::holds_alternative<PrefabObjectAddress>(record.target);
                transform != std::holds_alternative<Math::Transform>(record.value) ||
                (transform ? record.componentSchemaVersion != 0 : record.componentSchemaVersion == 0))
                return Invalid<void>("Override target, schema version and value kind disagree.");
            return ValidateValue(record.value);
        }

        /** @brief Add an exact independently encoded array child before aggregate writer allocation. */
        Result<void> Charge(const Wire &encoded, std::size_t &bytes, const PrefabProjectPolicy &policy, const bool valueRecord) {
            const auto size = encoded.dump(0).size();
            if ((valueRecord && size > policy.maximumOverrideValueBytes) || bytes > policy.maximumOverrideSetBytes ||
                size > policy.maximumOverrideSetBytes - bytes || policy.maximumOverrideSetBytes - bytes - size < 2)
                return Result<void>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
            bytes += size + 2;  // Nonempty indentation-zero arrays add exactly a comma/newline or opening/closing newlines per child.
            return Result<void>::Success();
        }

        /** @brief Apply the same standalone value/source admission to both preserved conflict and orphan evidence. */
        Result<void> ValidatePreservedRecord(const PrefabValueOverrideRecord &record, const PrefabSourceRevision &source,
                                             const PrefabProjectPolicy &policy) {
            if (auto valid = ValidateRecord(record); valid.HasError())
                return valid;
            if (EncodeRecord(record).dump(0).size() > policy.maximumOverrideValueBytes)
                return Result<void>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
            if (!Detail::IsCanonicalProjectVersion(source.projectVersion))
                return Invalid<void>();
            return Result<void>::Success();
        }

        /** @brief Check complete header identity and count bounds before any retained-record storage allocation. */
        Result<void> ValidateSetShape(const PrefabValueOverrideSet &set, const PrefabProjectPolicy &policy) {
            if (!set.sourcePrefab.IsValid() || !Detail::IsCanonicalProjectVersion(set.authoredAgainst.projectVersion))
                return Invalid<void>();
            return ValidateOverrideCounts(set, policy);
        }

        /** @brief Admit every preserved record before encoding the whole authoring set. */
        Result<void> ValidateSet(const PrefabValueOverrideSet &set, const PrefabLimitProfile &limits) {
            const auto &policy = limits.Policy();
            if (auto shape = ValidateSetShape(set, policy); shape.HasError())
                return shape;
            std::vector<const PrefabValueOverrideRecord *> all;
            all.reserve(set.records.size() + set.conflicts.size() + set.orphans.size());
            std::size_t bytes = EncodeSet(set).dump(0).size();
            for (const auto &record : set.records) {
                if (auto valid = ValidateRecord(record); valid.HasError())
                    return valid;
                if (auto charged = Charge(EncodeRecord(record), bytes, policy, true); charged.HasError())
                    return charged;
                all.push_back(&record);
            }
            for (const auto &conflict : set.conflicts) {
                if (auto valid = ValidatePreservedRecord(conflict.original, conflict.authoredAgainst, policy); valid.HasError())
                    return valid;
                if (!Detail::IsCanonicalProjectVersion(conflict.observedSource.projectVersion))
                    return Invalid<void>();
                if (auto charged = Charge(EncodeValueConflict(conflict), bytes, policy, false); charged.HasError())
                    return charged;
                all.push_back(&conflict.original);
            }
            for (const auto &orphan : set.orphans) {
                if (auto valid = ValidatePreservedRecord(orphan.original, orphan.authoredAgainst, policy); valid.HasError())
                    return valid;
                if (orphan.reason > PrefabValueOverrideIssue::SchemaChanged)
                    return Invalid<void>();
                if (auto charged = Charge(EncodeValueOrphan(orphan), bytes, policy, false); charged.HasError())
                    return charged;
                all.push_back(&orphan.original);
            }
            if (auto valid = ValidateComponentRecords(set, policy, bytes); valid.HasError())
                return valid;
            std::ranges::sort(all, [](const auto *left, const auto *right) {
                return TargetLess(left->target, right->target);
            });
            for (std::size_t i = 1; i < all.size(); ++i)
                if (all[i - 1]->target == all[i]->target)
                    return Result<void>::Failure(
                        MakeError(PrefabErrors::OverrideConflict, "Duplicate instance value targets are not last-write-wins."));
            return bytes <= policy.maximumOverrideSetBytes ? Result<void>::Success() : Invalid<void>();
        }

        /** @brief Parse bounded source and schema identities before creating any record storage. */
        Result<PrefabValueOverrideSet> DecodeHeader(const Json &value, const PrefabProjectPolicy &policy) {
            if (!Foundation::HasAllowedFields(value, {"schema", "sourcePrefab", "authoredAgainst", "schemaRevision", "records", "conflicts",
                                                      "orphans"}) ||
                !value["sourcePrefab"].is_string() || !value["schemaRevision"].is_string())
                return Invalid<PrefabValueOverrideSet>();
            if (const auto version = Detail::ReadUnsigned<std::uint32_t>(value["schema"]); version.HasError() || version.Value() != 1)
                return Invalid<PrefabValueOverrideSet>("Unsupported value override schema.");
            for (const char *field : {"records", "conflicts", "orphans"})
                if (!value[field].is_array() || value[field].size() > policy.maximumOverrideRecords)
                    return Invalid<PrefabValueOverrideSet>();
            if (const auto issues = value["conflicts"].size() + value["orphans"].size();
                issues > policy.maximumConflictAndOrphanRecords || issues > policy.maximumOverrideRecords ||
                value["records"].size() > policy.maximumOverrideRecords - issues)
                return Result<PrefabValueOverrideSet>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
            auto asset = Assets::AssetId::Parse(value["sourcePrefab"].get_ref<const std::string &>());
            auto source = Detail::ParseRevision(value["authoredAgainst"]);
            auto schema = ParseSha256(value["schemaRevision"].get_ref<const std::string &>());
            if (asset.HasError() || source.HasError() || schema.HasError())
                return Invalid<PrefabValueOverrideSet>();
            return Result<PrefabValueOverrideSet>::Success({asset.Value(), std::move(source).Value(), schema.Value(), {}, {}, {}});
        }

        /** @brief Preserve all authored and observed value-conflict identities together after validating the complete entry. */
        Result<void> DecodeValueConflict(const Json &entry, PrefabValueOverrideSet &set) {
            if (!Foundation::HasAllowedFields(entry,
                                              {"record", "authoredAgainst", "schemaRevision", "actualSourceValue", "observedSource"}) ||
                !entry["actualSourceValue"].is_string() || !entry["schemaRevision"].is_string())
                return Invalid<void>();
            auto record = DecodeRecord(entry["record"]);
            auto source = Detail::ParseRevision(entry["observedSource"]);
            auto digest = ParseSha256(entry["actualSourceValue"].get_ref<const std::string &>());
            auto authored = Detail::ParseRevision(entry["authoredAgainst"]);
            auto schema = ParseSha256(entry["schemaRevision"].get_ref<const std::string &>());
            if (record.HasError() || source.HasError() || digest.HasError() || authored.HasError() || schema.HasError())
                return Invalid<void>();
            set.conflicts.emplace_back(std::move(record).Value(), std::move(authored).Value(), schema.Value(), digest.Value(),
                                       std::move(source).Value());
            return Result<void>::Success();
        }

        /** @brief Decode preserved evidence without discarding its exact original typed operation. */
        Result<void> DecodeIssues(const Json &value, PrefabValueOverrideSet &set) {
            for (const auto &entry : value["conflicts"]) {
                if (const auto record = entry.find("record"); record != entry.end() && IsComponentRecord(*record)) {
                    auto decoded = DecodeComponentConflict(entry);
                    if (decoded.HasError())
                        return Result<void>::Failure(decoded.ErrorValue());
                    set.componentConflicts.emplace_back(std::move(decoded).Value());
                    continue;
                }
                if (auto decoded = DecodeValueConflict(entry, set); decoded.HasError())
                    return decoded;
            }
            for (const auto &entry : value["orphans"]) {
                if (const auto record = entry.find("record"); record != entry.end() && IsComponentRecord(*record)) {
                    auto decoded = DecodeComponentOrphan(entry);
                    if (decoded.HasError())
                        return Result<void>::Failure(decoded.ErrorValue());
                    set.componentOrphans.emplace_back(std::move(decoded).Value());
                    continue;
                }
                if (!Foundation::HasAllowedFields(entry, {"record", "authoredAgainst", "schemaRevision", "reason"}) ||
                    !entry["schemaRevision"].is_string())
                    return Invalid<void>();
                auto record = DecodeRecord(entry["record"]);
                auto reason = Detail::ReadUnsigned<std::uint32_t>(entry["reason"]);
                if (record.HasError() || reason.HasError() ||
                    reason.Value() > static_cast<unsigned>(PrefabValueOverrideIssue::SchemaChanged))
                    return Invalid<void>();
                auto authored = Detail::ParseRevision(entry["authoredAgainst"]);
                auto schema = ParseSha256(entry["schemaRevision"].get_ref<const std::string &>());
                if (authored.HasError() || schema.HasError())
                    return Invalid<void>();
                set.orphans.emplace_back(std::move(record).Value(), std::move(authored).Value(), schema.Value(),
                                         static_cast<PrefabValueOverrideIssue>(reason.Value()));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ValidatePrefabValueOverrideSet */
    Result<void> ValidatePrefabValueOverrideSet(const PrefabValueOverrideSet &set, const PrefabLimitProfile &limits) {
        try {
            return ValidateSet(set, limits);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    /** @copydoc SerializePrefabValueOverrides */
    Result<std::string> SerializePrefabValueOverrides(const PrefabValueOverrideSet &set, const PrefabLimitProfile &limits) {
        try {
            if (auto valid = ValidateSet(set, limits); valid.HasError())
                return Result<std::string>::Failure(valid.ErrorValue());
            auto records = EncodeCombinedRecords(set);
            auto conflicts = EncodeCombinedConflicts(set);
            auto orphans = EncodeCombinedOrphans(set);
            auto bytes = EncodeSet(set, std::move(records), std::move(conflicts), std::move(orphans)).dump(0);
            if (bytes.size() > limits.Policy().maximumOverrideSetBytes)
                return Result<std::string>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
            return Result<std::string>::Success(std::move(bytes));
        } catch (const std::bad_alloc &) {
            return Result<std::string>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    /** @copydoc ParsePrefabValueOverrides */
    Result<PrefabValueOverrideSet> ParsePrefabValueOverrides(const std::string_view bytes, const PrefabLimitProfile &limits) {
        if (bytes.size() > limits.Policy().maximumOverrideSetBytes)
            return Result<PrefabValueOverrideSet>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
        try {
            Detail::JsonParseGuard guard{32};
            const auto value = Json::parse(bytes.begin(), bytes.end(), std::ref(guard), false);
            if (value.is_discarded() || guard.HasDuplicate() || guard.IsTooDeep())
                return Invalid<PrefabValueOverrideSet>();
            auto decoded = DecodeHeader(value, limits.Policy());
            if (decoded.HasError())
                return decoded;
            auto set = std::move(decoded).Value();
            for (const auto &record : value["records"]) {
                if (IsComponentRecord(record)) {
                    auto parsed = DecodeComponentRecord(record);
                    if (parsed.HasError())
                        return Result<PrefabValueOverrideSet>::Failure(parsed.ErrorValue());
                    set.componentRecords.emplace_back(std::move(parsed).Value());
                    continue;
                }
                auto parsed = DecodeRecord(record);
                if (parsed.HasError())
                    return Invalid<PrefabValueOverrideSet>();
                set.records.push_back(std::move(parsed).Value());
            }
            if (auto issues = DecodeIssues(value, set); issues.HasError())
                return Invalid<PrefabValueOverrideSet>();
            auto canonical = SerializePrefabValueOverrides(set, limits);
            if (canonical.HasError())
                return Result<PrefabValueOverrideSet>::Failure(canonical.ErrorValue());
            if (canonical.Value() != bytes)
                return Invalid<PrefabValueOverrideSet>("Override bytes are not canonical.");
            return Result<PrefabValueOverrideSet>::Success(std::move(set));
        } catch (const std::bad_alloc &) {
            return Result<PrefabValueOverrideSet>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        } catch (const Json::exception &) {
            return Invalid<PrefabValueOverrideSet>();
        }
    }
}  // namespace Horo::Prefab
