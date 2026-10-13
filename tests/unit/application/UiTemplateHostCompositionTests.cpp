#include "../../support/AllocationProbe.h"
#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiTemplateHostFixedMessages.h"
#include "UiTemplateHostPublicationFixture.h"

#include <algorithm>

namespace Horo::Application::TemplateHostTests {
    namespace {
        using namespace Runtime::Ui;
        using namespace UiTemplates::TestSupport;

        template <typename T> T Revision(const std::uint64_t value = 1) {
            return T::Create(value).Value();
        }

        /** @brief Real layout owner input adapter; its borrowed evaluator never survives the synchronous update. */
        class Layout final : public UiTemplateLayoutComposer, private UiLayoutEvaluator {
        public:
            bool fail{};
            bool staleStyle{};

            Result<UiLayoutSnapshot> Prepare(const UiElementTree &tree, const UiComputedStyleSnapshot &styles,
                                             std::span<const UiTemplateResolvedText> texts, const UiBindingStore *bindings,
                                             UiLayoutEngine &engine, const CancellationToken &) const override {
                if (fail)
                    return Result<UiLayoutSnapshot>::Failure(MakeError(UiErrors::LayoutInvalid));
                CHECK_FALSE(texts.empty());
                REQUIRE(bindings);
                CHECK(bindings->Find(tree, Identity<UiBindingId>(9)));
                const UiLayoutSourceRevisions
                    sources{tree.SourceDocumentRevision(),
                            tree.Revision(),
                            Revision<UiLayoutContentRevision>(),
                            Revision<UiLayoutStyleRevision>(styles.Descriptor().geometry.Value() + (staleStyle ? 1 : 0)),
                            Revision<UiLayoutIntrinsicRevision>(),
                            Revision<UiLayoutCanvasRevision>(),
                            Revision<UiLayoutPolicyRevision>()};
                return engine.Update(tree, {sources, {{0, 0}, {120, 60}}, {{0, 0}, {120, 60}}, this, UiCanvasScaleFactor{1, 1}});
            }

        private:
            Result<void> ResolveChildConstraints(const UiLayoutChildConstraintRequest &request,
                                                 std::span<UiLayoutConstraints> output) const override {
                std::ranges::fill(output, request.constraints);
                return Result<void>::Success();
            }

            Result<UiLayoutMeasurement> Measure(const UiLayoutMeasureRequest &request) const override {
                return Result<UiLayoutMeasurement>::Success({request.constraints.maximum, false, false});
            }

            Result<UiLayoutArrangement> Arrange(const UiLayoutArrangeRequest &request, std::span<UiLogicalRect> children) const override {
                std::ranges::fill(children, request.assignedContent);
                const auto rectangle = request.assignedContent;
                return Result<UiLayoutArrangement>::Success(
                    {rectangle, rectangle, rectangle, rectangle, rectangle, rectangle, NoUiBaseline});
            }
        };

        /** @brief Adds actual resource/sidecar files to the fixture's existing Assets registry before capture. */
        UiAssetDependency RegisterResource(PublicationFixture &fixture, const std::uint8_t marker, const std::string_view typeName,
                                           const std::string_view path, const std::string_view contents) {
            const auto asset = Assets::AssetId::FromBytes(Identity<UiDocumentId>(marker).Bytes());
            const auto type = Assets::AssetTypeId::Parse(typeName).Value();
            const auto metadata = std::string{path} + ".horo";
            fixture.source.Write(path, contents);
            fixture.source.Write(metadata,
                                 nlohmann::json{{"schemaVersion", 1}, {"assetId", asset.ToString()}, {"assetType", type.Value()}}.dump());
            const auto snapshot = fixture.source.registry.Snapshot();
            std::vector<Assets::AssetRecord> records{snapshot.Records().begin(), snapshot.Records().end()};
            records.emplace_back(asset, type, ProjectPath::Parse(path).Value(), ProjectPath::Parse(metadata).Value());
            const auto publication = fixture.source.registry.Publish(std::move(records));
            REQUIRE(publication.status == Assets::AssetRegistryBuildStatus::Complete);
            return {asset, type, true};
        }

