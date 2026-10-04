#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveTelemetry.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>

namespace Horo::Runtime {
    namespace {
        constexpr std::array<std::string_view, 9> Stages{"capture", "encode", "migrate",  "commit",     "restore",
                                                         "sync",    "queue",  "recovery", "participant"};
        constexpr std::array<std::string_view, 4> Outcomes{"succeeded", "failed", "cancelled", "interrupted"};
        constexpr std::array<std::string_view, 5> Evidence{"duration_ns", "bytes", "retries", "dropped_work", "queue_depth"};

        struct Summary {
            std::array<std::array<std::uint64_t, 4>, 9> counts{};
            std::array<std::array<std::uint64_t, 5>, 9> evidence{};
            std::uint64_t rows{}, invalid{}, missing{}, inspected{};
            bool truncated{};
        };

        /** @brief Saturates aggregate evidence rather than allowing retained inputs to wrap counters. */
        void Add(std::uint64_t &total, const std::uint64_t value) noexcept {
            total += std::min(value, std::numeric_limits<std::uint64_t>::max() - total);
        }

        /** @brief Accepts only fixed Save event names and unsigned scalar evidence; ignores all other fields. */
        void Include(Summary &summary, const std::string &row) {
            const auto record = nlohmann::json::parse(row, nullptr, false);
            if (record.is_discarded() || !record.is_object()) {
                ++summary.invalid;
                return;
            }
            const auto category = record.find("category");
            if (category == record.end() || *category != "runtime.save.stage")
                return;
            const auto fields = record.find("fields");
            if (fields == record.end() || !fields->is_object()) {
                ++summary.invalid;
                return;
            }
            const auto stage = fields->find("stage"), outcome = fields->find("outcome");
            if (stage == fields->end() || outcome == fields->end() || !stage->is_string() || !outcome->is_string()) {
                ++summary.invalid;
                return;
            }
            const auto stageIndex = std::ranges::find(Stages, stage->get_ref<const std::string &>());
            const auto outcomeIndex = std::ranges::find(Outcomes, outcome->get_ref<const std::string &>());
            std::array<std::uint64_t, 5> values{};
            for (std::size_t index = 0; index < Evidence.size(); ++index) {
                const auto value = fields->find(Evidence[index]);
                if (value == fields->end() || !value->is_number_unsigned()) {
                    ++summary.invalid;
                    return;
                }
                values[index] = value->get<std::uint64_t>();
            }
            if (stageIndex == Stages.end() || outcomeIndex == Outcomes.end()) {
                ++summary.invalid;
                return;
            }
            const auto index = static_cast<std::size_t>(stageIndex - Stages.begin());
            ++summary.counts[index][static_cast<std::size_t>(outcomeIndex - Outcomes.begin())];
            ++summary.rows;
            for (std::size_t field = 0; field < 4; ++field)
                Add(summary.evidence[index][field], values[field]);
            summary.evidence[index][4] = std::max(summary.evidence[index][4], values[4]);
        }

        /** @brief Reads bounded rows and bytes only during explicit diagnostic export. */
        void Inspect(Summary &summary, const std::filesystem::path &path, std::size_t &bytes) {
            std::ifstream input{path, std::ios::binary};
            if (!input) {
                ++summary.missing;
                return;
            }
            std::string row;
            row.reserve(4096);
            char value{};
            while (input.get(value)) {
                if (++bytes > 8U * 1024U * 1024U || summary.inspected >= 2048 || row.size() >= 4096) {
                    summary.truncated = true;
                    return;
                }
                if (value == '\n') {
                    ++summary.inspected;
                    Include(summary, row);
                    row.clear();
                } else {
                    row.push_back(value);
                }
            }
            if (!row.empty())
                ++summary.invalid;  // Unflushed partial rows are never authoritative observations.
        }

        /** @brief Serializes only canonical closed names and validated numeric aggregates. */
        std::string Serialize(const Summary &summary) {
            nlohmann::json output{{"version", 1},
                                  {"scope", "retained_logs"},
                                  {"observations", summary.rows},
                                  {"inspected_rows", summary.inspected},
                                  {"invalid_rows", summary.invalid},
                                  {"missing_sources", summary.missing},
                                  {"truncated", summary.truncated}};
            for (std::size_t stage = 0; stage < Stages.size(); ++stage) {
                auto &entry = output["stages"][Stages[stage]];
                for (std::size_t outcome = 0; outcome < Outcomes.size(); ++outcome)
                    entry["outcomes"][Outcomes[outcome]] = summary.counts[stage][outcome];
                for (std::size_t field = 0; field < Evidence.size(); ++field)
                    entry[Evidence[field]] = summary.evidence[stage][field];
            }
            return output.dump();
        }
    }  // namespace

    /** @copydoc SummarizeSaveTelemetry */
    Result<std::pair<std::string, std::string>> SummarizeSaveTelemetry(const std::span<const std::filesystem::path> retainedLogs) {
        try {
            Summary summary;
            std::size_t bytes{};
            summary.truncated = retainedLogs.size() > 32;
            for (const auto &path : retainedLogs.first(std::min<std::size_t>(retainedLogs.size(), 32))) {
                Inspect(summary, path, bytes);
                if (bytes > 8U * 1024U * 1024U || summary.inspected >= 2048)
                    break;
            }
            return Result<std::pair<std::string, std::string>>::Success({"save.summary", Serialize(summary)});
        } catch (const std::bad_alloc &) {
            return Result<std::pair<std::string, std::string>>::Failure(MakeError(SaveErrors::OperationAllocationFailed));
        }
    }
}  // namespace Horo::Runtime
