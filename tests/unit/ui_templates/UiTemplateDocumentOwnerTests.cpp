#include "Horo/UiTemplates/UiTemplateDocumentOwner.h"
#include "UiTemplateOwnerTestFixture.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <thread>

namespace Horo::UiTemplates {
    namespace {
        using namespace Runtime::Ui;

        using TestSupport::Identity;
        using TestSupport::OwnerOrdinaryDocument;

        using namespace TestSupport;
    }  // namespace

    TEST_CASE("All eight template value categories roundtrip without coercion or lost attachment identity",
              "[runtime_ui][template][headless]") {
        const auto provider = Provider();
        auto required = Binding(provider);
        required.requirement = UiBindingRequirement::Required;
        CHECK(ValidateUiBindingDescriptor(required, provider).HasError());
        required.fallback.reset();
        CHECK(ValidateUiBindingDescriptor(required, provider).HasValue());
        UiReference reference;
        reference.element = Identity<UiElementId>(1);
        const std::array<UiTemplateValue, 8> values{true,      std::int64_t{-42},
                                                    1.25,      std::string{"text"},
                                                    reference, UiStyleValue{UiStyleColor{}},
                                                    Message(), Binding(provider)};
        for (const auto &value : values) {
            CAPTURE(value.index());
            auto catalog = ValueCatalog(value);
            auto source = Document();
            source.instances.front().arguments.front().value = value;
            auto owner = UiTemplateDocumentOwner::Create(std::move(source), catalog);
            REQUIRE(owner.HasValue());
            auto saved = SerializeTemplateDocument(owner.Value()->Capture());
            REQUIRE(saved.HasValue());
            const std::array leases{UiTemplateCatalogLease{Reference(1), catalog}};
            auto reopened = DeserializeTemplateDocument(saved.Value(), leases, catalog, UiTemplateLimits{});
            REQUIRE(reopened.HasValue());
            CHECK(reopened.Value()->Capture().Source().instances.front().arguments.front().value.index() == value.index());
            CHECK(SerializeTemplateDocument(reopened.Value()->Capture()).Value() == saved.Value());
            if (TemplateValueKind(value) == UiTemplateValueKind::LocalizedText)
                CHECK(reopened.Value()->Capture().Expansions().front().localizedTexts.front().property == Identity<UiStylePropertyId>(1));
        }
    }

    TEST_CASE("Rich authored attachments survive persistence flatten cook and owner shutdown", "[runtime_ui][template][headless]") {
        auto catalog = ValueCatalog(Message());
        auto source = RichDocument();
        auto owner = UiTemplateDocumentOwner::Create(std::move(source), catalog);
        REQUIRE(owner.HasValue());
        auto saved = SerializeTemplateDocument(owner.Value()->Capture());
        REQUIRE(saved.HasValue());
        const std::array leases{UiTemplateCatalogLease{Reference(1), catalog}};
        auto reopened = DeserializeTemplateDocument(saved.Value(), leases, catalog, UiTemplateLimits{});
        REQUIRE(reopened.HasValue());
        const auto snapshot = reopened.Value()->Capture();
        Sha256Digest manifest;
        manifest.bytes.front() = 1;
        const auto first = snapshot.Cook({manifest});
        REQUIRE(first.HasValue());
        const auto second = snapshot.Cook({manifest});
        REQUIRE(second.HasValue());
        CHECK(first.Value().cacheIdentity == second.Value().cacheIdentity);
        CHECK(first.Value().richPayload == second.Value().richPayload);
        CHECK(first.Value().localizedTexts.size() == 2);
        CHECK(first.Value().localizedTexts.front().property == Identity<UiStylePropertyId>(1));
        manifest.bytes.front() = 2;
        CHECK(snapshot.Cook({manifest}).Value().cacheIdentity != first.Value().cacheIdentity);
        CHECK(snapshot.Cook({}).HasError());
        auto retiredOwner = std::move(reopened).Value();
        retiredOwner->Shutdown();
        retiredOwner.reset();
        CHECK(snapshot.Cook({manifest}).HasValue());
        CHECK(first.Value().bindings.front().target.element == Identity<UiElementId>(99));
        CHECK(CookedUiDocument::Decode(first.Value().ordinary.Payload()).HasValue());
    }

