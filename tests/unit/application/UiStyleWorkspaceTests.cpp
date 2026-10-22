#include "support/UiStyleWorkspaceFixture.h"

#include <algorithm>
using namespace Horo;
using namespace Horo::Editor;
using namespace Horo::Runtime::Ui;
using namespace Horo::UiStyleTest;

namespace {
    /** @brief Produce the complete authored edit used to verify transaction and persistence preservation. */
    UiElementPresentation EditedPresentation(const UiElementPresentation &baseline) {
        auto value = baseline;
        value.inlineStyle = {
            {CoreUiStylePropertyId(UiCoreStyleProperty::Fill), UiStyleValueSource::Literal(UiStyleColor{0.8F, 0.2F, 0.1F, 1}), false}};
        value.state.focusable = false;
        value.accessibility =
            UiElementAccessibility{.role = UiAccessibilityRole::Application,
                                   .name = UiLocalizedText::Create(UiMessageKey::Create("game.ui", "canvas").Value(), "Accessible canvas")
                                               .Value()};
        return value;
    }
}  // namespace

TEST_CASE("Real tracked style assets cook load and activate through the actual workspace factory", "[application][ui][style][pipeline]") {
    Project project;
    const auto before = project.resources->Capture();
    REQUIRE(before.themes);
    REQUIRE(before.themes->size() == 1);
    CHECK(before.themes->front().id.Bytes() == project.styleAsset.Bytes());
    CHECK(before.themes->front().label == "assets/tema_日本 style.uistyle");
    CHECK_FALSE(before.StyleGeneration().IsValid());
    project.Prepare();
    const auto first = project.resources->Capture();
    REQUIRE(first.styles);
    CHECK(first.theme == project.style.style.id);
    CHECK(first.resourceLease != nullptr);
    CHECK(first.styles->HasAsset(project.style.style.id));
    const auto firstReceipt = std::static_pointer_cast<const Application::UiStyleWorkspaceCandidate>(first.resourceLease);
    CHECK(firstReceipt->cookedAssets == 1);
    CHECK(firstReceipt->cacheHits == 0);
    const auto policy = CreateCoreUiStyleSchema({}).Value();
    CHECK(std::ranges::equal(first.styles->Properties(), policy.properties, [](const auto &a, const auto &b) {
        return a.id == b.id && a.category == b.category && a.maximumEnumValue == b.maximumEnumValue;
    }));
    project.Prepare();
    const auto cached = project.resources->Capture();
    const auto cachedReceipt = std::static_pointer_cast<const Application::UiStyleWorkspaceCandidate>(cached.resourceLease);
    CHECK(cachedReceipt->cookedAssets == 0);
    CHECK(cachedReceipt->cacheHits == 1);
    CHECK(cached.StyleGeneration() != first.StyleGeneration());
    CHECK(cached.publication > first.publication);
    CHECK(first.styles->State() == RuntimeStyleRegistryState::Active);
}

