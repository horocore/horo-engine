#include "Horo/Runtime/Ui/UiBindingStore.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>

namespace {
    std::atomic<std::size_t> &BindingAllocations() {
        static std::atomic<std::size_t> counter{};
        return counter;
    }
}  // namespace

void *operator new(const std::size_t bytes) {
    BindingAllocations().fetch_add(1);
    const auto release = [](std::byte *memory) noexcept {
        ::operator delete(memory);
    };
    std::unique_ptr<std::byte, decltype(release)> memory{static_cast<std::byte *>(std::malloc(bytes == 0 ? 1 : bytes)), release};
    if (!memory)
        throw std::bad_alloc{};
    return memory.release();
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
            return Take(UiOwnershipGeneration::Create(31));
        }

        UiElementTree Tree(UiElementSlotAllocator &slots, const std::uint32_t generation = 1) {
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
            };
            return Take(UiElementTree::Create(slots, descriptor, elements));
        }

        UiBindingProviderSchema Schema(const UiBindingProviderFlags flags = UiBindingProviderFlags::None) {
            const auto property = [](const std::string_view id, const UiBindingValueType type) {
                return UiBindingPropertyDescriptor{.id = Take(UiBindingPropertyId::Parse(id)),
                                                   .type = type,
                                                   .limits = {.maximumBytes = 128,
                                                              .maximumElements = 1,
                                                              .minimumScalar = 0.0,
                                                              .maximumScalar = 1.0}};
            };
            const std::array properties{property("enabled", UiBindingValueType::Boolean),
                                        property("label", UiBindingValueType::BoundedText),
                                        property("progress", UiBindingValueType::FixedScalar),
                                        property("unused", UiBindingValueType::Boolean)};
            UiBindingProviderDescriptor descriptor{Take(UiBindingProviderTypeId::Parse("game.hud.player")),
                                                   ModuleId{"game.hud"},
                                                   {1, 0, 0},
                                                   properties,
                                                   UiBindingProviderScopeBit(UiBindingProviderScopeKind::Player),
                                                   flags};
            descriptor.schema.fingerprint = ComputeUiBindingSchemaFingerprint(descriptor);
            return Take(UiBindingProviderSchema::Create(descriptor));
        }

        struct Fixture final {
            UiElementSlotAllocator slots{Take(UiElementSlotAllocator::Create(Owner()))};
            UiElementTree tree{Tree(slots)};
            UiBindingProviderSchema schema{Schema()};
            UiBindingProviderInstanceId provider{Owner(), 7, 1};
            std::array<UiBindingPropertyUpdate, 3> initial{{{0, true}, {1, std::string(40, 'a')}, {2, 0.5}}};

            UiResolvedBindingDescriptor Binding(const std::uint8_t id, const std::uint8_t element, const std::uint16_t property,
                                                const UiBindingTargetProperty target) const {
                const auto &source = schema.Properties()[property];
                return {provider,
                        {.id = Stable<UiBindingId>(id),
                         .source = {schema.Type(), source.id, {1, 0}, source.signatureFingerprint},
                         .target = {Stable<UiElementId>(element), target, {.maximumBytes = 128}}}};
            }

            std::array<UiResolvedBindingDescriptor, 4> Bindings() const {
                using enum UiBindingTargetProperty;
                auto optional = Binding(12, 5, 1, Text);
                optional.binding.requirement = UiBindingRequirement::Optional;
                optional.binding.fallback = std::string{"missing"};
                return {Binding(10, 2, 1, Text), Binding(11, 3, 2, Progress), optional, Binding(13, 4, 0, Enabled)};
            }

            UiBindingStore Store(const UiBindingStoreLimits &limits = {}) const {
                const std::array registrations{UiBindingProviderRegistration{provider, UiBindingProviderScopeKind::Player, &schema,
                                                                             Rev<UiBindingSnapshotRevision>(), initial}};
                const auto bindings = Bindings();
                return Take(UiBindingStore::Create(tree, registrations, bindings, limits));
            }

            UiLayoutEngine Layout(const std::uint32_t capacity = 8) const {
                return Take(UiLayoutEngine::Create(
                    {tree.Instance(), tree.Canvas(), tree.SourceDocument(), 8, capacity, 3, Rev<UiInteractionRevision>()}));
            }

            UiBindingChangeBatch Batch(const std::span<const UiBindingPropertyUpdate> changes, const std::uint64_t expected = 1,
                                       const std::uint64_t revision = 2) const {
                return {provider, schema.Version(), Rev<UiBindingSnapshotRevision>(expected), Rev<UiBindingSnapshotRevision>(revision),
                        changes};
            }
        };

        void ClearDirty(UiBindingStore &store) {
            std::array<UiBindingTargetDirty, 8> output{};
            REQUIRE(store.DrainDirty(output).HasValue());
        }

        template <typename T> void ErrorIs(const Result<T> &result, const ErrorCodeDescriptor &error) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == error.code.Value());
        }

        class BoundTextMetrics final : public UiLayoutIntrinsicProvider {
        public:
            const UiElementTree &tree;
            const UiBindingStore &bindings;
            UiElementHandle firstText;
            UiBindingId firstBinding;
            UiBindingId secondBinding;
            mutable std::size_t calls{};

            BoundTextMetrics(const UiElementTree &source, const UiBindingStore &store)
                : tree(source), bindings(store), firstText(Take(source.Find(Stable<UiElementId>(2)))),
                  firstBinding(Stable<UiBindingId>(10)), secondBinding(Stable<UiBindingId>(12)) {}

            Result<UiLayoutIntrinsicMeasurement> MeasureText(const UiLayoutIntrinsicRequest &request) const override {
                ++calls;
                const auto id = request.element == firstText ? firstBinding : secondBinding;
                const auto *target = bindings.Find(tree, id);
                if (!target)
                    return Result<UiLayoutIntrinsicMeasurement>::Failure(MakeError(UiErrors::LayoutIntrinsicUnavailable));
                return Result<UiLayoutIntrinsicMeasurement>::Success(
                    {{static_cast<UiScalar>(std::get<std::string>(target->value).size() * 64), 64}});
            }

            Result<UiLayoutIntrinsicMeasurement> MeasureImage(const UiLayoutIntrinsicRequest &) const override {
                return Result<UiLayoutIntrinsicMeasurement>::Failure(MakeError(UiErrors::LayoutIntrinsicUnavailable));
            }
        };

        std::array<UiLayoutElementDescriptor, 5> LayoutDescriptors(const UiElementTree &tree) {
            std::array<UiLayoutElementDescriptor, 5> result{};
            for (std::size_t index = 0; index < result.size(); ++index) {
                auto &descriptor = result[index];
                descriptor.element = Take(tree.Find(Stable<UiElementId>(static_cast<std::uint8_t>(index + 1))));
                if (index == 1 || index == 4)
                    descriptor.intrinsic = {UiLayoutIntrinsicKind::Text, true};
                else if (index != 0) {
                    descriptor.style.width = UiLength::Dip(64);
                    descriptor.style.height = UiLength::Dip(64);
                }
            }
            return result;
        }

        UiLayoutUpdateRequest Request(const UiElementTree &tree, const UiBindingStore &bindings, const UiLayoutEvaluator &evaluator) {
            return {{tree.SourceDocumentRevision(), tree.Revision(), bindings.Current().content, Rev<UiLayoutStyleRevision>(),
                     Rev<UiLayoutIntrinsicRevision>(), Rev<UiLayoutCanvasRevision>(), Rev<UiLayoutPolicyRevision>()},
                    {{0, 0}, {8192, 8192}},
                    {{0, 0}, {8192, 8192}},
                    &evaluator};
        }

        TEST_CASE("Versioned binding deltas drive retained text and incremental production layout", "[runtime_ui][binding][batch]") {
            Fixture fixture;
            auto store = fixture.Store();
            auto layout = fixture.Layout();
            BoundTextMetrics metrics{fixture.tree, store};
            const auto descriptors = LayoutDescriptors(fixture.tree);
            auto evaluator = Take(UiDeclarativeLayoutEvaluator::Create(descriptors, &metrics));
            {
                const auto initial = Take(layout.Update(fixture.tree, Request(fixture.tree, store, evaluator)));
                CHECK(metrics.calls == 2);
            }
            ClearDirty(store);
            const std::array changes{UiBindingPropertyUpdate{1, std::string(55, 'b')}};
            const std::array batches{fixture.Batch(changes)};
            const auto applied = Take(store.Apply(fixture.tree, batches, layout));
            CHECK(applied.targetsChanged == 2);
            CHECK(applied.content.Value() == 2);
            const auto updated = Take(layout.Update(fixture.tree, Request(fixture.tree, store, evaluator)));
            CHECK(metrics.calls == 4);
            const auto element = Take(fixture.tree.Find(Stable<UiElementId>(2)));
            CHECK(Take(updated.Get(element)).arrangement.contentBox.extent.width == 55 * 64);
            std::array<UiBindingTargetDirty, 4> dirty{};
            REQUIRE(Take(store.DrainDirty(dirty)) == 2);
            CHECK(dirty[0].binding == Stable<UiBindingId>(10));
            CHECK(dirty[1].binding == Stable<UiBindingId>(12));
            CHECK(HasFlag(dirty[0].dirty, UiBindingDirty::Layout));
            CHECK_FALSE(HasFlag(dirty[0].dirty, UiBindingDirty::Actions));
        }

        TEST_CASE("Paint and action updates do not rebuild unaffected text or layout", "[runtime_ui][binding][incremental]") {
            Fixture fixture;
            auto store = fixture.Store();
            auto layout = fixture.Layout();
            BoundTextMetrics metrics{fixture.tree, store};
            const auto descriptors = LayoutDescriptors(fixture.tree);
            auto evaluator = Take(UiDeclarativeLayoutEvaluator::Create(descriptors, &metrics));
            const auto initial = Take(layout.Update(fixture.tree, Request(fixture.tree, store, evaluator)));
            ClearDirty(store);
            const std::array changes{UiBindingPropertyUpdate{0, false}, UiBindingPropertyUpdate{2, 0.75}};
            const std::array batches{fixture.Batch(changes)};
            const auto applied = Take(store.Apply(fixture.tree, batches, layout));
            CHECK(applied.targetsChanged == 2);
            CHECK(applied.content == initial.Descriptor().sources.content);
            const auto same = Take(layout.Update(fixture.tree, Request(fixture.tree, store, evaluator)));
            CHECK(same.Descriptor().interaction == initial.Descriptor().interaction);
            CHECK(metrics.calls == 2);
            CHECK(std::get<double>(store.Find(fixture.tree, Stable<UiBindingId>(11))->value) == 0.75);
            std::array<UiBindingTargetDirty, 4> dirty{};
            REQUIRE(Take(store.DrainDirty(dirty)) == 2);
            CHECK_FALSE(HasFlag(dirty[0].dirty, UiBindingDirty::Layout));
            CHECK(HasFlag(dirty[1].dirty, UiBindingDirty::Actions));
            CHECK(Take(store.Apply(fixture.tree, {}, layout)).targetsChanged == 0);
            const std::array equal{fixture.Batch(changes, 2, 5)};
            CHECK(Take(store.Apply(fixture.tree, equal, layout)).targetsChanged == 0);
            CHECK(Take(store.DrainDirty(dirty)) == 0);
        }

        TEST_CASE("A malformed or stale batch cannot partially mutate targets or consume provider revision",
                  "[runtime_ui][binding][rollback]") {
            Fixture fixture;
            auto store = fixture.Store();
            auto layout = fixture.Layout();
            ClearDirty(store);
            const std::array good{UiBindingPropertyUpdate{0, false}, UiBindingPropertyUpdate{2, 0.75}};
            const std::array wrongType{UiBindingPropertyUpdate{0, false}, UiBindingPropertyUpdate{2, true}};
            const std::array bad{fixture.Batch(wrongType)};
            ErrorIs(store.Apply(fixture.tree, bad, layout), UiErrors::BindingValueInvalid);
            CHECK(std::get<bool>(store.Find(fixture.tree, Stable<UiBindingId>(13))->value));
            const std::array duplicate{UiBindingPropertyUpdate{0, false}, UiBindingPropertyUpdate{0, true}};
            ErrorIs(store.Apply(fixture.tree, std::array{fixture.Batch(duplicate)}, layout), UiErrors::BindingDescriptorConflict);
            const std::array reversed{UiBindingPropertyUpdate{2, 0.75}, UiBindingPropertyUpdate{0, false}};
            ErrorIs(store.Apply(fixture.tree, std::array{fixture.Batch(reversed)}, layout), UiErrors::BindingDescriptorConflict);
            const auto batch = fixture.Batch(good);
            ErrorIs(store.Apply(fixture.tree, std::array{batch, batch}, layout), UiErrors::BindingDescriptorConflict);
            auto incompatible = batch;
            ++incompatible.schema.minor;
            ErrorIs(store.Apply(fixture.tree, std::array{incompatible}, layout), UiErrors::BindingSchemaIncompatible);
            auto foreign = batch;
            ++foreign.provider.generation;
            ErrorIs(store.Apply(fixture.tree, std::array{foreign}, layout), UiErrors::HandleStale);
            CHECK(Take(store.Apply(fixture.tree, std::array{batch}, layout)).targetsChanged == 2);
            ErrorIs(store.Apply(fixture.tree, std::array{batch}, layout), UiErrors::RevisionStale);
            ErrorIs(store.Apply(fixture.tree, std::array{fixture.Batch(good, 2, 2)}, layout), UiErrors::RevisionStale);
        }

        TEST_CASE("Layout queue backpressure rolls back binding values and prior invalidations", "[runtime_ui][binding][capacity]") {
            Fixture fixture;
            auto store = fixture.Store();
            auto layout = fixture.Layout(1);
            ClearDirty(store);
            const auto before = store.Current();
            const std::array changes{UiBindingPropertyUpdate{1, std::string(60, 'x')}};
            ErrorIs(store.Apply(fixture.tree, std::array{fixture.Batch(changes)}, layout), UiErrors::CapacityExceeded);
            CHECK(store.Current().revision == before.revision);
            CHECK(std::get<std::string>(store.Find(fixture.tree, Stable<UiBindingId>(10))->value).size() == 40);
            std::array<UiBindingTargetDirty, 4> dirty{};
            CHECK(Take(store.DrainDirty(dirty)) == 0);
            REQUIRE(
                layout.Invalidate({Take(fixture.tree.Find(Stable<UiElementId>(3))), fixture.tree.Revision(), UiLayoutDirtyKind::Arrange})
                    .HasValue());
            const std::array invalidations{UiLayoutInvalidation{Take(fixture.tree.Find(Stable<UiElementId>(2))), fixture.tree.Revision(),
                                                                UiLayoutDirtyKind::Measure},
                                           UiLayoutInvalidation{Take(fixture.tree.Find(Stable<UiElementId>(5))), fixture.tree.Revision(),
                                                                UiLayoutDirtyKind::Measure}};
            ErrorIs(layout.InvalidateBatch(fixture.tree, invalidations), UiErrors::CapacityExceeded);
            REQUIRE(
                layout.InvalidateBatch(fixture.tree, std::array{UiLayoutInvalidation{{}, fixture.tree.Revision(), UiLayoutDirtyKind::All}})
                    .HasValue());
            CHECK(Take(store.Apply(fixture.tree, std::array{fixture.Batch(changes)}, layout)).targetsChanged == 2);
        }

        TEST_CASE("Unregister closes old provider batches and removes required values with typed optional fallback",
                  "[runtime_ui][binding][unregister]") {
            Fixture fixture;
            auto store = fixture.Store();
            auto layout = fixture.Layout();
            ClearDirty(store);
            const auto revoked = Take(store.Unregister(fixture.tree, fixture.provider, layout));
            CHECK(revoked.targetsChanged == 4);
            CHECK(revoked.requiredUnavailable == 3);
            CHECK(store.Find(fixture.tree, Stable<UiBindingId>(10)) == nullptr);
            const auto *optional = store.Find(fixture.tree, Stable<UiBindingId>(12));
            REQUIRE(optional);
            CHECK(optional->origin == UiBindingValueOrigin::Fallback);
            CHECK(std::get<std::string>(optional->value) == "missing");
            CHECK(Take(store.Unregister(fixture.tree, fixture.provider, layout)).targetsChanged == 0);
            const std::array changes{UiBindingPropertyUpdate{1, std::string{"late"}}};
            ErrorIs(store.Apply(fixture.tree, std::array{fixture.Batch(changes)}, layout), UiErrors::BindingLifecycleUnavailable);
        }

        TEST_CASE("Reload and tree mutation reject old bindings; retirement and shutdown are idempotent",
                  "[runtime_ui][binding][lifetime]") {
            Fixture fixture;
            auto store = fixture.Store();
            auto layout = fixture.Layout();
            auto replacement = Tree(fixture.slots, 2);
            CHECK(store.Find(replacement, Stable<UiBindingId>(10)) == nullptr);
            ErrorIs(store.Apply(replacement, {}, layout), UiErrors::HandleOwnerMismatch);
            const std::array changes{UiBindingPropertyUpdate{1, std::string{"new generation"}}};
            auto old = fixture.Batch(changes);
            ++fixture.provider.generation;
            auto reloaded = fixture.Store();
            ErrorIs(reloaded.Apply(fixture.tree, std::array{old}, layout), UiErrors::HandleStale);
            auto commands = Take(UiStructuralCommandBuffer::Create(fixture.tree.Instance(), fixture.tree.Canvas(),
                                                                   fixture.tree.SourceDocumentRevision(), fixture.tree.Revision(), 1));
            REQUIRE(commands.Add(UiRemoveElementCommand{Take(fixture.tree.Find(Stable<UiElementId>(2)))}).HasValue());
            REQUIRE(fixture.tree.CommitDeferred(commands, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands).HasValue());
            ErrorIs(store.Apply(fixture.tree, std::array{old}, layout), UiErrors::RevisionStale);
            CHECK(store.Find(fixture.tree, Stable<UiBindingId>(10)) == nullptr);
            store.BeginRetirement();
            store.BeginRetirement();
            ErrorIs(store.Apply(fixture.tree, {}, layout), UiErrors::BindingLifecycleUnavailable);
            store.Shutdown();
            store.Shutdown();
            ErrorIs(store.DrainDirty({}), UiErrors::BindingLifecycleUnavailable);
            CHECK_FALSE(store.Current().revision.IsValid());
        }

        TEST_CASE("Preparation rejects conflicts, missing required values and unsupported executable metadata",
                  "[runtime_ui][binding][prepare]") {
            Fixture fixture;
            auto bindings = fixture.Bindings();
            const std::array registrations{UiBindingProviderRegistration{fixture.provider, UiBindingProviderScopeKind::Player,
                                                                         &fixture.schema, Rev<UiBindingSnapshotRevision>(),
                                                                         fixture.initial}};
            auto duplicate = bindings;
            duplicate[1].binding.id = duplicate[0].binding.id;
            ErrorIs(UiBindingStore::Create(fixture.tree, registrations, duplicate), UiErrors::BindingDescriptorConflict);
            duplicate = bindings;
            duplicate[2].binding.target = duplicate[0].binding.target;
            ErrorIs(UiBindingStore::Create(fixture.tree, registrations, duplicate), UiErrors::BindingDescriptorConflict);
            auto missing = registrations;
            missing[0].values = {};
            ErrorIs(UiBindingStore::Create(fixture.tree, missing, bindings), UiErrors::BindingProviderUnknown);
            const std::array optional{bindings[2]};
            auto fallback = Take(UiBindingStore::Create(fixture.tree, missing, optional));
            CHECK(fallback.Find(fixture.tree, optional[0].binding.id)->origin == UiBindingValueOrigin::Fallback);
            bindings[0].binding.converter = UiBindingConverterDescriptor{Take(UiBindingConverterId::Parse("game.hud.identity")),
                                                                         UiBindingValueType::BoundedText, UiBindingValueType::BoundedText};
            ErrorIs(UiBindingStore::Create(fixture.tree, registrations, bindings), UiErrors::BindingConverterInvalid);
            auto wrongScope = registrations;
            wrongScope[0].scope = UiBindingProviderScopeKind::Scene;
            ErrorIs(UiBindingStore::Create(fixture.tree, wrongScope, fixture.Bindings()), UiErrors::BindingAccessInvalid);
            ErrorIs(UiBindingStore::Create(fixture.tree, registrations, fixture.Bindings(), {.valueBytes = 1}),
                    UiErrors::BindingCapacityExceeded);
        }

        TEST_CASE("Bounded text and range violations preserve accumulated dirty evidence", "[runtime_ui][binding][values]") {
            Fixture fixture;
            auto store = fixture.Store({.changeBytes = 128});
            auto layout = fixture.Layout();
            ClearDirty(store);
            const std::array tooLong{UiBindingPropertyUpdate{1, std::string(129, 'x')}};
            ErrorIs(store.Apply(fixture.tree, std::array{fixture.Batch(tooLong)}, layout), UiErrors::BindingCapacityExceeded);
            const std::array malformed{UiBindingPropertyUpdate{1, std::string{"\xc0\xaf"}}};
            ErrorIs(store.Apply(fixture.tree, std::array{fixture.Batch(malformed)}, layout), UiErrors::BindingValueInvalid);
            const std::array outside{UiBindingPropertyUpdate{2, 1.5}};
            ErrorIs(store.Apply(fixture.tree, std::array{fixture.Batch(outside)}, layout), UiErrors::BindingValueInvalid);
            const std::array nonFinite{UiBindingPropertyUpdate{2, std::numeric_limits<double>::quiet_NaN()}};
            ErrorIs(store.Apply(fixture.tree, std::array{fixture.Batch(nonFinite)}, layout), UiErrors::BindingValueInvalid);
            const std::array good{UiBindingPropertyUpdate{2, 0.25}};
            REQUIRE(store.Apply(fixture.tree, std::array{fixture.Batch(good)}, layout).HasValue());
            std::array<UiBindingTargetDirty, 1> output{};
            ErrorIs(store.DrainDirty({}), UiErrors::BindingCapacityExceeded);
            CHECK(Take(store.DrainDirty(output)) == 1);
            CHECK(output[0].binding == Stable<UiBindingId>(11));
            const std::array unused{UiBindingPropertyUpdate{3, true}};
            CHECK(Take(store.Apply(fixture.tree, std::array{fixture.Batch(unused, 2, 3)}, layout)).targetsChanged == 0);
            CHECK(Take(store.DrainDirty(output)) == 0);
        }

        TEST_CASE("Successful text batches and empty frames allocate nothing after preparation", "[runtime_ui][binding][allocation]") {
            Fixture fixture;
            auto store = fixture.Store();
            auto layout = fixture.Layout();
            BoundTextMetrics metrics{fixture.tree, store};
            const auto descriptors = LayoutDescriptors(fixture.tree);
            auto evaluator = Take(UiDeclarativeLayoutEvaluator::Create(descriptors, &metrics));
            REQUIRE(layout.Update(fixture.tree, Request(fixture.tree, store, evaluator)).HasValue());
            ClearDirty(store);
            const std::array changes{UiBindingPropertyUpdate{1, std::string(128, 'z')}};
            const std::array batches{fixture.Batch(changes)};
            std::array<UiBindingTargetDirty, 4> dirty{};
            auto request = Request(fixture.tree, store, evaluator);
            const auto before = BindingAllocations().load();
            const auto applied = store.Apply(fixture.tree, batches, layout);
            request.sources.content = store.Current().content;
            const auto arranged = layout.Update(fixture.tree, request);
            const auto copied = store.DrainDirty(dirty);
            const auto idle = store.Apply(fixture.tree, {}, layout);
            const auto revoked = store.Unregister(fixture.tree, fixture.provider, layout);
            const auto after = BindingAllocations().load();
            REQUIRE(applied.HasValue());
            REQUIRE(arranged.HasValue());
            REQUIRE(copied.HasValue());
            REQUIRE(idle.HasValue());
            REQUIRE(revoked.HasValue());
            CHECK(after == before);
            CHECK(std::get<std::string>(changes[0].value).size() == 128);
        }

        TEST_CASE("Several providers publish atomically and property target limits cannot leave an earlier source applied",
                  "[runtime_ui][binding][coherence]") {
            Fixture fixture;
            const UiBindingProviderInstanceId second{Owner(), 8, 1};
            const std::array registrations{UiBindingProviderRegistration{fixture.provider, UiBindingProviderScopeKind::Player,
                                                                         &fixture.schema, Rev<UiBindingSnapshotRevision>(),
                                                                         fixture.initial},
                                           UiBindingProviderRegistration{second, UiBindingProviderScopeKind::Player, &fixture.schema,
                                                                         Rev<UiBindingSnapshotRevision>(), fixture.initial}};
            auto bindings = fixture.Bindings();
            bindings[1].provider = second;
            bindings[1].binding.target.limits.maximumScalar = 0.8;
            auto store = Take(UiBindingStore::Create(fixture.tree, registrations, bindings));
            auto layout = fixture.Layout();
            ClearDirty(store);
            const std::array firstChanges{UiBindingPropertyUpdate{0, false}};
            const std::array invalidSecond{UiBindingPropertyUpdate{2, 0.9}};
            auto secondBatch = fixture.Batch(invalidSecond);
            secondBatch.provider = second;
            const std::array bad{fixture.Batch(firstChanges), secondBatch};
            ErrorIs(store.Apply(fixture.tree, bad, layout), UiErrors::BindingValueInvalid);
            CHECK(std::get<bool>(store.Find(fixture.tree, Stable<UiBindingId>(13))->value));
            CHECK(store.Current().revision.Value() == 1);
            const std::array secondChanges{UiBindingPropertyUpdate{2, 0.7}};
            secondBatch.changes = secondChanges;
            const std::array good{fixture.Batch(firstChanges), secondBatch};
            REQUIRE(store.Apply(fixture.tree, good, layout).HasValue());
            CHECK_FALSE(std::get<bool>(store.Find(fixture.tree, Stable<UiBindingId>(13))->value));
            CHECK(std::get<double>(store.Find(fixture.tree, Stable<UiBindingId>(11))->value) == 0.7);
            ErrorIs(store.Apply(fixture.tree, std::array{secondBatch, good[0]}, layout), UiErrors::BindingDescriptorConflict);
        }

        TEST_CASE("Snapshot exhaustion and immutable providers reject changes without wrapping or executing metadata",
                  "[runtime_ui][binding][revision]") {
            Fixture fixture;
            auto layout = fixture.Layout();
            const std::array registrations{
                UiBindingProviderRegistration{fixture.provider, UiBindingProviderScopeKind::Player, &fixture.schema,
                                              Rev<UiBindingSnapshotRevision>(std::numeric_limits<std::uint64_t>::max()), fixture.initial}};
            auto store = Take(UiBindingStore::Create(fixture.tree, registrations, fixture.Bindings()));
            const std::array changes{UiBindingPropertyUpdate{2, 0.75}};
            ErrorIs(store.Apply(fixture.tree, std::array{fixture.Batch(changes, std::numeric_limits<std::uint64_t>::max(), 1)}, layout),
                    UiErrors::RevisionStale);
            fixture.schema = Schema(UiBindingProviderFlags::Immutable);
            auto immutable = fixture.Store();
            ErrorIs(immutable.Apply(fixture.tree, std::array{fixture.Batch(changes)}, layout), UiErrors::BindingAccessInvalid);
            auto movable = fixture.Store();
            auto moved = std::move(movable);
            ErrorIs(movable.Apply(fixture.tree, {}, layout), UiErrors::BindingLifecycleUnavailable);
            REQUIRE(moved.Find(fixture.tree, Stable<UiBindingId>(10)));
        }

        TEST_CASE("Failed unregister leaves registration retryable and dirty capacity never truncates",
                  "[runtime_ui][binding][retirement]") {
            Fixture fixture;
            auto store = fixture.Store();
            auto small = fixture.Layout(1);
            ErrorIs(store.Unregister(fixture.tree, fixture.provider, small), UiErrors::CapacityExceeded);
            REQUIRE(store.Find(fixture.tree, Stable<UiBindingId>(10)));
            auto layout = fixture.Layout();
            std::array<UiBindingTargetDirty, 1> sentinel{};
            sentinel[0].binding = Stable<UiBindingId>(99);
            ErrorIs(store.DrainDirty(sentinel), UiErrors::BindingCapacityExceeded);
            CHECK(sentinel[0].binding == Stable<UiBindingId>(99));
            REQUIRE(store.Unregister(fixture.tree, fixture.provider, layout).HasValue());
            layout.Shutdown();
            auto active = fixture.Store();
            const std::array changes{UiBindingPropertyUpdate{2, 0.75}};
            ErrorIs(active.Apply(fixture.tree, std::array{fixture.Batch(changes)}, layout), UiErrors::LayoutLifecycleUnavailable);
            CHECK(std::get<double>(active.Find(fixture.tree, Stable<UiBindingId>(11))->value) == 0.5);
            fixture.tree.Shutdown();
            CHECK(active.Find(fixture.tree, Stable<UiBindingId>(10)) == nullptr);
            ErrorIs(active.Apply(fixture.tree, {}, layout), UiErrors::BindingLifecycleUnavailable);
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