    TEST_CASE("Flattened template cook input owns ordinary resolved values independently of source lifetime",
              "[runtime_ui][template][headless]") {
        auto owner = Owner();
        auto flattened = owner->Capture().Flatten();
        REQUIRE(flattened.HasValue());
        const auto sourceRevision = owner->Capture().Source().ordinary.Revision();
        CHECK(flattened.Value().ordinary.Revision() == sourceRevision);
        CHECK(flattened.Value().ordinary.Elements().size() == 2);
        CHECK(flattened.Value().acceptedDependencies.size() == 1);
        CHECK(owner->Capture().Source().ordinary.Elements().size() == 1);
        CHECK(owner->Capture().Source().instances.size() == 1);
        owner->Shutdown();
        owner.reset();
        const auto &element = flattened.Value().ordinary.Elements().back();
        REQUIRE(element.properties.size() == 1);
        CHECK(std::get<std::string>(element.properties.front().value) == "authored value");
        const auto cooked = CookedUiDocument::Cook(flattened.Value().ordinary);
        REQUIRE(cooked.HasValue());
        CHECK(cooked.Value().Elements().size() == 2);
    }

    TEST_CASE("Linked insertion and recursive detach publish only complete owner transactions", "[runtime_ui][template][headless]") {
        auto owner = Owner();
        auto instance = Document().instances.front();
        instance.id = Identity<UiTemplateInstanceId>(2);
        auto inserted = owner->PrepareInsert(instance, Catalog(Source(1)));
        REQUIRE(inserted.HasValue());
        CHECK(owner->Capture().Source().instances.size() == 1);
        REQUIRE(owner->Commit(inserted.Value()).HasValue());
        CHECK(owner->Capture().Source().instances.size() == 2);
        const auto durableBefore = SerializeTemplateDocument(owner->Capture());
        REQUIRE(durableBefore.HasValue());
        CHECK(owner->PrepareInsert(instance, Catalog(Source(1))).HasError());
        CHECK(SerializeTemplateDocument(owner->Capture()).Value() == durableBefore.Value());
        const auto before = owner->Capture();
        const auto derived = before.Expansions().back().root;
        const std::array invalid{UiTemplateDetachIdentity{derived, Identity<UiElementId>(99)}};
        CHECK(owner->PrepareDetach(instance.id, invalid).HasError());
        CHECK(owner->Capture().Source().ordinary.Revision() == before.Source().ordinary.Revision());
        CHECK(SerializeTemplateDocument(owner->Capture()).Value() == durableBefore.Value());
        const std::array allocation{UiTemplateDetachIdentity{derived, Identity<UiElementId>(50)}};
        auto detached = owner->PrepareDetach(instance.id, allocation);
        REQUIRE(detached.HasValue());
        CHECK(owner->Capture().Source().instances.size() == 2);
        CHECK(SerializeTemplateDocument(owner->Capture()).Value() == durableBefore.Value());
        REQUIRE(owner->Commit(detached.Value()).HasValue());
        const auto after = owner->Capture();
        CHECK(after.Source().instances.size() == 1);
        CHECK(after.Source().ordinary.Elements().size() == 2);
        CHECK(owner->UpdateAvailable(instance.id, Reference(2)).HasError());
        CHECK(std::get<std::string>(after.Source().ordinary.Elements().back().properties.front().value) == "authored value");
        REQUIRE(owner->Undo().HasValue());
        CHECK(owner->Capture().Source().instances.size() == 2);
        CHECK(owner->Capture().Expansions().back().root == derived);
    }