        /** @brief Captures a mixed localized/style/binding source and a real catalog resource into the existing fixture. */
        void RichSource(PublicationFixture &fixture) {
            auto provisional = ValueCatalog(Message());
            auto source = *provisional->Resolve(Reference(1)).Value();
            auto schemas = provisional->Schemas();
            const auto style = Identity<UiStylePropertyId>(2);
            schemas.properties.emplace_back(Kind(), style, UiTemplates::UiTemplateValueKind::Style, "",
                                            UiStylePropertyDescriptor{.id = style,
                                                                      .category = UiStyleValueCategory::Color,
                                                                      .defaultValue = UiStyleColor{}});
            source.elements.front().properties.emplace_back(style, UiStyleValue{UiStyleColor{0.2F, 0.4F, 0.6F, 1.0F}});
            auto catalog = UiTemplates::UiTemplateCatalog::Create({source}, schemas, {});
            REQUIRE(catalog.HasValue());
            const auto bytes = UiTemplates::SerializeTemplateSource(catalog.Value(), Reference(1)).Value();
            fixture.source.reference.revision = ComputeSha256(std::as_bytes(std::span{bytes}));
            source.dependency.revision = fixture.source.reference.revision;
            auto pinned = UiTemplates::UiTemplateCatalog::Create({std::move(source)}, std::move(schemas), {});
            REQUIRE(pinned.HasValue());
            fixture.source.catalog = std::make_shared<const UiTemplates::UiTemplateCatalog>(std::move(pinned).Value());
            auto authored = RichDocument();
            authored.instances.front().accepted = fixture.source.reference;
            fixture.source.owner =
                std::move(UiTemplates::UiTemplateDocumentOwner::Create(std::move(authored), fixture.source.catalog)).Value();
            fixture.source.WriteSource(bytes);
            fixture.resources.messages = {
                {Message().Key(), RegisterResource(fixture, 8, "game.messages", "assets/messages.json", "validated game messages")}};
        }

        /** @brief Supplies the existing real registry with an optional concrete token for host-resolution coverage. */
        RuntimeStyleRegistry Registry(const bool token) {
            UiStyleRegistryDefinition
                definition{{{Identity<UiStylePropertyId>(2), UiStyleValueCategory::Color, UiStyleValue{UiStyleColor{}}, {.paint = true}}},
                           {{.id = Identity<RuntimeStyleAssetId>(20)}}};
            if (token)
                definition.assets.front().tokens.emplace_back(Identity<UiStyleTokenId>(30), UiStyleValueCategory::Color,
                                                              UiStyleValueSource::Literal(
                                                                  UiStyleValue{UiStyleColor{0.7F, 0.2F, 0.1F, 1.0F}}),
                                                              false);
            auto registry = RuntimeStyleRegistry::Create(std::move(definition), Revision<RuntimeStyleGeneration>());
            REQUIRE(registry.HasValue());
            return std::move(registry).Value();
        }

        /** @brief Complete root-owned domain inputs borrowed only during one synchronous composition. */
        struct Composition final {
            RuntimeStyleRegistry registry;
            UiBindingProviderSchema provider{Provider()};
            Layout layout;
            FixedMessages messages;
            std::vector<UiStyleElementInput> styles;
            std::vector<UiAccessibilityNodeInput> semantics;
            std::vector<UiTemplateAccessibleTextTarget> accessible;
            std::array<UiBindingPropertyUpdate, 1> values{{{0, UiBindingValue{0.75}}}};
            std::array<UiBindingProviderRegistration, 1> providers;
            std::array<UiTemplateBindingResolution, 1> bindings;
            UiTemplateCanvasComposition input;

            explicit Composition(UiReloadGeneration &runtime, const UiTemplateHostCook &cook, const bool token = false)
                : registry(Registry(token)) {
                auto &canvas = *runtime.Canvas(runtime.Canvases().front().id);
                Bindings(canvas.tree);
                CanvasInputs(canvas, cook);
                Configure(canvas);
            }

            /** @brief Registers one exact incarnation and schema snapshot, not a provider lookup callback. */
            void Bindings(const UiElementTree &tree) {
                const UiBindingProviderInstanceId providerId{tree.Instance().ownership, 20, 1};
                providers = {
                    {{providerId, UiBindingProviderScopeKind::GameInstance, &provider, Revision<UiBindingSnapshotRevision>(), values}}};
                bindings = {{{Identity<UiBindingId>(9), providerId, {}}}};
            }

