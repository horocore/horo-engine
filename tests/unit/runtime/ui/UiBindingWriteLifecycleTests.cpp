#include "UiBindingWriteTestFixture.h"

#include <catch2/generators/catch_generators.hpp>

namespace Horo::Runtime::Ui::BindingWriteTests {
    TEST_CASE("Reload draft compatibility uses actual provider permission and committed target evidence", "[runtime_ui][binding][reload]") {
        Fixture fixture;
        auto source = fixture.Store();
        auto authority = fixture.Authority(0);
        fixture.Admit(source, 10, authority);
        auto oldTree = std::move(fixture.tree);
        std::array<UiElementDescriptor, 7> elements{};
        for (std::size_t i = 0; i < elements.size(); ++i)
            elements[i] = {Stable<UiElementId>(static_cast<std::uint8_t>(i + 1)), i == 0 ? UiElementId{} : Stable<UiElementId>(1)};
        fixture.tree = Take(UiElementTree::Create(fixture.slots,
                                                  {oldTree.Instance(),
                                                   oldTree.Canvas(),
                                                   oldTree.SourceDocument(),
                                                   Rev<UiDocumentRevision>(2),
                                                   Rev<UiRuntimeTreeRevision>(2),
                                                   {8, 8, 8}},
                                                  elements));
        auto replacement = fixture.Store();
        fixture.Admit(replacement, 10, authority);
        CHECK(replacement.ReloadCompatible(source, oldTree, fixture.tree, Stable<UiElementId>(2)));
        auto layout = fixture.Layout();
        const std::array changes{UiBindingPropertyUpdate{0, true}};
        const std::array batches{UiBindingChangeBatch{fixture.provider, fixture.schema.Version(), Rev<UiBindingSnapshotRevision>(1),
                                                      Rev<UiBindingSnapshotRevision>(2), changes}};
        REQUIRE(replacement.Apply(fixture.tree, batches, layout).HasValue());
        CHECK_FALSE(replacement.ReloadCompatible(source, oldTree, fixture.tree, Stable<UiElementId>(2)));
    }

    TEST_CASE("Reload revokes binding admission before deferred authority abandonment", "[runtime_ui][binding][write][reload]") {
        WriteSession fixture;
        auto authority = fixture.Authority(0);
        fixture.Admit(fixture.store, 10, authority);
        fixture.QueueCurrentChange();
        const auto before = WriteAllocations().load();
        const auto closed = fixture.store.CloseReloadAdmission();
        const auto after = WriteAllocations().load();
        REQUIRE(closed.HasValue());
        CHECK(before == after);
        CHECK(fixture.state->abandons == 0);
        CHECK(fixture.store.ValidateOwner(fixture.tree).HasError());
        std::array<UiBindingWriteResult, 1> outcomes;
        CHECK(fixture.store.DrainWriteResults(outcomes).Value() == 0);
        REQUIRE(fixture.store.DrainReloadRetirement().HasValue());
        CHECK(fixture.state->abandons == 1);
        CHECK(fixture.store.DrainWriteResults(outcomes).Value() == 1);
        CHECK(outcomes[0].disposition == UiBindingWriteDisposition::Cancelled);
        CHECK(outcomes[0].cancellation == UiBindingWriteCancellationReason::OwnerRetired);
        REQUIRE(fixture.store.DrainReloadRetirement().HasValue());
        fixture.store.Shutdown();
        CHECK(fixture.state->abandons == 1);
    }

