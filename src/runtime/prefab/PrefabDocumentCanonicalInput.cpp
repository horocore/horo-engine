#include "Horo/Prefab/PrefabDocument.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "JsonUtils.h"

#include <algorithm>
#include <new>
#include <nlohmann/json.hpp>

namespace Horo::Prefab {
    namespace {
        using Ordered = nlohmann::ordered_json;

        /** @brief Exact per-row spelling is independent of the allowed ordering of complete records. */
        std::vector<std::string> Rows(const Ordered &array) {
            std::vector<std::string> rows;
            rows.reserve(array.size());
            for (const auto &row : array)
                rows.push_back(row.dump(0));
            std::ranges::sort(rows);
            return rows;
        }

        /** @brief Replaces only a proven-equal multiset of canonical rows in validation scratch. */
        bool MatchLayer(Ordered &actual, const Ordered &expected) {
            if (!actual.is_object() || !expected.is_object())
                return false;
            for (const auto *field : {"records", "conflicts", "orphans"}) {
                if (!actual.contains(field) || !actual[field].is_array() || !expected.contains(field))
                    return false;
                if (actual[field].size() != expected[field].size())
                    return false;
                if (Rows(actual[field]) != Rows(expected[field]))
                    return false;
                actual[field] = expected[field];
            }
            return actual == expected;
        }

        /** @brief Preserves concrete placement identity/order and all metadata while normalizing only a present layer. */
        bool MatchPlacement(Ordered &actual, const Ordered &expected) {
            if (actual.contains("valueOverrides") != expected.contains("valueOverrides"))
                return false;
            return !actual.contains("valueOverrides") || MatchLayer(actual["valueOverrides"], expected["valueOverrides"]);
        }

        /** @brief Permits no composition-shape, placement-order or field-order changes outside admitted override arrays. */
        bool MatchComposition(Ordered &actual, const Ordered &expected) {
            if (actual.contains("variantOverrides") != expected.contains("variantOverrides"))
                return false;
            if (actual.contains("variantOverrides") && !MatchLayer(actual["variantOverrides"], expected["variantOverrides"]))
                return false;
            if (actual.contains("nestedPlacements") != expected.contains("nestedPlacements"))
                return false;
            if (!actual.contains("nestedPlacements"))
                return true;
            auto &placements = actual["nestedPlacements"];
            const auto &targets = expected["nestedPlacements"];
            if (!placements.is_array() || placements.size() != targets.size())
                return false;
            for (std::size_t i = 0; i < placements.size(); ++i)
                if (!MatchPlacement(placements[i], targets[i]))
                    return false;
            return true;
        }

        /** @brief Releases bounded duplicate/depth validation storage before allocating ordered validation scratch. */
        bool ValidJson(std::string_view source) {
            Foundation::JsonParseGuard guard{PrefabHardLimits::SourceJsonDepth};
            const auto checked = guard.Parse(source.begin(), source.end());
            return !checked.is_discarded() && !guard.HasDuplicate() && !guard.IsTooDeep();
        }

        /** @brief Establishes a duplicate/depth/lexeme witness before any validation-only array substitution. */
        bool MatchInput(std::string_view source, const std::string &canonical) {
            if (!ValidJson(source))
                return false;
            auto actual = Ordered::parse(source);
            if (actual.dump(2) + "\n" != source)
                return false;
            const auto expected = Ordered::parse(canonical);
            if (actual.contains("composition") != expected.contains("composition"))
                return false;
            if (actual.contains("composition") && !MatchComposition(actual["composition"], expected["composition"]))
                return false;
            return actual.dump(2) + "\n" == canonical;
        }
    }  // namespace

    /** @copydoc PrefabDocument::MatchesCanonicalInput */
    Result<bool> PrefabDocument::MatchesCanonicalInput(std::string_view source) const {
        if (source.size() > PrefabHardLimits::SourceDocumentBytes)
            return Result<bool>::Success(false);
        try {
            auto canonical = SerializeCanonical();
            if (canonical.HasError())
                return Result<bool>::Failure(canonical.ErrorValue());
            if (canonical.Value() == source)
                return Result<bool>::Success(true);
            return Result<bool>::Success(MatchInput(source, canonical.Value()));
        } catch (const std::bad_alloc &) {
            return Result<bool>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        } catch (const Ordered::exception &) {
            return Result<bool>::Success(false);
        }
    }
}  // namespace Horo::Prefab