            /** @brief Supplies actual focus and complete unfiltered semantic declarations for every cooked element. */
            void CanvasInputs(UiReloadCanvas &canvas, const UiTemplateHostCook &cook) {
                const auto &tree = canvas.tree;
                const auto revision = tree.SourceDocumentRevision().Value();
                std::vector<UiFocusNodeDescriptor> focus;
                for (const auto &element : cook.Document().ordinary.Elements()) {
                    const auto handle = tree.Find(element.id).Value();
                    styles.emplace_back(handle, Identity<RuntimeStyleAssetId>(20));
                    semantics.emplace_back(UiAccessibilityNodeInput{.element = element.id, .role = UiAccessibilityRole::Group});
                    focus.emplace_back(UiFocusNodeDescriptor{handle,
                                                             element.id,
                                                             element.parent,
                                                             {},
                                                             UiFocusBringIntoViewPolicy::Nearest,
                                                             true,
                                                             true,
                                                             true});
                }
                const UiFocusOwnerContext owner{tree.Instance(),
                                                tree.Canvas(),
                                                tree.SourceDocument(),
                                                tree.SourceDocumentRevision(),
                                                tree.Revision(),
                                                Revision<UiInteractionRevision>(revision),
                                                {UiFocusPlayerId{tree.Instance().ownership, 3, 1},
                                                 UiFocusPresentationLayerId{tree.Instance().ownership, 4, 1}}};
                canvas.focus.emplace(std::move(UiFocusGraph::Create({owner, tree.Root().Value().id,
                                                                     UiFocusRecoveryPolicy::AncestorThenDefaultThenFirst, 64, 8, 8},
                                                                    focus))
                                         .Value());
                for (const auto &text : cook.Document().localizedTexts)
                    accessible.emplace_back(text.element, text.property, UiTemplateAccessibleTextField::Name);
            }

            /** @brief Binds explicit domain limits and coherent tree/document/interaction revisions. */
            void Configure(const UiReloadCanvas &canvas) {
                const auto &tree = canvas.tree;
                const auto revision = tree.SourceDocumentRevision().Value();
                input = {canvas.id,
                         &registry,
                         {tree.Instance(), tree.Canvas(), tree.SourceDocument(), 64, 4, 64, 3, Revision<RuntimeStyleGeneration>(),
                          Revision<UiStylePublicationRevision>(revision)},
                         {tree.SourceDocumentRevision(), tree.Revision(), registry.Generation(), Revision<UiStyleContentRevision>(),
                          Revision<UiStylePolicyRevision>(), Revision<UiInteractionRevision>(revision)},
                         styles,
                         providers,
                         bindings,
                         &layout,
                         {tree.Instance(), tree.Canvas(), tree.SourceDocument(), 64, 64, 3, Revision<UiInteractionRevision>(revision)},
                         {tree.Instance(), tree.Canvas(), tree.SourceDocument(), {64, 64, 64, 4096}, 3},
                         semantics,
                         accessible};
            }

            UiTemplateHostCook Compose(UiTemplateHostCook cook, UiReloadGeneration &runtime) const {
                auto result = UiTemplateHost::Compose(std::move(cook), runtime, std::span{&input, 1}, &messages);
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }
        };

        /** @brief Retains real detached owners for independent invalid-snapshot and domain-failure cases. */
        struct CompositionFailure final {
            PublicationFixture fixture;
            UiTemplateHostCook cook;
            UiReloadGeneration runtime;
            Composition input;
            CancellationSource cancellation;
            const UiTemplateLocalizationSnapshotQuery *query;
            const ErrorCodeDescriptor *expected{&UiErrors::LocalizedMessageInvalid};

            CompositionFailure() : cook(Cook(fixture)), runtime(fixture.Generation(cook)), input(runtime, cook), query(&input.messages) {}

            static UiTemplateHostCook Cook(PublicationFixture &fixture) {
                RichSource(fixture);
                return fixture.Cook(fixture.source.owner->Capture());
            }

            void Check() {
                const auto result = UiTemplateHost::Compose(cook, runtime, std::span{&input.input, 1}, query, cancellation.Token());
                REQUIRE(result.HasError());
                CHECK(result.ErrorValue().code.Value() == expected->code.Value());
                CHECK(runtime.Instance().State() == UiRuntimeInstanceState::Prepared);
                CHECK(fixture.source.owner->Capture().Source().ordinary.Revision().Value() == 1);
                CHECK(UiTemplateHost::Create(std::move(fixture.source.owner), std::move(runtime), cook).HasError());
                CHECK(fixture.source.owner != nullptr);
            }
        };

