#include "editor/screens/workspace/prefab/PrefabAssetPreview.h"

#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Runtime/Scene/PrimitiveMesh.h"
#include "Horo/Scene/PrefabComponentProjection.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace Horo::Editor {
    namespace {
        constexpr std::size_t MaximumTriangles = 20000;
        constexpr std::size_t MaximumSamples = 2000000;

        struct Triangle {
            std::array<Math::Vec3, 3> points;
        };

        using PreviewResult = Result<Assets::AssetPreviewImage>;

        /** @brief Projects finite world coordinates through the fixed right-handed isometric camera. */
        Math::Vec3 Project(const Math::Vec3 point) {
            return {point.x * 0.832F - point.z * 0.555F, point.x * 0.213F - point.y * 0.923F + point.z * 0.320F,
                    -(point.x * 0.512F + point.y * 0.384F + point.z * 0.768F)};
        }

        /** @brief Collects real canonical primitive triangles in deterministic parent-before-child order. */
        Result<std::vector<Triangle>> Collect(const Prefab::EffectivePrefabCandidate &candidate, const CancellationToken &cancellation) {
            std::vector<Triangle> triangles;
            std::map<Prefab::ExpandedPrefabObjectKey, Math::Mat4> transforms;
            for (const auto &resolved : candidate.Objects()) {
                if (cancellation.IsCancellationRequested())
                    return Result<std::vector<Triangle>>::Failure(MakeError(Prefab::PrefabErrors::Cancelled));
                auto local = resolved.effectiveLocalTransform.TryToMatrix();
                if (local.HasError())
                    return Result<std::vector<Triangle>>::Failure(local.ErrorValue());
                auto world = local.Value();
                if (resolved.parent) {
                    const auto parent = transforms.find(*resolved.parent);
                    if (parent == transforms.end())
                        return Result<std::vector<Triangle>>::Failure(MakeError(Prefab::PrefabErrors::AdmissionRejected));
                    world = Math::Multiply(parent->second, world);
                }
                transforms.emplace(resolved.key, world);
                Runtime::RuntimeComponentSet components;
                std::optional<Runtime::PrimitiveMeshDescriptor> primitive;
                for (const auto &component : resolved.object.components) {
                    auto projected = SceneSource::ProjectPrefabBuiltinComponent(component, components, primitive);
                    if (projected.HasError())
                        return Result<std::vector<Triangle>>::Failure(projected.ErrorValue());
                }
                if (!primitive)
                    continue;
                auto generated = Runtime::PrimitiveMeshGenerator::Generate(*primitive);
                if (generated.HasError())
                    return Result<std::vector<Triangle>>::Failure(generated.ErrorValue());
                const auto &mesh = generated.Value();
                if (!mesh.IsValid() || mesh.indices.size() / 3 > MaximumTriangles - triangles.size())
                    return Result<std::vector<Triangle>>::Failure(MakeError(Prefab::PrefabErrors::WorkBudgetExceeded));
                for (std::size_t index = 0; index < mesh.indices.size(); index += 3) {
                    if (cancellation.IsCancellationRequested())
                        return Result<std::vector<Triangle>>::Failure(MakeError(Prefab::PrefabErrors::Cancelled));
                    Triangle triangle;
                    for (std::size_t corner = 0; corner < 3; ++corner) {
                        auto point = Math::TryTransformPoint(world, mesh.vertices[mesh.indices[index + corner]].position);
                        if (point.HasError())
                            return Result<std::vector<Triangle>>::Failure(point.ErrorValue());
                        triangle.points[corner] = Project(point.Value());
                        if (!Math::IsFinite(triangle.points[corner]))
                            return Result<std::vector<Triangle>>::Failure(MakeError(Prefab::PrefabErrors::AdmissionRejected));
                    }
                    triangles.push_back(triangle);
                }
            }
            return Result<std::vector<Triangle>>::Success(std::move(triangles));
        }

        /** @brief Signed triangle edge function used by the deterministic pixel-center depth test. */
        float Edge(const Math::Vec3 a, const Math::Vec3 b, const float x, const float y) {
            return (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x);
        }

        /** @brief Fits all projected geometry inside the image without depending on editor camera state. */
        Result<void> Fit(std::vector<Triangle> &triangles, const std::uint32_t width, const std::uint32_t height) {
            double minX = std::numeric_limits<double>::max(), minY = minX, minZ = minX;
            double maxX = std::numeric_limits<double>::lowest(), maxY = maxX, maxZ = maxX;
            for (const auto &triangle : triangles)
                for (const auto &point : triangle.points) {
                    minX = std::min(minX, static_cast<double>(point.x));
                    maxX = std::max(maxX, static_cast<double>(point.x));
                    minY = std::min(minY, static_cast<double>(point.y));
                    maxY = std::max(maxY, static_cast<double>(point.y));
                    minZ = std::min(minZ, static_cast<double>(point.z));
                    maxZ = std::max(maxZ, static_cast<double>(point.z));
                }
            const double scale = std::min((width * 0.84) / std::max(maxX - minX, 0.0001), (height * 0.84) / std::max(maxY - minY, 0.0001));
            for (auto &triangle : triangles)
                for (auto &point : triangle.points) {
                    point.x = static_cast<float>(width * 0.5 + (point.x - (minX + maxX) * 0.5) * scale);
                    point.y = static_cast<float>(height * 0.5 + (point.y - (minY + maxY) * 0.5) * scale);
                    point.z = static_cast<float>((static_cast<double>(point.z) - minZ) / std::max(maxZ - minZ, 0.0001));
                    if (!Math::IsFinite(point))
                        return Result<void>::Failure(MakeError(Prefab::PrefabErrors::AdmissionRejected));
                }
            return Result<void>::Success();
        }

        /** @brief Rasterizes one bounded triangle, checking cooperative stop before each scanline. */
        Result<void> Rasterize(const Triangle &triangle, Assets::AssetPreviewImage &image, std::vector<float> &depth, std::size_t &samples,
                               const CancellationToken &cancellation) {
            const auto &[a, b, c] = triangle.points;
            const float area = Edge(a, b, c.x, c.y);
            if (std::abs(area) < 0.0001F)
                return Result<void>::Success();
            const int minX = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
            const int minY = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
            const int maxX = std::min(static_cast<int>(image.width) - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
            const int maxY = std::min(static_cast<int>(image.height) - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
            const auto normal = Math::Cross(b - a, c - a);
            const float length = Math::Length(normal);
            const auto shade = static_cast<std::uint8_t>(150.0F + 90.0F * (length > 0.0001F ? std::abs(normal.z) / length : 0.0F));
            for (int y = minY; y <= maxY; ++y) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(Prefab::PrefabErrors::Cancelled));
                for (int x = minX; x <= maxX; ++x) {
                    if (++samples > MaximumSamples)
                        return Result<void>::Failure(MakeError(Prefab::PrefabErrors::WorkBudgetExceeded));
                    const float w0 = Edge(b, c, x + 0.5F, y + 0.5F) / area;
                    const float w1 = Edge(c, a, x + 0.5F, y + 0.5F) / area;
                    const float w2 = Edge(a, b, x + 0.5F, y + 0.5F) / area;
                    if (std::min({w0, w1, w2}) < 0)
                        continue;
                    const auto pixel = static_cast<std::size_t>(y) * image.width + static_cast<std::size_t>(x);
                    const float z = w0 * a.z + w1 * b.z + w2 * c.z;
                    if (z >= depth[pixel])
                        continue;
                    depth[pixel] = z;
                    image.pixels[pixel * 4] = shade;
                    image.pixels[pixel * 4 + 1] = shade;
                    image.pixels[pixel * 4 + 2] = shade;
                    image.pixels[pixel * 4 + 3] = 255;
                }
            }
            return Result<void>::Success();
        }

        /** @brief Owns immutable source identity; captures and verifies the full closure under the host lease. */
        class Provider final : public Assets::IAssetPreviewProvider {
        public:
            Provider(Assets::AssetRegistrySnapshot registry, Assets::AssetId asset, PrefabHierarchySourceCapture capture)
                : registry_(std::move(registry)), asset_(asset), capture_(std::move(capture)) {}

            /** @copydoc Assets::IAssetPreviewProvider::GeneratePreview */
            Result<Assets::AssetPreviewImage> GeneratePreview(const Assets::AssetPreviewInput &input,
                                                              const CancellationToken &cancellation) const override {
                static const auto prefabType = Assets::AssetTypeId::Parse("core.prefab").Value();
                if (!capture_ || input.assetType != prefabType)
                    return PreviewResult::Failure(MakeError(Prefab::PrefabErrors::DependencyUnavailable));
                auto limits = Prefab::PrefabLimitProfile::Create({});
                const std::array roots{asset_};
                // Release the project lease before CPU rasterization so background previews do not hold up source saves.
                auto captured = [&]() -> Result<PrefabHierarchySources> {
                    auto prepared = capture_(registry_, roots, limits.Value(), cancellation);
                    if (prepared.HasError())
                        return Result<PrefabHierarchySources>::Failure(prepared.ErrorValue());
                    return Result<PrefabHierarchySources>::Success(std::move(prepared).Value().sources);
                }();
                if (captured.HasError())
                    return PreviewResult::Failure(captured.ErrorValue());
                const auto *source = captured.Value().inputs.Find(asset_);
                if (!source || !std::ranges::equal(source->bytes, input.editorPayload))
                    return PreviewResult::Failure(MakeError(Prefab::PrefabErrors::ResolutionStale));
                auto candidate =
                    captured.Value().resolver.Resolve(asset_, Prefab::PrefabInstanceId::Create(1).Value(), limits.Value(), cancellation);
                if (candidate.HasError())
                    return PreviewResult::Failure(candidate.ErrorValue());
                auto image = RenderPrefabAssetPreview(candidate.Value(), input.width, input.height, cancellation);
                if (image.HasError())
                    return image;
                auto current = capture_(registry_, roots, limits.Value(), cancellation);
                if (current.HasError())
                    return PreviewResult::Failure(current.ErrorValue());
                if (current.Value().sources.inputs.ClosureDigest() != captured.Value().inputs.ClosureDigest())
                    return PreviewResult::Failure(MakeError(Prefab::PrefabErrors::ResolutionStale));
                auto unchanged = current.Value().sources.inputs.VerifyUnchanged(cancellation);
                if (unchanged.HasError())
                    return PreviewResult::Failure(unchanged.ErrorValue());
                return image;
            }

        private:
            Assets::AssetRegistrySnapshot registry_;
            Assets::AssetId asset_;
            PrefabHierarchySourceCapture capture_;
        };
    }  // namespace

    /** @copydoc RenderPrefabAssetPreview */
    Result<Assets::AssetPreviewImage> RenderPrefabAssetPreview(const Prefab::EffectivePrefabCandidate &candidate, const std::uint32_t width,
                                                               const std::uint32_t height, const CancellationToken &cancellation) {
        if (width < 16 || height < 16 || width > 256 || height > 256)
            return PreviewResult::Failure(MakeError(Prefab::PrefabErrors::AdmissionRejected));
        auto collected = Collect(candidate, cancellation);
        if (collected.HasError())
            return PreviewResult::Failure(collected.ErrorValue());
        auto triangles = std::move(collected).Value();
        if (triangles.empty())
            return PreviewResult::Failure(MakeError(Prefab::PrefabErrors::DependencyUnavailable));
        if (auto fit = Fit(triangles, width, height); fit.HasError())
            return PreviewResult::Failure(fit.ErrorValue());
        Assets::AssetPreviewImage image{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4)};
        std::vector<float> depth(static_cast<std::size_t>(width) * height, std::numeric_limits<float>::infinity());
        std::size_t samples{};
        for (const auto &triangle : triangles) {
            if (auto rendered = Rasterize(triangle, image, depth, samples, cancellation); rendered.HasError())
                return PreviewResult::Failure(rendered.ErrorValue());
        }
        return PreviewResult::Success(std::move(image));
    }

    /** @copydoc CreatePrefabAssetPreviewProvider */
    std::shared_ptr<const Assets::IAssetPreviewProvider> CreatePrefabAssetPreviewProvider(Assets::AssetRegistrySnapshot registry,
                                                                                          const Assets::AssetId asset,
                                                                                          PrefabHierarchySourceCapture capture) {
        return std::make_shared<const Provider>(std::move(registry), asset, std::move(capture));
    }
}  // namespace Horo::Editor
