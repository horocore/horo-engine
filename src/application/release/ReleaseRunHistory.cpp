#include "Horo/Release/ReleaseRunHistory.h"

#include "Horo/Release/ReleaseErrors.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace Horo::Release {
    namespace {
        using Json = nlohmann::ordered_json;
        constexpr std::uintmax_t MaximumHistoryBytes = 2U * 1024U * 1024U;
        constexpr std::size_t MaximumCapacity = 1024U;

        [[nodiscard]] Error InvalidHistory() {
            return MakeError(ReleaseErrors::PipelineOutputInvalid);
        }

        /** @brief Only completed jobs may leave durable history while another job is active. */
        [[nodiscard]] bool IsTerminal(const ReleaseJobState state) {
            using enum ReleaseJobState;
            return state == Succeeded || state == Failed || state == Cancelled;
        }

        /** @brief Checks the closed enum ranges before accepting durable records. */
        [[nodiscard]] bool ValidEntry(const ReleaseRunHistoryEntry &entry) {
            if (entry.job.value == 0U || entry.target.value == 0U || entry.operation == 0U || entry.createdUtcMilliseconds <= 0 ||
                entry.updatedUtcMilliseconds < entry.createdUtcMilliseconds ||
                (entry.finishedUtcMilliseconds.has_value() && (*entry.finishedUtcMilliseconds < entry.createdUtcMilliseconds ||
                                                               *entry.finishedUtcMilliseconds != entry.updatedUtcMilliseconds)) ||
                entry.state > ReleaseJobState::Cancelled || (entry.candidate.has_value() && entry.candidate->value == 0U) ||
                (entry.interruptedByRestart && (entry.state != ReleaseJobState::Failed || entry.finishedUtcMilliseconds.has_value())))
                return false;
            return std::ranges::all_of(entry.stages, [](const ReleaseStageState state) {
                return state <= ReleaseStageState::NotApplicable;
            }) && std::ranges::all_of(entry.attempts, [](const std::optional<ReleaseStageAttemptId> attempt) {
                return !attempt.has_value() || attempt->value != 0U;
            });
        }

        /** @brief Serializes only typed public-safe fields; worker messages and credentials are intentionally absent. */
        [[nodiscard]] Json WriteEntry(const ReleaseRunHistoryEntry &entry) {
            Json stages = Json::array();
            Json attempts = Json::array();
            for (const auto state : entry.stages)
                stages.push_back(static_cast<std::uint8_t>(state));
            for (const auto attempt : entry.attempts)
                attempts.push_back(attempt.has_value() ? Json(attempt->value) : Json(nullptr));
            return {{"job", entry.job.value},
                    {"target", entry.target.value},
                    {"operation", entry.operation},
                    {"revision", entry.revision},
                    {"state", static_cast<std::uint8_t>(entry.state)},
                    {"stages", std::move(stages)},
                    {"attempts", std::move(attempts)},
                    {"candidate", entry.candidate.has_value() ? Json(entry.candidate->value) : Json(nullptr)},
                    {"createdUtcMilliseconds", entry.createdUtcMilliseconds},
                    {"finishedUtcMilliseconds",
                     entry.finishedUtcMilliseconds.has_value() ? Json(*entry.finishedUtcMilliseconds) : Json(nullptr)},
                    {"updatedUtcMilliseconds", entry.updatedUtcMilliseconds},
                    {"interruptedByRestart", entry.interruptedByRestart}};
        }

        /** @brief Reads one exact supported record schema without accepting unknown or missing fields. */
        [[nodiscard]] ReleaseRunHistoryEntry ReadEntry(const Json &json, const std::uint8_t schemaVersion) {
            if (!json.is_object() || json.size() != (schemaVersion == 1U ? 11U : 12U) || !json.at("stages").is_array() ||
                json.at("stages").size() != ReleaseStageCount || !json.at("attempts").is_array() ||
                json.at("attempts").size() != ReleaseStageCount)
                throw std::invalid_argument("Invalid release history entry");
            ReleaseRunHistoryEntry entry;
            entry.job = {json.at("job").get<std::uint64_t>()};
            entry.target = {json.at("target").get<std::uint64_t>()};
            entry.operation = json.at("operation").get<OperationId>();
            entry.revision = json.at("revision").get<std::uint64_t>();
            entry.state = static_cast<ReleaseJobState>(json.at("state").get<std::uint8_t>());
            for (std::size_t index = 0; index < ReleaseStageCount; ++index) {
                entry.stages[index] = static_cast<ReleaseStageState>(json.at("stages").at(index).get<std::uint8_t>());
                if (!json.at("attempts").at(index).is_null())
                    entry.attempts[index] = ReleaseStageAttemptId{json.at("attempts").at(index).get<std::uint64_t>()};
            }
            if (!json.at("candidate").is_null())
                entry.candidate = ReleaseCandidateId{json.at("candidate").get<std::uint64_t>()};
            entry.createdUtcMilliseconds = json.at("createdUtcMilliseconds").get<std::int64_t>();
            entry.updatedUtcMilliseconds = json.at("updatedUtcMilliseconds").get<std::int64_t>();
            if (!json.at("finishedUtcMilliseconds").is_null())
                entry.finishedUtcMilliseconds = json.at("finishedUtcMilliseconds").get<std::int64_t>();
            if (schemaVersion == 2U)
                entry.interruptedByRestart = json.at("interruptedByRestart").get<bool>();
            if (!ValidEntry(entry))
                throw std::invalid_argument("Invalid release history identity");
            return entry;
        }

        /** @brief Reads the old file only after a bounded non-symlink file check. */
        [[nodiscard]] Result<std::string> ReadExisting(const std::filesystem::path &path) {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            if (error == std::errc::no_such_file_or_directory)
                return Result<std::string>::Success({});
            if (error || !std::filesystem::is_regular_file(status))
                return Result<std::string>::Failure(InvalidHistory());
            const auto size = std::filesystem::file_size(path, error);
            if (error || size == 0U || size > MaximumHistoryBytes)
                return Result<std::string>::Failure(InvalidHistory());
            std::ifstream input(path, std::ios::binary);
            if (!input)
                return Result<std::string>::Failure(InvalidHistory());
            std::string bytes(static_cast<std::size_t>(size), '\0');
            input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (input.gcount() != static_cast<std::streamsize>(bytes.size()) || input.peek() != std::char_traits<char>::eof())
                return Result<std::string>::Failure(InvalidHistory());
            return Result<std::string>::Success(std::move(bytes));
        }
    }  // namespace

    /** @copydoc ReleaseRunHistory::ReleaseRunHistory */
    ReleaseRunHistory::ReleaseRunHistory(DurableFileSystem &files, std::filesystem::path path, const std::size_t capacity,
                                         ExclusiveFileLock lock, std::vector<ReleaseRunHistoryEntry> entries, const std::uint64_t dropped,
                                         const std::uint64_t highestCandidate)
        : files_(files), path_(std::move(path)), capacity_(capacity), lock_(std::move(lock)), entries_(std::move(entries)),
          dropped_(dropped), highestCandidate_(highestCandidate) {}

    /** @copydoc ReleaseRunHistory::Open */
    Result<std::unique_ptr<ReleaseRunHistory>> ReleaseRunHistory::Open(DurableFileSystem &files, const std::filesystem::path &path,
                                                                       const std::size_t capacity) {
        if (!path.is_absolute() || capacity == 0U || capacity > MaximumCapacity)
            return Result<std::unique_ptr<ReleaseRunHistory>>::Failure(InvalidHistory());
        auto lockPath = path;
        lockPath += ".lock";
        auto lock = files.TryAcquireExclusive(lockPath, "release-history");
        if (lock.HasError())
            return Result<std::unique_ptr<ReleaseRunHistory>>::Failure(std::move(lock).ErrorValue());
        auto old = ReadExisting(path);
        if (old.HasError())
            return Result<std::unique_ptr<ReleaseRunHistory>>::Failure(std::move(old).ErrorValue());
        std::vector<ReleaseRunHistoryEntry> entries;
        std::uint64_t dropped = 0U;
        std::uint64_t highestCandidate = 0U;
        if (!old.Value().empty()) {
            try {
                const Json document = Json::parse(old.Value());
                if (!document.is_object() || document.size() != 4U ||
                    (document.at("schemaVersion") != 1 && document.at("schemaVersion") != 2) || !document.at("entries").is_array() ||
                    document.at("entries").size() > capacity)
                    return Result<std::unique_ptr<ReleaseRunHistory>>::Failure(InvalidHistory());
                const auto schemaVersion = document.at("schemaVersion").get<std::uint8_t>();
                dropped = document.at("dropped").get<std::uint64_t>();
                highestCandidate = document.at("highestCandidate").get<std::uint64_t>();
                for (const auto &value : document.at("entries")) {
                    auto entry = ReadEntry(value, schemaVersion);
                    if ((!entries.empty() && entries.back().job.value >= entry.job.value) ||
                        (entry.candidate.has_value() && entry.candidate->value > highestCandidate))
                        return Result<std::unique_ptr<ReleaseRunHistory>>::Failure(InvalidHistory());
                    if (!IsTerminal(entry.state)) {
                        entry.state = ReleaseJobState::Failed;
                        entry.finishedUtcMilliseconds.reset();
                        entry.interruptedByRestart = true;
                    }
                    entries.push_back(std::move(entry));
                }
            } catch (const Json::exception &) {
                return Result<std::unique_ptr<ReleaseRunHistory>>::Failure(InvalidHistory());
            } catch (const std::invalid_argument &) {
                return Result<std::unique_ptr<ReleaseRunHistory>>::Failure(InvalidHistory());
            }
        }
        return Result<std::unique_ptr<ReleaseRunHistory>>::Success(
            std::unique_ptr<ReleaseRunHistory>(  // NOSONAR: Private constructor requires explicit lock ownership.
                new ReleaseRunHistory(files, path, capacity, std::move(lock).Value(), std::move(entries), dropped, highestCandidate)));
    }

    /** @copydoc ReleaseRunHistory::Record */
    Result<void> ReleaseRunHistory::Record(const ReleaseJobSnapshot &snapshot, const std::int64_t updatedUtcMilliseconds) {
        ReleaseRunHistoryEntry entry;
        entry.job = snapshot.id;
        entry.target = snapshot.target;
        entry.operation = snapshot.operation;
        entry.revision = snapshot.revision;
        entry.state = snapshot.state;
        for (std::size_t index = 0; index < ReleaseStageCount; ++index) {
            entry.stages[index] = snapshot.stages[index].state;
            entry.attempts[index] = snapshot.stages[index].attempt;
        }
        if (snapshot.candidate.has_value())
            entry.candidate = snapshot.candidate->id;
        entry.createdUtcMilliseconds = updatedUtcMilliseconds;
        entry.updatedUtcMilliseconds = updatedUtcMilliseconds;
        const bool terminal = IsTerminal(entry.state);
        if (terminal)
            entry.finishedUtcMilliseconds = updatedUtcMilliseconds;
        if (!ValidEntry(entry))
            return Result<void>::Failure(InvalidHistory());
        std::lock_guard lock(mutex_);
        auto updated = entries_;
        if (auto found = std::ranges::lower_bound(updated, entry.job.value, {},
                                                  [](const ReleaseRunHistoryEntry &value) {
            return value.job.value;
        });
            found != updated.end() && found->job == entry.job) {
            if (entry.revision < found->revision || entry.target != found->target || entry.operation != found->operation ||
                found->interruptedByRestart || (IsTerminal(found->state) && !terminal))
                return Result<void>::Failure(InvalidHistory());
            entry.createdUtcMilliseconds = found->createdUtcMilliseconds;
            entry.updatedUtcMilliseconds = std::max(entry.updatedUtcMilliseconds, entry.createdUtcMilliseconds);
            if (terminal)
                entry.finishedUtcMilliseconds = entry.updatedUtcMilliseconds;
            *found = entry;
        } else
            updated.insert(found, entry);
        std::uint64_t nextDropped = dropped_;
        if (updated.size() > capacity_) {
            const auto evicted = std::ranges::find_if(updated, [](const ReleaseRunHistoryEntry &value) {
                return IsTerminal(value.state);
            });
            if (evicted == updated.end())
                return Result<void>::Failure(InvalidHistory());
            updated.erase(evicted);
            if (nextDropped == std::numeric_limits<std::uint64_t>::max())
                return Result<void>::Failure(InvalidHistory());
            ++nextDropped;
        }
        const std::uint64_t nextHighestCandidate =
            entry.candidate.has_value() ? std::max(highestCandidate_, entry.candidate->value) : highestCandidate_;
        Json document = {{"schemaVersion", 2},
                         {"dropped", nextDropped},
                         {"highestCandidate", nextHighestCandidate},
                         {"entries", Json::array()}};
        for (const auto &value : updated)
            document["entries"].push_back(WriteEntry(value));
        const std::string bytes = document.dump();
        if (bytes.size() > MaximumHistoryBytes)
            return Result<void>::Failure(InvalidHistory());
        auto prepared = path_;
        prepared += ".pending";
        if (auto written = files_.WriteDurable(prepared, std::as_bytes(std::span{bytes})); written.HasError())
            return written;
        if (auto replaced = files_.AtomicReplace(prepared, path_); replaced.HasError())
            return replaced;
        entries_ = std::move(updated);
        dropped_ = nextDropped;
        highestCandidate_ = nextHighestCandidate;
        return Result<void>::Success();
    }

    /** @copydoc ReleaseRunHistory::List */
    std::vector<ReleaseRunHistoryEntry> ReleaseRunHistory::List() const {
        std::lock_guard lock(mutex_);
        return entries_;
    }

    /** @copydoc ReleaseRunHistory::Dropped */
    std::uint64_t ReleaseRunHistory::Dropped() const {
        std::lock_guard lock(mutex_);
        return dropped_;
    }

    /** @copydoc ReleaseRunHistory::HighestCandidate */
    std::uint64_t ReleaseRunHistory::HighestCandidate() const {
        std::lock_guard lock(mutex_);
        return highestCandidate_;
    }
}  // namespace Horo::Release