    TEST_CASE("Deferred reload retirement pins producer code through reentrant abandonment and shutdown",
              "[runtime_ui][binding][write][reload][leases]") {
        WriteSession fixture;
        auto authority = fixture.Authority(0);
        authority->disposition = UiBindingWriteDisposition::Pending;
        authority->reentrantStore = &fixture.store;
        authority->abandonRetires = true;
        fixture.Admit(fixture.store, 10, authority);
        fixture.QueueCurrentChange();
        REQUIRE(Process(fixture.store, fixture.tree, fixture.layout).disposition == UiBindingWriteDisposition::Pending);
        const std::weak_ptr<TypedAuthority> producer = authority;
        authority.reset();
        REQUIRE(fixture.store.CloseReloadAdmission().HasValue());
        CHECK_FALSE(producer.expired());
        CHECK(fixture.state->abandons == 0);
        SECTION("explicit drain before shutdown") {
            REQUIRE(fixture.store.DrainReloadRetirement().HasValue());
            CHECK(fixture.state->abandons == 1);
            CHECK_FALSE(producer.expired());
            CHECK(fixture.state->destroyed == 0);
        }
        SECTION("shutdown owns an undrained deferred callback") {
            CHECK(fixture.state->prepares == 1);
        }
        fixture.store.Shutdown();
        CHECK(producer.expired());
        CHECK(fixture.state->destroyed == 1);
        CHECK(fixture.state->abandons == 1);
        CHECK(fixture.state->commits == 0);
        fixture.store.Shutdown();
        CHECK(fixture.state->abandons == 1);
    }