    TEST_CASE("Template owner rejects another thread before reading or changing authority", "[runtime_ui][template][headless]") {
        auto owner = Owner();
        const auto catalog = Catalog(Source(2));
        const auto prepared = owner->PrepareRebase(Identity<UiTemplateInstanceId>(1), Reference(2), catalog);
        REQUIRE(prepared.HasValue());
        const auto before = owner->Capture();
        const auto durable = SerializeTemplateDocument(before).Value();
        std::array<bool, 8> rejected{};
        bool detachedSnapshotReadable{};
        std::thread other{[&] {
            const auto wrong = [](const auto &result) {
                return result.HasError() && result.ErrorValue().code.Value() == UiErrors::TemplateDocumentWrongThread.code.Value();
            };
            rejected = {wrong(owner->PrepareRebase(Identity<UiTemplateInstanceId>(1), Reference(2), catalog)),
                        wrong(owner->PrepareInsert(Document().instances.front(), catalog)),
                        wrong(owner->PrepareInsertIndependent(Document().instances.front(), catalog, {})),
                        wrong(owner->PrepareDetach(Identity<UiTemplateInstanceId>(1), {})),
                        wrong(owner->PrepareUndo()),
                        wrong(owner->CanCommit(prepared.Value())),
                        wrong(owner->Commit(prepared.Value())),
                        wrong(owner->UpdateAvailable(Identity<UiTemplateInstanceId>(1), Reference(2)))};
            detachedSnapshotReadable = SerializeTemplateDocument(before).HasValue();
        }};
        other.join();
        CHECK(std::ranges::all_of(rejected, [](const bool value) {
            return value;
        }));
        CHECK(detachedSnapshotReadable);
        CHECK(SerializeTemplateDocument(owner->Capture()).Value() == durable);
        REQUIRE(owner->Commit(prepared.Value()).HasValue());
    }

    TEST_CASE("Prepared undo preserves both publication and history until exact owner commit", "[runtime_ui][template][headless]") {
        auto owner = Owner();
        CHECK(owner->PrepareUndo().HasError());
        auto instance = Document().instances.front();
        instance.id = Identity<UiTemplateInstanceId>(2);
        const auto inserted = owner->PrepareInsert(instance, Catalog(Source(1)));
        REQUIRE(inserted.HasValue());
        REQUIRE(owner->Commit(inserted.Value()).HasValue());
        const auto current = SerializeTemplateDocument(owner->Capture()).Value();
        CancellationSource cancellation;
        const auto cancelled = owner->PrepareUndo(cancellation.Token());
        REQUIRE(cancelled.HasValue());
        cancellation.RequestCancellation();
        CHECK(owner->Commit(cancelled.Value()).HasError());
        CHECK(SerializeTemplateDocument(owner->Capture()).Value() == current);
        const auto prepared = owner->PrepareUndo();
        REQUIRE(prepared.HasValue());
        CHECK(prepared.Value().Preview().Source().instances.size() == 1);
        CHECK(owner->Capture().Source().instances.size() == 2);
        REQUIRE(owner->CanCommit(prepared.Value()).HasValue());
        const auto restored = owner->Commit(prepared.Value());
        REQUIRE(restored.HasValue());
        CHECK(restored.Value().Source().instances.size() == 1);
        CHECK(restored.Value().Source().ordinary.Revision() > inserted.Value().Preview().Source().ordinary.Revision());
        CHECK(owner->Commit(prepared.Value()).HasError());
        CHECK(owner->PrepareUndo().HasError());
    }

