#include "UiBindingWriteTestFixture.h"

#include <catch2/generators/catch_generators.hpp>
#include <limits>

namespace Horo::Runtime::Ui::BindingWriteTests {
    TEST_CASE("Write-only properties do not poll or subscribe to provider read deltas", "[runtime_ui][binding][write][direction]") {
        WriteSession fixture;
        fixture.Admit(fixture.store, 15, fixture.Authority(3));
        CHECK_FALSE(std::get<bool>(fixture.store.Find(fixture.tree, Stable<UiBindingId>(15))->value));
        const std::array changes{UiBindingPropertyUpdate{3, true}};
        const std::array batches{UiBindingChangeBatch{fixture.provider, fixture.schema.Version(), Rev<UiBindingSnapshotRevision>(),
                                                      Rev<UiBindingSnapshotRevision>(2), changes}};
        REQUIRE(fixture.store.Apply(fixture.tree, batches, fixture.layout).HasValue());
        CHECK_FALSE(std::get<bool>(fixture.store.Find(fixture.tree, Stable<UiBindingId>(15))->value));
        fixture.state->revision = Rev<UiBindingSnapshotRevision>(2);
        const auto edit = Take(fixture.store.BeginEdit(fixture.tree, Stable<UiBindingId>(15), fixture.Source(7)));
        REQUIRE(fixture.store.QueueWrite(fixture.tree, edit, fixture.Request(fixture.router, 7, true), UiBindingCommitTrigger::Change)
                    .HasValue());
        CHECK(Process(fixture.store, fixture.tree, fixture.layout).disposition == UiBindingWriteDisposition::Ready);
        CHECK(std::get<bool>(fixture.store.Find(fixture.tree, Stable<UiBindingId>(15))->value));
        CHECK(fixture.state->revision.Value() == 3);
    }

    TEST_CASE("Rejected write-only controls reconcile to their authored nondefault UI value without reading the provider",
              "[runtime_ui][binding][write][initial_target]") {
        const auto kind = GENERATE(UiControlKind::Toggle, UiControlKind::Slider, UiControlKind::TextInput);
        Fixture fixture;
        const auto test = DescribeControl(kind, true);
        auto binding = fixture.Binding(20, test.element, test.property, test.target, UiBindingDirection::TargetToSource);
        binding.initialTarget = test.initial;
        const UiBindingValue authored = *binding.initialTarget;
        auto store = fixture.WriteOnlyStore(binding);
        const auto *initial = store.Find(fixture.tree, binding.binding.id);
        REQUIRE(initial);
        CHECK(initial->origin == UiBindingValueOrigin::UiLocal);
        CHECK(initial->value == authored);
        binding.initialTarget = false;
        CHECK(store.Find(fixture.tree, binding.binding.id)->value == authored);

        auto control = fixture.Control(test);
        auto router = fixture.Router();
        auto layout = fixture.Layout();
        auto authority = fixture.Authority(test.property);
        authority->disposition = UiBindingWriteDisposition::Rejected;
        fixture.Admit(store, 20, authority, test.trigger);
        const auto edit = Take(store.BeginEdit(fixture.tree, binding.binding.id, fixture.Source(test.element)));
        fixture.StageDefault(control, Text("edit"));
        const auto request = fixture.DefaultRequest(router, control);
        UiBindingWriteActionHandler handler{store, fixture.tree, edit, control};
        REQUIRE(router.Dispatch(request, handler).HasValue());
        CHECK(Process(store, fixture.tree, layout).disposition == UiBindingWriteDisposition::Rejected);
        CHECK(store.Find(fixture.tree, binding.binding.id)->value == authored);
        REQUIRE(store.ReconcileControl(fixture.tree, binding.binding.id, control).HasValue());
        CheckControlValue(control, authored);
        CHECK(fixture.state->revision.Value() == 1);
        CHECK(fixture.state->commits == 0);
        CHECK_FALSE(std::get<bool>(fixture.state->values[0]));
        CHECK(std::get<double>(fixture.state->values[2]) == 0.5);
        CHECK(std::get<UiActionText>(fixture.state->values[1]).View() == "old");
    }

    TEST_CASE("Write-only initial projections require a typed bounded authored seed and readable bindings refuse seeds",
              "[runtime_ui][binding][write][initial_target][validation]") {
        Fixture fixture;
        const std::array values{UiBindingPropertyUpdate{0, false}, UiBindingPropertyUpdate{1, std::string{"old"}},
                                UiBindingPropertyUpdate{2, 0.5}};
        const std::array registrations{UiBindingProviderRegistration{fixture.provider, UiBindingProviderScopeKind::Player, &fixture.schema,
                                                                     Rev<UiBindingSnapshotRevision>(), values}};
        auto binding = fixture.Binding(20, 3, 2, UiBindingTargetProperty::ScalarValue, UiBindingDirection::TargetToSource);
        SECTION("absent authored value") {
            binding.initialTarget.reset();
        }
        SECTION("wrong type") {
            binding.initialTarget = true;
        }
        SECTION("property limit rejects even when target permits") {
            binding.initialTarget = 1.25;
            binding.binding.target.limits.maximumScalar = 2.0;
        }
        SECTION("target limit rejects even when property permits") {
            binding.initialTarget = 0.75;
            binding.binding.target.limits.maximumScalar = 0.5;
        }
        SECTION("nonfinite scalar") {
            binding.initialTarget = std::numeric_limits<double>::quiet_NaN();
        }
        SECTION("oversized text") {
            binding = fixture.Binding(20, 4, 1, UiBindingTargetProperty::Text, UiBindingDirection::TargetToSource);
            binding.initialTarget = std::string(65, 'x');
        }
        SECTION("malformed UTF-8") {
            binding = fixture.Binding(20, 4, 1, UiBindingTargetProperty::Text, UiBindingDirection::TargetToSource);
            binding.initialTarget = std::string{"\xc0\xaf"};
        }
        SECTION("read-only seed") {
            binding.binding.direction = UiBindingDirection::SourceToTarget;
            binding.initialTarget = 0.75;
        }
        SECTION("two-way seed cannot override committed source") {
            binding.binding.direction = UiBindingDirection::TwoWay;
            binding.initialTarget = 0.75;
        }
        REQUIRE(UiBindingStore::Create(fixture.tree, registrations, std::array{binding}).HasError());
        CHECK(fixture.state->prepares == 0);
        CHECK(fixture.state->commits == 0);
        CHECK(fixture.tree.State() == UiElementTreeState::Active);
    }

