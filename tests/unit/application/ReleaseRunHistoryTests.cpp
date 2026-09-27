#include "Horo/Release/ReleaseRunHistory.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
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
    CHECK(history->Record(Snapshot(2U), 1200).HasValue());
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
    REQUIRE(history->Record(completed, 1000).HasValue());
    REQUIRE(history->Record(Snapshot(2U), 2000).HasValue());
    CHECK(history->HighestCandidate() == 42U);
    history.reset();
    auto reopened = ReleaseRunHistory::Open(files, path, 1U);
    REQUIRE(reopened.HasValue());
    CHECK(reopened.Value()->HighestCandidate() == 42U);
    CHECK(reopened.Value()->List().front().job.value == 2U);
}
