#include "OwnedJsonReader.h"
#include "PrefabValueOverridesInternal.h"

#include <algorithm>
#include <new>

namespace Horo::Prefab::ValueDetail {
    /** @brief Encode metadata independently so exact array lengths can be admitted before the complete writer is allocated. */
    Wire EncodeSet(const PrefabValueOverrideSet &set, Wire records, Wire conflicts, Wire orphans) {
        return Wire{{"schema", 1U},
                    {"sourcePrefab", set.sourcePrefab.ToString()},
                    {"authoredAgainst", EncodeRevision(set.authoredAgainst)},
                    {"schemaRevision", FormatSha256(set.schemaRevision)},
                    {"records", std::move(records)},
                    {"conflicts", std::move(conflicts)},
                    {"orphans", std::move(orphans)}};
    }

}  // namespace Horo::Prefab::ValueDetail

namespace Horo::Prefab {
    namespace {
        using namespace ValueDetail;

        /** @brief Reject target/type confusion even when no live component schema is available. */
        Result<void> ValidateRecord(const PrefabValueOverrideRecord &record) {
            if (record.target.valueless_by_exception() || record.value.valueless_by_exception())
                return Invalid<void>();
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

        /** @brief Orders only bounded borrowed record references; source payloads and preserved contexts are never copied or mutated. */
        template <class Entry, class Record> std::vector<const Entry *> Ordered(const std::vector<Entry> &entries, Record record) {
            std::vector<const Entry *> ordered;
            ordered.reserve(entries.size());
            for (const auto &entry : entries)
                ordered.push_back(&entry);
            std::ranges::sort(ordered, [&](const auto *a, const auto *b) {
                return TargetLess(record(*a).target, record(*b).target);
            });
            return ordered;
        }

        /** @brief Detects duplicate semantic targets before any value-specific error can depend on which duplicate arrived first. */
        Result<void> PreflightValueClaims(const PrefabValueOverrideSet &set) {
            std::vector<const PrefabValueOverrideRecord *> records;
            records.reserve(set.records.size() + set.conflicts.size() + set.orphans.size());
            for (const auto &record : set.records)
                records.push_back(&record);
            for (const auto &conflict : set.conflicts)
                records.push_back(&conflict.original);
            for (const auto &orphan : set.orphans)
                records.push_back(&orphan.original);
            if (std::ranges::any_of(records, [](const auto *record) {
                return record->target.valueless_by_exception();
            }))
                return Invalid<void>();
            std::ranges::sort(records, [](const auto *a, const auto *b) {
                return TargetLess(a->target, b->target);
            });
            for (std::size_t i = 1; i < records.size(); ++i)
                if (records[i - 1]->target == records[i]->target)
                    return Result<void>::Failure(
                        MakeError(PrefabErrors::OverrideConflict, "Duplicate instance value targets are not last-write-wins."));
            return Result<void>::Success();
        }

        /** @brief Admits ordinary values in canonical stable-target order. */
        Result<void> ValidateValues(const PrefabValueOverrideSet &set, const PrefabProjectPolicy &policy, std::size_t &bytes) {
            for (const auto *record : Ordered(set.records, [](const auto &entry) -> const auto & {
                return entry;
            })) {
                if (auto valid = ValidateRecord(*record); valid.HasError())
                    return valid;
                if (auto charged = Charge(EncodeRecord(*record), bytes, policy, true); charged.HasError())
                    return charged;
            }
            return Result<void>::Success();
        }

        /** @brief Keeps complete conflict evidence while selecting deterministic validation failures. */
        Result<void> ValidateConflicts(const PrefabValueOverrideSet &set, const PrefabProjectPolicy &policy, std::size_t &bytes) {
            for (const auto *conflict : Ordered(set.conflicts, [](const auto &entry) -> const auto & {
                return entry.original;
            })) {
                if (auto valid = ValidatePreservedRecord(conflict->original, conflict->authoredAgainst, policy); valid.HasError())
                    return valid;
                if (!Detail::IsCanonicalProjectVersion(conflict->observedSource.projectVersion))
                    return Invalid<void>();
                if (auto charged = Charge(EncodeValueConflict(*conflict), bytes, policy, false); charged.HasError())
                    return charged;
            }
            return Result<void>::Success();
        }

        /** @brief Keeps original orphan records losslessly without allowing their serialized order to select diagnostics. */
        Result<void> ValidateOrphans(const PrefabValueOverrideSet &set, const PrefabProjectPolicy &policy, std::size_t &bytes) {
            for (const auto *orphan : Ordered(set.orphans, [](const auto &entry) -> const auto & {
                return entry.original;
            })) {
                if (auto valid = ValidatePreservedRecord(orphan->original, orphan->authoredAgainst, policy); valid.HasError())
                    return valid;
                if (orphan->reason > PrefabValueOverrideIssue::SchemaChanged)
                    return Invalid<void>();
                if (auto charged = Charge(EncodeValueOrphan(*orphan), bytes, policy, false); charged.HasError())
                    return charged;
            }
            return Result<void>::Success();
        }

        /** @brief Runs shared structural claims before canonical value/topology admission and exact byte accounting. */
        Result<void> ValidateSet(const PrefabValueOverrideSet &set, const PrefabLimitProfile &limits) {
            const auto &policy = limits.Policy();
            if (auto shape = ValidateSetShape(set, policy); shape.HasError())
                return shape;
            if (auto claims = PreflightValueClaims(set); claims.HasError())
                return claims;
            if (auto claims = PreflightComponentClaims(set); claims.HasError())
                return claims;
            std::size_t bytes = EncodeSet(set).dump(0).size();
            if (auto values = ValidateValues(set, policy, bytes); values.HasError())
                return values;
            if (auto conflicts = ValidateConflicts(set, policy, bytes); conflicts.HasError())
                return conflicts;
            if (auto orphans = ValidateOrphans(set, policy, bytes); orphans.HasError())
                return orphans;
            if (auto components = ValidateComponentRecords(set, policy, bytes); components.HasError())
                return components;
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

        /** @brief Fixes syntax-error order using bounded owned row bytes before any decoded record storage is admitted. */
        std::vector<const Json *> RawOrder(const Json &rows) {
            std::vector<std::pair<std::string, const Json *>> keys;
            keys.reserve(rows.size());
            for (const auto &row : rows)
                keys.emplace_back(row.dump(0), &row);
            std::ranges::sort(keys, {}, &decltype(keys)::value_type::first);
            std::vector<const Json *> ordered;
            ordered.reserve(keys.size());
            for (const auto &key : keys)
                ordered.push_back(key.second);
            return ordered;
        }

        /** @brief Decodes both supported normal operation families through their existing strict typed codecs. */
        Result<void> DecodeRecords(const Json &value, PrefabValueOverrideSet &set) {
            for (const auto *record : RawOrder(value["records"])) {
                if (IsComponentRecord(*record)) {
                    auto parsed = DecodeComponentRecord(*record);
                    if (parsed.HasError())
                        return Result<void>::Failure(parsed.ErrorValue());
                    set.componentRecords.emplace_back(std::move(parsed).Value());
                } else {
                    auto parsed = DecodeRecord(*record);
                    if (parsed.HasError())
                        return Result<void>::Failure(parsed.ErrorValue());
                    set.records.push_back(std::move(parsed).Value());
                }
            }
            return Result<void>::Success();
        }

        /** @brief Retains exact original context for one value orphan; no unavailable target is silently reassigned. */
        Result<void> DecodeValueOrphan(const Json &entry, PrefabValueOverrideSet &set) {
            if (!Foundation::HasAllowedFields(entry, {"record", "authoredAgainst", "schemaRevision", "reason"}) ||
                !entry["schemaRevision"].is_string())
                return Invalid<void>();
            auto record = DecodeRecord(entry["record"]);
            auto reason = Detail::ReadUnsigned<std::uint32_t>(entry["reason"]);
            if (record.HasError() || reason.HasError() || reason.Value() > static_cast<unsigned>(PrefabValueOverrideIssue::SchemaChanged))
                return Invalid<void>();
            auto authored = Detail::ParseRevision(entry["authoredAgainst"]);
            auto schema = ParseSha256(entry["schemaRevision"].get_ref<const std::string &>());
            if (authored.HasError() || schema.HasError())
                return Invalid<void>();
            set.orphans.emplace_back(std::move(record).Value(), std::move(authored).Value(), schema.Value(),
                                     static_cast<PrefabValueOverrideIssue>(reason.Value()));
            return Result<void>::Success();
        }

        /** @brief Decodes preserved conflict entries in a record-order-independent syntax order. */
        Result<void> DecodeConflicts(const Json &value, PrefabValueOverrideSet &set) {
            for (const auto *entry : RawOrder(value["conflicts"])) {
                if (const auto record = entry->find("record"); record != entry->end() && IsComponentRecord(*record)) {
                    auto decoded = DecodeComponentConflict(*entry);
                    if (decoded.HasError())
                        return Result<void>::Failure(decoded.ErrorValue());
                    set.componentConflicts.emplace_back(std::move(decoded).Value());
                } else if (auto decoded = DecodeValueConflict(*entry, set); decoded.HasError())
                    return decoded;
            }
            return Result<void>::Success();
        }

        /** @brief Decodes preserved unavailable intent through the same strict value and component codecs. */
        Result<void> DecodeOrphans(const Json &value, PrefabValueOverrideSet &set) {
            for (const auto *entry : RawOrder(value["orphans"])) {
                if (const auto record = entry->find("record"); record != entry->end() && IsComponentRecord(*record)) {
                    auto decoded = DecodeComponentOrphan(*entry);
                    if (decoded.HasError())
                        return Result<void>::Failure(decoded.ErrorValue());
                    set.componentOrphans.emplace_back(std::move(decoded).Value());
                } else if (auto decoded = DecodeValueOrphan(*entry, set); decoded.HasError())
                    return decoded;
            }
            return Result<void>::Success();
        }

        /** @brief Stores decoded records under canonical semantic keys while preserving all exact original payloads and contexts. */
        void CanonicalizeRecords(PrefabValueOverrideSet &set) {
            std::ranges::sort(set.records, [](const auto &a, const auto &b) {
                return TargetLess(a.target, b.target);
            });
            std::ranges::sort(set.conflicts, [](const auto &a, const auto &b) {
                return TargetLess(a.original.target, b.original.target);
            });
            std::ranges::sort(set.orphans, [](const auto &a, const auto &b) {
                return TargetLess(a.original.target, b.original.target);
            });
            std::ranges::sort(set.componentRecords, ComponentRecordLess);
            std::ranges::sort(set.componentConflicts, [](const auto &a, const auto &b) {
                return ComponentRecordLess(a.original, b.original);
            });
            std::ranges::sort(set.componentOrphans, [](const auto &a, const auto &b) {
                return ComponentRecordLess(a.original, b.original);
            });
        }

        /** @brief Compares exact record bytes as a bounded multiset; only array order loses significance. */
        bool SameRows(const Json &input, Wire expected) {
            std::vector<std::string> actualBytes;
            std::vector<std::string> expectedBytes;
            for (const auto &row : input)
                actualBytes.push_back(row.dump(0));
            for (const auto &row : expected.get_ref<Wire::array_t &>())
                expectedBytes.push_back(row.dump(0));
            std::ranges::sort(actualBytes);
            std::ranges::sort(expectedBytes);
            return actualBytes == expectedBytes;
        }

        /** @brief Keeps the original canonical header, scalar spelling and field order independently from allowed record permutations. */
        bool SameHeader(const Json &input, const PrefabValueOverrideSet &set, const std::string_view bytes) {
            return input.dump(0) == bytes && input["schema"].dump(0) == "1" &&
                   input["sourcePrefab"].dump(0) == Wire{set.sourcePrefab.ToString()}.dump(0) &&
                   input["authoredAgainst"].dump(0) == EncodeRevision(set.authoredAgainst).dump(0) &&
                   input["schemaRevision"].dump(0) == Wire{FormatSha256(set.schemaRevision)}.dump(0);
        }

        /** @brief Admits a complete parsed candidate before normalizing only its typed record storage. */
        Result<PrefabValueOverrideSet> DecodeParsed(const Json &value, std::string_view bytes, const PrefabLimitProfile &limits) {
            auto decoded = DecodeHeader(value, limits.Policy());
            if (decoded.HasError())
                return decoded;
            auto set = std::move(decoded).Value();
            if (auto records = DecodeRecords(value, set); records.HasError())
                return Result<PrefabValueOverrideSet>::Failure(records.ErrorValue());
            if (auto conflicts = DecodeConflicts(value, set); conflicts.HasError())
                return Result<PrefabValueOverrideSet>::Failure(conflicts.ErrorValue());
            if (auto orphans = DecodeOrphans(value, set); orphans.HasError())
                return Result<PrefabValueOverrideSet>::Failure(orphans.ErrorValue());
            if (auto valid = ValidateSet(set, limits); valid.HasError())
                return Result<PrefabValueOverrideSet>::Failure(valid.ErrorValue());
            if (!SameHeader(value, set, bytes) || !SameRows(value["records"], EncodeCombinedRecords(set)) ||
                !SameRows(value["conflicts"], EncodeCombinedConflicts(set)) || !SameRows(value["orphans"], EncodeCombinedOrphans(set)))
                return Invalid<PrefabValueOverrideSet>("Override bytes are not canonical apart from record order.");
            CanonicalizeRecords(set);
            return Result<PrefabValueOverrideSet>::Success(std::move(set));
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
            auto value = JsonEncoding::Detail::ReadOwnedJson(bytes, limits.Policy().maximumOverrideSetBytes, 32);
            if (!value)
                return Invalid<PrefabValueOverrideSet>();
            return DecodeParsed(value->Get(), bytes, limits);
        } catch (const std::bad_alloc &) {
            return Result<PrefabValueOverrideSet>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        } catch (const Json::exception &) {
            return Invalid<PrefabValueOverrideSet>();
        }
    }
}  // namespace Horo::Prefab
