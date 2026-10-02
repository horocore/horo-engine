#include "Horo/Runtime/Ui/UiBindingStore.h"
#include "Horo/Runtime/Ui/UiErrors.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <utility>

namespace {
    std::atomic<std::size_t> &WriteAllocations() noexcept {
        static std::atomic<std::size_t> allocations{};
        return allocations;
    }
}  // namespace

void *operator new(const std::size_t bytes) {
    WriteAllocations().fetch_add(1, std::memory_order_relaxed);
    void *const memory = std::malloc(bytes == 0 ? 1 : bytes);
    return memory ? memory : throw std::bad_alloc{};
}

void operator delete(void *memory) noexcept {
    std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
    std::free(memory);
}

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T> T Take(Result<T> result) {
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        template <typename Id> Id Stable(const std::uint8_t marker) {
            SerializedUiId bytes{};
            bytes.back() = marker;
            return Take(Id::Create(bytes));
        }

        template <typename Revision> Revision Rev(const std::uint64_t value = 1) {
            return Take(Revision::Create(value));
        }

        UiOwnershipGeneration Owner() {
            return Take(UiOwnershipGeneration::Create(751));
        }

        UiActionText Text(const std::string_view text) {
            return Take(UiActionText::Create(text));
        }

        template <typename T> void ErrorIs(const Result<T> &result, const ErrorCodeDescriptor &error) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == error.code.Value());
        }

        struct ProviderState final {
            UiBindingSnapshotRevision revision{Rev<UiBindingSnapshotRevision>()};
            std::array<UiActionValue, 4> values{false, Text("old"), 0.5, true};
            std::size_t prepares{};
            std::size_t commits{};
            std::size_t abandons{};
            std::size_t destroyed{};
            bool commitFenceValid{true};
        };

        class TypedAuthority final : public UiBindingWriteAuthority {
        public:
            enum class Reentry {
                None,
                Retirement,
                Shutdown
            };

            UiBindingWriteFence fence;
            std::shared_ptr<ProviderState> state;
            UiBindingWriteDisposition disposition{UiBindingWriteDisposition::Ready};
            std::optional<Error> failure;
            std::optional<UiBindingWriteCommand> reservation;
            bool active{true};
            UiBindingStore *reentrantStore{};
            Reentry prepareReentry{Reentry::None};
            bool abandonRetires{};

            TypedAuthority(UiBindingWriteFence permission, std::shared_ptr<ProviderState> owner)
                : fence(permission), state(std::move(owner)) {}

            ~TypedAuthority() override {
                ++state->destroyed;
            }

            const UiBindingWriteFence &Fence() const noexcept override {
                return fence;
            }

            bool Active() const noexcept override {
                return active;
            }

            Result<UiBindingWriteDisposition> Prepare(const UiBindingWriteCommand &command) override {
                ++state->prepares;
                if (!active || command.fence != fence || command.expected != state->revision ||
                    command.fence.property >= state->values.size())
                    return Result<UiBindingWriteDisposition>::Failure(MakeError(UiErrors::RevisionStale));
                const auto &current = state->values[command.fence.property];
                if (command.value.index() != current.index())
                    return Result<UiBindingWriteDisposition>::Failure(MakeError(UiErrors::BindingTypeMismatch));
                if (const auto *scalar = std::get_if<double>(&command.value);
                    scalar && (!std::isfinite(*scalar) || *scalar < 0.0 || *scalar > 1.0))
                    return Result<UiBindingWriteDisposition>::Failure(MakeError(UiErrors::BindingValueInvalid));
                if (failure)
                    return Result<UiBindingWriteDisposition>::Failure(*failure);
                reservation = command;
                if (reentrantStore && prepareReentry == Reentry::Retirement)
                    reentrantStore->BeginRetirement();
                if (reentrantStore && prepareReentry == Reentry::Shutdown)
                    reentrantStore->Shutdown();
                return Result<UiBindingWriteDisposition>::Success(disposition);
            }

            void Commit(const UiBindingWriteCommand &command) noexcept override {
                if (!active || !reservation || command.fence != fence || command.expected != state->revision ||
                    reservation->request != command.request || reservation->operation != command.operation ||
                    reservation->source != command.source) {
                    state->commitFenceValid = false;
                    return;
                }
                state->values[command.fence.property] = command.value;
                state->revision = command.expected.Next().Value();
                ++state->commits;
                reservation.reset();
            }

            void Abandon(const UiBindingWriteCommand &) noexcept override {
                ++state->abandons;
                reservation.reset();
                if (reentrantStore && abandonRetires)
                    reentrantStore->BeginRetirement();
            }

            void Revoke() noexcept {
                active = false;
                reservation.reset();
            }
        };

        struct Fixture final {
            UiElementSlotAllocator slots{Take(UiElementSlotAllocator::Create(Owner()))};
            UiElementTree tree{MakeTree()};
            UiBindingProviderSchema schema{MakeSchema()};
            UiBindingProviderInstanceId provider{Owner(), 9, 1};
            std::shared_ptr<ProviderState> state{std::make_shared<ProviderState>()};

            UiElementTree MakeTree(const std::uint32_t generation = 1) {
                const UiElementTreeDescriptor descriptor{{Owner(), 1, generation},
                                                         {Owner(), 2, generation},
                                                         Stable<UiDocumentId>(1),
                                                         Rev<UiDocumentRevision>(generation),
                                                         Rev<UiRuntimeTreeRevision>(generation),
                                                         {8, 8, 8}};
                const std::array elements{
                    UiElementDescriptor{Stable<UiElementId>(1), {}},
                    UiElementDescriptor{Stable<UiElementId>(2), Stable<UiElementId>(1)},
                    UiElementDescriptor{Stable<UiElementId>(3), Stable<UiElementId>(1)},
                    UiElementDescriptor{Stable<UiElementId>(4), Stable<UiElementId>(1)},
                    UiElementDescriptor{Stable<UiElementId>(5), Stable<UiElementId>(1)},
                    UiElementDescriptor{Stable<UiElementId>(6), Stable<UiElementId>(1)},
                    UiElementDescriptor{Stable<UiElementId>(7), Stable<UiElementId>(1)},
                };
                return Take(UiElementTree::Create(slots, descriptor, elements));
            }

            static UiBindingProviderSchema MakeSchema() {
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
                const std::array properties{property("checked", UiBindingValueType::Boolean),
                                            property("label", UiBindingValueType::BoundedText),
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

            UiResolvedBindingDescriptor Binding(const std::uint8_t id, const std::uint8_t element, const std::uint16_t property,
                                                const UiBindingTargetProperty target,
                                                const UiBindingDirection direction = UiBindingDirection::TwoWay) const {
                const auto &source = schema.Properties()[property];
                return {provider,
                        {.id = Stable<UiBindingId>(id),
                         .source = {schema.Type(), source.id, {1, 0}, source.signatureFingerprint},
                         .target = {Stable<UiElementId>(element), target, {.maximumBytes = 64, .minimumScalar = 0.0, .maximumScalar = 1.0}},
                         .direction = direction}};
            }

            UiBindingStore Store() const {
                const std::array initial{UiBindingPropertyUpdate{0, false}, UiBindingPropertyUpdate{1, std::string{"old"}},
                                         UiBindingPropertyUpdate{2, 0.5}, UiBindingPropertyUpdate{3, true}};
                const std::array registrations{UiBindingProviderRegistration{provider, UiBindingProviderScopeKind::Player, &schema,
                                                                             Rev<UiBindingSnapshotRevision>(), initial}};
                const std::array bindings{Binding(10, 2, 0, UiBindingTargetProperty::BooleanValue),
                                          Binding(11, 4, 1, UiBindingTargetProperty::Text),
                                          Binding(12, 3, 2, UiBindingTargetProperty::ScalarValue),
                                          Binding(13, 6, 0, UiBindingTargetProperty::BooleanValue),
                                          Binding(14, 5, 1, UiBindingTargetProperty::Text, UiBindingDirection::SourceToTarget),
                                          Binding(15, 7, 3, UiBindingTargetProperty::BooleanValue, UiBindingDirection::TargetToSource)};
                return Take(UiBindingStore::Create(tree, registrations, bindings));
            }

            UiLayoutEngine Layout(const std::uint32_t capacity = 8) const {
                return Take(UiLayoutEngine::Create(
                    {tree.Instance(), tree.Canvas(), tree.SourceDocument(), 8, capacity, 3, Rev<UiInteractionRevision>()}));
            }

            UiActionOwnerContext Context() const {
                return {tree.Instance(),       tree.Canvas(),
                        tree.SourceDocument(), tree.SourceDocumentRevision(),
                        tree.Revision(),       Rev<UiInteractionRevision>()};
            }

            UiActionSource Source(const std::uint8_t element) const {
                return {Context(), Take(tree.Find(Stable<UiElementId>(element)))};
            }

            UiActionId Action() const {
                return Stable<UiActionId>(90);
            }

            std::shared_ptr<TypedAuthority> Authority(const std::uint16_t property) const {
                return std::make_shared<TypedAuthority>(UiBindingWriteFence{provider, UiBindingProviderScopeKind::Player, schema.Version(),
                                                                            property, schema.Properties()[property].signatureFingerprint,
                                                                            static_cast<std::uint64_t>(100 + property)},
                                                        state);
            }

            UiBindingWriteAdmission Admission(const std::uint8_t binding, const std::shared_ptr<TypedAuthority> &authority,
                                              const UiBindingCommitTrigger trigger = UiBindingCommitTrigger::Change) const {
                return {Stable<UiBindingId>(binding), Context(), Action(), trigger, UiBindingConflictPolicy::RejectStale, authority};
            }

            void Admit(UiBindingStore &store, const std::uint8_t binding, const std::shared_ptr<TypedAuthority> &authority,
                       const UiBindingCommitTrigger trigger = UiBindingCommitTrigger::Change) const {
                const std::array admissions{Admission(binding, authority, trigger)};
                REQUIRE(store.AdmitWrites(tree, admissions).HasValue());
            }

            UiActionRouter Router(const std::uint32_t capacity = 4) const {
                return Take(UiActionRouter::Create({Context(), capacity}));
            }

            UiActionRequest Request(UiActionRouter &router, const std::uint8_t element, const UiActionValue &value) const {
                UiActionPayload payload;
                REQUIRE(payload.Add(value).HasValue());
                REQUIRE(router.Enqueue(Source(element), UiGameplayActionCommand{Action(), payload}).HasValue());
                auto request = Take(router.TryDequeue());
                REQUIRE(request.has_value());
                return std::move(*request);
            }

            UiControlStateMachine Control(const UiControlKind kind) const {
                const std::uint8_t element = kind == UiControlKind::Toggle ? 2 : kind == UiControlKind::Slider ? 3 : 4;
                UiControlDescriptorBase base{Context(), Source(element).element, Action()};
                if (kind == UiControlKind::Toggle)
                    return Take(UiControlStateMachine::Create(UiToggleControlDescriptor{base, false}));
                if (kind == UiControlKind::Slider)
                    return Take(UiControlStateMachine::Create(UiSliderControlDescriptor{base, 0.0, 1.0, 0.25, 0.5}));
                return Take(UiControlStateMachine::Create(UiTextInputControlDescriptor{base, Text("old"), 64, true}));
            }

            UiControlInput Input(const UiControlStateMachine &control, const UiControlInputKind kind, const std::uint64_t sequence,
                                 const UiActionText text = {}) const {
                return {{control.Owner(), control.Element()},
                        kind,
                        UiControlActivationSource::Keyboard,
                        sequence,
                        0,
                        kind == UiControlInputKind::AdjustPress ? UiControlAdjustment::Increase : UiControlAdjustment::Count,
                        text};
            }

            UiActionRequest DefaultRequest(UiActionRouter &router, UiControlStateMachine &control) const {
                const auto preview = Take(control.PeekDefault());
                REQUIRE(preview.has_value());
                REQUIRE(router.Enqueue(preview->source, UiGameplayActionCommand{preview->action, preview->payload}).HasValue());
                auto request = Take(router.TryDequeue());
                REQUIRE(request.has_value());
                return std::move(*request);
            }
        };

        UiBindingWriteResult Process(UiBindingStore &store, const UiElementTree &tree, UiLayoutEngine &layout) {
            const auto result = Take(store.ProcessWrite(tree, layout));
            REQUIRE(result.has_value());
            return *result;
        }

        void ClearDirty(UiBindingStore &store) {
            std::array<UiBindingTargetDirty, 8> dirty{};
            REQUIRE(store.DrainDirty(dirty).HasValue());
        }

        TEST_CASE("Admitted control defaults commit typed provider state through the versioned binding owner",
                  "[runtime_ui][binding][write][controls]") {
            const auto kind = GENERATE(UiControlKind::Toggle, UiControlKind::Slider, UiControlKind::TextInput);
            Fixture fixture;
            auto store = fixture.Store();
            auto layout = fixture.Layout();
            auto router = fixture.Router();
            auto control = fixture.Control(kind);
            const std::uint8_t binding = kind == UiControlKind::Toggle ? 10 : kind == UiControlKind::Slider ? 12 : 11;
            const std::uint16_t property = kind == UiControlKind::Toggle ? 0 : kind == UiControlKind::Slider ? 2 : 1;
            const auto trigger = kind == UiControlKind::TextInput ? UiBindingCommitTrigger::Submit : UiBindingCommitTrigger::Change;
            const auto authority = fixture.Authority(property);
            fixture.Admit(store, binding, authority, trigger);
            CHECK(fixture.state->prepares == 0);
            ClearDirty(store);
            const auto edit = Take(store.BeginEdit(fixture.tree, Stable<UiBindingId>(binding), {control.Owner(), control.Element()}));
            REQUIRE(control.Handle(fixture.Input(control, UiControlInputKind::FocusGained, 1)).HasValue());
            if (kind == UiControlKind::Slider) {
                REQUIRE(control.Handle(fixture.Input(control, UiControlInputKind::AdjustPress, 2)).HasValue());
            } else {
                if (kind == UiControlKind::TextInput)
                    REQUIRE(control.Handle(fixture.Input(control, UiControlInputKind::TextInput, 2, Text("new"))).HasValue());
                REQUIRE(control.Handle(fixture.Input(control, UiControlInputKind::SubmitPress, 3)).HasValue());
                REQUIRE(control.Handle(fixture.Input(control, UiControlInputKind::SubmitRelease, 4)).HasValue());
            }
            const auto request = fixture.DefaultRequest(router, control);
            const auto current = store.Current();
            UiBindingWriteActionHandler handler{store, fixture.tree, edit, control};
            const auto queued = Take(router.Dispatch(request, handler));
            CHECK(queued.kind == UiActionResultKind::Pending);
            CHECK(queued.request == request.id);
            CHECK(fixture.state->commits == 0);
            CHECK(store.Current().revision == current.revision);
            ErrorIs(store.ReconcileControl(fixture.tree, Stable<UiBindingId>(binding), control), UiErrors::BindingDescriptorConflict);
            const auto committed = Process(store, fixture.tree, layout);
            CHECK(committed.disposition == UiBindingWriteDisposition::Ready);
            CHECK(committed.operation == queued.operation);
            CHECK(fixture.state->revision.Value() == 2);
            CHECK(fixture.state->commits == 1);
            CHECK(fixture.state->commitFenceValid);
            REQUIRE(store.ReconcileControl(fixture.tree, Stable<UiBindingId>(binding), control).HasValue());
            if (kind == UiControlKind::Toggle) {
                CHECK(std::get<bool>(fixture.state->values[0]));
                CHECK(std::get<bool>(store.Find(fixture.tree, Stable<UiBindingId>(13))->value));
            } else if (kind == UiControlKind::Slider) {
                CHECK(std::get<double>(fixture.state->values[2]) == 0.75);
                CHECK(store.Current().content == current.content);
            } else {
                CHECK(std::get<UiActionText>(fixture.state->values[1]).View() == "oldnew");
                CHECK(std::get<std::string>(store.Find(fixture.tree, Stable<UiBindingId>(14))->value) == "oldnew");
                CHECK(store.Current().content != current.content);
            }
            CHECK_FALSE(Take(store.ProcessWrite(fixture.tree, layout)).has_value());
            std::array<UiBindingWriteResult, 1> terminal{};
            CHECK(Take(store.DrainWriteResults(terminal)) == 1);
            CHECK(Take(store.DrainWriteResults(terminal)) == 0);
            CHECK(fixture.state->abandons == 0);
        }

        TEST_CASE("Production action dispatch suppresses a refused control write and queue backpressure preserves the staged default",
                  "[runtime_ui][binding][write][routing]") {
            Fixture fixture;
            auto store = fixture.Store();
            auto router = fixture.Router(1);
            auto layout = fixture.Layout();
            auto control = fixture.Control(UiControlKind::Toggle);
            fixture.Admit(store, 10, fixture.Authority(0), UiBindingCommitTrigger::Submit);
            const auto edit = Take(store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)));
            REQUIRE(control.Handle(fixture.Input(control, UiControlInputKind::FocusGained, 1)).HasValue());
            REQUIRE(control.Handle(fixture.Input(control, UiControlInputKind::SubmitPress, 2)).HasValue());
            REQUIRE(control.Handle(fixture.Input(control, UiControlInputKind::SubmitRelease, 3)).HasValue());
            const auto preview = Take(control.PeekDefault());
            REQUIRE(preview);
            SECTION("typed provider policy refusal suppresses default through the router") {
                REQUIRE(router.Enqueue(preview->source, UiGameplayActionCommand{preview->action, preview->payload}).HasValue());
                UiBindingWriteActionHandler handler{store, fixture.tree, edit, control};
                ErrorIs(router.DispatchNext(handler), UiErrors::BindingAccessInvalid);
                CHECK_FALSE(Take(control.PeekDefault()).has_value());
            }
            SECTION("action queue capacity failure cannot mutate control or binding values") {
                REQUIRE(router.Enqueue(fixture.Source(3), UiGameplayActionCommand{fixture.Action(), {}}).HasValue());
                ErrorIs(router.Enqueue(preview->source, UiGameplayActionCommand{preview->action, preview->payload}),
                        UiErrors::ActionQueueCapacityExceeded);
                CHECK(Take(control.PeekDefault()).has_value());
                REQUIRE(control.SuppressDefault().HasValue());
            }
            CHECK_FALSE(std::get<UiToggleControlState>(control.Snapshot().Value()).checked);
            CHECK_FALSE(std::get<bool>(store.Find(fixture.tree, Stable<UiBindingId>(10))->value));
            CHECK_FALSE(Take(store.ProcessWrite(fixture.tree, layout)).has_value());
            CHECK(fixture.state->prepares == 0);
            REQUIRE(store.CancelEdit(edit).HasValue());
            REQUIRE(store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)).HasValue());
        }

        TEST_CASE("Malformed routed control requests always suppress the staged default before returning failure",
                  "[runtime_ui][binding][write][routing]") {
            Fixture fixture;
            auto store = fixture.Store();
            auto router = fixture.Router();
            auto layout = fixture.Layout();
            auto control = fixture.Control(UiControlKind::Toggle);
            fixture.Admit(store, 10, fixture.Authority(0));
            const auto edit = Take(store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)));
            REQUIRE(control.Handle(fixture.Input(control, UiControlInputKind::FocusGained, 1)).HasValue());
            REQUIRE(control.Handle(fixture.Input(control, UiControlInputKind::SubmitPress, 2)).HasValue());
            REQUIRE(control.Handle(fixture.Input(control, UiControlInputKind::SubmitRelease, 3)).HasValue());
            auto request = fixture.DefaultRequest(router, control);
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
            UiBindingWriteActionHandler handler{store, fixture.tree, edit, control};
            REQUIRE(router.Dispatch(request, handler).HasError());
            CHECK_FALSE(Take(control.PeekDefault()).has_value());
            CHECK_FALSE(std::get<UiToggleControlState>(control.Snapshot().Value()).checked);
            CHECK_FALSE(std::get<bool>(store.Find(fixture.tree, Stable<UiBindingId>(10))->value));
            CHECK_FALSE(Take(store.ProcessWrite(fixture.tree, layout)).has_value());
            CHECK(fixture.state->prepares == 0);
            REQUIRE(store.CancelEdit(edit).HasValue());
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

        TEST_CASE("Edit and command fences reject recycled UI and changed provider evidence", "[runtime_ui][binding][write][stale]") {
            Fixture fixture;
            auto store = fixture.Store();
            auto layout = fixture.Layout();
            auto router = fixture.Router();
            const auto authority = fixture.Authority(0);
            fixture.Admit(store, 10, authority);
            auto stale = fixture.Source(2);
            ++stale.element.generation;
            ErrorIs(store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), stale), UiErrors::RevisionStale);
            const auto edit = Take(store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)));
            const auto request = fixture.Request(router, 2, true);
            SECTION("read publication before queue") {
                const std::array updates{UiBindingPropertyUpdate{0, true}};
                const std::array batches{UiBindingChangeBatch{fixture.provider, fixture.schema.Version(), Rev<UiBindingSnapshotRevision>(),
                                                              Rev<UiBindingSnapshotRevision>(2), updates}};
                REQUIRE(store.Apply(fixture.tree, batches, layout).HasValue());
                ErrorIs(store.QueueWrite(fixture.tree, edit, request, UiBindingCommitTrigger::Change), UiErrors::RevisionStale);
            }
            SECTION("provider advances after queue before prepare") {
                REQUIRE(store.QueueWrite(fixture.tree, edit, request, UiBindingCommitTrigger::Change).HasValue());
                fixture.state->revision = Rev<UiBindingSnapshotRevision>(2);
                const auto result = Process(store, fixture.tree, layout);
                CHECK(result.disposition == UiBindingWriteDisposition::Rejected);
                REQUIRE(result.error);
                CHECK(result.error->code.Value() == UiErrors::RevisionStale.code.Value());
                CHECK_FALSE(std::get<bool>(store.Find(fixture.tree, Stable<UiBindingId>(10))->value));
            }
            SECTION("document replacement") {
                REQUIRE(store.QueueWrite(fixture.tree, edit, request, UiBindingCommitTrigger::Change).HasValue());
                auto replacement = fixture.MakeTree(2);
                const auto result = Process(store, replacement, layout);
                CHECK(result.disposition == UiBindingWriteDisposition::Rejected);
            }
            SECTION("provider permission replacement") {
                REQUIRE(store.QueueWrite(fixture.tree, edit, request, UiBindingCommitTrigger::Change).HasValue());
                ++authority->fence.capability;
                CHECK(Process(store, fixture.tree, layout).disposition == UiBindingWriteDisposition::Rejected);
                CHECK(fixture.state->prepares == 0);
            }
            CHECK(fixture.state->commits == 0);
        }

        TEST_CASE("Provider callback reentry cannot retire or free the active write transaction",
                  "[runtime_ui][binding][write][reentrancy]") {
            Fixture fixture;
            auto store = fixture.Store();
            auto router = fixture.Router();
            auto layout = fixture.Layout();
            auto authority = fixture.Authority(0);
            authority->reentrantStore = &store;
            SECTION("Prepare attempts retirement") {
                authority->prepareReentry = TypedAuthority::Reentry::Retirement;
            }
            SECTION("Prepare attempts shutdown") {
                authority->prepareReentry = TypedAuthority::Reentry::Shutdown;
            }
            fixture.Admit(store, 10, authority);
            const auto edit = Take(store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)));
            REQUIRE(store.QueueWrite(fixture.tree, edit, fixture.Request(router, 2, true), UiBindingCommitTrigger::Change).HasValue());
            const auto before = store.Current();
            const auto outcome = Process(store, fixture.tree, layout);
            CHECK(outcome.disposition == UiBindingWriteDisposition::Rejected);
            REQUIRE(outcome.error);
            CHECK(outcome.error->code.Value() == UiErrors::ActionHandlerFailed.code.Value());
            CHECK(fixture.state->commits == 0);
            CHECK(fixture.state->abandons == 1);
            CHECK(store.Current().revision == before.revision);
            const auto *target = store.Find(fixture.tree, Stable<UiBindingId>(10));
            REQUIRE(target);
            CHECK_FALSE(std::get<bool>(target->value));
            CHECK_FALSE(Take(store.ProcessWrite(fixture.tree, layout)).has_value());
            authority->reentrantStore = nullptr;
        }

        TEST_CASE("Abandon callback retirement cannot recursively cancel one operation", "[runtime_ui][binding][write][reentrancy]") {
            Fixture fixture;
            auto store = fixture.Store();
            auto router = fixture.Router();
            auto layout = fixture.Layout();
            auto authority = fixture.Authority(0);
            authority->disposition = UiBindingWriteDisposition::Pending;
            authority->reentrantStore = &store;
            authority->abandonRetires = true;
            fixture.Admit(store, 10, authority);
            const auto edit = Take(store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)));
            const auto queued =
                Take(store.QueueWrite(fixture.tree, edit, fixture.Request(router, 2, true), UiBindingCommitTrigger::Change));
            CHECK(Process(store, fixture.tree, layout).disposition == UiBindingWriteDisposition::Pending);
            store.BeginRetirement();
            CHECK(fixture.state->abandons == 1);
            CHECK(fixture.state->commits == 0);
            std::array<UiBindingWriteResult, 1> outcomes{};
            CHECK(Take(store.DrainWriteResults(outcomes)) == 1);
            CHECK(outcomes[0].request == queued.request);
            CHECK(outcomes[0].operation == queued.operation);
            CHECK(outcomes[0].disposition == UiBindingWriteDisposition::Cancelled);
            CHECK(outcomes[0].cancellation == UiBindingWriteCancellationReason::OwnerRetired);
            CHECK(Take(store.DrainWriteResults(outcomes)) == 0);
            CHECK_FALSE(Take(store.ProcessWrite(fixture.tree, layout)).has_value());
            authority->reentrantStore = nullptr;
        }

        TEST_CASE("A pending binding does not starve another admitted binding", "[runtime_ui][binding][write][fairness]") {
            Fixture fixture;
            auto store = fixture.Store();
            auto router = fixture.Router();
            auto layout = fixture.Layout();
            auto pending = fixture.Authority(0);
            pending->disposition = UiBindingWriteDisposition::Pending;
            auto ready = fixture.Authority(2);
            const std::array admissions{fixture.Admission(10, pending), fixture.Admission(12, ready)};
            REQUIRE(store.AdmitWrites(fixture.tree, admissions).HasValue());
            const auto first = Take(store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)));
            const auto second = Take(store.BeginEdit(fixture.tree, Stable<UiBindingId>(12), fixture.Source(3)));
            const auto firstResult =
                Take(store.QueueWrite(fixture.tree, first, fixture.Request(router, 2, true), UiBindingCommitTrigger::Change));
            const auto secondResult =
                Take(store.QueueWrite(fixture.tree, second, fixture.Request(router, 3, 0.75), UiBindingCommitTrigger::Change));
            const auto pendingOutcome = Process(store, fixture.tree, layout);
            CHECK(pendingOutcome.request == firstResult.request);
            CHECK(pendingOutcome.disposition == UiBindingWriteDisposition::Pending);
            const auto readyOutcome = Process(store, fixture.tree, layout);
            CHECK(readyOutcome.request == secondResult.request);
            CHECK(readyOutcome.disposition == UiBindingWriteDisposition::Ready);
            CHECK(fixture.state->commits == 1);
            CHECK(std::get<double>(fixture.state->values[2]) == 0.75);
            const auto conflict = Process(store, fixture.tree, layout);
            CHECK(conflict.request == firstResult.request);
            CHECK(conflict.disposition == UiBindingWriteDisposition::Rejected);
            REQUIRE(conflict.error);
            CHECK(conflict.error->code.Value() == UiErrors::RevisionStale.code.Value());
            CHECK(fixture.state->prepares == 2);
            CHECK(fixture.state->abandons == 1);
        }

        TEST_CASE("Concurrent same-revision edits have deterministic first-commit conflict rejection",
                  "[runtime_ui][binding][write][conflict]") {
            Fixture fixture;
            auto store = fixture.Store();
            auto layout = fixture.Layout();
            auto router = fixture.Router();
            const auto authority = fixture.Authority(0);
            const std::array admissions{fixture.Admission(10, authority), fixture.Admission(13, authority)};
            REQUIRE(store.AdmitWrites(fixture.tree, admissions).HasValue());
            const auto first = Take(store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)));
            const auto second = Take(store.BeginEdit(fixture.tree, Stable<UiBindingId>(13), fixture.Source(6)));
            REQUIRE(store.QueueWrite(fixture.tree, first, fixture.Request(router, 2, true), UiBindingCommitTrigger::Change).HasValue());
            REQUIRE(store.QueueWrite(fixture.tree, second, fixture.Request(router, 6, false), UiBindingCommitTrigger::Change).HasValue());
            CHECK(Process(store, fixture.tree, layout).disposition == UiBindingWriteDisposition::Ready);
            const auto rejected = Process(store, fixture.tree, layout);
            CHECK(rejected.disposition == UiBindingWriteDisposition::Rejected);
            REQUIRE(rejected.error);
            CHECK(rejected.error->code.Value() == UiErrors::RevisionStale.code.Value());
            CHECK(fixture.state->commits == 1);
            CHECK(fixture.state->prepares == 1);
            CHECK(std::get<bool>(fixture.state->values[0]));
            std::array<UiBindingWriteResult, 1> tooSmall{};
            ErrorIs(store.DrainWriteResults(tooSmall), UiErrors::BindingCapacityExceeded);
            std::array<UiBindingWriteResult, 2> outcomes{};
            CHECK(Take(store.DrainWriteResults(outcomes)) == 2);
            CHECK_FALSE(Take(store.ProcessWrite(fixture.tree, layout)).has_value());
        }

        TEST_CASE("Typed drafts and commit triggers validate before provider preparation", "[runtime_ui][binding][write][validation]") {
            Fixture fixture;
            auto store = fixture.Store();
            auto router = fixture.Router();
            auto layout = fixture.Layout();
            SECTION("scalar wrong type and outside bounds preserve edit for correction") {
                fixture.Admit(store, 12, fixture.Authority(2));
                const auto edit = Take(store.BeginEdit(fixture.tree, Stable<UiBindingId>(12), fixture.Source(3)));
                ErrorIs(store.QueueWrite(fixture.tree, edit, fixture.Request(router, 3, true), UiBindingCommitTrigger::Change),
                        UiErrors::BindingTypeMismatch);
                ErrorIs(store.QueueWrite(fixture.tree, edit, fixture.Request(router, 3, 1.25), UiBindingCommitTrigger::Change),
                        UiErrors::BindingValueInvalid);
                REQUIRE(store.QueueWrite(fixture.tree, edit, fixture.Request(router, 3, 0.25), UiBindingCommitTrigger::Change).HasValue());
                CHECK(Process(store, fixture.tree, layout).disposition == UiBindingWriteDisposition::Ready);
            }
            SECTION("oversized text preserves prior value") {
                fixture.Admit(store, 11, fixture.Authority(1), UiBindingCommitTrigger::Submit);
                const auto edit = Take(store.BeginEdit(fixture.tree, Stable<UiBindingId>(11), fixture.Source(4)));
                ErrorIs(store.QueueWrite(fixture.tree, edit, fixture.Request(router, 4, Text(std::string(65, 'x'))),
                                         UiBindingCommitTrigger::Submit),
                        UiErrors::BindingValueInvalid);
                CHECK(std::get<std::string>(store.Find(fixture.tree, Stable<UiBindingId>(11))->value) == "old");
                CHECK(fixture.state->prepares == 0);
            }
            SECTION("malformed action is rejected at the action boundary") {
                UiActionPayload payload;
                UiActionText malformed;
                malformed.bytes[0] = static_cast<char>(0xff);
                malformed.size = 1;
                ErrorIs(payload.Add(malformed), UiErrors::ActionPayloadInvalid);
                ErrorIs(payload.Add(std::numeric_limits<double>::infinity()), UiErrors::ActionPayloadInvalid);
                fixture.Admit(store, 10, fixture.Authority(0));
                const auto edit = Take(store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)));
                auto request = fixture.Request(router, 2, true);
                request.origin = UiActionOrigin::Form;
                ErrorIs(store.QueueWrite(fixture.tree, edit, request, UiBindingCommitTrigger::Change), UiErrors::ActionCommandInvalid);
            }
            SECTION("blur requires its exact explicit trigger") {
                fixture.Admit(store, 10, fixture.Authority(0), UiBindingCommitTrigger::Blur);
                const auto edit = Take(store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)));
                const auto request = fixture.Request(router, 2, true);
                ErrorIs(store.QueueWrite(fixture.tree, edit, request, UiBindingCommitTrigger::Change), UiErrors::BindingAccessInvalid);
                ErrorIs(store.QueueWrite(fixture.tree, edit, request, UiBindingCommitTrigger::Submit), UiErrors::BindingAccessInvalid);
                UiBindingWriteActionHandler handler{store, fixture.tree, edit, UiBindingCommitTrigger::Blur};
                CHECK(Take(router.Dispatch(request, handler)).kind == UiActionResultKind::Pending);
                CHECK(Process(store, fixture.tree, layout).disposition == UiBindingWriteDisposition::Ready);
            }
        }

        TEST_CASE("Write-only properties do not poll or subscribe to provider read deltas", "[runtime_ui][binding][write][direction]") {
            Fixture fixture;
            auto store = fixture.Store();
            auto layout = fixture.Layout();
            auto router = fixture.Router();
            fixture.Admit(store, 15, fixture.Authority(3));
            CHECK_FALSE(std::get<bool>(store.Find(fixture.tree, Stable<UiBindingId>(15))->value));
            const std::array changes{UiBindingPropertyUpdate{3, true}};
            const std::array batches{UiBindingChangeBatch{fixture.provider, fixture.schema.Version(), Rev<UiBindingSnapshotRevision>(),
                                                          Rev<UiBindingSnapshotRevision>(2), changes}};
            REQUIRE(store.Apply(fixture.tree, batches, layout).HasValue());
            CHECK_FALSE(std::get<bool>(store.Find(fixture.tree, Stable<UiBindingId>(15))->value));
            fixture.state->revision = Rev<UiBindingSnapshotRevision>(2);
            const auto edit = Take(store.BeginEdit(fixture.tree, Stable<UiBindingId>(15), fixture.Source(7)));
            REQUIRE(store.QueueWrite(fixture.tree, edit, fixture.Request(router, 7, true), UiBindingCommitTrigger::Change).HasValue());
            CHECK(Process(store, fixture.tree, layout).disposition == UiBindingWriteDisposition::Ready);
            CHECK(std::get<bool>(store.Find(fixture.tree, Stable<UiBindingId>(15))->value));
            CHECK(fixture.state->revision.Value() == 3);
        }

        TEST_CASE("Layout backpressure rejects a prepared text write without publishing any owner",
                  "[runtime_ui][binding][write][atomic]") {
            Fixture fixture;
            auto store = fixture.Store();
            auto layout = fixture.Layout(1);
            auto router = fixture.Router();
            auto control = fixture.Control(UiControlKind::TextInput);
            fixture.Admit(store, 11, fixture.Authority(1), UiBindingCommitTrigger::Submit);
            ClearDirty(store);
            const auto before = store.Current();
            const auto edit = Take(store.BeginEdit(fixture.tree, Stable<UiBindingId>(11), fixture.Source(4)));
            REQUIRE(
                store.QueueWrite(fixture.tree, edit, fixture.Request(router, 4, Text("new")), UiBindingCommitTrigger::Submit).HasValue());
            const auto outcome = Process(store, fixture.tree, layout);
            CHECK(outcome.disposition == UiBindingWriteDisposition::Rejected);
            REQUIRE(outcome.error);
            CHECK(outcome.error->code.Value() == UiErrors::CapacityExceeded.code.Value());
            CHECK(fixture.state->commits == 0);
            CHECK(fixture.state->revision.Value() == 1);
            CHECK(std::get<UiActionText>(fixture.state->values[1]).View() == "old");
            CHECK(store.Current().revision == before.revision);
            CHECK(store.Current().content == before.content);
            CHECK(std::get<std::string>(store.Find(fixture.tree, Stable<UiBindingId>(11))->value) == "old");
            CHECK(std::get<std::string>(store.Find(fixture.tree, Stable<UiBindingId>(14))->value) == "old");
            std::array<UiBindingTargetDirty, 8> dirty{};
            CHECK(Take(store.DrainDirty(dirty)) == 0);
            REQUIRE(layout.Invalidate({fixture.Source(3).element, fixture.tree.Revision(), UiLayoutDirtyKind::Arrange}).HasValue());
            REQUIRE(store.ReconcileControl(fixture.tree, Stable<UiBindingId>(11), control).HasValue());
        }

        TEST_CASE("Provider rejection cancellation and error preserve committed values and exact terminal evidence",
                  "[runtime_ui][binding][write][feedback]") {
            Fixture fixture;
            auto store = fixture.Store();
            auto layout = fixture.Layout();
            auto router = fixture.Router();
            auto control = fixture.Control(UiControlKind::Toggle);
            auto authority = fixture.Authority(0);
            fixture.Admit(store, 10, authority);
            SECTION("expected rejection") {
                authority->disposition = UiBindingWriteDisposition::Rejected;
            }
            SECTION("producer cancellation") {
                authority->disposition = UiBindingWriteDisposition::Cancelled;
            }
            SECTION("original provider error") {
                authority->failure = WithCause(MakeError(UiErrors::ActionHandlerFailed, "provider reservation failed"),
                                               MakeError(UiErrors::RevisionStale, "provider transaction moved"));
            }
            const auto edit = Take(store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)));
            REQUIRE(control.Handle(fixture.Input(control, UiControlInputKind::FocusGained, 1)).HasValue());
            REQUIRE(control.Handle(fixture.Input(control, UiControlInputKind::SubmitPress, 2)).HasValue());
            REQUIRE(control.Handle(fixture.Input(control, UiControlInputKind::SubmitRelease, 3)).HasValue());
            const auto queued = Take(store.QueueControlDefault(fixture.tree, edit, control, fixture.DefaultRequest(router, control)));
            CHECK(std::get<UiToggleControlState>(control.Snapshot().Value()).checked);
            const auto outcome = Process(store, fixture.tree, layout);
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
            REQUIRE(store.ReconcileControl(fixture.tree, Stable<UiBindingId>(10), control).HasValue());
            CHECK_FALSE(std::get<UiToggleControlState>(control.Snapshot().Value()).checked);
            CHECK(std::get<UiToggleControlState>(control.Snapshot().Value()).focused);
            CHECK(fixture.state->abandons == 1);
            CHECK_FALSE(Take(store.ProcessWrite(fixture.tree, layout)).has_value());
        }

        TEST_CASE("Pending producer writes are fenced by revocation reload retirement and shutdown exactly once",
                  "[runtime_ui][binding][write][lifecycle]") {
            Fixture fixture;
            auto store = fixture.Store();
            auto layout = fixture.Layout();
            auto router = fixture.Router();
            auto authority = fixture.Authority(0);
            authority->disposition = UiBindingWriteDisposition::Pending;
            fixture.Admit(store, 10, authority);
            const auto edit = Take(store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)));
            const auto queued =
                Take(store.QueueWrite(fixture.tree, edit, fixture.Request(router, 2, true), UiBindingCommitTrigger::Change));
            CHECK(Process(store, fixture.tree, layout).disposition == UiBindingWriteDisposition::Pending);
            CHECK(fixture.state->prepares == 1);
            auto expectedCancellation = UiBindingWriteCancellationReason::Count;
            SECTION("revocation") {
                expectedCancellation = UiBindingWriteCancellationReason::ProviderUnavailable;
                authority->Revoke();
                CHECK(Process(store, fixture.tree, layout).disposition == UiBindingWriteDisposition::Cancelled);
            }
            SECTION("retirement") {
                expectedCancellation = UiBindingWriteCancellationReason::OwnerRetired;
                store.BeginRetirement();
                store.BeginRetirement();
            }
            SECTION("shutdown") {
                store.Shutdown();
                store.Shutdown();
            }
            SECTION("reload replacement") {
                auto replacement = fixture.MakeTree(2);
                CHECK(Process(store, replacement, layout).disposition == UiBindingWriteDisposition::Rejected);
            }
            authority->disposition = UiBindingWriteDisposition::Ready;
            const auto late = store.ProcessWrite(fixture.tree, layout);
            CHECK((late.HasError() || !late.Value().has_value()));
            CHECK(fixture.state->prepares == 1);
            CHECK(fixture.state->commits == 0);
            CHECK(fixture.state->abandons == 1);
            std::array<UiBindingWriteResult, 1> outcomes{};
            const auto drained = store.DrainWriteResults(outcomes);
            if (drained.HasValue()) {
                CHECK(drained.Value() == 1);
                CHECK(outcomes[0].request == queued.request);
                CHECK(outcomes[0].operation == queued.operation);
                CHECK(outcomes[0].cancellation == expectedCancellation);
                CHECK(Take(store.DrainWriteResults(outcomes)) == 0);
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
                const auto edit = Take(store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)));
                REQUIRE(store.QueueWrite(fixture.tree, edit, fixture.Request(router, 2, true), UiBindingCommitTrigger::Change).HasValue());
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
            Fixture fixture;
            auto store = fixture.Store();
            auto layout = fixture.Layout();
            auto router = fixture.Router();
            auto authority = fixture.Authority(0);
            authority->disposition = UiBindingWriteDisposition::Pending;
            fixture.Admit(store, 10, authority);
            ClearDirty(store);
            const auto before = store.Current();
            const auto source = fixture.Source(2);
            const auto edit = Take(store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), source));
            SECTION("pending owner work is cancelled and releases once") {
                REQUIRE(store.QueueWrite(fixture.tree, edit, fixture.Request(router, 2, true), UiBindingCommitTrigger::Change).HasValue());
                CHECK(Process(store, fixture.tree, layout).disposition == UiBindingWriteDisposition::Pending);
            }
            SECTION("unqueued drafts are invalidated") {
                CHECK(fixture.state->prepares == 0);
            }
            auto owner = fixture.Context();
            owner.interaction = Rev<UiInteractionRevision>(2);
            REQUIRE(store.UpdateWritePresentation(fixture.tree, owner).HasValue());
            ErrorIs(store.UpdateWritePresentation(fixture.tree, owner), UiErrors::RevisionStale);
            ErrorIs(store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), source), UiErrors::RevisionStale);
            ErrorIs(store.CancelEdit(edit), UiErrors::RevisionStale);
            CHECK(store.Current().revision == before.revision);
            CHECK(store.Current().content == before.content);
            CHECK_FALSE(std::get<bool>(store.Find(fixture.tree, Stable<UiBindingId>(10))->value));
            std::array<UiBindingTargetDirty, 8> dirty{};
            CHECK(Take(store.DrainDirty(dirty)) == 0);
            std::array<UiBindingWriteResult, 1> outcomes{};
            const auto terminal = Take(store.DrainWriteResults(outcomes));
            CHECK(terminal == fixture.state->prepares);
            if (terminal != 0) {
                CHECK(outcomes[0].disposition == UiBindingWriteDisposition::Cancelled);
                CHECK(outcomes[0].cancellation == UiBindingWriteCancellationReason::PresentationChanged);
            }
            CHECK(fixture.state->abandons == terminal);
            CHECK_FALSE(Take(store.ProcessWrite(fixture.tree, layout)).has_value());
            authority->disposition = UiBindingWriteDisposition::Ready;
            auto freshSource = source;
            freshSource.owner = owner;
            auto freshRouter = Take(UiActionRouter::Create({owner, 4, router.LastIssuedSequence()}));
            const auto freshEdit = Take(store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), freshSource));
            UiActionPayload payload;
            REQUIRE(payload.Add(true).HasValue());
            REQUIRE(freshRouter.Enqueue(freshSource, UiGameplayActionCommand{fixture.Action(), payload}).HasValue());
            UiBindingWriteActionHandler handler{store, fixture.tree, freshEdit, UiBindingCommitTrigger::Change};
            REQUIRE(freshRouter.DispatchNext(handler).HasValue());
            CHECK(Process(store, fixture.tree, layout).disposition == UiBindingWriteDisposition::Ready);
            CHECK(fixture.state->commits == 1);
        }

        TEST_CASE("Failed unregister still fences provider writes before fallback layout backpressure",
                  "[runtime_ui][binding][write][unregister]") {
            Fixture fixture;
            auto store = fixture.Store();
            auto layout = fixture.Layout(1);
            auto router = fixture.Router();
            auto authority = fixture.Authority(0);
            authority->disposition = UiBindingWriteDisposition::Pending;
            fixture.Admit(store, 10, authority);
            const auto edit = Take(store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)));
            REQUIRE(store.QueueWrite(fixture.tree, edit, fixture.Request(router, 2, true), UiBindingCommitTrigger::Change).HasValue());
            CHECK(Process(store, fixture.tree, layout).disposition == UiBindingWriteDisposition::Pending);
            authority->Revoke();
            ErrorIs(store.Unregister(fixture.tree, fixture.provider, layout), UiErrors::CapacityExceeded);
            REQUIRE(store.Find(fixture.tree, Stable<UiBindingId>(10)));
            authority->active = true;
            authority->disposition = UiBindingWriteDisposition::Ready;
            ErrorIs(store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)), UiErrors::BindingLifecycleUnavailable);
            CHECK_FALSE(Take(store.ProcessWrite(fixture.tree, layout)).has_value());
            CHECK(fixture.state->commits == 0);
            CHECK(fixture.state->abandons == 1);
            std::array<UiBindingWriteResult, 1> outcomes{};
            CHECK(Take(store.DrainWriteResults(outcomes)) == 1);
            CHECK(outcomes[0].disposition == UiBindingWriteDisposition::Cancelled);
            CHECK(outcomes[0].cancellation == UiBindingWriteCancellationReason::ProviderUnavailable);
            auto available = fixture.Layout();
            REQUIRE(store.Unregister(fixture.tree, fixture.provider, available).HasValue());
            CHECK(store.Find(fixture.tree, Stable<UiBindingId>(10)) == nullptr);
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
                const auto edit = Take(store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)));
                REQUIRE(store.QueueWrite(fixture.tree, edit, fixture.Request(router, 2, true), UiBindingCommitTrigger::Change).HasValue());
                CHECK(Process(store, fixture.tree, layout).disposition == UiBindingWriteDisposition::Pending);
                authority.reset();
                CHECK_FALSE(lease.expired());
            }
            CHECK(lease.expired());
            CHECK(fixture.state->abandons == 1);
            CHECK(fixture.state->destroyed == 1);
            CHECK(fixture.state->commits == 0);
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
            CHECK_FALSE(router.LastIssuedSequence().IsValid());
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
            Fixture fixture;
            auto store = fixture.Store();
            auto layout = fixture.Layout();
            auto router = fixture.Router();
            fixture.Admit(store, 10, fixture.Authority(0));
            const auto first = Take(store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)));
            const auto request = fixture.Request(router, 2, true);
            REQUIRE(store.QueueWrite(fixture.tree, first, request, UiBindingCommitTrigger::Change).HasValue());
            CHECK(Process(store, fixture.tree, layout).disposition == UiBindingWriteDisposition::Ready);
            std::array<UiBindingWriteResult, 1> terminal{};
            CHECK(Take(store.DrainWriteResults(terminal)) == 1);
            const auto second = Take(store.BeginEdit(fixture.tree, Stable<UiBindingId>(10), fixture.Source(2)));
            ErrorIs(store.QueueWrite(fixture.tree, second, request, UiBindingCommitTrigger::Change), UiErrors::RevisionStale);
            CHECK(fixture.state->commits == 1);
            REQUIRE(store.QueueWrite(fixture.tree, second, fixture.Request(router, 2, false), UiBindingCommitTrigger::Change).HasValue());
            CHECK(Process(store, fixture.tree, layout).disposition == UiBindingWriteDisposition::Ready);
            CHECK(fixture.state->commits == 2);
            CHECK_FALSE(std::get<bool>(fixture.state->values[0]));
        }

        TEST_CASE("Prepared successful text writes allocate nothing across command queue and publication",
                  "[runtime_ui][binding][write][allocation]") {
            Fixture fixture;
            auto store = fixture.Store();
            auto layout = fixture.Layout();
            auto router = fixture.Router();
            fixture.Admit(store, 11, fixture.Authority(1), UiBindingCommitTrigger::Submit);
            ClearDirty(store);
            const auto binding = Stable<UiBindingId>(11);
            const auto source = fixture.Source(4);
            const auto action = fixture.Action();
            UiActionPayload payload;
            REQUIRE(payload.Add(Text(std::string(64, 'x'))).HasValue());
            std::array<UiBindingWriteResult, 1> outcomes{};
            std::array<UiBindingTargetDirty, 8> dirty{};
            const auto before = WriteAllocations().load(std::memory_order_relaxed);
            const auto edit = store.BeginEdit(fixture.tree, binding, source);
            const auto enqueued = router.Enqueue(source, UiGameplayActionCommand{action, payload});
            const auto request = router.TryDequeue();
            const auto queued = store.QueueWrite(fixture.tree, edit.Value(), *request.Value(), UiBindingCommitTrigger::Submit);
            const auto processed = store.ProcessWrite(fixture.tree, layout);
            const auto terminal = store.DrainWriteResults(outcomes);
            const auto notifications = store.DrainDirty(dirty);
            const auto after = WriteAllocations().load(std::memory_order_relaxed);
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
    }  // namespace
}  // namespace Horo::Runtime::Ui