TEST_CASE("Real factory style capability validates canonical edits before history and preserves complete records across save reload",
          "[application][ui][style][pipeline]") {
    Project project;
    project.Prepare();
    DocumentIdentityRegistry identities;
    const auto identity = identities.Open({DocumentKind::UiCanvas, SourceDocumentId::Parse("Hud.uicanvas").Value()}).Value().identity;
    auto opened = UiCanvasDocument::Open(identity, project.WriteCanvas());
    REQUIRE(opened.HasValue());
    auto owner = std::move(opened).Value();
    REQUIRE(owner.Select(owner.CurrentState(), project.Identity<UiElementId>(13)).HasValue());
    const auto resources = project.resources->Capture();
    const auto context = Context(owner, resources);
    const auto baseline = *owner.Document().Elements()[0].presentation;
    auto value = EditedPresentation(baseline);
    UiInspectorDraft draft;
    REQUIRE(draft.Begin(context, {.layout = baseline.layout}));
    const auto prepared = draft.PreparePresentation(context, value, true);
    REQUIRE(prepared.HasValue());
    REQUIRE(ExecuteUiInspectorPresentation(owner, context, 1, prepared.Value(), &resources).HasValue());
    CHECK(*owner.Document().Elements()[0].presentation == value);
    REQUIRE(owner.Undo(owner.CurrentState()).HasValue());
    CHECK(*owner.Document().Elements()[0].presentation == baseline);
    REQUIRE(owner.Redo(owner.CurrentState()).HasValue());
    CHECK(*owner.Document().Elements()[0].presentation == value);
    auto saved = owner.Save(project.root, project.mutations, project.files);
    REQUIRE(saved.HasValue());
    REQUIRE(saved.Value().status == UiCanvasDocumentSaveStatus::Saved);
    REQUIRE(owner.Reload().HasValue());
    CHECK(*owner.Document().Elements()[0].presentation == value);
    CHECK_FALSE(owner.IsDirty());
    const auto bytes = SerializeUiDocument(owner.Document()).Value();
    REQUIRE(owner.Select(owner.CurrentState(), project.Identity<UiElementId>(13)).HasValue());
    const auto now = Context(owner, resources);
    value.state.enabled = false;
    // The actual selected theme has a later matching Availability write after sealing Fill.
    REQUIRE(resources.Validate(owner.Document(), value).HasError());
    CHECK(ExecuteUiInspectorPresentation(owner, now, 1, {now.element, value}, &resources).HasError());
    CHECK(SerializeUiDocument(owner.Document()).Value() == bytes);
    CHECK_FALSE(owner.CanUndo());
    value = *owner.Document().Elements()[0].presentation;
    value.inlineStyle.push_back(
        {CoreUiStylePropertyId(UiCoreStyleProperty::ImageFit), UiStyleValueSource::Literal(UiStyleEnumValue{99}), false});
    CHECK(ExecuteUiInspectorPresentation(owner, now, 1, {now.element, value}, &resources).HasError());
    CHECK(SerializeUiDocument(owner.Document()).Value() == bytes);
    CHECK_FALSE(owner.IsDirty());
}