    TEST_CASE("History eviction retains catalog retirement outside the publication safe point", "[runtime_ui][template][headless]") {
        UiTemplateLimits limits;
        limits.maximumUndoEntries = 1;
        auto source = Document();
        source.instances.clear();
        auto created = UiTemplateDocumentOwner::Create(std::move(source), Catalog(Source(1), limits));
        REQUIRE(created.HasValue());
        auto owner = std::move(created).Value();
        std::size_t retired{};
        {
            const auto catalog = Catalog(Source(1), limits);
            const std::shared_ptr<const UiTemplateCatalog> tracked{catalog.get(), [catalog, &retired](const UiTemplateCatalog *) {
                ++retired;
            }};
            const auto inserted = owner->PrepareInsert(Document().instances.front(), tracked);
            REQUIRE(inserted.HasValue());
            REQUIRE(owner->Commit(inserted.Value()).HasValue());
        }
        {
            const auto second = owner->PrepareRebase(Identity<UiTemplateInstanceId>(1), Reference(2), Catalog(Source(2), limits));
            REQUIRE(second.HasValue());
            REQUIRE(owner->Commit(second.Value()).HasValue());
        }
        CHECK(retired == 0);
        {
            const auto third = owner->PrepareRebase(Identity<UiTemplateInstanceId>(1), Reference(3), Catalog(Source(3), limits));
            REQUIRE(third.HasValue());
            REQUIRE(owner->Commit(third.Value()).HasValue());
            CHECK(retired == 0);
        }
        CHECK(retired == 1);
    }

    TEST_CASE("Prepared undo cannot cross a later owner transaction or shutdown", "[runtime_ui][template][headless]") {
        auto owner = Owner();
        auto instance = Document().instances.front();
        instance.id = Identity<UiTemplateInstanceId>(2);
        const auto inserted = owner->PrepareInsert(instance, Catalog(Source(1)));
        REQUIRE(inserted.HasValue());
        REQUIRE(owner->Commit(inserted.Value()).HasValue());
        const auto prepared = owner->PrepareUndo();
        REQUIRE(prepared.HasValue());
        SECTION("later publication") {
            instance.id = Identity<UiTemplateInstanceId>(3);
            const auto next = owner->PrepareInsert(instance, Catalog(Source(1)));
            REQUIRE(next.HasValue());
            REQUIRE(owner->Commit(next.Value()).HasValue());
            CHECK(owner->Commit(prepared.Value()).HasError());
            CHECK(owner->Capture().Source().instances.size() == 3);
            REQUIRE(owner->Undo().HasValue());
            CHECK(owner->Capture().Source().instances.size() == 2);
        }
        SECTION("shutdown") {
            owner->Shutdown();
            CHECK(owner->Commit(prepared.Value()).HasError());
            CHECK(prepared.Value().Preview().Source().instances.size() == 1);
        }
    }

    TEST_CASE("Detach preserves host slot identity while freshly allocating template owned content", "[runtime_ui][template][headless]") {
        auto templateSource = Source(1);
        templateSource.slots.push_back({Identity<UiTemplateSlotId>(1), templateSource.root, 0, 4, {Kind()}, {}});
        auto catalog = Catalog(std::move(templateSource));
        auto source = Document();
        source.instances.front().slots.push_back(
            {Identity<UiTemplateSlotId>(1), {{Identity<UiElementId>(10), {}, Kind(), {{"text", std::string{"preserved slot"}}}, {}}}, {}});
        auto owner = UiTemplateDocumentOwner::Create(std::move(source), catalog);
        REQUIRE(owner.HasValue());
        const auto snapshot = owner.Value()->Capture();
        REQUIRE(snapshot.Expansions().front().templateOwnedElements.size() == 1);
        const std::array allocation{UiTemplateDetachIdentity{snapshot.Expansions().front().root, Identity<UiElementId>(50)}};
        auto prepared = owner.Value()->PrepareDetach(Identity<UiTemplateInstanceId>(1), allocation);
        REQUIRE(prepared.HasValue());
        REQUIRE(owner.Value()->Commit(prepared.Value()).HasValue());
        const auto detached = owner.Value()->Capture();
        CHECK(detached.Source().instances.empty());
        const auto slot = std::ranges::find(detached.Source().ordinary.Elements(), Identity<UiElementId>(10), &UiDocumentElement::id);
        REQUIRE(slot != detached.Source().ordinary.Elements().end());
        CHECK(slot->parent == Identity<UiElementId>(50));
        CHECK(std::get<std::string>(slot->properties.front().value) == "preserved slot");
        REQUIRE(owner.Value()->Undo().HasValue());
        CHECK(owner.Value()->Capture().Source().instances.front().slots.front().elements.front().id == Identity<UiElementId>(10));
    }

