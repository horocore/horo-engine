#include "../support/AssetImportTestSupport.h"
#include "Horo/Assets/AssetImportOperation.h"
#include "editor/modals/asset_import/AssetImportSourcePreview.h"
#include "editor/renderer/EditorGuiRenderer.h"

#include <catch2/catch_test_macros.hpp>
#include <fstream>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    class PreviewProvider final : public Assets::IAssetPreviewProvider {
    public:
        Result<Assets::AssetPreviewImage> GeneratePreview(const Assets::AssetPreviewInput &input,
                                                          const CancellationToken &) const override {
            if (invalid)
                return Result<Assets::AssetPreviewImage>::Success({});
            return Result<Assets::AssetPreviewImage>::Success(
                {.width = input.width,
                 .height = input.height,
                 .pixels = std::vector<std::uint8_t>(static_cast<std::size_t>(input.width) * input.height * 4U, 0x80)});
        }

        bool invalid{};
    };

    class PreviewRenderer final : public IEditorGuiRenderer {
    public:
        Result<void> Initialize() override {
            return Result<void>::Success();
        }

        Result<void> BeginFrame() override {
            return Result<void>::Success();
        }

        Result<void> RenderDrawData() override {
            return Result<void>::Success();
        }

        Result<std::uintptr_t> CreateTexture(const EditorRgba8ImageView &image) override {
            REQUIRE(image.IsValid());
            REQUIRE(image.width == 272);
            REQUIRE(image.height == 232);
            ++uploads;
            return Result<std::uintptr_t>::Success(42);
        }

        void DestroyTexture(const std::uintptr_t id) noexcept override {
            destroyed = id;
        }

        void Shutdown() noexcept override {}

        int uploads{};
        std::uintptr_t destroyed{};
    };

    struct PreviewFixture {
        PreviewFixture() {
            std::ofstream{source} << "mesh-source";
            item.absoluteSourcePath = source;
            item.sourceExtension = "obj";
            contribution.previewProvider = provider;
        }

        Horo::Tests::ScopedAssetImportTempDirectory directory{"horo-import-preview"};
        std::filesystem::path source = directory.Path() / "mesh with spaces.obj";
        Assets::AssetImportItem item{.sourceFile = ProjectPath::Parse("mesh.obj").Value()};
        std::shared_ptr<PreviewProvider> provider = std::make_shared<PreviewProvider>();
        Assets::AssetImporterContribution contribution = Horo::Tests::BasicAssetImporterContribution();
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 8}};
        PreviewRenderer renderer;
    };
}  // namespace

TEST_CASE("Source preview uploads once and releases its texture when selection disappears", "[unit][editor][asset-import]") {
    PreviewFixture fixture;
    AssetImportSourcePreview preview{fixture.jobs, fixture.renderer};
    preview.Update(&fixture.item, &fixture.contribution);
    fixture.jobs.Shutdown(ShutdownPolicy::Drain);
    preview.Update(&fixture.item, &fixture.contribution);
    REQUIRE(preview.TextureId() == 42);
    preview.Update(&fixture.item, &fixture.contribution);
    REQUIRE(fixture.renderer.uploads == 1);
    preview.Update(nullptr, nullptr);
    REQUIRE(preview.TextureId() == 0);
    REQUIRE(fixture.renderer.destroyed == 42);
}

TEST_CASE("Source preview rejects invalid sources and provider results", "[unit][editor][asset-import]") {
    PreviewFixture fixture;
    SECTION("missing source") {
        std::filesystem::remove(fixture.source);
    }
    SECTION("oversized source") {
        std::filesystem::resize_file(fixture.source, 64U * 1024U * 1024U + 1U);
    }
    SECTION("invalid image") {
        fixture.provider->invalid = true;
    }
    SECTION("relative source") {
        fixture.item.absoluteSourcePath = "mesh.obj";
    }
    SECTION("unavailable importer") {
        fixture.contribution.strategy.reset();
    }
    SECTION("unavailable provider") {
        fixture.contribution.previewProvider.reset();
    }
    AssetImportSourcePreview preview{fixture.jobs, fixture.renderer};
    preview.Update(&fixture.item, &fixture.contribution);
    fixture.jobs.Shutdown(ShutdownPolicy::Drain);
    preview.Update(&fixture.item, &fixture.contribution);
    REQUIRE(preview.TextureId() == 0);
    REQUIRE(fixture.renderer.uploads == 0);
}

TEST_CASE("Source preview handles stopped jobs and destroys retained textures on teardown", "[unit][editor][asset-import]") {
    PreviewFixture fixture;
    SECTION("submission after shutdown") {
        fixture.jobs.Shutdown(ShutdownPolicy::Drain);
        AssetImportSourcePreview preview{fixture.jobs, fixture.renderer};
        preview.Update(&fixture.item, &fixture.contribution);
        REQUIRE(preview.TextureId() == 0);
        REQUIRE(fixture.renderer.uploads == 0);
    }
    SECTION("retained upload") {
        {
            AssetImportSourcePreview preview{fixture.jobs, fixture.renderer};
            preview.Update(&fixture.item, &fixture.contribution);
            fixture.jobs.Shutdown(ShutdownPolicy::Drain);
            preview.Update(&fixture.item, &fixture.contribution);
            REQUIRE(preview.TextureId() == 42);
        }
        REQUIRE(fixture.renderer.destroyed == 42);
    }
}