    TEST_CASE("Layout backpressure rejects a prepared text write without publishing any owner", "[runtime_ui][binding][write][atomic]") {
        WriteSession fixture{1, 4};
        auto control = fixture.Control(UiControlKind::TextInput);
        fixture.Admit(fixture.store, 11, fixture.Authority(1), UiBindingCommitTrigger::Submit);
        ClearDirty(fixture.store);
        const auto before = fixture.store.Current();
        const auto edit = Take(fixture.store.BeginEdit(fixture.tree, Stable<UiBindingId>(11), fixture.Source(4)));
        REQUIRE(
            fixture.store.QueueWrite(fixture.tree, edit, fixture.Request(fixture.router, 4, Text("new")), UiBindingCommitTrigger::Submit)
                .HasValue());
        const auto outcome = Process(fixture.store, fixture.tree, fixture.layout);
        CHECK(outcome.disposition == UiBindingWriteDisposition::Rejected);
        REQUIRE(outcome.error);
        CHECK(outcome.error->code.Value() == UiErrors::CapacityExceeded.code.Value());
        CHECK(fixture.state->commits == 0);
        CHECK(fixture.state->revision.Value() == 1);
        CHECK(std::get<UiActionText>(fixture.state->values[1]).View() == "old");
        CHECK(fixture.store.Current().revision == before.revision);
        CHECK(fixture.store.Current().content == before.content);
        CHECK(std::get<std::string>(fixture.store.Find(fixture.tree, Stable<UiBindingId>(11))->value) == "old");
        CHECK(std::get<std::string>(fixture.store.Find(fixture.tree, Stable<UiBindingId>(14))->value) == "old");
        std::array<UiBindingTargetDirty, 8> dirty{};
        CHECK(Take(fixture.store.DrainDirty(dirty)) == 0);
        REQUIRE(fixture.layout.Invalidate({fixture.Source(3).element, fixture.tree.Revision(), UiLayoutDirtyKind::Arrange}).HasValue());
        REQUIRE(fixture.store.ReconcileControl(fixture.tree, Stable<UiBindingId>(11), control).HasValue());
    }

    TEST_CASE("Prepared successful text writes allocate nothing across command queue and publication",
              "[runtime_ui][binding][write][allocation]") {
        WriteSession fixture;
        fixture.Admit(fixture.store, 11, fixture.Authority(1), UiBindingCommitTrigger::Submit);
        ClearDirty(fixture.store);
        const auto binding = Stable<UiBindingId>(11);
        const auto source = fixture.Source(4);
        const auto action = fixture.Action();
        UiActionPayload payload;
        REQUIRE(payload.Add(Text(std::string(64, 'x'))).HasValue());
        std::array<UiBindingWriteResult, 1> outcomes{};
        std::array<UiBindingTargetDirty, 8> dirty{};
        const auto before = WriteAllocations().load();
        const auto edit = fixture.store.BeginEdit(fixture.tree, binding, source);
        const auto enqueued = fixture.router.Enqueue(source, UiGameplayActionCommand{action, payload});
        const auto request = fixture.router.TryDequeue();
        const auto queued = fixture.store.QueueWrite(fixture.tree, edit.Value(), *request.Value(), UiBindingCommitTrigger::Submit);
        const auto processed = fixture.store.ProcessWrite(fixture.tree, fixture.layout);
        const auto terminal = fixture.store.DrainWriteResults(outcomes);
        const auto notifications = fixture.store.DrainDirty(dirty);
        const auto after = WriteAllocations().load();
        REQUIRE(edit.HasValue());
        REQUIRE(enqueued.HasValue());
        REQUIRE(request.HasValue());
        REQUIRE(queued.HasValue());
        REQUIRE(processed.HasValue());
        REQUIRE(processed.Value().has_value());
        CHECK(processed.Value()->disposition == UiBindingWriteDisposition::Ready);
        REQUIRE(terminal.HasValue());
        REQUIRE(notifications.HasValue());
        CHECK(terminal.Value() == 1);
        CHECK(notifications.Value() == 2);
        CHECK(fixture.state->commits == 1);
        CHECK(after == before);
    }
}  // namespace Horo::Runtime::Ui::BindingWriteTests
