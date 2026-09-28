#include "editor/modals/build_release/BuildReleaseModalSession.h"

#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Release;

    class FakeReleaseJobs final : public IBuildReleaseJobs {
    public:
        std::vector<ReleaseJobSnapshot> snapshots;
        std::vector<ReleaseDiagnostic> diagnostics;
        int submissions{};
        int cancellations{};
        ReleaseJobId cancelled{};

        [[nodiscard]] std::vector<ReleaseJobSnapshot> List() const override {
            return snapshots;
        }

        [[nodiscard]] std::optional<ReleaseDiagnostic> Diagnostic(const ReleaseJobId, const ReleaseDiagnosticId id) const override {
            for (const auto &diagnostic : diagnostics) {
                if (diagnostic.id == id)
                    return diagnostic;
            }
            return std::nullopt;
        }

        [[nodiscard]] Result<ReleaseJobId> Submit(ReleaseExecutionPlan) override {
            ++submissions;
            return Result<ReleaseJobId>::Success(ReleaseJobId{3});
        }

        [[nodiscard]] Result<void> RequestCancel(const ReleaseJobId job) override {
            ++cancellations;
            cancelled = job;
            return Result<void>::Success();
        }
    };

    [[nodiscard]] ReleaseJobSnapshot Job(const std::uint64_t id, const ReleaseJobState state) {
        ReleaseJobSnapshot snapshot;
        snapshot.id = ReleaseJobId{id};
        snapshot.state = state;
        return snapshot;
    }

    TEST_CASE("Reopening the release modal reconnects to an active job without submission or cancellation", "[unit][editor][release]") {
        FakeReleaseJobs jobs;
        jobs.snapshots = {Job(1, ReleaseJobState::Succeeded), Job(2, ReleaseJobState::Running)};

        {
            BuildReleaseModalSession modal(jobs);
            modal.Open();
            REQUIRE(modal.Selected() != nullptr);
            REQUIRE(modal.Selected()->id.value == 2);
        }

        BuildReleaseModalSession reopened(jobs);
        reopened.Open();
        REQUIRE(reopened.Selected() != nullptr);
        REQUIRE(reopened.Selected()->id.value == 2);
        REQUIRE(jobs.submissions == 0);
        REQUIRE(jobs.cancellations == 0);

        REQUIRE(reopened.RequestCancel().HasValue());
        REQUIRE(jobs.cancellations == 1);
        REQUIRE(jobs.cancelled.value == 2);
    }

    TEST_CASE("Release modal distinguishes finalized candidates from final-verified candidates", "[unit][editor][release]") {
        FakeReleaseJobs jobs;
        auto snapshot = Job(4, ReleaseJobState::Running);
        snapshot.candidate = ReleaseCandidateSnapshot{ReleaseCandidateId{8}, ReleaseCandidateState::Finalized};
        jobs.snapshots.push_back(snapshot);

        BuildReleaseModalSession modal(jobs);
        modal.Open();
        REQUIRE_FALSE(modal.SelectedCandidateIsFinalVerified());

        jobs.snapshots[0].candidate->state = ReleaseCandidateState::FinalVerified;
        modal.Refresh();
        REQUIRE_FALSE(modal.SelectedCandidateIsFinalVerified());

        jobs.snapshots[0].stages[static_cast<std::size_t>(ReleaseStage::FinalVerifying)].state = ReleaseStageState::Succeeded;
        modal.Refresh();
        REQUIRE(modal.SelectedCandidateIsFinalVerified());
    }

    TEST_CASE("Release modal preserves selection and reads bounded diagnostics from the service", "[unit][editor][release]") {
        FakeReleaseJobs jobs;
        jobs.snapshots = {Job(5, ReleaseJobState::Failed), Job(6, ReleaseJobState::Succeeded)};
        jobs.snapshots[0].recentDiagnostics = {ReleaseDiagnosticId{11}};
        ReleaseDiagnostic diagnostic;
        diagnostic.id = ReleaseDiagnosticId{11};
        diagnostic.job = ReleaseJobId{5};
        diagnostic.message = "package verification failed";
        jobs.diagnostics.push_back(diagnostic);

        BuildReleaseModalSession modal(jobs);
        modal.Open();
        REQUIRE(modal.Select(ReleaseJobId{5}));
        modal.Refresh();
        REQUIRE(modal.Selected()->id.value == 5);
        REQUIRE(modal.SelectedDiagnostics().size() == 1);
        REQUIRE(modal.SelectedDiagnostics()[0].message == "package verification failed");
        REQUIRE_FALSE(modal.RequestCancel().HasValue());
        REQUIRE(jobs.cancellations == 0);
    }
}  // namespace
