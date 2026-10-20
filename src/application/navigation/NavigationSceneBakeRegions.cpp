#include "NavigationSceneBakeGraphInternal.h"

#include <algorithm>
#include <bit>

namespace Horo::Application::NavigationBakeDetail {
    namespace {
        /** @brief Fixed-width canonical region hashing; qualified metadata is far below SHA-256's length ceiling. */
        class RegionHash final {
        public:
            RegionHash() {
                constexpr std::string_view domain{"horo.navigation.source-regions.v1"};
                static_cast<void>(hash_.Update(std::as_bytes(std::span{domain.data(), domain.size()})));
            }

            void Integer(std::uint64_t value) {
                std::array<std::byte, 8> bytes{};
                for (std::size_t i = 0; i < bytes.size(); ++i)
                    bytes[i] = static_cast<std::byte>(value >> (i * 8U));
                static_cast<void>(hash_.Update(bytes));
            }

            void Vector(Math::Vec3 value) {
                Float(value.x);
                Float(value.y);
                Float(value.z);
            }

            void Float(float value) {
                Integer(std::bit_cast<std::uint32_t>(value == 0.0F ? 0.0F : value));
            }

            Sha256Digest Finish() {
                return hash_.Finalize();
            }

        private:
            Sha256Builder hash_;
        };
    }  // namespace

    /** @copydoc SourceRegionsDigest */
    Sha256Digest SourceRegionsDigest(std::span<const SourceRegion> regions) {
        RegionHash hash;
        hash.Integer(regions.size());
        for (const auto &region : regions) {
            const auto &a = region.authored;
            hash.Integer(region.owner.value);
            hash.Integer(a.id.Value());
            hash.Integer(a.surface.Value());
            hash.Integer(a.schemaVersion);
            hash.Integer(a.generation);
            hash.Integer(static_cast<std::uint64_t>(a.mode));
            hash.Integer(static_cast<std::uint64_t>(a.sourceSelection));
            hash.Vector(a.localBounds.center);
            hash.Vector(a.localBounds.halfExtents);
            for (const auto value : region.world.values)
                hash.Float(value);
        }
        return hash.Finish();
    }

    /** @copydoc CaptureSceneRegions */
    Result<void> CaptureSceneRegions(Graph &graph, const NavigationDefinitionBakeSource &definition,
                                     const NavigationSceneGeometryPolicy &policy, CancellationToken cancellation) {
        std::size_t count{};
        for (const auto &node : graph.nodes) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(Navigation::NavigationErrors::BakeInputCancelled));
            const auto &authored = node.object->components.navigationRegion;
            if (!authored || !authored->enabled)
                continue;
            if (const auto &surface = *graph.nodes[graph.surfaces.at(authored->surface)].object->components.navigationSurface;
                !surface.enabled || surface.definition != definition.asset)
                continue;
            if (count == policy.maximumRegions)
                return Result<void>::Failure(MakeError(Navigation::NavigationErrors::CapacityExceeded,
                                                       "Navigation region metadata exceeds the qualified Scene bound"));
            auto inverse = Math::TryInverseAffine(node.world);
            if (inverse.HasError())
                return Result<void>::Failure(inverse.ErrorValue());
            graph.regions[authored->surface].values.emplace_back(*authored, Runtime::SceneObjectId{node.object->id.value}, node.world,
                                                                 inverse.Value());
            ++count;
        }
        for (auto &[surface, regions] : graph.regions) {
            static_cast<void>(surface);
            std::ranges::sort(regions.values, {}, [](const auto &region) {
                return region.authored.id;
            });
            regions.digest = SourceRegionsDigest(regions.values);
            regions.hasIncludes = std::ranges::any_of(regions.values, [](const auto &region) {
                return region.authored.mode == Runtime::NavigationRegionMode::Include;
            });
            regions.hasExcludes = std::ranges::any_of(regions.values, [](const auto &region) {
                return region.authored.mode == Runtime::NavigationRegionMode::Exclude;
            });
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Application::NavigationBakeDetail