    TEST_CASE("Independent insertion cancellation and retirement preserve original authority", "[runtime_ui][template][headless]") {
        auto owner = Owner();
        auto record = Document().instances.front();
        record.id = Identity<UiTemplateInstanceId>(2);
        const std::array lineage{record.id};
        const auto derived =
            DeriveTemplateElementId(owner->Capture().Source().ordinary.Id(), lineage, Identity<UiTemplateLocalElementId>(1));
        REQUIRE(derived.HasValue());
        const std::array allocation{UiTemplateDetachIdentity{derived.Value(), Identity<UiElementId>(50)}};
        CancellationSource cancellation;
        auto cancelled = owner->PrepareInsertIndependent(record, Catalog(Source(1)), allocation, cancellation.Token());
        REQUIRE(cancelled.HasValue());
        cancellation.RequestCancellation();
        CHECK(owner->Commit(cancelled.Value()).HasError());
        CHECK(owner->Capture().Source().ordinary.Elements().size() == 1);
        auto prepared = owner->PrepareInsertIndependent(record, Catalog(Source(1)), allocation);
        REQUIRE(prepared.HasValue());
        REQUIRE(owner->Commit(prepared.Value()).HasValue());
        CHECK(owner->Capture().Source().instances.size() == 1);
        CHECK(owner->Capture().Source().ordinary.Elements().size() == 2);
        REQUIRE(owner->Undo().HasValue());
        CHECK(owner->Capture().Source().ordinary.Elements().size() == 1);
        owner->Shutdown();
        CHECK(owner->Commit(prepared.Value()).HasError());
        CHECK(prepared.Value().Preview().Source().ordinary.Elements().size() == 2);
    }

    TEST_CASE("Linked source persists canonical authority and reopens exact mixed accepted revisions", "[runtime_ui][template][headless]") {
        auto first = Catalog(Source(1));
        auto second = Catalog(Source(2));
        auto source = Document();
        auto other = source.instances.front();
        other.id = Identity<UiTemplateInstanceId>(2);
        other.accepted = Reference(2);
        source.instances.insert(source.instances.begin(), other);
        const std::array leases{UiTemplateCatalogLease{Reference(1), first}, UiTemplateCatalogLease{Reference(2), second}};
        auto owner = UiTemplateDocumentOwner::Create(std::move(source), leases, first, UiTemplateLimits{});
        REQUIRE(owner.HasValue());
        auto saved = SerializeTemplateDocument(owner.Value()->Capture());
        REQUIRE(saved.HasValue());
        CHECK(saved.Value().find("authored value") != std::string::npos);
        auto reopened = DeserializeTemplateDocument(saved.Value(), leases, first, UiTemplateLimits{});
        REQUIRE(reopened.HasValue());
        auto resaved = SerializeTemplateDocument(reopened.Value()->Capture());
        REQUIRE(resaved.HasValue());
        CHECK(resaved.Value() == saved.Value());
        CHECK(reopened.Value()->Capture().Expansions().front().elements == owner.Value()->Capture().Expansions().front().elements);
        CHECK(reopened.Value()->Capture().Expansions().back().elements == owner.Value()->Capture().Expansions().back().elements);
        CHECK(DeserializeTemplateDocument(saved.Value(), std::span{leases}.first(1), first, UiTemplateLimits{}).HasError());
        UiTemplateLimits changed;
        changed.maximumElements = 100;
        CHECK(DeserializeTemplateDocument(saved.Value(), leases, first, changed).HasError());
        CHECK(DeserializeTemplateDocument("{\"schema\":1,\"schema\":1}", leases, first, UiTemplateLimits{}).HasError());
        CHECK(DeserializeTemplateDocument("{\"schema\":2}", leases, first, UiTemplateLimits{}).HasError());
        CHECK(DeserializeTemplateDocument(std::string(4 * 1024 * 1024 + 1, 'x'), leases, first, UiTemplateLimits{}).HasError());
    }