    TEST_CASE("Provider rejection cancellation and error preserve committed values and exact terminal evidence",
              "[runtime_ui][binding][write][feedback]") {
        WriteSession fixture;
        auto control = fixture.Control(UiControlKind::Toggle);
        auto authority = fixture.Authority(0);
        fixture.Admit(fixture.store, 10, authority);
        SECTION("expected rejection") {
            authority->disposition = UiBindingWriteDisposition::Rejected;
        }
        SECTION("producer cancellation") {
            authority->disposition = UiBindingWriteDisposition::Cancelled;
        }
        SECTION("original provider error") {
            authority->translatePrivateFailure = GENERATE(false, true);
            authority->failure = WithCause(MakeError(UiErrors::ActionHandlerFailed, "provider reservation failed"),
                                           MakeError(UiErrors::RevisionStale, "provider transaction moved"));
        }
        const auto edit = Take(fixture.store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)));
        fixture.StageToggle(control);
        const auto queued =
            Take(fixture.store.QueueControlDefault(fixture.tree, edit, control, fixture.DefaultRequest(fixture.router, control)));
        CHECK(std::get<UiToggleControlState>(control.Snapshot().Value()).checked);
        const auto outcome = Process(fixture.store, fixture.tree, fixture.layout);
        CHECK(outcome.request == queued.request);
        CHECK(outcome.operation == queued.operation);
        CHECK(outcome.disposition != UiBindingWriteDisposition::Ready);
        if (authority->disposition == UiBindingWriteDisposition::Cancelled)
            CHECK(outcome.cancellation == UiBindingWriteCancellationReason::ProviderCancelled);
        else
            CHECK(outcome.cancellation == UiBindingWriteCancellationReason::Count);
        if (authority->failure) {
            REQUIRE(outcome.error);
            CHECK(outcome.error->code.Value() == authority->failure->code.Value());
            CHECK(outcome.error->domain.Value() == authority->failure->domain.Value());
            CHECK(outcome.error->message == authority->failure->message);
            CHECK(outcome.error->severity == authority->failure->severity);
            REQUIRE(outcome.error->cause.Get());
            CHECK(outcome.error->cause.Get()->code.Value() == UiErrors::RevisionStale.code.Value());
        }
        CHECK(fixture.state->commits == 0);
        CHECK(fixture.state->revision.Value() == 1);
        CHECK_FALSE(std::get<bool>(fixture.state->values[0]));
        CHECK_FALSE(std::get<bool>(fixture.store.Find(fixture.tree, Stable<UiBindingId>(10))->value));
        REQUIRE(fixture.store.ReconcileControl(fixture.tree, Stable<UiBindingId>(10), control).HasValue());
        CHECK_FALSE(std::get<UiToggleControlState>(control.Snapshot().Value()).checked);
        CHECK(std::get<UiToggleControlState>(control.Snapshot().Value()).focused);
        CHECK(fixture.state->abandons == 1);
        CHECK_FALSE(Take(fixture.store.ProcessWrite(fixture.tree, fixture.layout)).has_value());
    }

    TEST_CASE("Private provider failures cancel an existing pending reservation without publishing committed state",
              "[runtime_ui][binding][write][feedback][lifecycle]") {
        WriteSession fixture;
        auto authority = fixture.Authority(0);
        authority->disposition = UiBindingWriteDisposition::Pending;
        fixture.Admit(fixture.store, 10, authority);
        const auto queued = fixture.QueueCurrentChange();
        CHECK(Process(fixture.store, fixture.tree, fixture.layout).disposition == UiBindingWriteDisposition::Pending);
        REQUIRE(authority->reservation);
        authority->translatePrivateFailure = true;
        authority->failure = WithCause(MakeError(UiErrors::ActionHandlerFailed, "private pending work failed"),
                                       MakeError(UiErrors::RevisionStale, "private transaction moved"));
        const auto outcome = Process(fixture.store, fixture.tree, fixture.layout);
        CHECK(outcome.request == queued.request);
        CHECK(outcome.operation == queued.operation);
        CHECK(outcome.disposition == UiBindingWriteDisposition::Rejected);
        REQUIRE(outcome.error);
        CHECK(outcome.error->code.Value() == authority->failure->code.Value());
        CHECK(outcome.error->domain.Value() == authority->failure->domain.Value());
        CHECK(outcome.error->message == authority->failure->message);
        CHECK(outcome.error->severity == authority->failure->severity);
        CHECK(outcome.error->cause.Get() == authority->failure->cause.Get());
        CHECK(fixture.state->commits == 0);
        CHECK(fixture.state->revision.Value() == 1);
        CHECK_FALSE(std::get<bool>(fixture.state->values[0]));
        CHECK_FALSE(std::get<bool>(fixture.store.Find(fixture.tree, Stable<UiBindingId>(10))->value));
        CHECK(fixture.state->abandons == 1);
        CHECK_FALSE(authority->reservation);
        CHECK_FALSE(Take(fixture.store.ProcessWrite(fixture.tree, fixture.layout)).has_value());
        const std::weak_ptr<TypedAuthority> lease = authority;
        authority.reset();
        fixture.store.Shutdown();
        CHECK(lease.expired());
        CHECK(fixture.state->destroyed == 1);
        CHECK(fixture.state->abandons == 1);
    }

    TEST_CASE("Pending producer writes are fenced by revocation reload retirement and shutdown exactly once",
              "[runtime_ui][binding][write][lifecycle]") {
        WriteSession fixture;
        auto authority = fixture.Authority(0);
        authority->disposition = UiBindingWriteDisposition::Pending;
        fixture.Admit(fixture.store, 10, authority);
        const auto queued = fixture.QueueCurrentChange();
        CHECK(Process(fixture.store, fixture.tree, fixture.layout).disposition == UiBindingWriteDisposition::Pending);
        CHECK(fixture.state->prepares == 1);
        auto expectedCancellation = UiBindingWriteCancellationReason::Count;
        SECTION("revocation") {
            expectedCancellation = UiBindingWriteCancellationReason::ProviderUnavailable;
            authority->Revoke();
            CHECK(Process(fixture.store, fixture.tree, fixture.layout).disposition == UiBindingWriteDisposition::Cancelled);
        }
        SECTION("retirement") {
            expectedCancellation = UiBindingWriteCancellationReason::OwnerRetired;
            fixture.store.BeginRetirement();
            fixture.store.BeginRetirement();
        }
        SECTION("shutdown") {
            fixture.store.Shutdown();
            fixture.store.Shutdown();
        }
        SECTION("reload replacement") {
            auto replacement = fixture.MakeTree(2);
            CHECK(Process(fixture.store, replacement, fixture.layout).disposition == UiBindingWriteDisposition::Rejected);
        }
        authority->disposition = UiBindingWriteDisposition::Ready;
        const auto late = fixture.store.ProcessWrite(fixture.tree, fixture.layout);
        CHECK((late.HasError() || !late.Value().has_value()));
        CHECK(fixture.state->prepares == 1);
        CHECK(fixture.state->commits == 0);
        CHECK(fixture.state->abandons == 1);
        std::array<UiBindingWriteResult, 1> outcomes{};
        const auto drained = fixture.store.DrainWriteResults(outcomes);
        if (drained.HasValue()) {
            CHECK(drained.Value() == 1);
            CHECK(outcomes[0].request == queued.request);
            CHECK(outcomes[0].operation == queued.operation);
            CHECK(outcomes[0].cancellation == expectedCancellation);
            CHECK(Take(fixture.store.DrainWriteResults(outcomes)) == 0);
        }
    }

    TEST_CASE("A pending provider can complete once and old adapter leases survive until store shutdown",
              "[runtime_ui][binding][write][leases]") {
        Fixture fixture;
        auto layout = fixture.Layout();
        auto router = fixture.Router();
        auto authority = fixture.Authority(0);
        std::weak_ptr<TypedAuthority> old = authority;
        {
            auto store = fixture.Store();
            fixture.Admit(store, 10, authority);
            authority->disposition = UiBindingWriteDisposition::Pending;
            fixture.QueueChange(store, router);
            CHECK(Process(store, fixture.tree, layout).disposition == UiBindingWriteDisposition::Pending);
            authority->disposition = UiBindingWriteDisposition::Ready;
            authority.reset();
            CHECK_FALSE(old.expired());
            CHECK(Process(store, fixture.tree, layout).disposition == UiBindingWriteDisposition::Ready);
            CHECK_FALSE(Take(store.ProcessWrite(fixture.tree, layout)).has_value());
            CHECK(fixture.state->commits == 1);
            CHECK(fixture.state->destroyed == 0);
            store.Shutdown();
            CHECK(old.expired());
        }
        CHECK(fixture.state->destroyed == 1);
        CHECK(fixture.state->abandons == 0);
    }

    TEST_CASE("New presentation cancels prior writes without read rebuild and accepts only fresh presented input",
              "[runtime_ui][binding][write][presentation]") {
        WriteSession fixture;
        auto authority = fixture.Authority(0);
        authority->disposition = UiBindingWriteDisposition::Pending;
        fixture.Admit(fixture.store, 10, authority);
        ClearDirty(fixture.store);
        const auto before = fixture.store.Current();
        const auto source = fixture.Source(2);
        const auto edit = Take(fixture.store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), source));
        SECTION("pending owner work is cancelled and releases once") {
            REQUIRE(fixture.store.QueueWrite(fixture.tree, edit, fixture.Request(fixture.router, 2, true), UiBindingCommitTrigger::Change)
                        .HasValue());
            CHECK(Process(fixture.store, fixture.tree, fixture.layout).disposition == UiBindingWriteDisposition::Pending);
        }
        SECTION("unqueued drafts are invalidated") {
            CHECK(fixture.state->prepares == 0);
        }
        auto owner = fixture.Context();
        owner.interaction = Rev<UiInteractionRevision>(2);
        REQUIRE(fixture.store.UpdateWritePresentation(fixture.tree, owner).HasValue());
        ErrorIs(fixture.store.UpdateWritePresentation(fixture.tree, owner), UiErrors::RevisionStale);
        ErrorIs(fixture.store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), source), UiErrors::RevisionStale);
        ErrorIs(fixture.store.CancelEdit(edit), UiErrors::RevisionStale);
        CHECK(fixture.store.Current().revision == before.revision);
        CHECK(fixture.store.Current().content == before.content);
        CHECK_FALSE(std::get<bool>(fixture.store.Find(fixture.tree, Stable<UiBindingId>(10))->value));
        std::array<UiBindingTargetDirty, 8> dirty{};
        CHECK(Take(fixture.store.DrainDirty(dirty)) == 0);
        std::array<UiBindingWriteResult, 1> outcomes{};
        const auto terminal = Take(fixture.store.DrainWriteResults(outcomes));
        CHECK(terminal == fixture.state->prepares);
        if (terminal != 0) {
            CHECK(outcomes[0].disposition == UiBindingWriteDisposition::Cancelled);
            CHECK(outcomes[0].cancellation == UiBindingWriteCancellationReason::PresentationChanged);
        }
        CHECK(fixture.state->abandons == terminal);
        CHECK_FALSE(Take(fixture.store.ProcessWrite(fixture.tree, fixture.layout)).has_value());
        authority->disposition = UiBindingWriteDisposition::Ready;
        CommitFreshPresentation(fixture, owner, source);
    }

    TEST_CASE("Failed unregister still fences provider writes before fallback layout backpressure",
              "[runtime_ui][binding][write][unregister]") {
        WriteSession fixture{1, 4};
        auto authority = fixture.Authority(0);
        authority->disposition = UiBindingWriteDisposition::Pending;
        fixture.Admit(fixture.store, 10, authority);
        fixture.QueueCurrentChange();
        CHECK(Process(fixture.store, fixture.tree, fixture.layout).disposition == UiBindingWriteDisposition::Pending);
        authority->Revoke();
        ErrorIs(fixture.store.Unregister(fixture.tree, fixture.provider, fixture.layout), UiErrors::CapacityExceeded);
        REQUIRE(fixture.store.Find(fixture.tree, Stable<UiBindingId>(10)));
        authority->active = true;
        authority->disposition = UiBindingWriteDisposition::Ready;
        ErrorIs(fixture.store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)), UiErrors::BindingLifecycleUnavailable);
        CHECK_FALSE(Take(fixture.store.ProcessWrite(fixture.tree, fixture.layout)).has_value());
        CHECK(fixture.state->commits == 0);
        CHECK(fixture.state->abandons == 1);
        std::array<UiBindingWriteResult, 1> outcomes{};
        CHECK(Take(fixture.store.DrainWriteResults(outcomes)) == 1);
        CHECK(outcomes[0].disposition == UiBindingWriteDisposition::Cancelled);
        CHECK(outcomes[0].cancellation == UiBindingWriteCancellationReason::ProviderUnavailable);
        auto available = fixture.Layout();
        REQUIRE(fixture.store.Unregister(fixture.tree, fixture.provider, available).HasValue());
        CHECK(fixture.store.Find(fixture.tree, Stable<UiBindingId>(10)) == nullptr);
    }

    TEST_CASE("Destroyed stores cancel pending provider reservations and release their old adapter lease",
              "[runtime_ui][binding][write][destructor]") {
        Fixture fixture;
        auto layout = fixture.Layout();
        auto router = fixture.Router();
        std::weak_ptr<TypedAuthority> lease;
        {
            auto store = fixture.Store();
            auto authority = fixture.Authority(0);
            authority->disposition = UiBindingWriteDisposition::Pending;
            lease = authority;
            fixture.Admit(store, 10, authority);
            fixture.QueueChange(store, router);
            CHECK(Process(store, fixture.tree, layout).disposition == UiBindingWriteDisposition::Pending);
            authority.reset();
            CHECK_FALSE(lease.expired());
        }
        CHECK(lease.expired());
        CHECK(fixture.state->abandons == 1);
        CHECK(fixture.state->destroyed == 1);
        CHECK(fixture.state->commits == 0);
    }
}  // namespace Horo::Runtime::Ui::BindingWriteTests
