#include "Horo/Navigation/Backends/RecastDetourProvider.h"

#include "Horo/Navigation/NavigationErrors.h"
#include "runtime/navigation/backends/recast_detour/RecastDetourProviderInternal.h"
#include "runtime/navigation/backends/recast_detour/RecastDetourProviderValidation.h"

#include <DetourAlloc.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace Horo::Navigation {
    namespace {
        constexpr std::uint16_t NullPolygonIndex = 0xffffU;
        constexpr std::uint16_t TraversablePolygonFlag = 1U;
        constexpr std::size_t MaximumVerticesPerPolygon = 6;

        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        struct TileDataDeleter final {
            void operator()(unsigned char *data) const noexcept {
                dtFree(data);
            }
        };

        using TileDataPtr = std::unique_ptr<unsigned char, TileDataDeleter>;

        struct NativeTopologyInput final {
            std::vector<unsigned short> vertices;
            std::vector<unsigned short> polygons;
            std::vector<unsigned short> polygonFlags;
            std::vector<unsigned char> polygonAreas;
            std::vector<NavigationPolygonAdjacency> adjacency;
            Math::Vec3 minimum;
            Math::Vec3 maximum;
        };

        struct PolygonEdge final {
            std::uint32_t first{};
            std::uint32_t second{};
            std::uint32_t polygon{};
            std::uint8_t edge{};
            bool ascending{};
        };

        [[nodiscard]] Result<void> ValidateCreateInfo(const RecastDetourProviderCreateInfo &info) {
            if (!Detail::HasValidIdentityAndTopologyBounds(info))
                return Failure<void>(NavigationErrors::CapabilityDescriptorInvalid);
            if (!Detail::HasValidAgentSettings(info))
                return Failure<void>(NavigationErrors::CapabilityDescriptorInvalid);
            if (!Detail::HasValidQuerySettings(info))
                return Failure<void>(NavigationErrors::CapabilityDescriptorInvalid);
            if (!Detail::HasValidMemoryBudget(info))
                return Failure<void>(NavigationErrors::CapacityExceeded);
            if (!std::ranges::all_of(info.vertices, [](const Math::Vec3 vertex) {
                return Math::IsFinite(vertex);
            }))
                return Failure<void>(NavigationErrors::ProviderFailed);
            return Result<void>::Success();
        }

        [[nodiscard]] bool IsCounterClockwiseConvex(const GroundedNavigationPolygon &polygon,
                                                    const std::span<const Math::Vec3> vertices) noexcept {
            for (std::uint8_t corner = 0; corner < polygon.vertexCount; ++corner) {
                const Math::Vec3 &first = vertices[polygon.vertexIndices[corner]];
                const Math::Vec3 &second = vertices[polygon.vertexIndices[(corner + 1U) % polygon.vertexCount]];
                const Math::Vec3 &third = vertices[polygon.vertexIndices[(corner + 2U) % polygon.vertexCount]];
                if (const double cross = ((static_cast<double>(second.x) - first.x) * (static_cast<double>(third.z) - second.z)) -
                                         ((static_cast<double>(second.z) - first.z) * (static_cast<double>(third.x) - second.x));
                    !std::isfinite(cross) || cross <= std::numeric_limits<double>::epsilon())
                    return false;
            }
            return true;
        }

        [[nodiscard]] Result<void> AppendValidatedPolygonEdges(const RecastDetourProviderCreateInfo &info, const std::size_t polygonIndex,
                                                               std::vector<PolygonEdge> &edges) {
            const GroundedNavigationPolygon &polygon = info.polygons[polygonIndex];
            if (polygon.vertexCount < 3 || polygon.vertexCount > MaximumVerticesPerPolygon || !polygon.area.IsValid())
                return Failure<void>(NavigationErrors::ProviderFailed);
            if (!std::ranges::all_of(polygon.vertexIndices.begin() + polygon.vertexCount, polygon.vertexIndices.end(),
                                     [](const std::uint32_t index) {
                return index == 0;
            }))
                return Failure<void>(NavigationErrors::ProviderFailed);

            for (std::uint8_t edgeIndex = 0; edgeIndex < polygon.vertexCount; ++edgeIndex) {
                const std::uint32_t first = polygon.vertexIndices[edgeIndex];
                const std::uint32_t second = polygon.vertexIndices[(edgeIndex + 1U) % polygon.vertexCount];
                if (const auto priorEnd = polygon.vertexIndices.begin() + edgeIndex;
                    first >= info.vertices.size() || second >= info.vertices.size() || first == second ||
                    std::find(polygon.vertexIndices.begin(), priorEnd, first) != priorEnd)
                    return Failure<void>(NavigationErrors::ProviderFailed);
                edges.push_back({.first = std::min(first, second),
                                 .second = std::max(first, second),
                                 .polygon = static_cast<std::uint32_t>(polygonIndex),
                                 .edge = edgeIndex,
                                 .ascending = first < second});
            }
            if (!IsCounterClockwiseConvex(polygon, info.vertices))
                return Failure<void>(NavigationErrors::ProviderFailed);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::vector<PolygonEdge>> ValidatePolygons(const RecastDetourProviderCreateInfo &info) {
            std::vector<PolygonEdge> edges;
            edges.reserve(info.polygons.size() * MaximumVerticesPerPolygon);
            for (std::size_t polygonIndex = 0; polygonIndex < info.polygons.size(); ++polygonIndex) {
                if (auto appended = AppendValidatedPolygonEdges(info, polygonIndex, edges); appended.HasError())
                    return Result<std::vector<PolygonEdge>>::Failure(appended.ErrorValue());
            }
            std::ranges::sort(edges, [](const PolygonEdge &left, const PolygonEdge &right) {
                return std::tuple{left.first, left.second, left.polygon, left.edge, static_cast<std::uint8_t>(left.ascending)} <
                       std::tuple{right.first, right.second, right.polygon, right.edge, static_cast<std::uint8_t>(right.ascending)};
            });
            for (std::size_t index = 0; index < edges.size();) {
                std::size_t end = index + 1U;
                while (end < edges.size() && edges[end].first == edges[index].first && edges[end].second == edges[index].second)
                    ++end;
                if (end - index > 2U)
                    return Failure<std::vector<PolygonEdge>>(NavigationErrors::ProviderFailed);
                if (end - index == 2U && edges[index].ascending == edges[index + 1U].ascending)
                    return Failure<std::vector<PolygonEdge>>(NavigationErrors::ProviderFailed);
                index = end;
            }
            return Result<std::vector<PolygonEdge>>::Success(std::move(edges));
        }

        [[nodiscard]] Result<void> TranslateVertices(const RecastDetourProviderCreateInfo &info, NativeTopologyInput &translated) {
            translated.minimum = info.vertices.front();
            translated.maximum = info.vertices.front();
            for (const Math::Vec3 vertex : info.vertices) {
                translated.minimum.x = std::min(translated.minimum.x, vertex.x);
                translated.minimum.y = std::min(translated.minimum.y, vertex.y);
                translated.minimum.z = std::min(translated.minimum.z, vertex.z);
                translated.maximum.x = std::max(translated.maximum.x, vertex.x);
                translated.maximum.y = std::max(translated.maximum.y, vertex.y);
                translated.maximum.z = std::max(translated.maximum.z, vertex.z);
            }

            translated.vertices.resize(info.vertices.size() * 3U);
            for (std::size_t index = 0; index < info.vertices.size(); ++index) {
                const Math::Vec3 relative = info.vertices[index] - translated.minimum;
                const std::array<double, 3> quantized{std::round(relative.x / info.cellSizeMeters),
                                                      std::round(relative.y / info.cellHeightMeters),
                                                      std::round(relative.z / info.cellSizeMeters)};
                if (std::ranges::any_of(quantized, [](const double value) {
                    return value < 0.0 || value > static_cast<double>(std::numeric_limits<unsigned short>::max());
                }))
                    return Failure<void>(NavigationErrors::ProviderFailed);
                translated.vertices[(index * 3U) + 0U] = static_cast<unsigned short>(quantized[0]);
                translated.vertices[(index * 3U) + 1U] = static_cast<unsigned short>(quantized[1]);
                translated.vertices[(index * 3U) + 2U] = static_cast<unsigned short>(quantized[2]);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> TranslatePolygonData(const RecastDetourProviderCreateInfo &info, NativeTopologyInput &translated) {
            translated.polygons.assign(info.polygons.size() * MaximumVerticesPerPolygon * 2U, NullPolygonIndex);
            translated.polygonFlags.assign(info.polygons.size(), TraversablePolygonFlag);
            translated.polygonAreas.resize(info.polygons.size());
            std::vector<std::uint64_t> areas;
            areas.reserve(info.polygons.size());
            for (const GroundedNavigationPolygon &polygon : info.polygons)
                areas.push_back(polygon.area.Value());
            std::ranges::sort(areas);
            areas.erase(std::ranges::unique(areas).begin(), areas.end());
            if (areas.size() > 64U)
                return Failure<void>(NavigationErrors::ProviderFailed);
            for (std::size_t polygonIndex = 0; polygonIndex < info.polygons.size(); ++polygonIndex) {
                const GroundedNavigationPolygon &polygon = info.polygons[polygonIndex];
                const std::size_t offset = polygonIndex * MaximumVerticesPerPolygon * 2U;
                for (std::uint8_t vertexIndex = 0; vertexIndex < polygon.vertexCount; ++vertexIndex)
                    // Detour's segment clipper expects clockwise XZ polygons; preserve the public canonical CCW contract and
                    // reverse only the adapter-owned native vertex order.
                    translated.polygons[offset + vertexIndex] =
                        static_cast<unsigned short>(polygon.vertexIndices[polygon.vertexCount - 1U - vertexIndex]);
                translated.polygonAreas[polygonIndex] =
                    static_cast<unsigned char>(std::ranges::lower_bound(areas, polygon.area.Value()) - areas.begin());
            }
            return Result<void>::Success();
        }

        /**
         * @brief Maps a canonical edge index to the edge index after reversing polygon winding for Detour.
         * @param vertexCount Active vertex count of the validated polygon.
         * @param canonicalEdge Edge index in the canonical counter-clockwise polygon.
         * @return Corresponding edge index in the reversed native polygon.
         */
        [[nodiscard]] constexpr std::uint8_t DetourEdgeForReversedWinding(const std::uint8_t vertexCount,
                                                                          const std::uint8_t canonicalEdge) noexcept {
            return static_cast<std::uint8_t>((vertexCount + vertexCount - 2U - canonicalEdge) % vertexCount);
        }

        void LinkPolygonNeighbors(const RecastDetourProviderCreateInfo &info, const std::vector<PolygonEdge> &edges,
                                  NativeTopologyInput &translated) {
            std::size_t index{};
            while (index + 1U < edges.size()) {
                const PolygonEdge &first = edges[index];
                const PolygonEdge &second = edges[index + 1U];
                if (first.first != second.first || first.second != second.second) {
                    ++index;
                    continue;
                }
                const std::size_t firstOffset = (first.polygon * MaximumVerticesPerPolygon * 2U) + MaximumVerticesPerPolygon +
                                                DetourEdgeForReversedWinding(info.polygons[first.polygon].vertexCount, first.edge);
                const std::size_t secondOffset = (second.polygon * MaximumVerticesPerPolygon * 2U) + MaximumVerticesPerPolygon +
                                                 DetourEdgeForReversedWinding(info.polygons[second.polygon].vertexCount, second.edge);
                translated.polygons[firstOffset] = static_cast<unsigned short>(second.polygon);
                translated.polygons[secondOffset] = static_cast<unsigned short>(first.polygon);
                index += 2U;
            }
        }

        void BuildPolygonAdjacency(const std::vector<PolygonEdge> &edges, NativeTopologyInput &translated) {
            translated.adjacency.assign(translated.polygonAreas.size(), {});
            std::size_t index{};
            while (index + 1U < edges.size()) {
                const PolygonEdge &first = edges[index];
                const PolygonEdge &second = edges[index + 1U];
                if (first.first != second.first || first.second != second.second) {
                    ++index;
                    continue;
                }
                auto append = [&translated](const std::uint32_t polygon, const std::uint32_t neighbor) {
                    NavigationPolygonAdjacency &adjacency = translated.adjacency[polygon];
                    if (adjacency.count < adjacency.neighbors.size())
                        adjacency.neighbors[adjacency.count++] = neighbor;
                };
                append(first.polygon, second.polygon);
                append(second.polygon, first.polygon);
                index += 2U;
            }
        }

        [[nodiscard]] Result<NativeTopologyInput> TranslateTopology(const RecastDetourProviderCreateInfo &info) {
            auto validatedEdges = ValidatePolygons(info);
            if (validatedEdges.HasError())
                return Result<NativeTopologyInput>::Failure(validatedEdges.ErrorValue());

            NativeTopologyInput translated;
            if (auto vertices = TranslateVertices(info, translated); vertices.HasError())
                return Result<NativeTopologyInput>::Failure(vertices.ErrorValue());
            if (auto polygons = TranslatePolygonData(info, translated); polygons.HasError())
                return Result<NativeTopologyInput>::Failure(polygons.ErrorValue());
            LinkPolygonNeighbors(info, validatedEdges.Value(), translated);
            BuildPolygonAdjacency(validatedEdges.Value(), translated);
            return Result<NativeTopologyInput>::Success(std::move(translated));
        }

        /**
         * @brief Limits Detour BV traversal to the nodes actually written by its recursive builder.
         * @param mesh Initialized single-tile mesh whose section pointers must already be fixed.
         * @param polygonCount Number of grounded polygons used to build the tile.
         * @return Success after trimming the in-memory traversal count, or a provider failure for an unexpected tile layout.
         * @note Detour reserves a 2*N-node BV section but writes a full binary tree with 2*N-1 nodes. The header must retain 2*N
         *       through init/addTile because those functions derive following section offsets from it; only the initialized header's
         *       traversal count is narrowed afterward, leaving the owned blob layout and lifetime unchanged.
         */
        [[nodiscard]] Result<void> NormalizeBoundingVolumeTraversalCount(const dtNavMesh &mesh, const std::size_t polygonCount) {
            if (const auto maximumPolygonCount = (static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1U) / 2U;
                mesh.getMaxTiles() != 1 || polygonCount == 0 || polygonCount > maximumPolygonCount)
                return Failure<void>(NavigationErrors::ProviderFailed);

            const dtMeshTile *tile = mesh.getTile(0);
            if (tile == nullptr || tile->header == nullptr || tile->bvTree == nullptr ||
                tile->header->polyCount != static_cast<int>(polygonCount))
                return Failure<void>(NavigationErrors::ProviderFailed);

            const auto reservedNodeCount = polygonCount * 2U;
            if (tile->header->bvNodeCount != static_cast<int>(reservedNodeCount))
                return Failure<void>(NavigationErrors::ProviderFailed);
            tile->header->bvNodeCount = static_cast<int>(reservedNodeCount - 1U);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<NavMeshPtr> BuildNavMesh(const RecastDetourProviderCreateInfo &info, const NativeTopologyInput &input) {
            dtNavMeshCreateParams parameters{};
            parameters.verts = input.vertices.data();
            parameters.vertCount = static_cast<int>(info.vertices.size());
            parameters.polys = input.polygons.data();
            parameters.polyFlags = input.polygonFlags.data();
            parameters.polyAreas = input.polygonAreas.data();
            parameters.polyCount = static_cast<int>(info.polygons.size());
            parameters.nvp = static_cast<int>(MaximumVerticesPerPolygon);
            parameters.walkableHeight = info.walkableHeightMeters;
            parameters.walkableRadius = info.walkableRadiusMeters;
            parameters.walkableClimb = info.walkableClimbMeters;
            parameters.cs = info.cellSizeMeters;
            parameters.ch = info.cellHeightMeters;
            parameters.buildBvTree = true;
            const std::array<float, 3> minimum{input.minimum.x, input.minimum.y, input.minimum.z};
            const std::array<float, 3> maximum{input.maximum.x, input.maximum.y, input.maximum.z};
            std::ranges::copy(minimum, parameters.bmin);
            std::ranges::copy(maximum, parameters.bmax);

            unsigned char *rawTileData{};
            int tileDataSize{};
            if (!dtCreateNavMeshData(&parameters, &rawTileData, &tileDataSize) || rawTileData == nullptr || tileDataSize <= 0)
                return Failure<NavMeshPtr>(NavigationErrors::ProviderFailed);
            TileDataPtr tileData{rawTileData};
            if (static_cast<std::size_t>(tileDataSize) > info.maximumOwnedBytes)
                return Failure<NavMeshPtr>(NavigationErrors::CapacityExceeded);

            NavMeshPtr mesh{dtAllocNavMesh()};
            if (!mesh)
                return Failure<NavMeshPtr>(NavigationErrors::CapacityExceeded);
            if (const dtStatus initialized = mesh->init(tileData.get(), tileDataSize, DT_TILE_FREE_DATA); dtStatusFailed(initialized))
                return Failure<NavMeshPtr>(dtStatusDetail(initialized, DT_OUT_OF_MEMORY) ? NavigationErrors::CapacityExceeded
                                                                                         : NavigationErrors::ProviderFailed);
            if (tileData.release() != rawTileData)
                return Failure<NavMeshPtr>(NavigationErrors::ProviderFailed);
            if (const auto normalized = NormalizeBoundingVolumeTraversalCount(*mesh, info.polygons.size()); normalized.HasError())
                return Result<NavMeshPtr>::Failure(normalized.ErrorValue());
            return Result<NavMeshPtr>::Success(std::move(mesh));
        }

        [[nodiscard]] Result<std::vector<QuerySlot>> BuildQuerySlots(const RecastDetourProviderCreateInfo &info, const dtNavMesh &mesh) {
            std::vector<QuerySlot> slots;
            slots.reserve(info.maximumConcurrentQueries);
            const auto polygonScratch = std::max(info.maximumQueryNodes, info.maximumResultPoints);
            for (std::uint32_t index = 0; index < info.maximumConcurrentQueries; ++index) {
                slots.emplace_back();
                QuerySlot &slot = slots.back();
                slot.query.reset(dtAllocNavMeshQuery());
                if (!slot.query || dtStatusFailed(slot.query->init(&mesh, static_cast<int>(info.maximumQueryNodes))))
                    return Failure<std::vector<QuerySlot>>(NavigationErrors::CapacityExceeded);
                slot.polygonPath.resize(polygonScratch);
                slot.straightPoints.resize(static_cast<std::size_t>(info.maximumResultPoints) * 3U);
                slot.straightFlags.resize(info.maximumResultPoints);
                slot.straightPolygons.resize(info.maximumResultPoints);
                slot.searchNodes.resize(info.maximumQueryNodes);
                slot.openNodes.resize(info.maximumQueryNodes);
                slot.polygonPathIndices.resize(info.maximumQueryNodes);
            }
            return Result<std::vector<QuerySlot>>::Success(std::move(slots));
        }

        [[nodiscard]] Result<std::vector<Math::Vec3>> BuildPolygonCenters(const RecastDetourProviderCreateInfo &info) {
            try {
                std::vector<Math::Vec3> centers;
                centers.reserve(info.polygons.size());
                for (const GroundedNavigationPolygon &polygon : info.polygons) {
                    Math::Vec3 center{};
                    for (std::uint8_t index = 0; index < polygon.vertexCount; ++index)
                        center += info.vertices[polygon.vertexIndices[index]];
                    center = center / static_cast<float>(polygon.vertexCount);
                    if (!Math::IsFinite(center))
                        return Failure<std::vector<Math::Vec3>>(NavigationErrors::ProviderFailed);
                    centers.push_back(center);
                }
                return Result<std::vector<Math::Vec3>>::Success(std::move(centers));
            } catch (const std::bad_alloc &) {
                return Failure<std::vector<Math::Vec3>>(NavigationErrors::CapacityExceeded);
            }
        }

        [[nodiscard]] Result<std::vector<dtPolyRef>> BuildPolygonReferences(const dtNavMesh &mesh, const std::size_t polygonCount) {
            const dtMeshTile *tile = mesh.getTile(0);
            if (tile == nullptr || tile->header == nullptr || tile->header->polyCount != static_cast<int>(polygonCount))
                return Failure<std::vector<dtPolyRef>>(NavigationErrors::ProviderFailed);
            const dtPolyRef base = mesh.getPolyRefBase(tile);
            try {
                std::vector<dtPolyRef> references;
                references.reserve(polygonCount);
                for (std::size_t index = 0; index < polygonCount; ++index) {
                    const dtPolyRef reference = base + static_cast<dtPolyRef>(index);
                    if (!mesh.isValidPolyRef(reference))
                        return Failure<std::vector<dtPolyRef>>(NavigationErrors::ProviderFailed);
                    references.push_back(reference);
                }
                return Result<std::vector<dtPolyRef>>::Success(std::move(references));
            } catch (const std::bad_alloc &) {
                return Failure<std::vector<dtPolyRef>>(NavigationErrors::CapacityExceeded);
            }
        }

    }  // namespace

    /** @copydoc CreateRecastDetourNavigationQueryBackend */
    Result<std::unique_ptr<INavigationQueryBackend>> CreateRecastDetourNavigationQueryBackend(const RecastDetourProviderCreateInfo &info) {
        if (const auto validated = ValidateCreateInfo(info); validated.HasError())
            return Result<std::unique_ptr<INavigationQueryBackend>>::Failure(validated.ErrorValue());
        try {
            auto areaRegistry = NavigationAreaRegistry::Create(info.areas, info.filters);
            if (areaRegistry.HasError())
                return Result<std::unique_ptr<INavigationQueryBackend>>::Failure(areaRegistry.ErrorValue());
            for (const GroundedNavigationPolygon &polygon : info.polygons) {
                if (const auto resolved = areaRegistry.Value().ResolveArea(polygon.area); resolved.HasError())
                    return Result<std::unique_ptr<INavigationQueryBackend>>::Failure(resolved.ErrorValue());
            }
            auto translated = TranslateTopology(info);
            if (translated.HasError())
                return Result<std::unique_ptr<INavigationQueryBackend>>::Failure(translated.ErrorValue());
            auto mesh = BuildNavMesh(info, translated.Value());
            if (mesh.HasError())
                return Result<std::unique_ptr<INavigationQueryBackend>>::Failure(mesh.ErrorValue());
            auto centers = BuildPolygonCenters(info);
            if (centers.HasError())
                return Result<std::unique_ptr<INavigationQueryBackend>>::Failure(centers.ErrorValue());
            auto references = BuildPolygonReferences(*mesh.Value(), info.polygons.size());
            if (references.HasError())
                return Result<std::unique_ptr<INavigationQueryBackend>>::Failure(references.ErrorValue());
            auto slots = BuildQuerySlots(info, *mesh.Value());
            if (slots.HasError())
                return Result<std::unique_ptr<INavigationQueryBackend>>::Failure(slots.ErrorValue());
            std::vector<Math::Vec3> vertices{info.vertices.begin(), info.vertices.end()};
            std::vector<GroundedNavigationPolygon> polygons{info.polygons.begin(), info.polygons.end()};
            RecastDetourQueryBackendData data{.mesh = std::move(mesh).Value(),
                                              .slots = std::move(slots).Value(),
                                              .vertices = std::move(vertices),
                                              .polygons = std::move(polygons),
                                              .adjacency = std::move(translated).Value().adjacency,
                                              .polygonCenters = std::move(centers).Value(),
                                              .polygonReferences = std::move(references).Value(),
                                              .areaRegistry = std::move(areaRegistry).Value()};
            return MakeRecastDetourNavigationQueryBackend(info, std::move(data));
        } catch (const std::bad_alloc &) {
            return Failure<std::unique_ptr<INavigationQueryBackend>>(NavigationErrors::CapacityExceeded);
        }
    }
}  // namespace Horo::Navigation