TEST_CASE("Real producer rejects sidecar identity drift and preserves last good style publication", "[application][ui][style][pipeline]") {
    Project project;
    project.Prepare();
    const auto last = project.resources->Capture();
    project.style.style.id = RuntimeStyleAssetId::Create(project.workspace.Bytes()).Value();
    project.WriteStyle();
    REQUIRE(project.resources->SelectTheme(last.theme).HasValue());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    bool failed = false;
    while (project.resources->Capture().pending && std::chrono::steady_clock::now() < deadline) {
        const auto result = project.resources->Refresh();
        if (result.HasError())
            failed = true;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(failed);
    CHECK_FALSE(project.resources->Capture().pending);
    CHECK_FALSE(project.resources->Capture().StyleGeneration().IsValid());
    CHECK(project.resources->Capture().resourceLease == last.resourceLease);
    CHECK(project.resources->Capture().theme == last.theme);
    CHECK(last.styles->State() == RuntimeStyleRegistryState::Active);
}

TEST_CASE("Registry drift closes real style admission and rejects late preparation without replacing pinned leases",
          "[application][ui][style][pipeline]") {
    Project project;
    project.Prepare();
    const auto pinned = project.resources->Capture();
    REQUIRE(project.resources->SelectTheme(pinned.theme).HasValue());
    const auto records = project.assets.Snapshot().Records();
    REQUIRE(project.assets.Publish(std::vector<Assets::AssetRecord>{records.begin(), records.end()}).status ==
            Assets::AssetRegistryBuildStatus::Complete);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    bool failed = false;
    while (project.resources->Capture().pending && std::chrono::steady_clock::now() < deadline) {
        const auto result = project.resources->Refresh();
        if (result.HasError())
            failed = true;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(failed);
    CHECK_FALSE(project.resources->Capture().StyleGeneration().IsValid());
    CHECK(pinned.styles->State() == RuntimeStyleRegistryState::Active);
    REQUIRE(project.resources->Refresh().HasValue());
    project.Prepare();
    CHECK(project.resources->Capture().assets.Revision() != pinned.assets.Revision());
}

TEST_CASE("Actual factory shutdown cancels and joins preparation before releasing native source authority",
          "[application][ui][style][pipeline]") {
    Project project;
    project.Prepare();
    const auto pinned = project.resources->Capture();
    REQUIRE(project.resources->SelectTheme(pinned.theme).HasValue());
    project.resources.reset();
    auto available = project.mutations.TryAcquire({project.root, ProjectMutationOwner::Asset, "test-after-style-shutdown"});
    REQUIRE(available.HasValue());
    CHECK(pinned.styles->State() == RuntimeStyleRegistryState::Active);
}

TEST_CASE("Selected root resource failure preserves the actual prior publication and unrelated malformed assets do not join closure",
          "[application][ui][style][pipeline]") {
    Project project;
    std::ofstream(project.root / "assets" / "unrelated.uistyle") << "invalid";
    std::ofstream(project.root / "assets" / "unrelated.uistyle.horo")
        << nlohmann::json{{"schemaVersion", 1}, {"assetId", project.workspace.ToString()}, {"assetType", UiStyleAssetTypeName}};
    project.Rebuild();
    REQUIRE(project.resources->Refresh().HasValue());
    project.Prepare();
    const auto pinned = project.resources->Capture();
    auto bytes = project.workspace.Bytes();
    bytes.back() ^= 0x20;
    const auto image = Assets::AssetId::FromBytes(bytes);
    project.style.defaults.image = UiStyleImage{.asset = image};
    project.style.dependencies = {{image, Assets::AssetTypeId::Parse("game.image").Value(), true}};
    project.WriteStyle();
    REQUIRE(project.resources->SelectTheme(pinned.theme).HasValue());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    bool failed = false;
    while (project.resources->Capture().pending && std::chrono::steady_clock::now() < deadline) {
        const auto result = project.resources->Refresh();
        if (result.HasError())
            failed = true;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(failed);
    CHECK_FALSE(project.resources->Capture().StyleGeneration().IsValid());
    CHECK(project.resources->Capture().resourceLease == pinned.resourceLease);
}

TEST_CASE("Actual style contribution revalidates domain source identity and semantic revision for fresh and cached payloads",
          "[application][ui][style][cache]") {
    Project project;
    const auto schema = CreateCoreUiStyleSchema({}).Value();
    const auto target = AssetCookTargetId::Parse("headless-null").Value();
    auto contribution =
        Application::MakeUiStyleCookerContribution({project.style}, schema, target, RuntimeStyleGeneration::Create(1).Value());
    REQUIRE(contribution.HasValue());
    const auto source = SerializeUiStyleAsset(project.style).Value();
    const std::span sourceBytes{reinterpret_cast<const std::uint8_t *>(source.data()), source.size()};
    const Assets::CookSourceView input{project.styleAsset,
                                       Assets::AssetTypeId::Parse(UiStyleAssetTypeName).Value(),
                                       target,
                                       ComputeSha256(std::as_bytes(sourceBytes)),
                                       sourceBytes,
                                       "assets/theme.uistyle"};
    const auto cooked = contribution.Value().strategy->Cook(input, {});
    REQUIRE(cooked.HasValue());
    CHECK(contribution.Value().strategy->ValidateCookedPayload(input, cooked.Value().payload).HasValue());
    auto foreign = project.style;
    foreign.revision = UiStyleContentRevision::Create(2).Value();
    const auto wrongPayload = CookUiStyleAsset(foreign, schema).Value();
    CHECK(contribution.Value().strategy->ValidateCookedPayload(input, wrongPayload).HasError());
    auto wrongSource = input;
    wrongSource.id = project.workspace;
    CHECK(contribution.Value().strategy->Cook(wrongSource, {}).HasError());
    auto wrongSchema = schema;
    wrongSchema.fingerprint.bytes[0] ^= 1;
    CHECK(contribution.Value().strategy->ValidateCookedPayload(input, CookUiStyleAsset(project.style, wrongSchema).Value()).HasError());
}
