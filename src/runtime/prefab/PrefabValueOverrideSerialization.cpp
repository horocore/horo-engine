#include "PrefabValueOverridesInternal.h"

#include <algorithm>
#include <new>

namespace Horo::Prefab {
    namespace {
        using namespace ValueDetail;

        template <class T, class Target> std::vector<const T *> Ordered(const std::vector<T> &items, Target target) {
            std::vector<const T *> ordered;
            ordered.reserve(items.size());
            for (const auto &item : items)
                ordered.push_back(&item);
            std::ranges::sort(ordered, [&target](const T *left, const T *right) {
                return TargetLess(target(*left), target(*right));
            });
            return ordered;
        }

        /** @brief Preserve exact conflict source evidence alongside the original owned operation. */
        Wire EncodeConflict(const PrefabValueOverrideConflict &conflict) {
            return Wire{{"record", EncodeRecord(conflict.original)},
                        {"authoredAgainst", EncodeRevision(conflict.authoredAgainst)},
                        {"schemaRevision", FormatSha256(conflict.schemaRevision)},
                        {"actualSourceValue", FormatSha256(conflict.actualSourceValue)},
                        {"observedSource", EncodeRevision(conflict.observedSource)}};
        }

        /** @brief Preserve unapplied intent with a closed typed unavailability reason. */
        Wire EncodeOrphan(const PrefabValueOverrideOrphan &orphan) {
            return Wire{{"record", EncodeRecord(orphan.original)},
                        {"authoredAgainst", EncodeRevision(orphan.authoredAgainst)},
                        {"schemaRevision", FormatSha256(orphan.schemaRevision)},
                        {"reason", static_cast<unsigned>(orphan.reason)}};
        }

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
            const bool transform = std::holds_alternative<PrefabObjectAddress>(record.target);
            if (transform != std::holds_alternative<Math::Transform>(record.value) ||
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

        /** @brief Admit every preserved record before encoding the whole authoring set. */
        Result<void> ValidateSet(const PrefabValueOverrideSet &set, const PrefabLimitProfile &limits) {
            const auto &policy = limits.Policy();
            if (!set.sourcePrefab.IsValid() || !Detail::IsCanonicalProjectVersion(set.authoredAgainst.projectVersion))
                return Invalid<void>();
            if (set.records.size() > policy.maximumOverrideRecords || set.conflicts.size() > policy.maximumConflictAndOrphanRecords ||
                set.orphans.size() > policy.maximumConflictAndOrphanRecords - set.conflicts.size() ||
                set.conflicts.size() + set.orphans.size() > policy.maximumOverrideRecords - set.records.size())
                return Result<void>::Failure(MakeError(PrefabErrors::WorkBudgetExceeded));
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
                if (auto charged = Charge(EncodeConflict(conflict), bytes, policy, false); charged.HasError())
                    return charged;
                all.push_back(&conflict.original);
            }
            for (const auto &orphan : set.orphans) {
                if (auto valid = ValidatePreservedRecord(orphan.original, orphan.authoredAgainst, policy); valid.HasError())
                    return valid;
                if (orphan.reason > PrefabValueOverrideIssue::SchemaChanged)
                    return Invalid<void>();
                if (auto charged = Charge(EncodeOrphan(orphan), bytes, policy, false); charged.HasError())
                    return charged;
                all.push_back(&orphan.original);
            }
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
            const auto version = Detail::ReadUnsigned<std::uint32_t>(value["schema"]);
            if (version.HasError() || version.Value() != 1)
                return Invalid<PrefabValueOverrideSet>("Unsupported value override schema.");
            for (const char *field : {"records", "conflicts", "orphans"})
                if (!value[field].is_array() || value[field].size() > policy.maximumOverrideRecords)
                    return Invalid<PrefabValueOverrideSet>();
            auto asset = Assets::AssetId::Parse(value["sourcePrefab"].get_ref<const std::string &>());
            auto source = Detail::ParseRevision(value["authoredAgainst"]);
            auto schema = ParseSha256(value["schemaRevision"].get_ref<const std::string &>());
            if (asset.HasError() || source.HasError() || schema.HasError())
                return Invalid<PrefabValueOverrideSet>();
            return Result<PrefabValueOverrideSet>::Success({asset.Value(), std::move(source).Value(), schema.Value(), {}, {}, {}});
        }

        /** @brief Decode preserved evidence without discarding its exact original typed operation. */
        Result<void> DecodeIssues(const Json &value, PrefabValueOverrideSet &set) {
            for (const auto &entry : value["conflicts"]) {
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
                set.conflicts.push_back(
                    {std::move(record).Value(), std::move(authored).Value(), schema.Value(), digest.Value(), std::move(source).Value()});
            }
            for (const auto &entry : value["orphans"]) {
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
                set.orphans.push_back({std::move(record).Value(), std::move(authored).Value(), schema.Value(),
                                       static_cast<PrefabValueOverrideIssue>(reason.Value())});
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
            auto records = Wire::array();
            auto conflicts = Wire::array();
            auto orphans = Wire::array();
            for (const auto *record : Ordered(set.records, [](const auto &item) -> const auto & {
                return item.target;
            }))
                records.push_back(EncodeRecord(*record));
            for (const auto *conflict : Ordered(set.conflicts, [](const auto &item) -> const auto & {
                return item.original.target;
            }))
                conflicts.push_back(EncodeConflict(*conflict));
            for (const auto *orphan : Ordered(set.orphans, [](const auto &item) -> const auto & {
                return item.original.target;
            }))
                orphans.push_back(EncodeOrphan(*orphan));
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
