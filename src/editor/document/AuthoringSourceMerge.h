#pragma once

/** @file AuthoringSourceMerge.h @brief Private three-way merge helpers for authored document records. */

#include <array>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>

namespace Horo::Editor {
    /** @brief Returns the selected side of a three-way merge, or nullopt on an overlapping edit. */
    template <typename Value, typename Equal = std::equal_to<>>
    [[nodiscard]] std::optional<std::reference_wrapper<const Value>> SelectAuthoringMerge(const Value &base, const Value &local,
                                                                                          const Value &disk, Equal equal = {}) {
        if (equal(base, local))
            return std::cref(disk);
        if (equal(base, disk) || equal(local, disk))
            return std::cref(local);
        return std::nullopt;
    }

    /** @brief Merges additions, removals and replacements by stable identity, rejecting same-record conflicts. */
    template <typename Range, typename Key, typename Equal = std::equal_to<>>
    [[nodiscard]] auto MergeAuthoringRecords(const Range &base, const Range &local, const Range &disk, Key key, Equal equal = {}) {
        using Record = std::ranges::range_value_t<Range>;
        using Id = std::remove_cvref_t<std::invoke_result_t<Key, const Record &>>;
        using Selected = std::map<Id, const Record *>;
        std::map<Id, std::array<const Record *, 3>> entries;
        const std::array versions{&base, &local, &disk};
        for (std::size_t index = 0; index < versions.size(); ++index) {
            for (const auto &record : *versions[index])
                entries[std::invoke(key, record)][index] = &record;
        }
        const auto equivalent = [&](const Record *left, const Record *right) {
            return left == right || (left && right && equal(*left, *right));
        };
        Selected merged;
        for (const auto &[id, versionsForId] : entries) {
            const auto &[baseRecord, localRecord, diskRecord] = versionsForId;
            const auto selected = SelectAuthoringMerge(baseRecord, localRecord, diskRecord, equivalent);
            if (!selected)
                return std::optional<Selected>{};
            if (const auto *record = selected->get())
                merged.emplace(id, record);
        }
        return std::optional{std::move(merged)};
    }
}  // namespace Horo::Editor