    TEST_CASE("Template document admission applies custom aggregate limits before candidate storage", "[runtime_ui][template][headless]") {
        SECTION("Ordinary and expanded elements share one admitted limit") {
            UiTemplateLimits limits;
            limits.maximumElements = 1;
            const auto catalog = Catalog(Source(1), limits);
            CHECK(UiTemplateDocumentOwner::Create(Document(), catalog).HasError());
        }
        SECTION("An ordinary document larger than admitted limits cannot underflow the remaining budget") {
            UiTemplateLimits limits;
            limits.maximumElements = 2;
            const auto catalog = Catalog(Source(1), limits);
            const std::array elements{UiDocumentElement{Identity<UiElementId>(99), {}, Kind(), {}, {}},
                                      UiDocumentElement{Identity<UiElementId>(98), Identity<UiElementId>(99), Kind(), {}, {}},
                                      UiDocumentElement{Identity<UiElementId>(97), Identity<UiElementId>(99), Kind(), {}, {}}};
            CHECK(UiTemplateDocumentOwner::Create({OwnerOrdinaryDocument(elements), {}}, catalog).HasError());
        }
        SECTION("Ordinary and template dependency occurrences share one admitted limit") {
            UiTemplateLimits limits;
            limits.maximumDependencies = 1;
            const auto catalog = Catalog(Source(1), limits);
            auto source = Document();
            const std::array dependencies{
                UiAssetDependency{Reference(1).asset.asset, Assets::AssetTypeId::Parse("ui.template").Value(), true}};
            CHECK(UiTemplateDocumentOwner::Create({OwnerOrdinaryDocument(source.ordinary.Elements(), dependencies), source.instances},
                                                  catalog)
                      .HasError());
        }
    }

    TEST_CASE("Ordinary and expanded properties share the admitted aggregate policy", "[runtime_ui][template][headless]") {
        UiTemplateLimits limits;
        limits.maximumProperties = 1;
        const auto catalog = Catalog(Source(1), limits);
        auto source = Document();
        auto root = source.ordinary.Elements().front();
        root.properties.push_back({"ordinary", std::string{"value"}});
        const std::array elements{std::move(root)};
        CHECK(UiTemplateDocumentOwner::Create({OwnerOrdinaryDocument(elements), source.instances}, catalog).HasError());
    }

    TEST_CASE("Template document rebasing cannot weaken the original aggregate policy", "[runtime_ui][template][headless]") {
        UiTemplateLimits limits;
        limits.maximumElements = 2;
        auto owner = UiTemplateDocumentOwner::Create(Document(), Catalog(Source(1), limits));
        REQUIRE(owner.HasValue());
        auto target = Source(2);
        target.elements.push_back({Identity<UiTemplateLocalElementId>(2), target.root, Kind(), {}});
        CHECK(owner.Value()->PrepareRebase(Identity<UiTemplateInstanceId>(1), Reference(2), Catalog(std::move(target))).HasError());
        CHECK(owner.Value()->Capture().Source().ordinary.Revision().Value() == 1);
    }

    TEST_CASE("Catalog admission rejects zero undo retention before any document owner is admitted", "[runtime_ui][template][headless]") {
        UiTemplateLimits limits;
        limits.maximumUndoEntries = 0;
        CHECK_FALSE(limits.IsValid());
        UiTemplateSchemaSnapshot schemas{{Kind()}, {{Kind(), Identity<UiStylePropertyId>(1), UiTemplateValueKind::Text, "text", {}}}, {}};
        CHECK(UiTemplateCatalog::Create({Source(1)}, std::move(schemas), {}, limits).HasError());
    }

