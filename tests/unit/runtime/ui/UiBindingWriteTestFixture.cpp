#include "UiBindingWriteTestFixture.h"

namespace Horo::Runtime::Ui::BindingWriteTests {
    UiOwnershipGeneration Owner() {
        return Take(UiOwnershipGeneration::Create(751));
    }

    UiActionText Text(const std::string_view text) {
        return Take(UiActionText::Create(text));
    }

    UiElementTree Fixture::MakeTree(const std::uint32_t generation) {
        const UiElementTreeDescriptor descriptor{{Owner(), 1, generation},
                                                 {Owner(), 2, generation},
                                                 Stable<UiDocumentId>(1),
                                                 Rev<UiDocumentRevision>(generation),
                                                 Rev<UiRuntimeTreeRevision>(generation),
                                                 {8, 8, 8}};
        std::array<UiElementDescriptor, 7> elements{};
        const auto root = Stable<UiElementId>(1);
        for (std::size_t index = 0; index < elements.size(); ++index)
            elements[index] = {Stable<UiElementId>(static_cast<std::uint8_t>(index + 1)), index == 0 ? UiElementId{} : root};
        return Take(UiElementTree::Create(slots, descriptor, elements));
    }

    UiBindingProviderSchema Fixture::MakeSchema() {
        const auto property = [](const std::string_view name, const UiBindingValueType type,
                                 const UiBindingAccess access = UiBindingAccess::ReadWriteCommand) {
            return UiBindingPropertyDescriptor{.id = Take(UiBindingPropertyId::Parse(name)),
                                               .type = type,
                                               .access = access,
                                               .limits = {.maximumBytes = 64,
                                                          .maximumElements = 1,
                                                          .minimumScalar = 0.0,
                                                          .maximumScalar = 1.0}};
        };
        const std::array properties{property("checked", UiBindingValueType::Boolean), property("label", UiBindingValueType::BoundedText),
                                    property("value", UiBindingValueType::FixedScalar),
                                    property("write", UiBindingValueType::Boolean, UiBindingAccess::WriteCommand)};
        UiBindingProviderDescriptor descriptor{Take(UiBindingProviderTypeId::Parse("game.settings.player")),
                                               ModuleId{"game.settings"},
                                               {1, 0, 0},
                                               properties,
                                               UiBindingProviderScopeBit(UiBindingProviderScopeKind::Player)};
        descriptor.schema.fingerprint = ComputeUiBindingSchemaFingerprint(descriptor);
        return Take(UiBindingProviderSchema::Create(descriptor));
    }

    UiResolvedBindingDescriptor Fixture::Binding(const std::uint8_t id, const std::uint8_t element, const std::uint16_t property,
                                                 const UiBindingTargetProperty target, const UiBindingDirection direction) const {
        const auto &source = schema.Properties()[property];
        return {provider,
                {.id = Stable<UiBindingId>(id),
                 .source = {schema.Type(), source.id, {1, 0}, source.signatureFingerprint},
                 .target = {Stable<UiElementId>(element), target, {.maximumBytes = 64, .minimumScalar = 0.0, .maximumScalar = 1.0}},
                 .direction = direction},
                direction == UiBindingDirection::TargetToSource ? std::optional<UiBindingValue>{false} : std::nullopt};
    }

    UiBindingStore Fixture::Store() const {
        using enum UiBindingTargetProperty;
        const std::array initial{UiBindingPropertyUpdate{0, false}, UiBindingPropertyUpdate{1, std::string{"old"}},
                                 UiBindingPropertyUpdate{2, 0.5}, UiBindingPropertyUpdate{3, true}};
        const std::array registrations{UiBindingProviderRegistration{provider, UiBindingProviderScopeKind::Player, &schema,
                                                                     Rev<UiBindingSnapshotRevision>(), initial}};
        const std::array bindings{Binding(10, 2, 0, BooleanValue),
                                  Binding(11, 4, 1, Text),
                                  Binding(12, 3, 2, ScalarValue),
                                  Binding(13, 6, 0, BooleanValue),
                                  Binding(14, 5, 1, Text, UiBindingDirection::SourceToTarget),
                                  Binding(15, 7, 3, BooleanValue, UiBindingDirection::TargetToSource)};
        return Take(UiBindingStore::Create(tree, registrations, bindings));
    }

    UiLayoutEngine Fixture::Layout(const std::uint32_t capacity) const {
        return Take(
            UiLayoutEngine::Create({tree.Instance(), tree.Canvas(), tree.SourceDocument(), 8, capacity, 3, Rev<UiInteractionRevision>()}));
    }

    UiActionOwnerContext Fixture::Context() const {
        return {tree.Instance(),       tree.Canvas(),
                tree.SourceDocument(), tree.SourceDocumentRevision(),
                tree.Revision(),       Rev<UiInteractionRevision>()};
    }

    UiActionSource Fixture::Source(const std::uint8_t element) const {
        return {Context(), Take(tree.Find(Stable<UiElementId>(element)))};
    }

