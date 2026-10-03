#include "UiBindingWriteTestFixture.h"

#include <catch2/generators/catch_generators.hpp>
#include <limits>

namespace Horo::Runtime::Ui::BindingWriteTests {
    TEST_CASE("Admitted control defaults commit typed provider state through the versioned binding owner",
              "[runtime_ui][binding][write][controls]") {
        const auto kind = GENERATE(UiControlKind::Toggle, UiControlKind::Slider, UiControlKind::TextInput);
        WriteSession fixture;
        auto control = fixture.Control(kind);
        const auto test = DescribeControl(kind);
        const auto authority = fixture.Authority(test.property);
        fixture.Admit(fixture.store, test.binding, authority, test.trigger);
        CHECK(fixture.state->prepares == 0);
        ClearDirty(fixture.store);
        const auto edit =
            Take(fixture.store.BeginEdit(fixture.tree, Stable<UiBindingId>(test.binding), {control.Owner(), control.Element()}));
        fixture.StageDefault(control, Text("new"));
        const auto request = fixture.DefaultRequest(fixture.router, control);
        const auto current = fixture.store.Current();
        UiBindingWriteActionHandler handler{fixture.store, fixture.tree, edit, control};
        const auto queued = Take(fixture.router.Dispatch(request, handler));
        CHECK(queued.kind == UiActionResultKind::Pending);
        CHECK(queued.request == request.id);
        CHECK(fixture.state->commits == 0);
        CHECK(fixture.store.Current().revision == current.revision);
        ErrorIs(fixture.store.ReconcileControl(fixture.tree, Stable<UiBindingId>(test.binding), control),
                UiErrors::BindingDescriptorConflict);
        const auto committed = Process(fixture.store, fixture.tree, fixture.layout);
        CHECK(committed.disposition == UiBindingWriteDisposition::Ready);
        CHECK(committed.operation == queued.operation);
        CHECK(fixture.state->revision.Value() == 2);
        CHECK(fixture.state->commits == 1);
        CHECK(fixture.state->commitFenceValid);
        REQUIRE(fixture.store.ReconcileControl(fixture.tree, Stable<UiBindingId>(test.binding), control).HasValue());
        CheckCommittedValue(fixture, fixture.store, kind, current.content);
        CHECK_FALSE(Take(fixture.store.ProcessWrite(fixture.tree, fixture.layout)).has_value());
        std::array<UiBindingWriteResult, 1> terminal{};
        CHECK(Take(fixture.store.DrainWriteResults(terminal)) == 1);
        CHECK(Take(fixture.store.DrainWriteResults(terminal)) == 0);
        CHECK(fixture.state->abandons == 0);
    }