    TEST_CASE("Rejected template rebase leaves complete accepted authority and projection unchanged", "[runtime_ui][template][headless]") {
        auto owner = Owner();
        const auto before = owner->Capture();
        auto target = Source(2);
        target.parameters.front().maximumTextBytes = 1;
        const auto catalog = Catalog(std::move(target));
        REQUIRE(owner->UpdateAvailable(Identity<UiTemplateInstanceId>(1), Reference(2)).Value());
        CHECK(owner->PrepareRebase(Identity<UiTemplateInstanceId>(1), Reference(2), catalog).HasError());
        const auto after = owner->Capture();
        CHECK(after.Source().ordinary.Revision() == before.Source().ordinary.Revision());
        CHECK(after.Source().instances.front().accepted.revision == before.Source().instances.front().accepted.revision);
        CHECK(std::get<std::string>(after.Source().instances.front().arguments.front().value) == "authored value");
        CHECK(after.Expansions().front().elements == before.Expansions().front().elements);
    }

    TEST_CASE("Explicit rebase is atomic and undo restores authority with monotonic revision", "[runtime_ui][template][headless]") {
        auto owner = Owner();
        const auto before = owner->Capture();
        const auto catalog = Catalog(Source(2));
        const auto prepared = owner->PrepareRebase(Identity<UiTemplateInstanceId>(1), Reference(2), catalog);
        REQUIRE(prepared.HasValue());
        CHECK(owner->Capture().Source().ordinary.Revision().Value() == 1);
        CHECK(prepared.Value().Preview().Source().ordinary.Revision().Value() == 2);
        const auto committed = owner->Commit(prepared.Value());
        REQUIRE(committed.HasValue());
        CHECK(committed.Value().Source().instances.front().accepted.revision == Reference(2).revision);
        CHECK(std::get<std::string>(committed.Value().Source().instances.front().arguments.front().value) == "authored value");
        CHECK(committed.Value().Expansions().front().elements == before.Expansions().front().elements);
        CHECK(owner->Commit(prepared.Value()).HasError());
        const auto undone = owner->Undo();
        REQUIRE(undone.HasValue());
        CHECK(undone.Value().Source().ordinary.Revision().Value() == 3);
        CHECK(undone.Value().Source().instances.front().accepted.revision == Reference(1).revision);
        CHECK(undone.Value().Expansions().front().elements == before.Expansions().front().elements);
        CHECK(owner->Undo().HasError());
    }

    TEST_CASE("Cancelled foreign and retired owner candidates cannot publish and snapshots retain their data",
              "[runtime_ui][template][headless]") {
        auto owner = Owner();
        auto foreign = Owner();
        CancellationSource cancellation;
        const auto prepared =
            owner->PrepareRebase(Identity<UiTemplateInstanceId>(1), Reference(2), Catalog(Source(2)), cancellation.Token());
        REQUIRE(prepared.HasValue());
        CHECK(foreign->Commit(prepared.Value()).HasError());
        cancellation.RequestCancellation();
        CHECK(owner->Commit(prepared.Value()).HasError());
        CHECK(owner->Capture().Source().ordinary.Revision().Value() == 1);
        const auto late = owner->PrepareRebase(Identity<UiTemplateInstanceId>(1), Reference(2), Catalog(Source(2)));
        REQUIRE(late.HasValue());
        const auto detached = late.Value().Preview();
        owner->Shutdown();
        CHECK(owner->Commit(late.Value()).HasError());
        CHECK(owner->PrepareRebase(Identity<UiTemplateInstanceId>(1), Reference(2), Catalog(Source(2))).HasError());
        owner.reset();
        CHECK(detached.Source().ordinary.Revision().Value() == 2);
        CHECK(detached.Expansions().front().elements.size() == 1);
    }
}  // namespace Horo::UiTemplates