    UiActionId Fixture::Action() const {
        return Stable<UiActionId>(90);
    }

    std::shared_ptr<TypedAuthority> Fixture::Authority(const std::uint16_t property) const {
        return std::make_shared<TypedAuthority>(UiBindingWriteFence{provider, UiBindingProviderScopeKind::Player, schema.Version(),
                                                                    property, schema.Properties()[property].signatureFingerprint,
                                                                    static_cast<std::uint64_t>(100 + property)},
                                                state);
    }

    UiBindingWriteAdmission Fixture::Admission(const std::uint8_t binding, const std::shared_ptr<TypedAuthority> &authority,
                                               const UiBindingCommitTrigger trigger) const {
        return {Stable<UiBindingId>(binding), Context(), Action(), trigger, UiBindingConflictPolicy::RejectStale, authority};
    }

    void Fixture::Admit(UiBindingStore &store, const std::uint8_t binding, const std::shared_ptr<TypedAuthority> &authority,
                        const UiBindingCommitTrigger trigger) const {
        const std::array admissions{Admission(binding, authority, trigger)};
        REQUIRE(store.AdmitWrites(tree, admissions).HasValue());
    }

    UiActionRouter Fixture::Router(const std::uint32_t capacity) const {
        return Take(UiActionRouter::Create({Context(), capacity}));
    }

    UiActionRequest Fixture::Request(UiActionRouter &router, const std::uint8_t element, const UiActionValue &value) const {
        UiActionPayload payload;
        REQUIRE(payload.Add(value).HasValue());
        REQUIRE(router.Enqueue(Source(element), UiGameplayActionCommand{Action(), payload}).HasValue());
        auto request = Take(router.TryDequeue());
        REQUIRE(request.has_value());
        return std::move(*request);
    }

    UiControlInput Fixture::Input(const UiControlStateMachine &control, const UiControlInputKind kind, const std::uint64_t sequence,
                                  const UiActionText &text) const {
        return {{control.Owner(), control.Element()},
                kind,
                UiControlActivationSource::Keyboard,
                sequence,
                0,
                kind == UiControlInputKind::AdjustPress ? UiControlAdjustment::Increase : UiControlAdjustment::Count,
                text};
    }

    UiActionRequest Fixture::DefaultRequest(UiActionRouter &router, const UiControlStateMachine &control) const {
        const auto preview = Take(control.PeekDefault());
        REQUIRE(preview.has_value());
        REQUIRE(router.Enqueue(preview->source, UiGameplayActionCommand{preview->action, preview->payload}).HasValue());
        auto request = Take(router.TryDequeue());
        REQUIRE(request.has_value());
        return std::move(*request);
    }

    ControlCase DescribeControl(const UiControlKind kind, const bool authored) {
        using enum UiControlKind;
        switch (kind) {
            case Toggle:
                return {kind, 2, 10, 0, UiBindingTargetProperty::BooleanValue, UiBindingCommitTrigger::Change, authored};
            case Slider:
                return {kind, 3, 12, 2, UiBindingTargetProperty::ScalarValue, UiBindingCommitTrigger::Change, authored ? 0.75 : 0.5};
            case TextInput:
                return {kind,
                        4,
                        11,
                        1,
                        UiBindingTargetProperty::Text,
                        UiBindingCommitTrigger::Submit,
                        std::string{authored ? "authored" : "old"}};
            default:
                FAIL("Only value-bearing controls participate in binding write tests");
                return {};
        }
    }

    UiControlStateMachine Fixture::Control(const UiControlKind kind) const {
        return Control(DescribeControl(kind));
    }

    UiControlStateMachine Fixture::Control(const ControlCase &test) const {
        using enum UiControlKind;
        const UiControlDescriptorBase base{Context(), Source(test.element).element, Action()};
        switch (test.kind) {
            case Toggle:
                return Take(UiControlStateMachine::Create(UiToggleControlDescriptor{base, std::get<bool>(test.initial)}));
            case Slider:
                return Take(UiControlStateMachine::Create(UiSliderControlDescriptor{base, 0.0, 1.0, 0.25, std::get<double>(test.initial)}));
            case TextInput:
                return Take(
                    UiControlStateMachine::Create(UiTextInputControlDescriptor{base, Text(std::get<std::string>(test.initial)), 64, true}));
            default:
                FAIL("Unsupported value-bearing control");
                return Control(Toggle);
        }
    }

    void Fixture::StageToggle(UiControlStateMachine &control) const {
        REQUIRE(control.Handle(Input(control, UiControlInputKind::FocusGained, 1)).HasValue());
        REQUIRE(control.Handle(Input(control, UiControlInputKind::SubmitPress, 2)).HasValue());
        REQUIRE(control.Handle(Input(control, UiControlInputKind::SubmitRelease, 3)).HasValue());
    }