        /** @brief Creates one actual paired rich host with optional authored child focus. */
        struct RichHost final {
            std::unique_ptr<UiTemplateHost> owner;
            UiElementId child;
        };

        RichHost CreateRichHost(PublicationFixture &fixture, const bool focusChild = false) {
            RichSource(fixture);
            auto cook = fixture.Cook(fixture.source.owner->Capture());
            auto runtime = fixture.Generation(cook);
            const auto child = cook.Document().styles.front().element;
            {
                Composition input{runtime, cook};
                if (focusChild) {
                    auto &canvas = *runtime.Canvas(runtime.Canvases().front().id);
                    REQUIRE(canvas.focus->SetFocus(canvas.tree.Find(child).Value()).HasValue());
                }
                cook = input.Compose(std::move(cook), runtime);
            }
            auto created = UiTemplateHost::Create(std::move(fixture.source.owner), std::move(runtime), cook);
            REQUIRE(created.HasValue());
            return {std::move(created).Value(), child};
        }

        /** @brief Composes all rich domains before the sole host performs actual runtime reconciliation. */
        UiTemplateHost::Prepared RichPair(PublicationFixture &fixture, UiTemplateHost &host,
                                          UiTemplates::UiTemplatePreparedTransaction document, const CancellationToken &cancellation = {}) {
            auto cook = fixture.Cook(document.Preview());
            auto runtime = fixture.Generation(cook);
            {
                Composition input{runtime, cook};
                cook = input.Compose(std::move(cook), runtime);
            }
            auto prepared = host.Prepare(std::move(document), std::move(runtime), cook, cancellation);
            REQUIRE(prepared.HasValue());
            return std::move(prepared).Value();
        }

        /** @brief Exercises failure/retry and measures the exact successful rich publication safe point. */
        bool PublishRichPair(UiTemplateHost &host, UiTemplateHost::Prepared &candidate, const CancellationSource &cancellation) {
            SECTION("cancelled rich pair retains both old authorities") {
                cancellation.RequestCancellation();
                const auto failed = host.Commit(candidate, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands);
                REQUIRE(failed.HasError());
                CHECK(failed.ErrorValue().code.Value() == UiErrors::AssetLoadCancelled.code.Value());
                CHECK(host.Capture().Source().ordinary.Revision().Value() == 1);
                CHECK(host.Acquire().Value().Get()->Instance().DocumentRevision().Value() == 1);
                CHECK(host.Acquire().Value().Projections().front().semantics.Descriptor().semanticRevision.Value() == 1);
                return false;
            }
            SECTION("invalid cutoff preserves the pair for retry") {
                CHECK(host.Commit(candidate, static_cast<UiStructuralCommitPoint>(255)).HasError());
                CHECK(host.Capture().Source().ordinary.Revision().Value() == 1);
            }
            SECTION("normal rich paired publication") {
                CHECK_FALSE(cancellation.Token().IsCancellationRequested());
            }
            const auto lease = CommitWithoutAllocation(host, candidate);
            CHECK(lease.Projections().size() == 1);
            return true;
        }
    }  // namespace

    TEST_CASE("Rich template host owns style message binding layout and semantic projections after adapter destruction",
              "[template][host][composition][lifetime]") {
        PublicationFixture fixture;
        RichSource(fixture);
        auto cook = fixture.Cook(fixture.source.owner->Capture());
        auto runtime = fixture.Generation(cook);
        {
            Composition input{runtime, cook};
            std::string label{"Odak"};
            const std::array actions{UiAccessibilityActionInput{UiAccessibilityActionId::Create(12).Value(),
                                                                UiAccessibilityActionKind::Focus,
                                                                UiAccessibilityActionValueKind::None,
                                                                {label, UiAccessibilityTextSource::ResolvedMessage}}};
            const std::array relations{
                UiAccessibilityRelationInput{UiAccessibilityRelationKind::Controls, cook.Document().styles.front().element}};
            input.semantics.front().actions = actions;
            input.semantics.front().relations = relations;
            cook = input.Compose(std::move(cook), runtime);
            label = "changed after capture";
        }
        auto created = UiTemplateHost::Create(std::move(fixture.source.owner), std::move(runtime), cook);
        REQUIRE(created.HasValue());
        auto host = std::move(created).Value();
        auto lease = std::move(host->Acquire()).Value();
        REQUIRE(lease.Projections().size() == 1);
        const auto &projection = lease.Projections().front();
        CHECK(projection.texts.size() == 2);
        CHECK(projection.texts.front().message.text == "Merhaba Ada — 42");
        CHECK(projection.texts.front().message.locale.Value() == "tr-TR");
        const auto child = cook.Document().styles.front().element;
        const auto handle = lease.Get()->Canvases().front().tree.Find(child).Value();
        const auto record = projection.styles.Get(handle).Value();
        CHECK(std::get<UiStyleColor>(projection.styles.Properties(record).front().value).red == 0.2F);
        CHECK(std::get<double>(
                  lease.Get()->Canvases().front().bindings->Find(lease.Get()->Canvases().front().tree, Identity<UiBindingId>(9))->value) ==
              0.75);
        host.reset();
        CHECK(projection.semantics.Text(projection.semantics.Nodes().front().name) == "Merhaba Ada — 42");
        const auto &root = projection.semantics.Nodes().front();
        CHECK(projection.semantics.Text(projection.semantics.Actions(root).front().name) == "Odak");
        CHECK(projection.semantics.Relations(root).size() == 1);
        CHECK(lease.Get()->Canvases().front().layout->Records().size() == 2);
    }

