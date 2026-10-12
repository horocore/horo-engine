#include "UiTemplateHostPublicationFixture.h"

#include <algorithm>

namespace Horo::Application::TemplateHostTests {
    namespace {
        /** @brief Encodes actual bytes as a real Assets artifact rather than manufacturing a loaded result. */
        void InsertArtifact(Assets::MemoryAssetProvider &provider, Assets::AssetId asset, const Assets::AssetTypeId &type,
                            const std::span<const std::uint8_t> payload) {
            Assets::AssetCookArtifact artifact;
            artifact.id = asset;
            artifact.type = type;
            artifact.target = AssetCookTargetId::Parse("linux-x64").Value();
            artifact.payload.assign(payload.begin(), payload.end());
            artifact.payloadDigest = ComputeSha256(std::as_bytes(std::span{artifact.payload}));
            auto encoded = Assets::EncodeCookedArtifact(artifact);
            REQUIRE(encoded.HasValue());
            provider.Insert(asset, std::move(encoded).Value());
        }
    }  // namespace

    /** @copydoc PublicationFixture::Cook */
    UiTemplateHostCook PublicationFixture::Cook(const UiTemplates::UiTemplateDocumentSnapshot &snapshot) const {
        const std::array inputs{source.Capture()};
        auto cooked = UiTemplateHost::Cook(snapshot, inputs, source.type, resources);
        REQUIRE(cooked.HasValue());
        return std::move(cooked).Value();
    }

    /** @copydoc PublicationFixture::Load */
    Runtime::Ui::UiRuntimeAssetLoadResult PublicationFixture::Load(const Runtime::Ui::CookedUiDocument &document) const {
        using namespace Runtime::Ui;
        const auto asset = Assets::AssetId::FromBytes(UiTemplates::TestSupport::Identity<UiDocumentId>(7).Bytes());
        const auto type = Assets::AssetTypeId::Parse("runtime.ui.document").Value();
        Assets::MemoryAssetProvider provider;
        InsertArtifact(provider, asset, type, document.Payload());
        std::vector<Assets::AssetRecord> records{
            {asset, type, ProjectPath::Parse("assets/cooked.ui").Value(), ProjectPath::Parse("assets/cooked.ui.horo").Value()}};
        const auto captured = source.registry.Snapshot();
        for (const auto &dependency : document.Dependencies()) {
            const auto *record = captured.Find(dependency.asset);
            REQUIRE(record);
            std::ifstream stream{source.temporary.Path() / record->sourcePath.String(), std::ios::binary};
            REQUIRE(stream.good());
            const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
            InsertArtifact(provider, record->id, record->type, bytes);
            records.push_back(*record);
        }
        Assets::AssetRegistry registry;
        REQUIRE(registry.Publish(std::move(records)).status == Assets::AssetRegistryBuildStatus::Complete);
        JobSystem jobs{JobSystemConfig{1, 8}};
        Assets::AssetLoadService assets{jobs, provider};
        UiRuntimeAssetLoadService loader{registry, assets};
        auto request = loader.LoadAsync({{asset, document.Id(), document.Canvases().front().id, document.SourceRevision()},
                                         type,
                                         AssetCookTargetId::Parse("linux-x64").Value()});
        REQUIRE(request.HasValue());
        auto handle = std::move(request).Value();
        REQUIRE(handle.Wait().HasValue());
        auto loaded = handle.TakeResult();
        REQUIRE(loaded.HasValue());
        return std::move(loaded).Value();
    }

    /** @copydoc PublicationFixture::Trees */
    std::vector<Runtime::Ui::UiReloadCanvas> PublicationFixture::Trees(const Runtime::Ui::CookedUiDocument &document) {
        using namespace Runtime::Ui;
        std::vector<UiReloadCanvas> canvases;
        for (const auto &canvas : document.Canvases()) {
            std::vector<UiElementDescriptor> elements;
            for (const auto &element : document.Elements()) {
                auto ancestor = &element;
                while (ancestor->parent.IsValid()) {
                    const auto found = std::ranges::find(document.Elements(), ancestor->parent, &UiDocumentElement::id);
                    REQUIRE(found != document.Elements().end());
                    ancestor = std::to_address(found);
                }
                if (ancestor->id == canvas.rootElement)
                    elements.push_back({element.id, element.parent});
            }
            const UiCanvasInstanceId runtimeCanvas{ownership, static_cast<std::uint32_t>(canvases.size() + 1), 1};
            auto tree = UiElementTree::Create(allocator,
                                              {instance,
                                               runtimeCanvas,
                                               document.Id(),
                                               document.SourceRevision(),
                                               UiRuntimeTreeRevision::Create(document.SourceRevision().Value()).Value(),
                                               {64, 64, 64}},
                                              elements);
            REQUIRE(tree.HasValue());
            canvases.push_back({canvas.id, std::move(tree).Value()});
        }
        return canvases;
    }

    /** @copydoc PublicationFixture::Generation */
    Runtime::Ui::UiReloadGeneration PublicationFixture::Generation(const UiTemplateHostCook &cook) {
        auto generation =
            Runtime::Ui::UiReloadGeneration::Create(Load(cook.Document().ordinary), instance, Trees(cook.Document().ordinary));
        REQUIRE(generation.HasValue());
        return std::move(generation).Value();
    }

    /** @copydoc PublicationFixture::Host */
    std::unique_ptr<UiTemplateHost> PublicationFixture::Host(const Runtime::Ui::UiHotReloadLimits limits) {
        auto cook = Cook(source.owner->Capture());
        auto generation = Generation(cook);
        auto host = UiTemplateHost::Create(std::move(source.owner), std::move(generation), std::move(cook), limits);
        REQUIRE(host.HasValue());
        return std::move(host).Value();
    }

    /** @copydoc PublicationFixture::Detach */
    UiTemplates::UiTemplatePreparedTransaction PublicationFixture::Detach(const UiTemplateHost &host,
                                                                          const CancellationToken &cancellation) const {
        const auto snapshot = host.Capture();
        const auto expanded = snapshot.Expansions().front().elements.front().id;
        const std::array identities{
            UiTemplates::UiTemplateDetachIdentity{expanded, UiTemplates::TestSupport::Identity<Runtime::Ui::UiElementId>(101)}};
        auto document = host.Authoring().PrepareDetach(snapshot.Source().instances.front().id, identities, cancellation);
        REQUIRE(document.HasValue());
        return std::move(document).Value();
    }

    /** @copydoc PublicationFixture::Pair */
    UiTemplateHost::Prepared PublicationFixture::Pair(UiTemplateHost &host, UiTemplates::UiTemplatePreparedTransaction document,
                                                      const CancellationToken &cancellation) {
        auto cook = Cook(document.Preview());
        auto runtime = Generation(cook);
        auto paired = host.Prepare(std::move(document), std::move(runtime), std::move(cook), cancellation);
        REQUIRE(paired.HasValue());
        return std::move(paired).Value();
    }
}  // namespace Horo::Application::TemplateHostTests