    TEST_CASE("Production action dispatch suppresses a refused control write and queue backpressure preserves the staged default",
              "[runtime_ui][binding][write][routing]") {
        WriteSession fixture{8, 1};
        auto control = fixture.Control(UiControlKind::Toggle);
        fixture.Admit(fixture.store, 10, fixture.Authority(0), UiBindingCommitTrigger::Submit);
        const auto edit = Take(fixture.store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)));
        fixture.StageToggle(control);
        const auto preview = Take(control.PeekDefault());
        REQUIRE(preview);
        SECTION("typed provider policy refusal suppresses default through the router") {
            REQUIRE(fixture.router.Enqueue(preview->source, UiGameplayActionCommand{preview->action, preview->payload}).HasValue());
            UiBindingWriteActionHandler handler{fixture.store, fixture.tree, edit, control};
            ErrorIs(fixture.router.DispatchNext(handler), UiErrors::BindingAccessInvalid);
            CHECK_FALSE(Take(control.PeekDefault()).has_value());
        }
        SECTION("action queue capacity failure cannot mutate control or binding values") {
            REQUIRE(fixture.router.Enqueue(fixture.Source(3), UiGameplayActionCommand{fixture.Action(), {}}).HasValue());
            ErrorIs(fixture.router.Enqueue(preview->source, UiGameplayActionCommand{preview->action, preview->payload}),
                    UiErrors::ActionQueueCapacityExceeded);
            CHECK(Take(control.PeekDefault()).has_value());
            REQUIRE(control.SuppressDefault().HasValue());
        }
        CHECK_FALSE(std::get<UiToggleControlState>(control.Snapshot().Value()).checked);
        CHECK_FALSE(std::get<bool>(fixture.store.Find(fixture.tree, Stable<UiBindingId>(10))->value));
        CHECK_FALSE(Take(fixture.store.ProcessWrite(fixture.tree, fixture.layout)).has_value());
        CHECK(fixture.state->prepares == 0);
        REQUIRE(fixture.store.CancelEdit(edit).HasValue());
        REQUIRE(fixture.store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)).HasValue());
    }

    TEST_CASE("Malformed routed control requests always suppress the staged default before returning failure",
              "[runtime_ui][binding][write][routing]") {
        WriteSession fixture;
        auto control = fixture.Control(UiControlKind::Toggle);
        fixture.Admit(fixture.store, 10, fixture.Authority(0));
        const auto edit = Take(fixture.store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)));
        fixture.StageToggle(control);
        auto request = fixture.DefaultRequest(fixture.router, control);
        SECTION("wrong action") {
            std::get<UiGameplayActionCommand>(request.command).action = Stable<UiActionId>(91);
        }
        SECTION("wrong source") {
            request.source = fixture.Source(6);
        }
        SECTION("wrong payload value") {
            UiActionPayload payload;
            REQUIRE(payload.Add(false).HasValue());
            std::get<UiGameplayActionCommand>(request.command).payload = payload;
        }
        SECTION("wrong payload shape") {
            std::get<UiGameplayActionCommand>(request.command).payload = {};
        }
        UiBindingWriteActionHandler handler{fixture.store, fixture.tree, edit, control};
        REQUIRE(fixture.router.Dispatch(request, handler).HasError());
        CHECK_FALSE(Take(control.PeekDefault()).has_value());
        CHECK_FALSE(std::get<UiToggleControlState>(control.Snapshot().Value()).checked);
        CHECK_FALSE(std::get<bool>(fixture.store.Find(fixture.tree, Stable<UiBindingId>(10))->value));
        CHECK_FALSE(Take(fixture.store.ProcessWrite(fixture.tree, fixture.layout)).has_value());
        CHECK(fixture.state->prepares == 0);
        REQUIRE(fixture.store.CancelEdit(edit).HasValue());
        REQUIRE(control.Handle(fixture.Input(control, UiControlInputKind::SubmitPress, 4)).HasValue());
    }

    TEST_CASE("Write capability admission is explicit exact and atomic", "[runtime_ui][binding][write][authority]") {
        Fixture fixture;
        auto store = fixture.Store();
        ErrorIs(store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)), UiErrors::BindingAccessInvalid);
        auto authority = fixture.Authority(0);
        auto admission = fixture.Admission(10, authority);
        SECTION("absent authority") {
            admission.authority.reset();
        }
        SECTION("wrong provider generation") {
            ++authority->fence.provider.generation;
        }
        SECTION("wrong scope") {
            authority->fence.scope = UiBindingProviderScopeKind::Scene;
        }
        SECTION("wrong schema") {
            ++authority->fence.schema.minor;
        }
        SECTION("wrong signature") {
            ++authority->fence.signature;
        }
        SECTION("wrong property") {
            authority->fence.property = 1;
        }
        SECTION("absent permission") {
            authority->fence.capability = 0;
        }
        SECTION("read-only binding") {
            admission.binding = Stable<UiBindingId>(14);
        }
        const std::array batch{admission};
        ErrorIs(store.AdmitWrites(fixture.tree, batch), UiErrors::BindingAccessInvalid);
        CHECK(fixture.state->prepares == 0);
        ErrorIs(store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)), UiErrors::BindingAccessInvalid);
    }

    TEST_CASE("Typed drafts and commit triggers validate before provider preparation", "[runtime_ui][binding][write][validation]") {
        using enum UiBindingCommitTrigger;
        WriteSession fixture;
        SECTION("scalar wrong type and outside bounds preserve edit for correction") {
            fixture.Admit(fixture.store, 12, fixture.Authority(2));
            const auto edit = Take(fixture.store.BeginEdit(fixture.tree, Stable<UiBindingId>(12), fixture.Source(3)));
            ErrorIs(fixture.store.QueueWrite(fixture.tree, edit, fixture.Request(fixture.router, 3, true), Change),
                    UiErrors::BindingTypeMismatch);
            ErrorIs(fixture.store.QueueWrite(fixture.tree, edit, fixture.Request(fixture.router, 3, 1.25), Change),
                    UiErrors::BindingValueInvalid);
            REQUIRE(fixture.store.QueueWrite(fixture.tree, edit, fixture.Request(fixture.router, 3, 0.25), Change).HasValue());
            CHECK(Process(fixture.store, fixture.tree, fixture.layout).disposition == UiBindingWriteDisposition::Ready);
        }
        SECTION("oversized text preserves prior value") {
            fixture.Admit(fixture.store, 11, fixture.Authority(1), Submit);
            const auto edit = Take(fixture.store.BeginEdit(fixture.tree, Stable<UiBindingId>(11), fixture.Source(4)));
            ErrorIs(fixture.store.QueueWrite(fixture.tree, edit, fixture.Request(fixture.router, 4, Text(std::string(65, 'x'))), Submit),
                    UiErrors::BindingValueInvalid);
            CHECK(std::get<std::string>(fixture.store.Find(fixture.tree, Stable<UiBindingId>(11))->value) == "old");
            CHECK(fixture.state->prepares == 0);
        }
        SECTION("malformed action is rejected at the action boundary") {
            UiActionPayload payload;
            UiActionText malformed;
            malformed.bytes[0] = static_cast<char>(0xff);
            malformed.size = 1;
            ErrorIs(payload.Add(malformed), UiErrors::ActionPayloadInvalid);
            ErrorIs(payload.Add(std::numeric_limits<double>::infinity()), UiErrors::ActionPayloadInvalid);
            fixture.Admit(fixture.store, 10, fixture.Authority(0));
            const auto edit = Take(fixture.store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)));
            auto request = fixture.Request(fixture.router, 2, true);
            request.origin = UiActionOrigin::Form;
            ErrorIs(fixture.store.QueueWrite(fixture.tree, edit, request, Change), UiErrors::ActionCommandInvalid);
        }
        SECTION("blur requires its exact explicit trigger") {
            fixture.Admit(fixture.store, 10, fixture.Authority(0), Blur);
            const auto edit = Take(fixture.store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)));
            const auto request = fixture.Request(fixture.router, 2, true);
            ErrorIs(fixture.store.QueueWrite(fixture.tree, edit, request, Change), UiErrors::BindingAccessInvalid);
            ErrorIs(fixture.store.QueueWrite(fixture.tree, edit, request, Submit), UiErrors::BindingAccessInvalid);
            UiBindingWriteActionHandler handler{fixture.store, fixture.tree, edit, Blur};
            CHECK(Take(fixture.router.Dispatch(request, handler)).kind == UiActionResultKind::Pending);
            CHECK(Process(fixture.store, fixture.tree, fixture.layout).disposition == UiBindingWriteDisposition::Ready);
        }
    }
}  // namespace Horo::Runtime::Ui::BindingWriteTests
