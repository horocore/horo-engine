#include "Horo/Release/ReleaseRunHistory.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>

using namespace Horo;
using namespace Horo::Release;

namespace {
    class TemporaryHistoryDirectory final {
    public:
        TemporaryHistoryDirectory()
            : path(std::filesystem::temp_directory_path() /
                   ("horo-release-history-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            std::filesystem::create_directories(path);
        }

        ~TemporaryHistoryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }

        std::filesystem::path path;
    };

    ReleaseJobSnapshot Snapshot(const std::uint64_t job) {
        ReleaseJobSnapshot snapshot;
        snapshot.id = {job};
        snapshot.target = {job + 100U};
        snapshot.operation = job + 200U;
        return snapshot;
    }
}  // namespace

TEST_CASE("Release history retains bounded typed state across store restart", "[release][history]") {
    TemporaryHistoryDirectory directory;
    NativeDurableFileSystem files;
    const auto path = directory.path / "history.json";
    auto opened = ReleaseRunHistory::Open(files, path, 2U);
    REQUIRE(opened.HasValue());
    auto history = std::move(opened).Value();
    auto first = Snapshot(1U);
    REQUIRE(history->Record(first, 1000).HasValue());
    first.state = ReleaseJobState::Running;
    first.revision = 1U;
    first.stages[0].state = ReleaseStageState::Succeeded;
    REQUIRE(history->Record(first, 1100).HasValue());
    first.state = ReleaseJobState::Failed;
    first.revision = 2U;
    REQUIRE(history->Record(first, 1150).HasValue());
    auto second = Snapshot(2U);
    second.state = ReleaseJobState::Failed;
    CHECK(history->Record(second, 1200).HasValue());
    CHECK(history->Record(Snapshot(3U), 1300).HasValue());
    CHECK(history->Dropped() == 1U);
    history.reset();

    auto reopened = ReleaseRunHistory::Open(files, path, 2U);
    REQUIRE(reopened.HasValue());
    const auto records = reopened.Value()->List();
    REQUIRE(records.size() == 2U);
    CHECK(records[0].job.value == 2U);
    CHECK(records[1].job.value == 3U);
    CHECK(reopened.Value()->Dropped() == 1U);
    std::ifstream input(path, std::ios::binary);
    const std::string stored{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    CHECK(stored.find("message") == std::string::npos);
    CHECK(stored.find("credential") == std::string::npos);
}

TEST_CASE("Release history preserves active jobs and rejects admission when only active jobs fit", "[release][history]") {
    TemporaryHistoryDirectory directory;
    NativeDurableFileSystem files;
    const auto path = directory.path / "history.json";
    auto opened = ReleaseRunHistory::Open(files, path, 2U);
    REQUIRE(opened.HasValue());
    auto history = std::move(opened).Value();
    auto first = Snapshot(1U);
    REQUIRE(history->Record(first, 1000).HasValue());
    auto second = Snapshot(2U);
    second.state = ReleaseJobState::Failed;
    REQUIRE(history->Record(second, 1100).HasValue());
    REQUIRE(history->Record(Snapshot(3U), 1200).HasValue());
    const auto active = history->List();
    REQUIRE(active.size() == 2U);
    CHECK(active[0].job.value == 1U);
    CHECK(active[1].job.value == 3U);
    CHECK(history->Dropped() == 1U);

    CHECK(history->Record(Snapshot(4U), 1300).HasError());
    CHECK(history->List().size() == 2U);
    CHECK(history->Dropped() == 1U);
    first.state = ReleaseJobState::Failed;
    first.revision = 1U;
    REQUIRE(history->Record(first, 1400).HasValue());
    REQUIRE(history->Record(Snapshot(4U), 1500).HasValue());
    const auto recent = history->List();
    REQUIRE(recent.size() == 2U);
    CHECK(recent[0].job.value == 3U);
    CHECK(recent[1].job.value == 4U);
    CHECK(history->Dropped() == 2U);
}

TEST_CASE("Release history identifies interrupted jobs after restart and releases their retention slot", "[release][history]") {
    TemporaryHistoryDirectory directory;
    NativeDurableFileSystem files;
    const auto path = directory.path / "history.json";
    auto opened = ReleaseRunHistory::Open(files, path, 2U);
    REQUIRE(opened.HasValue());
    auto history = std::move(opened).Value();
    auto running = Snapshot(1U);
    running.state = ReleaseJobState::Running;
    running.revision = 1U;
    running.stages[0].state = ReleaseStageState::Running;
    running.stages[0].attempt = ReleaseStageAttemptId{1U};
    REQUIRE(history->Record(running, 1000).HasValue());
    history.reset();

    auto restarted = ReleaseRunHistory::Open(files, path, 2U);
    REQUIRE(restarted.HasValue());
    history = std::move(restarted).Value();
    const auto interrupted = history->List();
    REQUIRE(interrupted.size() == 1U);
    CHECK(interrupted.front().state == ReleaseJobState::Failed);
    CHECK(interrupted.front().interruptedByRestart);
    CHECK_FALSE(interrupted.front().finishedUtcMilliseconds.has_value());
    CHECK(interrupted.front().stages[0] == ReleaseStageState::Running);
    CHECK(history->Record(running, 1100).HasError());
    REQUIRE(history->Record(Snapshot(2U), 1200).HasValue());
    history.reset();

    auto reopened = ReleaseRunHistory::Open(files, path, 2U);
    REQUIRE(reopened.HasValue());
    const auto records = reopened.Value()->List();
    REQUIRE(records.size() == 2U);
    CHECK(records.front().interruptedByRestart);
    CHECK(records.front().state == ReleaseJobState::Failed);
}

TEST_CASE("Release history migrates schema one snapshots when a later job is recorded", "[release][history]") {
    TemporaryHistoryDirectory directory;
    NativeDurableFileSystem files;
    const auto path = directory.path / "history.json";
    auto opened = ReleaseRunHistory::Open(files, path, 2U);
    REQUIRE(opened.HasValue());
    auto history = std::move(opened).Value();
    auto first = Snapshot(1U);
    first.state = ReleaseJobState::Failed;
    REQUIRE(history->Record(first, 1000).HasValue());
    history.reset();

    nlohmann::ordered_json old;
    {
        std::ifstream input(path, std::ios::binary);
        input >> old;
    }
    old["schemaVersion"] = 1;
    old["entries"][0].erase("interruptedByRestart");
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << old.dump();
    }
    auto restarted = ReleaseRunHistory::Open(files, path, 2U);
    REQUIRE(restarted.HasValue());
    history = std::move(restarted).Value();
    REQUIRE(history->Record(Snapshot(2U), 1100).HasValue());
    history.reset();

    std::ifstream input(path, std::ios::binary);
    const auto migrated = nlohmann::ordered_json::parse(input);
    CHECK(migrated.at("schemaVersion") == 2);
    CHECK_FALSE(migrated.at("entries").at(0).at("interruptedByRestart").get<bool>());
}

