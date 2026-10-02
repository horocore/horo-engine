#include "UiBindingWriteTestFixture.h"

#include <limits>

namespace Horo::Runtime::Ui::BindingWriteTests {
    TEST_CASE("Edit and command fences reject recycled UI and changed provider evidence", "[runtime_ui][binding][write][stale]") {
        WriteSession fixture;
        const auto authority = fixture.Authority(0);
        fixture.Admit(fixture.store, 10, authority);
        auto stale = fixture.Source(2);
        ++stale.element.generation;
        ErrorIs(fixture.store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), stale), UiErrors::RevisionStale);
        const auto edit = Take(fixture.store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)));
        const auto request = fixture.Request(fixture.router, 2, true);
        SECTION("read publication before queue") {
            const std::array updates{UiBindingPropertyUpdate{0, true}};
            const std::array batches{UiBindingChangeBatch{fixture.provider, fixture.schema.Version(), Rev<UiBindingSnapshotRevision>(),
                                                          Rev<UiBindingSnapshotRevision>(2), updates}};
            REQUIRE(fixture.store.Apply(fixture.tree, batches, fixture.layout).HasValue());
            ErrorIs(fixture.store.QueueWrite(fixture.tree, edit, request, UiBindingCommitTrigger::Change), UiErrors::RevisionStale);
        }
        SECTION("provider advances after queue before prepare") {
            REQUIRE(fixture.store.QueueWrite(fixture.tree, edit, request, UiBindingCommitTrigger::Change).HasValue());
            fixture.state->revision = Rev<UiBindingSnapshotRevision>(2);
            const auto result = Process(fixture.store, fixture.tree, fixture.layout);
            CHECK(result.disposition == UiBindingWriteDisposition::Rejected);
            REQUIRE(result.error);
            CHECK(result.error->code.Value() == UiErrors::RevisionStale.code.Value());
            CHECK_FALSE(std::get<bool>(fixture.store.Find(fixture.tree, Stable<UiBindingId>(10))->value));
        }
        SECTION("document replacement") {
            REQUIRE(fixture.store.QueueWrite(fixture.tree, edit, request, UiBindingCommitTrigger::Change).HasValue());
            auto replacement = fixture.MakeTree(2);
            const auto result = Process(fixture.store, replacement, fixture.layout);
            CHECK(result.disposition == UiBindingWriteDisposition::Rejected);
        }
        SECTION("provider permission replacement") {
            REQUIRE(fixture.store.QueueWrite(fixture.tree, edit, request, UiBindingCommitTrigger::Change).HasValue());
            ++authority->fence.capability;
            CHECK(Process(fixture.store, fixture.tree, fixture.layout).disposition == UiBindingWriteDisposition::Rejected);
            CHECK(fixture.state->prepares == 0);
        }
        CHECK(fixture.state->commits == 0);
    }

    TEST_CASE("Provider callback reentry cannot retire or free the active write transaction", "[runtime_ui][binding][write][reentrancy]") {
        WriteSession fixture;
        auto authority = fixture.Authority(0);
        authority->reentrantStore = &fixture.store;
        SECTION("Prepare attempts retirement") {
            authority->prepareReentry = TypedAuthority::Reentry::Retirement;
        }
        SECTION("Prepare attempts shutdown") {
            authority->prepareReentry = TypedAuthority::Reentry::Shutdown;
        }
        fixture.Admit(fixture.store, 10, authority);
        fixture.QueueCurrentChange();
        const auto before = fixture.store.Current();
        const auto outcome = Process(fixture.store, fixture.tree, fixture.layout);
        CHECK(outcome.disposition == UiBindingWriteDisposition::Rejected);
        REQUIRE(outcome.error);
        CHECK(outcome.error->code.Value() == UiErrors::ActionHandlerFailed.code.Value());
        CHECK(fixture.state->commits == 0);
        CHECK(fixture.state->abandons == 1);
        CHECK(fixture.store.Current().revision == before.revision);
        const auto *target = fixture.store.Find(fixture.tree, Stable<UiBindingId>(10));
        REQUIRE(target);
        CHECK_FALSE(std::get<bool>(target->value));
        CHECK_FALSE(Take(fixture.store.ProcessWrite(fixture.tree, fixture.layout)).has_value());
        authority->reentrantStore = nullptr;
    }

    TEST_CASE("Abandon callback retirement cannot recursively cancel one operation", "[runtime_ui][binding][write][reentrancy]") {
        WriteSession fixture;
        auto authority = fixture.Authority(0);
        authority->disposition = UiBindingWriteDisposition::Pending;
        authority->reentrantStore = &fixture.store;
        authority->abandonRetires = true;
        fixture.Admit(fixture.store, 10, authority);
        const auto queued = fixture.QueueCurrentChange();
        CHECK(Process(fixture.store, fixture.tree, fixture.layout).disposition == UiBindingWriteDisposition::Pending);
        fixture.store.BeginRetirement();
        CHECK(fixture.state->abandons == 1);
        CHECK(fixture.state->commits == 0);
        std::array<UiBindingWriteResult, 1> outcomes{};
        CHECK(Take(fixture.store.DrainWriteResults(outcomes)) == 1);
        CHECK(outcomes[0].request == queued.request);
        CHECK(outcomes[0].operation == queued.operation);
        CHECK(outcomes[0].disposition == UiBindingWriteDisposition::Cancelled);
        CHECK(outcomes[0].cancellation == UiBindingWriteCancellationReason::OwnerRetired);
        CHECK(Take(fixture.store.DrainWriteResults(outcomes)) == 0);
        CHECK_FALSE(Take(fixture.store.ProcessWrite(fixture.tree, fixture.layout)).has_value());
        authority->reentrantStore = nullptr;
    }

    TEST_CASE("A pending binding does not starve another admitted binding", "[runtime_ui][binding][write][fairness]") {
        WriteSession fixture;
        auto pending = fixture.Authority(0);
        pending->disposition = UiBindingWriteDisposition::Pending;
        auto ready = fixture.Authority(2);
        const std::array admissions{fixture.Admission(10, pending), fixture.Admission(12, ready)};
        REQUIRE(fixture.store.AdmitWrites(fixture.tree, admissions).HasValue());
        const auto first = Take(fixture.store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)));
        const auto second = Take(fixture.store.BeginEdit(fixture.tree, Stable<UiBindingId>(12), fixture.Source(3)));
        const auto firstResult =
            Take(fixture.store.QueueWrite(fixture.tree, first, fixture.Request(fixture.router, 2, true), UiBindingCommitTrigger::Change));
        const auto secondResult =
            Take(fixture.store.QueueWrite(fixture.tree, second, fixture.Request(fixture.router, 3, 0.75), UiBindingCommitTrigger::Change));
        const auto pendingOutcome = Process(fixture.store, fixture.tree, fixture.layout);
        CHECK(pendingOutcome.request == firstResult.request);
        CHECK(pendingOutcome.disposition == UiBindingWriteDisposition::Pending);
        const auto readyOutcome = Process(fixture.store, fixture.tree, fixture.layout);
        CHECK(readyOutcome.request == secondResult.request);
        CHECK(readyOutcome.disposition == UiBindingWriteDisposition::Ready);
        CHECK(fixture.state->commits == 1);
        CHECK(std::get<double>(fixture.state->values[2]) == 0.75);
        const auto conflict = Process(fixture.store, fixture.tree, fixture.layout);
        CHECK(conflict.request == firstResult.request);
        CHECK(conflict.disposition == UiBindingWriteDisposition::Rejected);
        REQUIRE(conflict.error);
        CHECK(conflict.error->code.Value() == UiErrors::RevisionStale.code.Value());
        CHECK(fixture.state->prepares == 2);
        CHECK(fixture.state->abandons == 1);
    }

    TEST_CASE("Concurrent same-revision edits have deterministic first-commit conflict rejection",
              "[runtime_ui][binding][write][conflict]") {
        WriteSession fixture;
        const auto authority = fixture.Authority(0);
        const std::array admissions{fixture.Admission(10, authority), fixture.Admission(13, authority)};
        REQUIRE(fixture.store.AdmitWrites(fixture.tree, admissions).HasValue());
        const auto first = Take(fixture.store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)));
        const auto second = Take(fixture.store.BeginEdit(fixture.tree, Stable<UiBindingId>(13), fixture.Source(6)));
        REQUIRE(fixture.store.QueueWrite(fixture.tree, first, fixture.Request(fixture.router, 2, true), UiBindingCommitTrigger::Change)
                    .HasValue());
        REQUIRE(fixture.store.QueueWrite(fixture.tree, second, fixture.Request(fixture.router, 6, false), UiBindingCommitTrigger::Change)
                    .HasValue());
        CHECK(Process(fixture.store, fixture.tree, fixture.layout).disposition == UiBindingWriteDisposition::Ready);
        const auto rejected = Process(fixture.store, fixture.tree, fixture.layout);
        CHECK(rejected.disposition == UiBindingWriteDisposition::Rejected);
        REQUIRE(rejected.error);
        CHECK(rejected.error->code.Value() == UiErrors::RevisionStale.code.Value());
        CHECK(fixture.state->commits == 1);
        CHECK(fixture.state->prepares == 1);
        CHECK(std::get<bool>(fixture.state->values[0]));
        std::array<UiBindingWriteResult, 1> tooSmall{};
        ErrorIs(fixture.store.DrainWriteResults(tooSmall), UiErrors::BindingCapacityExceeded);
        std::array<UiBindingWriteResult, 2> outcomes{};
        CHECK(Take(fixture.store.DrainWriteResults(outcomes)) == 2);
        CHECK_FALSE(Take(fixture.store.ProcessWrite(fixture.tree, fixture.layout)).has_value());
    }

    TEST_CASE("Action request high-water marks transfer across router moves and exhaust without wrapping",
              "[runtime_ui][binding][write][sequence]") {
        Fixture fixture;
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        auto router = Take(UiActionRouter::Create({fixture.Context(), 1, Rev<UiActionSequence>(maximum - 1)}));
        const UiGameplayActionCommand command{fixture.Action(), {}};
        const auto finalRequest = Take(router.Enqueue(fixture.Source(2), command));
        CHECK(finalRequest.sequence.Value() == maximum);
        CHECK(router.LastIssuedSequence().Value() == maximum);
        REQUIRE(router.TryDequeue().HasValue());
        ErrorIs(router.Enqueue(fixture.Source(2), command), UiErrors::GenerationExhausted);
        auto transferred = std::move(router);
        CHECK(transferred.LastIssuedSequence().Value() == maximum);
        transferred.Shutdown();
        CHECK(transferred.LastIssuedSequence().Value() == maximum);
        ErrorIs(UiActionRouter::Create({fixture.Context(), 1, transferred.LastIssuedSequence()}), UiErrors::ActionInvalid);

        auto initial = fixture.Router();
        CHECK_FALSE(initial.LastIssuedSequence().IsValid());
        const auto first = Take(initial.Enqueue(fixture.Source(2), command));
        REQUIRE(initial.TryDequeue().HasValue());
        auto owner = fixture.Context();
        owner.interaction = Rev<UiInteractionRevision>(2);
        auto replacement = Take(UiActionRouter::Create({owner, 1, initial.LastIssuedSequence()}));
        auto source = fixture.Source(2);
        source.owner = owner;
        const auto next = Take(replacement.Enqueue(source, command));
        CHECK(next.sequence.Value() == first.sequence.Value() + 1);
    }

    TEST_CASE("A completed action request cannot replay through a fresh binding edit", "[runtime_ui][binding][write][sequence]") {
        WriteSession fixture;
        fixture.Admit(fixture.store, 10, fixture.Authority(0));
        const auto first = Take(fixture.store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)));
        const auto request = fixture.Request(fixture.router, 2, true);
        REQUIRE(fixture.store.QueueWrite(fixture.tree, first, request, UiBindingCommitTrigger::Change).HasValue());
        CHECK(Process(fixture.store, fixture.tree, fixture.layout).disposition == UiBindingWriteDisposition::Ready);
        std::array<UiBindingWriteResult, 1> terminal{};
        CHECK(Take(fixture.store.DrainWriteResults(terminal)) == 1);
        const auto second = Take(fixture.store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)));
        ErrorIs(fixture.store.QueueWrite(fixture.tree, second, request, UiBindingCommitTrigger::Change), UiErrors::RevisionStale);
        CHECK(fixture.state->commits == 1);
        REQUIRE(fixture.store.QueueWrite(fixture.tree, second, fixture.Request(fixture.router, 2, false), UiBindingCommitTrigger::Change)
                    .HasValue());
        CHECK(Process(fixture.store, fixture.tree, fixture.layout).disposition == UiBindingWriteDisposition::Ready);
        CHECK(fixture.state->commits == 2);
        CHECK_FALSE(std::get<bool>(fixture.state->values[0]));
    }
}  // namespace Horo::Runtime::Ui::BindingWriteTests