    TEST_CASE("Template host resolves authored style tokens through the pinned real registry", "[template][host][composition][style]") {
        PublicationFixture fixture;
        RichSource(fixture);
        (void)RegisterResource(fixture, 20, "runtime.ui.style", "assets/style.json", "owned runtime style token");
        auto authored = fixture.source.owner->Capture().Source();
        const auto root = Identity<UiElementId>(99);
        authored.styles.emplace_back(root, UiStyleAssignment{Identity<UiStylePropertyId>(2),
                                                             UiStyleValueSource::Token(
                                                                 {Identity<RuntimeStyleAssetId>(20), Identity<UiStyleTokenId>(30)}),
                                                             false});
        auto owner = UiTemplates::UiTemplateDocumentOwner::Create(std::move(authored), fixture.source.catalog);
        REQUIRE(owner.HasValue());
        fixture.source.owner = std::move(owner).Value();
        auto cook = fixture.Cook(fixture.source.owner->Capture());
        auto runtime = fixture.Generation(cook);
        bool token = true;
        SECTION("registered token resolves without a retained registry pointer") {
            CHECK(token);
        }
        SECTION("missing token preserves the owning style failure") {
            token = false;
        }
        {
            Composition input{runtime, cook, token};
            auto composed = UiTemplateHost::Compose(cook, runtime, std::span{&input.input, 1}, &input.messages);
            if (!token) {
                REQUIRE(composed.HasError());
                CHECK(composed.ErrorValue().code.Value() == UiErrors::StyleReferenceInvalid.code.Value());
                CHECK(runtime.Instance().State() == UiRuntimeInstanceState::Prepared);
                return;
            }
            REQUIRE(composed.HasValue());
            cook = std::move(composed).Value();
        }
        auto created = UiTemplateHost::Create(std::move(fixture.source.owner), std::move(runtime), cook);
        REQUIRE(created.HasValue());
        auto lease = created.Value()->Acquire();
        REQUIRE(lease.HasValue());
        const auto handle = lease.Value().Get()->Canvases().front().tree.Find(root).Value();
        const auto &styles = lease.Value().Projections().front().styles;
        CHECK(std::get<UiStyleColor>(styles.Properties(styles.Get(handle).Value()).front().value).red == 0.7F);
    }

    TEST_CASE("Rich template composition failure never activates partial owners", "[template][host][composition][failure]") {
        CompositionFailure failure;
        auto &input = failure.input;
        auto &query = failure.query;
        SECTION("missing fixed snapshot") {
            query = nullptr;
        }
        SECTION("wrong snapshot identity") {
            input.messages.wrongIdentity = true;
        }
        SECTION("wrong message key") {
            input.messages.wrongKey = true;
        }
        SECTION("wrong selected locale") {
            input.messages.wrongLocale = true;
        }
        SECTION("query changes its pinned snapshot during resolution") {
            input.messages.mutateSnapshot = true;
        }
        SECTION("invalid Unicode") {
            input.messages.invalidUtf8 = true;
        }
        SECTION("split Unicode scalar") {
            input.messages.invalidSpan = true;
        }
        failure.Check();
    }