TEST_CASE("Release history rejects stale updates and corrupt or unsafe storage", "[release][history]") {
    TemporaryHistoryDirectory directory;
    NativeDurableFileSystem files;
    const auto path = directory.path / "history.json";
    CHECK(ReleaseRunHistory::Open(files, "relative/history.json").HasError());
    auto opened = ReleaseRunHistory::Open(files, path, 2U);
    REQUIRE(opened.HasValue());
    auto history = std::move(opened).Value();
    auto first = Snapshot(1U);
    first.revision = 2U;
    REQUIRE(history->Record(first, 1000).HasValue());
    first.revision = 1U;
    CHECK(history->Record(first, 1100).HasError());
    CHECK(history->Record(Snapshot(0U), 1100).HasError());
    history.reset();

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "{\"schemaVersion\":1,\"entries\":[]}";
    output.close();
    CHECK(ReleaseRunHistory::Open(files, path, 2U).HasError());
}

TEST_CASE("Release history retains candidate identity after its job ages out", "[release][history]") {
    TemporaryHistoryDirectory directory;
    NativeDurableFileSystem files;
    const auto path = directory.path / "history.json";
    auto opened = ReleaseRunHistory::Open(files, path, 1U);
    REQUIRE(opened.HasValue());
    auto history = std::move(opened).Value();
    auto completed = Snapshot(1U);
    completed.candidate = ReleaseCandidateSnapshot{ReleaseCandidateId{42U}, ReleaseCandidateState::FinalVerified};
    completed.state = ReleaseJobState::Succeeded;
    REQUIRE(history->Record(completed, 1000).HasValue());
    REQUIRE(history->Record(Snapshot(2U), 2000).HasValue());
    CHECK(history->HighestCandidate() == 42U);
    history.reset();
    auto reopened = ReleaseRunHistory::Open(files, path, 1U);
    REQUIRE(reopened.HasValue());
    CHECK(reopened.Value()->HighestCandidate() == 42U);
    CHECK(reopened.Value()->List().front().job.value == 2U);
}
