#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiNavigationInput.h"
#include "support/AllocationProbe.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename Id> Id Stable(const std::uint8_t value) {
            SerializedUiId bytes{};
            bytes.back() = value;
            return Id::Create(bytes).Value();
        }

        UiFocusOwnerContext Owner(const std::uint64_t revision = 3) {
            const auto generation = UiOwnershipGeneration::Create(73).Value();
            return {{generation, 1, 1},
                    {generation, 2, 1},
                    Stable<UiDocumentId>(1),
                    UiDocumentRevision::Create(1).Value(),
                    UiRuntimeTreeRevision::Create(1).Value(),
                    UiInteractionRevision::Create(revision).Value(),
                    {{UiFocusPlayerId{generation, 3, 1}}, {generation, 4, 1}}};
        }

        UiActionOwnerContext ActionOwner(const UiFocusOwnerContext &owner) {
            return {owner.instance, owner.canvas, owner.document, owner.documentRevision, owner.treeRevision, owner.interaction};
        }

        std::array<UiFocusNodeDescriptor, 6> Nodes(const UiFocusOwnerContext &owner = Owner()) {
            std::array<UiFocusNodeDescriptor, 6> nodes;
            for (std::size_t index = 0; index < nodes.size(); ++index) {
                auto &node = nodes[index];
                node.element = {owner.instance.ownership, static_cast<std::uint32_t>(10 + index), 1};
                node.id = Stable<UiElementId>(static_cast<std::uint8_t>(10 + index));
                if (index != 0)
                    node.parent = Stable<UiElementId>(index >= 4 ? 13 : 10);
                node.focusable = index != 0 && index != 3;
                constexpr std::array<std::uint8_t, 6> nextSlots{14, 12, 11, 14, 15, 14};
                const auto next = nextSlots[index];
                if (node.focusable)
                    node.links.targets.fill(Stable<UiElementId>(next));
            }
            return nodes;
        }

        UiFocusGraph Graph(const UiFocusOwnerContext &owner = Owner()) {
            auto result =
                UiFocusGraph::Create({owner, Stable<UiElementId>(11), UiFocusRecoveryPolicy::DefaultThenFirst, 8, 4, 4}, Nodes(owner));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        struct Fixture {
            Input::InputService input;
            Input::InputContextId contextId{"runtime.ui"};
            Input::InputContextToken context{input.Router().PushContext(contextId, Input::InputContextKind::FocusedGuiWidget)};
            UiFocusGraph graph{Graph()};
            UiActionRouter actions{std::move(UiActionRouter::Create({ActionOwner(Owner()), 8})).Value()};
            Input::GamepadDeviceId pad;
            Input::GamepadDeviceId other;
            UiNavigationInputDescriptor descriptor;
            std::optional<UiNavigationInput> adapter;
            std::uint64_t frame{};

            Fixture() {
                auto map = DefaultUiNavigationActions(contextId);
                for (std::size_t index = 0; index < UiNavigationActionCount; ++index)
                    descriptor.actions[index] = map[index].id;
                descriptor.player = 0;
                descriptor.modalityHysteresisMilliseconds = 100;
                REQUIRE(input.Router().SetActionMap(std::move(map)).HasValue());
                input.BeginFrame(0);
                pad = input.Collector().ConnectGamepad("Türkçe 日本語");
                other = input.Collector().ConnectGamepad("Another player's pad");
                static_cast<void>(input.CommitFrame());
                REQUIRE(input.Router().AssignGamepad(0, pad));
                REQUIRE(input.Router().AssignGamepad(1, other));
                auto created = UiNavigationInput::Create(descriptor, input.Router(), context, graph, actions);
                REQUIRE(created.HasValue());
                adapter.emplace(std::move(created).Value());
                REQUIRE(adapter->Pump(input.Router(), context, graph, actions, 0).HasValue());
            }

            Input::RawInputCollector &Begin() {
                input.BeginFrame(++frame);
                return input.Collector();
            }

            UiNavigationInputFrame Commit(const std::uint64_t time) {
                static_cast<void>(input.CommitFrame());
                const auto result = adapter->Pump(input.Router(), context, graph, actions, time);
                REQUIRE(result.HasValue());
                return result.Value();
            }

            std::uint32_t FocusSlot() const {
                return graph.CurrentFocus().Value()->element.slot;
            }
        };

        TEST_CASE("Consumed presses cannot begin held navigation in another consumer", "[runtime_ui][navigation]") {
            Fixture fixture;
            REQUIRE(fixture.Begin().SetGamepadButton(fixture.pad, Input::GamepadButton::DPadDown, true));
            static_cast<void>(fixture.input.CommitFrame());
            REQUIRE(fixture.input.Router().ReadAction(fixture.context, fixture.descriptor.actions[3], 0).pressed);
            const auto consumed = fixture.adapter->Pump(fixture.input.Router(), fixture.context, fixture.graph, fixture.actions, 1);
            REQUIRE(consumed.HasValue());
            CHECK_FALSE(consumed.Value().focus);
            CHECK(consumed.Value().presentation.modality == Input::InputModality::Unknown);
            fixture.Begin();
            CHECK_FALSE(fixture.Commit(500).focus);
            REQUIRE(fixture.Begin().SetGamepadButton(fixture.pad, Input::GamepadButton::DPadDown, false));
            fixture.Commit(501);
            REQUIRE(fixture.Begin().SetGamepadButton(fixture.pad, Input::GamepadButton::DPadDown, true));
            CHECK(fixture.Commit(502).focus.has_value());
        }

        TEST_CASE("Navigation construction refuses invalid policies and bounded capability overflow",
                  "[runtime_ui][navigation][validation]") {
            Fixture fixture;
            const auto create = [&](const UiNavigationInputDescriptor &descriptor) {
                return UiNavigationInput::Create(descriptor, fixture.input.Router(), fixture.context, fixture.graph, fixture.actions);
            };
            auto descriptor = fixture.descriptor;
            descriptor.repeatDelayMilliseconds = 0;
            CHECK(create(descriptor).HasError());
            descriptor = fixture.descriptor;
            descriptor.repeatIntervalMilliseconds = 10'001;
            CHECK(create(descriptor).HasError());
            descriptor = fixture.descriptor;
            descriptor.modalityHysteresisMilliseconds = 10'001;
            CHECK(create(descriptor).HasError());
            descriptor = fixture.descriptor;
            descriptor.actions[1] = descriptor.actions[0];
            CHECK(create(descriptor).HasError());
            descriptor = fixture.descriptor;
            descriptor.actions[0] = Input::ActionId{"missing"};
            CHECK(create(descriptor).HasError());
            auto profile = fixture.input.Router().Profile();
            std::vector<Input::InputBinding> bindings;
            for (auto key = static_cast<std::size_t>(Input::Key::A); key < static_cast<std::size_t>(Input::Key::A) + 33; ++key) {
                auto &binding = bindings.emplace_back();
                binding.key = static_cast<Input::Key>(key);
            }
            profile.overrides.emplace_back(fixture.descriptor.actions[0], bindings);
            REQUIRE(fixture.input.Router().SetProfile(profile).HasValue());
            const auto overflow = create(fixture.descriptor);
            REQUIRE(overflow.HasError());
            CHECK(overflow.ErrorValue().code.Value() == UiErrors::CapacityExceeded.code.Value());
            std::vector<Input::InputContextToken> tokens;
            for (std::size_t index = 0; index < 64; ++index)
                tokens.emplace_back(fixture.input.Router().PushContext(Input::InputContextId{"extra"}, Input::InputContextKind::Gameplay));
            CHECK(create(fixture.descriptor).HasError());
        }

        TEST_CASE("Canonical glyph vocabulary has stable supported controls and typed missing capabilities", "[runtime_ui][glyph]") {
            using enum Input::BindingControlKind;
            const std::array kinds{Key, PointerButton, GamepadButton, GamepadAxis};
            const std::array counts{static_cast<std::size_t>(Input::Key::Count), static_cast<std::size_t>(Input::PointerButton::Count),
                                    static_cast<std::size_t>(Input::GamepadButton::Count),
                                    static_cast<std::size_t>(Input::GamepadAxis::Count)};
            for (std::size_t kind = 0; kind < kinds.size(); ++kind) {
                for (std::size_t control = kind == 0 ? 1 : 0; control < counts[kind]; ++control) {
                    const Input::InputGlyphId id{kinds[kind], static_cast<std::uint16_t>(control)};
                    const auto before = Tests::AllocationProbe::Count();
                    const auto glyph = Input::CanonicalGlyph(id);
                    const auto after = Tests::AllocationProbe::Count();
                    CHECK(before == after);
                    CHECK(glyph.id == id);
                    CHECK(glyph.support == Input::InputGlyphSupport::CanonicalLabel);
                    CHECK_FALSE(glyph.label.empty());
                }
                CHECK(Input::CanonicalGlyph({kinds[kind], 65535}).support == Input::InputGlyphSupport::Unsupported);
            }
            CHECK(Input::CanonicalGlyph({Input::BindingControlKind::PointerWheelX, 0}).label == "Wheel X");
            CHECK(Input::CanonicalGlyph({Input::BindingControlKind::PointerWheelY, 0}).label == "Wheel Y");
            CHECK(Input::CanonicalGlyph({Input::BindingControlKind::PointerWheelX, 1}).support == Input::InputGlyphSupport::Unsupported);
            CHECK(Input::CanonicalGlyph({Input::BindingControlKind::RawGamepadAxis, 1}).support == Input::InputGlyphSupport::Unsupported);
        }

        TEST_CASE("Full action queue refuses submit without changing earlier admitted commands", "[runtime_ui][navigation][capacity]") {
            Fixture fixture;
            const UiActionSource source{fixture.actions.Owner(), fixture.graph.CurrentFocus().Value()->element};
            for (std::size_t index = 0; index < 8; ++index)
                REQUIRE(fixture.actions.Enqueue(source, UiNavigationCommand{UiNavigationDirection::Cancel, source.element}).HasValue());
            REQUIRE(fixture.Begin().SetGamepadButton(fixture.pad, Input::GamepadButton::South, true));
            static_cast<void>(fixture.input.CommitFrame());
            const auto refused = fixture.adapter->Pump(fixture.input.Router(), fixture.context, fixture.graph, fixture.actions, 1);
            REQUIRE(refused.HasError());
            CHECK(refused.ErrorValue().code.Value() == UiErrors::ActionQueueCapacityExceeded.code.Value());
            CHECK(fixture.actions.QueuedCount() == 8);
            CHECK(fixture.FocusSlot() == 11);
            REQUIRE(fixture.actions.TryDequeue().Value());
            fixture.Begin();
            CHECK_FALSE(fixture.Commit(2).requests[0]);
            CHECK(fixture.actions.QueuedCount() == 7);
        }

        TEST_CASE("Repeat deadlines do not wrap or fire repeatedly at an exhausted clock", "[runtime_ui][navigation]") {
            Fixture fixture;
            REQUIRE(fixture.Begin().SetGamepadButton(fixture.pad, Input::GamepadButton::DPadDown, true));
            REQUIRE(fixture.Commit(std::numeric_limits<std::uint64_t>::max() - 1).focus);
            fixture.Begin();
            CHECK_FALSE(fixture.Commit(std::numeric_limits<std::uint64_t>::max()).focus);
            fixture.Begin();
            CHECK_FALSE(fixture.Commit(std::numeric_limits<std::uint64_t>::max()).focus);
        }

        TEST_CASE("Canonical UI composition navigates and repeats one bounded step without allocating", "[runtime_ui][navigation]") {
            Fixture fixture;
            REQUIRE(fixture.Begin().SetGamepadButton(fixture.pad, Input::GamepadButton::DPadDown, true));
            static_cast<void>(fixture.input.CommitFrame());
            const auto before = Tests::AllocationProbe::Count();
            const auto first = fixture.adapter->Pump(fixture.input.Router(), fixture.context, fixture.graph, fixture.actions, 1);
            const auto after = Tests::AllocationProbe::Count();
            REQUIRE(first.HasValue());
            CHECK(after == before);
            REQUIRE(first.Value().focus);
            CHECK(fixture.FocusSlot() == 12);
            CHECK(first.Value().presentation.modality == Input::InputModality::Gamepad);
            CHECK(first.Value().presentation.device == fixture.pad);
            CHECK(first.Value().presentation.glyphs[6].label == "South");
            const auto duplicate = fixture.adapter->Pump(fixture.input.Router(), fixture.context, fixture.graph, fixture.actions, 1);
            CHECK(duplicate.Value().status == UiNavigationInputStatus::DuplicateFrame);
            CHECK(fixture.FocusSlot() == 12);
            fixture.Begin();
            CHECK_FALSE(fixture.Commit(350).focus);
            fixture.Begin();
            CHECK(fixture.Commit(351).focus.has_value());
            CHECK(fixture.FocusSlot() == 11);
            fixture.Begin();
            CHECK(fixture.Commit(10'000).focus.has_value());
            CHECK(fixture.FocusSlot() == 12);
            REQUIRE(fixture.Begin().SetGamepadButton(fixture.pad, Input::GamepadButton::DPadDown, false));
            CHECK_FALSE(fixture.Commit(10'001).focus);
        }

        TEST_CASE("Left stick uses normalized signed action and dominant-axis diagonal policy", "[runtime_ui][navigation]") {
            Fixture fixture;
            REQUIRE(fixture.Begin().SetGamepadAxis(fixture.pad, Input::GamepadAxis::LeftY, 0.1F));
            CHECK_FALSE(fixture.Commit(1).focus);
            CHECK(fixture.adapter->Presentation().modality == Input::InputModality::Unknown);
            REQUIRE(fixture.Begin().SetGamepadAxis(fixture.pad, Input::GamepadAxis::LeftY, 1.0F));
            REQUIRE(fixture.input.Collector().SetGamepadAxis(fixture.pad, Input::GamepadAxis::LeftX, 1.0F));
            const auto diagonal = fixture.Commit(2);
            REQUIRE(diagonal.focus);
            CHECK(fixture.FocusSlot() == 12);
            REQUIRE(fixture.Begin().SetGamepadAxis(fixture.pad, Input::GamepadAxis::LeftY, -1.0F));
            CHECK(fixture.Commit(3).focus.has_value());
            CHECK(fixture.FocusSlot() == 11);
        }

        TEST_CASE("Glyph modality changes preserve admitted action and exact focus ownership", "[runtime_ui][navigation][glyph]") {
            Fixture fixture;
            REQUIRE(fixture.Begin().SetGamepadButton(fixture.pad, Input::GamepadButton::South, true));
            static_cast<void>(fixture.input.CommitFrame());
            const auto before = Tests::AllocationProbe::Count();
            const auto submit = fixture.adapter->Pump(fixture.input.Router(), fixture.context, fixture.graph, fixture.actions, 1);
            const auto after = Tests::AllocationProbe::Count();
            REQUIRE(submit.HasValue());
            CHECK(before == after);
            REQUIRE(submit.Value().requests[0]);
            const auto request = fixture.actions.TryDequeue().Value();
            REQUIRE(request);

            class Pending final : public UiActionHandler {
                Result<UiActionResult> Handle(const UiActionRequest &action) override {
                    return UiActionResult::Pending(action.id, {action.id.ownership, UiActionOperationSequence::Create(8).Value()});
                }
            };

            Pending handler;

            const auto pending = fixture.actions.Dispatch(*request, handler);
            REQUIRE(pending.HasValue());
            CHECK(pending.Value().kind == UiActionResultKind::Pending);
            const auto focused = fixture.graph.CurrentFocus().Value();
            REQUIRE(fixture.Begin().SetGamepadButton(fixture.pad, Input::GamepadButton::South, false));
            fixture.input.Collector().SetKey(Input::Key::Enter, true);
            CHECK(fixture.Commit(2).presentation.modality == Input::InputModality::Gamepad);
            fixture.Begin();
            const auto switched = fixture.Commit(102);
            CHECK(switched.presentation.modality == Input::InputModality::KeyboardMouse);
            CHECK(switched.presentation.glyphs[6].label == "Enter");
            CHECK(fixture.graph.CurrentFocus().Value() == focused);
            CHECK(fixture.actions.QueuedCount() == 1);
            const auto terminal = UiActionResult::Completed(request->id, {}, pending.Value().operation);
            REQUIRE(terminal.HasValue());
            CHECK(terminal.Value().operation == pending.Value().operation);
            CHECK(request->source.owner == fixture.actions.Owner());
        }

        TEST_CASE("Background devices and pointer hover cannot switch an audience's modality", "[runtime_ui][navigation][glyph]") {
            Fixture fixture;
            REQUIRE(fixture.Begin().SetGamepadButton(fixture.other, Input::GamepadButton::South, true));
            fixture.input.Collector().SetPointerPosition(400, 300);
            const auto result = fixture.Commit(1);
            CHECK(result.presentation.modality == Input::InputModality::Unknown);
            CHECK(fixture.actions.QueuedCount() == 0);
            CHECK(fixture.FocusSlot() == 11);
        }

        TEST_CASE("Opposing directions cancel and simultaneous cancel wins submit", "[runtime_ui][navigation]") {
            Fixture fixture;
            auto &collector = fixture.Begin();
            REQUIRE(collector.SetGamepadButton(fixture.pad, Input::GamepadButton::DPadUp, true));
            REQUIRE(collector.SetGamepadButton(fixture.pad, Input::GamepadButton::DPadDown, true));
            REQUIRE(collector.SetGamepadButton(fixture.pad, Input::GamepadButton::South, true));
            REQUIRE(collector.SetGamepadButton(fixture.pad, Input::GamepadButton::East, true));
            const auto output = fixture.Commit(1);
            CHECK_FALSE(output.focus);
            CHECK_FALSE(output.requests[0]);
            REQUIRE(output.requests[1]);
            const auto request = fixture.actions.TryDequeue().Value();
            REQUIRE(request);
            CHECK(std::get<UiNavigationCommand>(request->command).direction == UiNavigationDirection::Cancel);
        }

        TEST_CASE("Window focus loss and suspension require neutral before held navigation resumes",
                  "[runtime_ui][navigation][lifecycle]") {
            Fixture fixture;
            REQUIRE(fixture.Begin().SetGamepadButton(fixture.pad, Input::GamepadButton::DPadDown, true));
            REQUIRE(fixture.Commit(1).focus);
            fixture.Begin().SetWindowState({false, true, true});
            CHECK(fixture.Commit(2).status == UiNavigationInputStatus::Blocked);
            fixture.Begin().SetWindowState({true, true, true});
            CHECK_FALSE(fixture.Commit(800).focus);
            REQUIRE(fixture.Begin().SetGamepadButton(fixture.pad, Input::GamepadButton::DPadDown, false));
            fixture.Commit(801);
            REQUIRE(fixture.Begin().SetGamepadButton(fixture.pad, Input::GamepadButton::DPadDown, true));
            CHECK(fixture.Commit(802).focus.has_value());
            fixture.adapter->Suspend();
            fixture.Begin();
            CHECK_FALSE(fixture.Commit(900).focus);
        }

        TEST_CASE("Top modal graph and Input preemption neutralize held actions and preserve restoration",
                  "[runtime_ui][navigation][modal]") {
            Fixture fixture;
            REQUIRE(fixture.Begin().SetGamepadButton(fixture.pad, Input::GamepadButton::DPadDown, true));
            REQUIRE(fixture.Commit(1).focus);
            const auto nodes = Nodes();
            const auto opened = fixture.graph.PushModal({nodes[3].element, nodes[3].id, nodes[4].id});
            REQUIRE(opened.HasValue());
            fixture.Begin();
            CHECK_FALSE(fixture.Commit(400).focus);
            CHECK(fixture.FocusSlot() == 14);
            REQUIRE(fixture.Begin().SetGamepadButton(fixture.pad, Input::GamepadButton::DPadDown, false));
            fixture.Commit(401);
            REQUIRE(fixture.Begin().SetGamepadButton(fixture.pad, Input::GamepadButton::DPadDown, true));
            CHECK(fixture.Commit(402).focus.has_value());
            CHECK(fixture.FocusSlot() == 15);
            REQUIRE(fixture.graph.PopModal(opened.Value().modal).HasValue());
            CHECK(fixture.FocusSlot() == 12);
            auto native = fixture.input.Router().PushContext(Input::InputContextId{"native"}, Input::InputContextKind::NativeDialog);
            fixture.Begin();
            CHECK(fixture.Commit(403).status == UiNavigationInputStatus::Blocked);
            native.Reset();
            fixture.Begin();
            CHECK_FALSE(fixture.Commit(1000).focus);
        }

        TEST_CASE("Disconnect and assignment replacement cannot transfer held input or glyph device generation",
                  "[runtime_ui][navigation][lifecycle]") {
            Fixture fixture;
            REQUIRE(fixture.Begin().SetGamepadButton(fixture.pad, Input::GamepadButton::South, true));
            REQUIRE(fixture.Commit(1).requests[0]);
            const auto oldGlyph = fixture.adapter->Presentation().glyphs[6].id;
            REQUIRE(fixture.Begin().DisconnectGamepad(fixture.pad));
            const auto replacement = fixture.input.Collector().ConnectGamepad("日本語 renamed");
            REQUIRE(fixture.input.Collector().SetGamepadButton(replacement, Input::GamepadButton::South, true));
            static_cast<void>(fixture.input.CommitFrame());
            REQUIRE(fixture.input.Router().AssignGamepad(0, replacement));
            const auto lost = fixture.adapter->Pump(fixture.input.Router(), fixture.context, fixture.graph, fixture.actions, 2);
            REQUIRE(lost.HasValue());
            CHECK(lost.Value().presentation.modality == Input::InputModality::Unknown);
            CHECK_FALSE(lost.Value().requests[0]);
            REQUIRE(fixture.Begin().SetGamepadButton(replacement, Input::GamepadButton::South, false));
            fixture.Commit(3);
            REQUIRE(fixture.Begin().SetGamepadButton(replacement, Input::GamepadButton::South, true));
            const auto reconnected = fixture.Commit(4);
            REQUIRE(reconnected.requests[0]);
            CHECK(reconnected.presentation.device == replacement);
            CHECK(reconnected.presentation.glyphs[6].id == oldGlyph);
            CHECK(replacement != fixture.pad);
            CHECK(fixture.actions.QueuedCount() == 2);
        }

        TEST_CASE("Published reload and profile changes require explicit rebind without touching old queued actions",
                  "[runtime_ui][navigation][reload]") {
            Fixture fixture;
            REQUIRE(fixture.Begin().SetGamepadButton(fixture.pad, Input::GamepadButton::South, true));
            REQUIRE(fixture.Commit(1).requests[0]);
            auto nodes = Nodes(Owner(4));
            nodes[1].element.slot = 21;
            auto reload = fixture.graph.Reload({Owner(4), nodes[1].id, UiFocusRecoveryPolicy::DefaultThenFirst, 8, 4, 4}, nodes);
            REQUIRE(reload.HasValue());
            auto replacementActions = std::move(UiActionRouter::Create({ActionOwner(Owner(4)), 8})).Value();
            fixture.Begin();
            static_cast<void>(fixture.input.CommitFrame());
            CHECK(fixture.adapter->Pump(fixture.input.Router(), fixture.context, fixture.graph, replacementActions, 2).HasError());
            CHECK(fixture.actions.QueuedCount() == 1);
            REQUIRE(fixture.adapter->Rebind(fixture.input.Router(), fixture.context, fixture.graph, replacementActions).HasValue());
            fixture.actions = std::move(replacementActions);
            CHECK(fixture.FocusSlot() == 21);
            CHECK_FALSE(
                fixture.adapter->Pump(fixture.input.Router(), fixture.context, fixture.graph, fixture.actions, 2).Value().requests[0]);
            auto profile = fixture.input.Router().Profile();
            Input::InputBinding key;
            key.key = Input::Key::Space;
            Input::InputBinding pad;
            pad.kind = Input::BindingControlKind::GamepadButton;
            pad.gamepadButton = Input::GamepadButton::North;
            profile.overrides.push_back({fixture.descriptor.actions[6], {key, pad}});
            REQUIRE(fixture.input.Router().SetProfile(profile).HasValue());
            fixture.Begin();
            CHECK(fixture.Commit(3).status == UiNavigationInputStatus::NeedsRebind);
            REQUIRE(fixture.adapter->Rebind(fixture.input.Router(), fixture.context, fixture.graph, fixture.actions).HasValue());
            CHECK(fixture.adapter->Presentation().glyphs[6].label == "North");
        }

        TEST_CASE("Foreign contexts stale owners clock reversal and stopped owners are refused", "[runtime_ui][navigation][validation]") {
            Fixture fixture;
            Input::InputRouter foreign;
            auto token = foreign.PushContext(fixture.contextId, Input::InputContextKind::FocusedGuiWidget);
            CHECK(fixture.adapter->Pump(foreign, token, fixture.graph, fixture.actions, 1).HasError());
            auto wrong = Graph(Owner(4));
            CHECK(fixture.adapter->Pump(fixture.input.Router(), fixture.context, wrong, fixture.actions, 1).HasError());
            fixture.Begin();
            fixture.Commit(10);
            fixture.Begin();
            static_cast<void>(fixture.input.CommitFrame());
            CHECK(fixture.adapter->Pump(fixture.input.Router(), fixture.context, fixture.graph, fixture.actions, 9).HasError());
            REQUIRE(fixture.graph.BeginRetirement().HasValue());
            CHECK(fixture.adapter->Pump(fixture.input.Router(), fixture.context, fixture.graph, fixture.actions, 11).HasError());
            fixture.adapter->Shutdown();
            fixture.adapter->Shutdown();
            CHECK(fixture.adapter->Pump(fixture.input.Router(), fixture.context, fixture.graph, fixture.actions, 12).Value().status ==
                  UiNavigationInputStatus::Stopped);
            CHECK(fixture.adapter->Presentation().modality == Input::InputModality::Unknown);
        }

        TEST_CASE("Raw unmapped navigation reports unsupported capability and glyph identities remain canonical",
                  "[runtime_ui][navigation][validation]") {
            Fixture fixture;
            auto profile = fixture.input.Router().Profile();
            Input::InputBinding raw;
            raw.kind = Input::BindingControlKind::RawGamepadButton;
            profile.overrides.push_back({fixture.descriptor.actions[6], {raw}});
            REQUIRE(fixture.input.Router().SetProfile(profile).HasValue());
            const auto candidate =
                UiNavigationInput::Create(fixture.descriptor, fixture.input.Router(), fixture.context, fixture.graph, fixture.actions);
            REQUIRE(candidate.HasError());
            CHECK(candidate.ErrorValue().code.Value() == UiErrors::NavigationCapabilityUnsupported.code.Value());
            CHECK(Input::CanonicalGlyph({Input::BindingControlKind::RawGamepadButton, 0}).support == Input::InputGlyphSupport::Unsupported);
            CHECK(Input::CanonicalGlyph({Input::BindingControlKind::GamepadButton, 65000}).support ==
                  Input::InputGlyphSupport::Unsupported);
            CHECK(Input::CanonicalGlyph({Input::BindingControlKind::Key, 0}).support == Input::InputGlyphSupport::Unsupported);
        }

        struct LedgerFixture {
            Input::InputRouter router;
            const Input::InputContextId firstId{"first"};
            const Input::InputContextId secondId{"second"};
            Input::InputContextToken context{router.PushContext(firstId, Input::InputContextKind::FocusedGuiWidget)};
            const Input::ActionId rawAction{"raw"};
            const Input::ActionId combined{"combined"};
            Input::RawInputSnapshot snapshot;

            LedgerFixture() {
                std::vector<Input::InputBinding> bindings;
                for (std::uint16_t index = 0; index < 256; ++index) {
                    auto &binding = bindings.emplace_back();
                    binding.kind = Input::BindingControlKind::RawGamepadButton;
                    binding.rawControl = index;
                }
                auto mixed = bindings;
                Input::InputBinding key;
                key.key = Input::Key::Enter;
                mixed.insert(mixed.begin(), key);
                REQUIRE(router
                            .SetActionMap({{rawAction, Input::ActionValueType::Digital, firstId, true, bindings},
                                           {combined, Input::ActionValueType::Digital, secondId, true, mixed}})
                            .HasValue());
                snapshot.frame = 1;
                snapshot.keyboard[static_cast<std::size_t>(Input::Key::Enter)] = {true, true, false};
                for (std::uint32_t index = 0; index < 17; ++index) {
                    auto &pad = snapshot.gamepads.emplace_back();
                    pad.id = {index, index + 1};
                    pad.rawButtons.resize(256, {true, true, false});
                }
                router.BeginFrame(snapshot);
                for (std::uint8_t player = 0; player < 17; ++player)
                    REQUIRE(router.AssignGamepad(player, snapshot.gamepads[player].id));
            }
        };

        TEST_CASE("Exact bounded Input ledger refuses saturation atomically and releases capacity each frame",
                  "[runtime_ui][navigation][capacity]") {
            LedgerFixture fixture;
            auto &router = fixture.router;
            const auto &context = fixture.context;
            for (std::uint8_t player = 0; player < 16; ++player) {
                const auto before = Tests::AllocationProbe::Count();
                const auto admitted = router.ReadActionEvidence(context, fixture.rawAction, player);
                const auto after = Tests::AllocationProbe::Count();
                CHECK(after == before);
                REQUIRE(admitted.status == Input::ActionReadStatus::Resolved);
                CHECK(admitted.value.pressed);
            }
            const auto refused = router.ReadActionEvidence(context, fixture.rawAction, 16);
            CHECK(refused.status == Input::ActionReadStatus::CapacityExceeded);
            CHECK_FALSE(refused.value.down);
            CHECK(router.LastActionStatus() == Input::ActionReadStatus::CapacityExceeded);
            const auto existing = router.ReadActionEvidence(context, fixture.rawAction, 0);
            CHECK(existing.status == Input::ActionReadStatus::Resolved);
            CHECK(existing.value.down);
            CHECK_FALSE(existing.value.pressed);
            auto next = router.PushContext(fixture.secondId, Input::InputContextKind::FocusedGuiWidget);
            const auto partial = router.ReadActionEvidence(next, fixture.combined, 16);
            CHECK(partial.status == Input::ActionReadStatus::CapacityExceeded);
            CHECK(router.ConsumeKey(next, Input::Key::Enter));
            fixture.snapshot.frame = 2;
            router.BeginFrame(fixture.snapshot);
            const auto reused = router.ReadActionEvidence(next, fixture.combined, 16);
            CHECK(reused.status == Input::ActionReadStatus::Resolved);
            CHECK(reused.value.pressed);
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