    void Fixture::StageDefault(UiControlStateMachine &control, const UiActionText &appended) const {
        if (control.Kind() == UiControlKind::Toggle) {
            StageToggle(control);
            return;
        }
        REQUIRE(control.Handle(Input(control, UiControlInputKind::FocusGained, 1)).HasValue());
        if (control.Kind() == UiControlKind::Slider) {
            REQUIRE(control.Handle(Input(control, UiControlInputKind::AdjustPress, 2)).HasValue());
            return;
        }
        REQUIRE(control.Handle(Input(control, UiControlInputKind::TextInput, 2, appended)).HasValue());
        REQUIRE(control.Handle(Input(control, UiControlInputKind::SubmitPress, 3)).HasValue());
        REQUIRE(control.Handle(Input(control, UiControlInputKind::SubmitRelease, 4)).HasValue());
    }

    UiBindingStore Fixture::WriteOnlyStore(const UiResolvedBindingDescriptor &binding) const {
        const std::array registrations{
            UiBindingProviderRegistration{provider, UiBindingProviderScopeKind::Player, &schema, Rev<UiBindingSnapshotRevision>(), {}}};
        return Take(UiBindingStore::Create(tree, registrations, std::array{binding}));
    }

    WriteSession::WriteSession(const std::uint32_t layoutCapacity, const std::uint32_t routerCapacity)
        : store(Store()), layout(Layout(layoutCapacity)), router(Router(routerCapacity)) {}

    UiBindingWriteResult Fixture::QueueChange(UiBindingStore &store, UiActionRouter &router, const std::uint8_t binding,
                                              const std::uint8_t element, const UiActionValue &value) const {
        const auto edit = Take(store.BeginEdit(tree, Stable<UiBindingId>(binding), Source(element)));
        return Take(store.QueueWrite(tree, edit, Request(router, element, value), UiBindingCommitTrigger::Change));
    }

    UiBindingWriteResult WriteSession::QueueCurrentChange(const std::uint8_t binding, const std::uint8_t element,
                                                          const UiActionValue &value) {
        return Fixture::QueueChange(store, router, binding, element, value);
    }

    UiBindingWriteResult Process(UiBindingStore &store, const UiElementTree &tree, UiLayoutEngine &layout) {
        const auto result = Take(store.ProcessWrite(tree, layout));
        REQUIRE(result.has_value());
        return *result;
    }

    void ClearDirty(UiBindingStore &store) {
        std::array<UiBindingTargetDirty, 8> dirty{};
        REQUIRE(store.DrainDirty(dirty).HasValue());
    }

    void CheckControlValue(const UiControlStateMachine &control, const UiBindingValue &expected) {
        using enum UiControlKind;
        switch (control.Kind()) {
            case Toggle:
                CHECK(std::get<UiToggleControlState>(control.Snapshot().Value()).checked == std::get<bool>(expected));
                break;
            case Slider:
                CHECK(std::get<UiSliderControlState>(control.Snapshot().Value()).value == std::get<double>(expected));
                break;
            case TextInput:
                CHECK(std::get<UiTextInputControlState>(control.Snapshot().Value()).text.View() == std::get<std::string>(expected));
                break;
            default:
                FAIL("Unsupported reconciliation control");
        }
    }

    void CheckCommittedValue(const Fixture &fixture, const UiBindingStore &store, const UiControlKind kind,
                             const UiLayoutContentRevision previous) {
        using enum UiControlKind;
        switch (kind) {
            case Toggle:
                CHECK(std::get<bool>(fixture.state->values[0]));
                CHECK(std::get<bool>(store.Find(fixture.tree, Stable<UiBindingId>(13))->value));
                break;
            case Slider:
                CHECK(std::get<double>(fixture.state->values[2]) == 0.75);
                CHECK(store.Current().content == previous);
                break;
            case TextInput:
                CHECK(std::get<UiActionText>(fixture.state->values[1]).View() == "oldnew");
                CHECK(std::get<std::string>(store.Find(fixture.tree, Stable<UiBindingId>(14))->value) == "oldnew");
                CHECK(store.Current().content != previous);
                break;
            default:
                FAIL("Unsupported committed control");
        }
    }

    void CommitFreshPresentation(WriteSession &fixture, const UiActionOwnerContext &owner, const UiActionSource &source) {
        auto freshSource = source;
        freshSource.owner = owner;
        auto freshRouter = Take(UiActionRouter::Create({owner, 4, fixture.router.LastIssuedSequence()}));
        const auto freshEdit = Take(fixture.store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), freshSource));
        UiActionPayload payload;
        REQUIRE(payload.Add(true).HasValue());
        REQUIRE(freshRouter.Enqueue(freshSource, UiGameplayActionCommand{fixture.Action(), payload}).HasValue());
        UiBindingWriteActionHandler handler{fixture.store, fixture.tree, freshEdit, UiBindingCommitTrigger::Change};
        REQUIRE(freshRouter.DispatchNext(handler).HasValue());
        CHECK(Process(fixture.store, fixture.tree, fixture.layout).disposition == UiBindingWriteDisposition::Ready);
        CHECK(fixture.state->commits == 1);
    }
}  // namespace Horo::Runtime::Ui::BindingWriteTests
