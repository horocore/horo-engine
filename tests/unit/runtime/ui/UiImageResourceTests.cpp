#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiImageResource.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        Assets::AssetId Asset(const std::uint8_t marker) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = marker;
            return Assets::AssetId::FromBytes(bytes);
        }

        Assets::AssetTypeId ImageType() {
            return Assets::AssetTypeId::Parse("ui.image").Value();
        }

        Assets::AssetTypeId AtlasType() {
            return Assets::AssetTypeId::Parse("ui.atlas").Value();
        }

        UiImagePage Page(const std::uint8_t assetMarker, const std::uint32_t width = 128, const std::uint32_t height = 64,
                         const Assets::AssetTypeId type = ImageType()) {
            return {.dependency = {Asset(assetMarker), type, true},
                    .extent = {width, height},
                    .colorSpace = UiImageColorSpace::Srgb,
                    .sampling = {UiImageFilter::Linear, UiImageFilter::Linear, UiImageMipmapMode::None}};
        }

        UiImageResource MakeImage(const UiImageFallbackPolicy fallback = UiImageFallbackPolicy::Reject) {
            const auto page = Page(1);
            auto result = UiImageResource::Create({UiImageResourceKind::Image, fallback, {&page, 1}, {}});
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        UiImageResourceRevision Revision(const std::uint64_t value) {
            return UiImageResourceRevision::Create(value).Value();
        }

        UiOwnershipGeneration Owner() {
            return UiOwnershipGeneration::Create(17).Value();
        }

        void RequireCode(const auto &result, const ErrorCodeDescriptor &descriptor) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == descriptor.code.Value());
        }

        TEST_CASE("Runtime UI image resources validate pages, sampling, UVs, and cook dependencies", "[runtime_ui][image]") {
            REQUIRE((UiImageExtent{1, 1}.IsValid()));
            REQUIRE_FALSE(UiImageExtent{}.IsValid());
            REQUIRE(UiImageUvRect{}.IsValid());
            REQUIRE_FALSE((UiImageUvRect{0.5F, 0.0F, 0.5F, 1.0F}).IsValid());
            REQUIRE((UiImageNineSliceInsets{8, 4, 8, 4}.IsValid({32, 16})));
            REQUIRE_FALSE((UiImageNineSliceInsets{16, 0, 17, 0}.IsValid({32, 16})));
            REQUIRE(UiImageSampling{}.IsValid());

            const auto page = Page(1, 256, 128);
            const auto result = UiImageResource::Create({UiImageResourceKind::Image, UiImageFallbackPolicy::Reject, {&page, 1}, {}});
            REQUIRE(result.HasValue());
            const auto &resource = result.Value();
            REQUIRE(resource.IsValid());
            REQUIRE(resource.Kind() == UiImageResourceKind::Image);
            REQUIRE(resource.Pages().size() == 1);
            REQUIRE(resource.Regions().empty());
            REQUIRE(resource.Dependencies().size() == 1);
            REQUIRE(resource.Dependencies().front().asset == Asset(1));
            REQUIRE(resource.ResolveRegion().HasValue());
            REQUIRE((resource.ResolveRegion().Value().pixelExtent == UiImageExtent{256, 128}));
            REQUIRE(resource.Page(1) == nullptr);
        }

        TEST_CASE("Runtime UI atlas regions are canonical and resolve without frame-hot allocation", "[runtime_ui][image][atlas]") {
            const std::array pages{Page(1), Page(2)};
            const std::array regions{
                UiSpriteRegion{3, 1, {0.5F, 0.0F, 1.0F, 1.0F}, {64, 64}, {4, 4, 4, 4}},
                UiSpriteRegion{1, 0, {0.0F, 0.0F, 0.5F, 1.0F}, {64, 64}, {}},
            };
            auto result = UiImageResource::Create({UiImageResourceKind::Atlas, UiImageFallbackPolicy::Transparent, pages, regions});
            REQUIRE(result.HasValue());
            const auto &resource = result.Value();
            REQUIRE(resource.Regions()[0].id == 1);
            REQUIRE(resource.Regions()[1].id == 3);
            REQUIRE(resource.Dependencies().size() == 2);
            REQUIRE(resource.FindRegion(1) != nullptr);
            REQUIRE(resource.FindRegion(NoUiImageRegion) == nullptr);
            REQUIRE(resource.ResolveRegion(3).HasValue());
            REQUIRE(resource.ResolveRegion(3).Value().page == 1);
            REQUIRE(resource.ResolveRegion().HasError());
            REQUIRE(resource.ResolveRegion(99).ErrorValue().code.Value() == UiErrors::ImageRegionInvalid.code.Value());
        }

        TEST_CASE("Runtime UI image resources reject malformed and conflicting declarations", "[runtime_ui][image]") {
            const auto page = Page(1);
            const auto noPages = UiImageResource::Create({UiImageResourceKind::Image, UiImageFallbackPolicy::Reject, {}, {}});
            RequireCode(noPages, UiErrors::ImageResourceInvalid);

            const std::array pages{Page(1)};
            const auto emptyAtlas = UiImageResource::Create({UiImageResourceKind::Atlas, UiImageFallbackPolicy::Reject, pages, {}});
            RequireCode(emptyAtlas, UiErrors::ImageResourceInvalid);

            const std::array conflictingPages{Page(1, 64, 64, ImageType()), Page(1, 64, 64, AtlasType())};
            const std::array region{UiSpriteRegion{1, 0, {}, {32, 32}, {}}};
            const auto conflicting =
                UiImageResource::Create({UiImageResourceKind::Atlas, UiImageFallbackPolicy::Reject, conflictingPages, region});
            RequireCode(conflicting, UiErrors::ImageResourceInvalid);

            const std::array duplicateRegions{
                UiSpriteRegion{1, 0, {}, {32, 32}, {}},
                UiSpriteRegion{1, 0, {}, {32, 32}, {}},
            };
            const auto duplicate =
                UiImageResource::Create({UiImageResourceKind::Atlas, UiImageFallbackPolicy::Reject, pages, duplicateRegions});
            RequireCode(duplicate, UiErrors::ImageResourceInvalid);
            REQUIRE(page.IsValid());
        }

        TEST_CASE("Runtime UI image registry preserves old generations across reload and close", "[runtime_ui][image][lifecycle]") {
            auto registryResult = UiImageResourceRegistry::Create({Owner(), 1});
            REQUIRE(registryResult.HasValue());
            auto registry = std::move(registryResult).Value();

            auto firstPublish =
                registry.Publish(MakeImage(UiImageFallbackPolicy::Transparent), Revision(1), UiImageResidencyState::Resident);
            REQUIRE(firstPublish.HasValue());
            const auto firstHandle = firstPublish.Value();
            auto oldSnapshotResult = registry.Acquire(firstHandle);
            REQUIRE(oldSnapshotResult.HasValue());
            auto oldSnapshot = std::move(oldSnapshotResult).Value();

            auto reload = registry.Reload(firstHandle, MakeImage(UiImageFallbackPolicy::Transparent), Revision(2),
                                          UiImageResidencyState::MissingFallback);
            REQUIRE(reload.HasValue());
            REQUIRE(reload.Value() != firstHandle);
            REQUIRE(oldSnapshot.IsValid());
            REQUIRE(oldSnapshot.Revision() == Revision(1));
            REQUIRE(oldSnapshot.IsDrawable());

            auto currentSnapshotResult = registry.Acquire(reload.Value());
            REQUIRE(currentSnapshotResult.HasValue());
            auto currentSnapshot = std::move(currentSnapshotResult).Value();
            REQUIRE(currentSnapshot.Residency() == UiImageResidencyState::MissingFallback);
            REQUIRE(currentSnapshot.IsDrawable());
            REQUIRE(registry.Acquire(firstHandle).HasError());

            registry.Close();
            REQUIRE(registry.State() == UiImageResourceRegistryState::Closed);
            REQUIRE_FALSE(registry.IsDrained());
            REQUIRE(registry.Acquire(reload.Value()).HasError());
            currentSnapshot = {};
            REQUIRE_FALSE(registry.IsDrained());
            oldSnapshot = {};
            REQUIRE(registry.IsDrained());
        }

        TEST_CASE("Runtime UI image registry enforces fallback, capacity, revisions, and retirement", "[runtime_ui][image][lifecycle]") {
            auto registryResult = UiImageResourceRegistry::Create({Owner(), 1});
            REQUIRE(registryResult.HasValue());
            auto registry = std::move(registryResult).Value();

            const auto rejectedFallback = registry.Publish(MakeImage(), Revision(1), UiImageResidencyState::MissingFallback);
            RequireCode(rejectedFallback, UiErrors::ImageResidencyInvalid);
            const auto published = registry.Publish(MakeImage(), Revision(1), UiImageResidencyState::Resident);
            REQUIRE(published.HasValue());
            RequireCode(registry.Publish(MakeImage(), Revision(2), UiImageResidencyState::Resident),
                        UiErrors::ImageResourceStorageExhausted);
            RequireCode(registry.Reload(published.Value(), MakeImage(), Revision(1), UiImageResidencyState::Resident),
                        UiErrors::RevisionStale);

            REQUIRE(registry.Retire(published.Value()).HasValue());
            REQUIRE(registry.Acquire(published.Value()).HasError());
            const auto republished = registry.Publish(MakeImage(), Revision(3), UiImageResidencyState::Resident);
            REQUIRE(republished.HasValue());
            REQUIRE(republished.Value().generation != published.Value().generation);
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
