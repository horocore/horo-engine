#include "PrefabValueOverridesInternal.h"

#include <algorithm>
#include <tuple>

namespace Horo::Prefab::ValueDetail {
    namespace {
        /** @brief Own fixed-size object identity while borrowing type text only for this synchronous serialization call. */
        struct SortKey {
            PrefabObjectAddress object;
            std::string_view type;
            std::uint64_t instance{};
            unsigned targetClass{};
            std::uint64_t property{};
            unsigned operation{};
        };

        /** @brief Pair complete owned wire bytes with source-borrowed ordering evidence; no views leave the builder. */
        struct OrderedRecord {
            SortKey key;
            Wire encoded;
        };

        /** @brief Preserve value-only V1 ordering while giving property and component targets one shared key space. */
        SortKey Key(const PrefabValueOverrideRecord &record) {
            if (const auto *object = std::get_if<PrefabObjectAddress>(&record.target))
                return {*object, {}, 0, 0, 0, 0};
            const auto &property = std::get<PrefabPropertyAddress>(record.target);
            return {property.Object(),
                    property.ComponentType().Value(),
                    property.ComponentInstance().Value(),
                    2,
                    property.Property().Value(),
                    0};
        }

        /** @brief Borrow target dimensions from the original record rather than a temporary copied component identity. */
        SortKey Key(const PrefabComponentOverrideRecord &record) {
            if (const auto *add = std::get_if<PrefabAddComponentOverride>(&record))
                return {add->object, add->payload.component.typeId.Value(), add->payload.instance.Value(), 1, 0, 5};
            const auto &remove = std::get<PrefabRemoveComponentOverride>(record);
            return {remove.target.object, remove.target.type.Value(), remove.target.instance.Value(), 1, 0, 6};
        }

        /** @brief Encode already bounded records while retaining exact source targets for canonical ordering. */
        template <class Entries, class Encoder, class Record>
        void AppendRows(std::vector<OrderedRecord> &rows, const Entries &entries, Encoder encoder, Record record) {
            for (const auto &entry : entries)
                rows.emplace_back(Key(record(entry)), encoder(entry));
        }

        /** @brief Publish one canonical wire array independent of typed-vector or serialized arrival order. */
        Wire Finish(std::vector<OrderedRecord> rows) {
            std::ranges::sort(rows, [](const OrderedRecord &left, const OrderedRecord &right) {
                const auto &a = left.key;
                const auto &b = right.key;
                if (const auto order = CompareObjectAddresses(a.object, b.object); order != 0)
                    return std::is_lt(order);
                return std::tie(a.type, a.instance, a.targetClass, a.property, a.operation) <
                       std::tie(b.type, b.instance, b.targetClass, b.property, b.operation);
            });
            auto encoded = Wire::array();
            encoded.get_ref<Wire::array_t &>().reserve(rows.size());
            for (auto &row : rows)
                encoded.push_back(std::move(row.encoded));
            return encoded;
        }
    }  // namespace

    /** @brief Preserve existing value conflict wire fields in the combined canonical array. */
    Wire EncodeValueConflict(const PrefabValueOverrideConflict &conflict) {
        return Wire{{"record", EncodeRecord(conflict.original)},
                    {"authoredAgainst", EncodeRevision(conflict.authoredAgainst)},
                    {"schemaRevision", FormatSha256(conflict.schemaRevision)},
                    {"actualSourceValue", FormatSha256(conflict.actualSourceValue)},
                    {"observedSource", EncodeRevision(conflict.observedSource)}};
    }

    /** @brief Preserve existing value orphan bytes in the same typed envelope as component orphans. */
    Wire EncodeValueOrphan(const PrefabValueOverrideOrphan &orphan) {
        return Wire{{"record", EncodeRecord(orphan.original)},
                    {"authoredAgainst", EncodeRevision(orphan.authoredAgainst)},
                    {"schemaRevision", FormatSha256(orphan.schemaRevision)},
                    {"reason", static_cast<unsigned>(orphan.reason)}};
    }

    /** @brief Serialize normal value and component operations into the existing single records array. */
    Wire EncodeCombinedRecords(const PrefabValueOverrideSet &set) {
        std::vector<OrderedRecord> rows;
        rows.reserve(set.records.size() + set.componentRecords.size());
        const auto record = [](const auto &entry) -> const auto & {
            return entry;
        };
        AppendRows(rows, set.records, EncodeRecord, record);
        AppendRows(rows, set.componentRecords, EncodeComponentRecord, record);
        return Finish(std::move(rows));
    }

    /** @brief Serialize all preserved conflict kinds without separating their stable semantic ordering. */
    Wire EncodeCombinedConflicts(const PrefabValueOverrideSet &set) {
        std::vector<OrderedRecord> rows;
        rows.reserve(set.conflicts.size() + set.componentConflicts.size());
        const auto record = [](const auto &entry) -> const auto & {
            return entry.original;
        };
        AppendRows(rows, set.conflicts, EncodeValueConflict, record);
        AppendRows(rows, set.componentConflicts, EncodeComponentConflict, record);
        return Finish(std::move(rows));
    }

    /** @brief Serialize all preserved unavailable intent with original record identity and lossless payload bytes. */
    Wire EncodeCombinedOrphans(const PrefabValueOverrideSet &set) {
        std::vector<OrderedRecord> rows;
        rows.reserve(set.orphans.size() + set.componentOrphans.size());
        const auto record = [](const auto &entry) -> const auto & {
            return entry.original;
        };
        AppendRows(rows, set.orphans, EncodeValueOrphan, record);
        AppendRows(rows, set.componentOrphans, EncodeComponentOrphan, record);
        return Finish(std::move(rows));
    }
}  // namespace Horo::Prefab::ValueDetail
