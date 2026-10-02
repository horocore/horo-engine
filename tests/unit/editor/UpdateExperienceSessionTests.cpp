#include "editor/update/UpdateExperienceSession.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

using namespace Horo;
using namespace Horo::Editor;

namespace {
    class FakeBackend final : public IEditorUpdateBackend {
    public:
        std::atomic<bool> enteredPrepare{false};
        std::atomic<bool> releasePrepare{true};
        std::atomic<bool> cancelled{false};
        std::atomic<unsigned> activationRequests{0U};

        Result<std::optional<EditorUpdateOffer>> Check(const EditorUpdateChannel &, CancellationToken) override {
            return Result<std::optional<EditorUpdateOffer>>::Success(
                EditorUpdateOffer{"0.4.2", "Long localized release notes", {"Project format changes"}, true});
        }

        Result<void> Prepare(const EditorUpdateOffer &, CancellationToken cancellation,
                             const std::function<void(EditorUpdatePhase, std::uint64_t, std::uint64_t)> &progress) override {
            enteredPrepare = true;
            progress(EditorUpdatePhase::Downloading, 5U, 10U);
            while (!releasePrepare && !cancellation.IsCancellationRequested())
                std::this_thread::yield();
            if (cancellation.IsCancellationRequested()) {
                cancelled = true;
                return JobCancelled();
            }
            progress(EditorUpdatePhase::Verifying, 10U, 10U);
            return Result<void>::Success();
        }

        Result<void> Activate(CancellationToken) override {
            ++activationRequests;
            return Result<void>::Success();
        }

        Result<void> Rollback(CancellationToken) override {
            return Result<void>::Success();
        }
    };

    void PollUntil(UpdateExperienceSession &session, const EditorUpdatePhase phase) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (std::chrono::steady_clock::now() < deadline) {
            session.Poll();
            if (session.Snapshot().phase == phase)
                return;
            std::this_thread::yield();
        }
        FAIL("Update session did not reach the expected phase");
    }

    void ConfirmHostOutcome(UpdateExperienceSession &session, const EditorUpdatePhase outcome) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (std::chrono::steady_clock::now() < deadline) {
            session.Poll();
            if (session.ReportVerifiedHostOutcome(outcome))
                return;
            std::this_thread::yield();
        }
        FAIL("Update handoff did not complete before host confirmation");
    }
}  // namespace

TEST_CASE("Update session keeps a worker alive across presentation closure and requires confirmation to activate", "[editor][update]") {
    JobSystem jobs({.workerCount = 1U});
    FakeBackend backend;
    UpdateExperienceSession session{jobs, backend};
    REQUIRE(session.CheckNow());
    PollUntil(session, EditorUpdatePhase::Available);
    CHECK(session.Snapshot().offer->version == "0.4.2");
    CHECK_FALSE(session.RestartNow(false));

    backend.releasePrepare = false;
    REQUIRE(session.Download());
    while (!backend.enteredPrepare)
        std::this_thread::yield();
    // The settings view may close here; it owns neither the job nor its cancellation source.
    backend.releasePrepare = true;
    PollUntil(session, EditorUpdatePhase::RestartRequired);
    CHECK_FALSE(session.RestartNow(false));
    REQUIRE(session.RestartNow(true));
    PollUntil(session, EditorUpdatePhase::Activating);
    CHECK_FALSE(session.ReportVerifiedHostOutcome(EditorUpdatePhase::RolledBack));
    ConfirmHostOutcome(session, EditorUpdatePhase::Active);
    CHECK_FALSE(session.Rollback(false));
    REQUIRE(session.Rollback(true));
    PollUntil(session, EditorUpdatePhase::RollbackPending);
    ConfirmHostOutcome(session, EditorUpdatePhase::RolledBack);
}

TEST_CASE("Update session shutdown cancels and joins its worker", "[editor][update]") {
    JobSystem jobs({.workerCount = 1U});
    FakeBackend backend;
    UpdateExperienceSession session{jobs, backend};
    REQUIRE(session.CheckNow());
    PollUntil(session, EditorUpdatePhase::Available);
    backend.releasePrepare = false;
    REQUIRE(session.Download());
    while (!backend.enteredPrepare)
        std::this_thread::yield();
    session.Shutdown();
    CHECK(backend.cancelled);
    CHECK_FALSE(session.CheckNow());
}

TEST_CASE("Less stable update channels require explicit confirmation", "[editor][update]") {
    JobSystem jobs({.workerCount = 1U});
    FakeBackend backend;
    UpdateExperienceSession session{jobs, backend};
    CHECK_FALSE(session.SetChannel({EditorUpdateChannelKind::Preview, {}}));
    CHECK(session.Snapshot().channel.kind == EditorUpdateChannelKind::Stable);
    REQUIRE(session.SetChannel({EditorUpdateChannelKind::Preview, {}}, true));
    CHECK(session.Snapshot().channel.kind == EditorUpdateChannelKind::Preview);
}

TEST_CASE("Install-on-exit requests only a staged helper handoff", "[editor][update]") {
    JobSystem jobs({.workerCount = 1U});
    FakeBackend backend;
    UpdateExperienceSession session{jobs, backend};
    session.SetInstallOnExit(true);
    REQUIRE(session.ActivateOnExit().HasValue());
    CHECK(backend.activationRequests == 0U);
    REQUIRE(session.CheckNow());
    PollUntil(session, EditorUpdatePhase::Available);
    REQUIRE(session.Download());
    PollUntil(session, EditorUpdatePhase::RestartRequired);
    const auto activated = session.ActivateOnExit();
    REQUIRE(activated.HasValue());
    CHECK(activated.Value());
    CHECK(backend.activationRequests == 1U);
    CHECK(session.Snapshot().phase == EditorUpdatePhase::Activating);
    REQUIRE(session.ActivateOnExit().HasValue());
    CHECK(backend.activationRequests == 1U);
}

TEST_CASE("Verified host outcome is the only way to restore active or rolled-back state on relaunch", "[editor][update]") {
    JobSystem jobs({.workerCount = 1U});
    FakeBackend backend;
    UpdateExperienceSession active{jobs, backend};
    CHECK_FALSE(active.ReportVerifiedHostOutcome(EditorUpdatePhase::Available));
    REQUIRE(active.ReportVerifiedHostOutcome(EditorUpdatePhase::Active));
    CHECK(active.Snapshot().phase == EditorUpdatePhase::Active);
    CHECK_FALSE(active.ReportVerifiedHostOutcome(EditorUpdatePhase::RolledBack));

    UpdateExperienceSession rolledBack{jobs, backend};
    REQUIRE(rolledBack.ReportVerifiedHostOutcome(EditorUpdatePhase::RolledBack));
    CHECK(rolledBack.Snapshot().phase == EditorUpdatePhase::RolledBack);
}