    TEST_CASE("Rich template composition preserves typed domain budget and cancellation failures",
              "[template][host][composition][failure]") {
        CompositionFailure failure;
        auto &input = failure.input;
        auto &expected = failure.expected;
        auto &cancellation = failure.cancellation;
        SECTION("typed argument failure") {
            input.messages.fail = true;
            expected = &UiErrors::LocalizedArgumentInvalid;
        }
        SECTION("output budget") {
            input.input.semanticOwner.limits.textBytes = 1;
            expected = &UiErrors::CapacityExceeded;
        }
        SECTION("localized field substitutions share the whole semantic text arena") {
            input.input.semanticOwner.limits.textBytes = 36;
            input.accessible.emplace_back(input.accessible.front().element, input.accessible.front().property,
                                          UiTemplateAccessibleTextField::Description);
            input.input.accessibleText = input.accessible;
            expected = &UiErrors::CapacityExceeded;
        }
        SECTION("layout domain failure") {
            input.layout.fail = true;
            expected = &UiErrors::LayoutInvalid;
        }
        SECTION("foreign style lineage") {
            input.layout.staleStyle = true;
            expected = &UiErrors::RevisionStale;
        }
        SECTION("cancellation during adapter call") {
            input.messages.cancel = &cancellation;
            expected = &UiErrors::AssetLoadCancelled;
        }
        failure.Check();
    }

    TEST_CASE("Template paired publication derives accessibility from reconciled preserved focus",
              "[template][host][composition][reload]") {
        PublicationFixture fixture;
        auto [host, child] = CreateRichHost(fixture, true);
        auto record = host->Capture().Source().instances.front();
        record.id = Identity<UiTemplates::UiTemplateInstanceId>(2);
        auto document = host->Authoring().PrepareInsert(record, fixture.source.catalog);
        REQUIRE(document.HasValue());
        {
            CancellationSource cancellation;
            auto candidate = RichPair(fixture, *host, std::move(document).Value(), cancellation.Token());
            if (!PublishRichPair(*host, candidate, cancellation))
                return;
        }
        CHECK(host->CollectRetired().Value() == 1);
        auto current = host->Acquire();
        REQUIRE(current.HasValue());
        const auto &projection = current.Value().Projections().front();
        REQUIRE(projection.semantics.FocusState());
        const auto &canvas = current.Value().Get()->Canvases().front();
        CHECK(canvas.focus->CurrentFocus().Value()->id == child);
        const auto node = projection.semantics.Get(projection.semantics.Find(child).Value()).Value();
        CHECK(node.state.Has(UiAccessibilityStateFlag::Focused));
        CHECK(projection.semantics.Descriptor().semanticRevision.Value() == 2);
    }

    TEST_CASE("Rich template detach undo and shutdown retain complete typed historical projections",
              "[template][host][composition][reload][lifetime]") {
        PublicationFixture fixture;
        auto [host, child] = CreateRichHost(fixture, true);
        {
            auto detached = RichPair(fixture, *host, fixture.Detach(*host));
            REQUIRE(host->Commit(detached, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands).HasValue());
        }
        CHECK(host->Capture().Source().instances.empty());
        auto historical = std::move(host->Acquire()).Value();
        CHECK(historical.CookedDocument().localizedTexts.size() == 2);
        CHECK(historical.Projections().front().texts.size() == 2);
        {
            auto undo = host->Authoring().PrepareUndo();
            REQUIRE(undo.HasValue());
            auto restored = RichPair(fixture, *host, std::move(undo).Value());
            REQUIRE(host->Commit(restored, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands).HasValue());
        }
        auto current = std::move(host->Acquire()).Value();
        CHECK(host->Capture().Source().instances.size() == 1);
        CHECK(current.Get()->Instance().DocumentRevision().Value() == 3);
        CHECK(current.Get()->Canvases().front().tree.Find(child).HasValue());
        CHECK_FALSE(host->IsCurrent(historical).Value());
        host->Shutdown();
        CHECK(host->Acquire().HasError());
        CHECK_FALSE(host->IsCurrent(current).Value());
        host.reset();
        CHECK(historical.Get()->Instance().State() == UiRuntimeInstanceState::Retiring);
        CHECK(current.Projections().front().texts.front().message.text == "Merhaba Ada — 42");
        CHECK(current.CookedDocument().ordinary.SourceRevision().Value() == 3);
        CHECK(current.Get()->Canvases().front().layout->Records().size() == 2);
    }
}  // namespace Horo::Application::TemplateHostTests
