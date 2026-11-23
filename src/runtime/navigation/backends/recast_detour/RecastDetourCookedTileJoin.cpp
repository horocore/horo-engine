#include "RecastDetourCookedTileJoin.h"

#include "Horo/Navigation/NavigationErrors.h"

#include <algorithm>
#include <new>
#include <numeric>
#include <tuple>

namespace Horo::Navigation::Detail {
    namespace {
        using Joined = Result<RecastDetourJoinedTileTopology>;

        enum class Side : std::uint8_t {
            Interior,
            LowX,
            HighX,
            LowZ,
            HighZ
        };

        /** @brief One ordered perimeter edge with exact endpoints and its neutral neighbor row. */
        struct Edge final {
            NavMeshTileKey tile;
            Math::Vec3 low;
            Math::Vec3 high;
            std::uint32_t lowIndex;
            std::uint32_t highIndex;
            std::uint32_t polygon;
            std::uint32_t neighborRow;
            Side side;
            bool ascending;

            [[nodiscard]] auto Key() const noexcept {
                return std::tie(tile.layer, low.x, low.y, low.z, high.x, high.y, high.z);
            }
        };

        /** @brief Classifies an exact tile-grid perimeter edge, leaving internal boundaries disconnected. */
        [[nodiscard]] Side BoundarySide(const Math::Aabb &bounds, const Math::Vec3 &a, const Math::Vec3 &b) noexcept {
            using enum Side;
            if (a.x == b.x && a.z != b.z) {
                if (a.x == bounds.minimum.x)
                    return LowX;
                if (a.x == bounds.maximum.x)
                    return HighX;
            }
            if (a.z == b.z && a.x != b.x) {
                if (a.z == bounds.minimum.z)
                    return LowZ;
                if (a.z == bounds.maximum.z)
                    return HighZ;
            }
            return Interior;
        }

        /** @brief Requires geometric edge matches to be reciprocal grid neighbors, with opposite winding. */
        [[nodiscard]] bool Opposes(const Edge &a, const Edge &b) noexcept {
            using enum Side;
            const auto dx = static_cast<std::int64_t>(b.tile.x) - a.tile.x;
            const auto dz = static_cast<std::int64_t>(b.tile.z) - a.tile.z;
            if (a.ascending == b.ascending)
                return false;
            if (dz == 0)
                return (dx == 1 && a.side == HighX && b.side == LowX) || (dx == -1 && a.side == LowX && b.side == HighX);
            return dx == 0 && ((dz == 1 && a.side == HighZ && b.side == LowZ) || (dz == -1 && a.side == LowZ && b.side == HighZ));
        }

        /** @brief Appends one tile's local neighbor table using aggregate vertex and polygon addresses. */
        void AppendTile(const NavigationTileBuildResult &tile, NavigationTileBuildResult &joined, std::vector<Edge> &edges) {
            const auto vertexBase = static_cast<std::uint32_t>(joined.vertices.size());
            const auto polygonBase = static_cast<std::uint32_t>(joined.polygons.size());
            joined.vertices.insert(joined.vertices.end(), tile.vertices.begin(), tile.vertices.end());
            for (const auto &polygon : tile.polygons) {
                const auto firstIndex = static_cast<std::uint32_t>(joined.polygonVertexIndices.size());
                const auto firstNeighbor = static_cast<std::uint32_t>(joined.polygonAdjacencies.size());
                const auto indices = std::span{tile.polygonVertexIndices}.subspan(polygon.vertexIndices.first, polygon.vertexIndices.count);
                const auto neighbors = std::span{tile.polygonAdjacencies}.subspan(polygon.adjacencies.first, polygon.adjacencies.count);
                const auto polygonIndex = static_cast<std::uint32_t>(joined.polygons.size());
                joined.polygons.push_back(
                    {{firstIndex, polygon.vertexIndices.count}, {firstNeighbor, polygon.adjacencies.count}, polygon.area});
                for (std::uint32_t i = 0; i < indices.size(); ++i) {
                    joined.polygonVertexIndices.push_back(vertexBase + indices[i]);
                    const auto neighbor = neighbors[i];
                    joined.polygonAdjacencies.push_back(neighbor == NavMeshBoundaryAdjacency ? neighbor : polygonBase + neighbor);
                    if (neighbor != NavMeshBoundaryAdjacency)
                        continue;
                    const auto next = indices[(i + 1) % indices.size()];
                    const auto &a = tile.vertices[indices[i]];
                    const auto &b = tile.vertices[next];
                    const auto side = BoundarySide(tile.bounds, a, b);
                    if (side == Side::Interior)
                        continue;
                    const bool ascending = std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
                    edges.push_back({tile.key, ascending ? a : b, ascending ? b : a, vertexBase + (ascending ? indices[i] : next),
                                     vertexBase + (ascending ? next : indices[i]), polygonIndex, firstNeighbor + i, side, ascending});
                }
            }
        }

        /** @brief Preserves both endpoints of a decoded tile-local authored link in the aggregate polygon table. */
        void AppendLinks(const NavigationTileBuildResult &tile, NavigationTileBuildResult &joined) {
            const auto polygonBase = static_cast<std::uint32_t>(joined.polygons.size() - tile.polygons.size());
            for (auto link : tile.offMeshLinks) {
                link.startPolygon += polygonBase;
                link.endPolygon += polygonBase;
                joined.offMeshLinks.push_back(link);
            }
        }

        /** @brief Requires one portable authored identity per routeable link in the exact provider closure. */
        [[nodiscard]] bool HasUniqueLinks(std::vector<NavMeshOffMeshLink> &links) {
            std::ranges::sort(links, {}, &NavMeshOffMeshLink::authoredLink);
            return std::ranges::adjacent_find(links, {}, &NavMeshOffMeshLink::authoredLink) == links.end();
        }

        /** @brief Returns a stable welded vertex representative and shortens subsequent lookups. */
        [[nodiscard]] std::uint32_t Representative(std::span<std::uint32_t> vertices, std::uint32_t id) noexcept {
            while (vertices[id] != id) {
                vertices[id] = vertices[vertices[id]];
                id = vertices[id];
            }
            return id;
        }

        /** @brief Welds only vertex addresses established by a paired edge. */
        void Weld(std::span<std::uint32_t> vertices, std::uint32_t a, std::uint32_t b) noexcept {
            a = Representative(vertices, a);
            b = Representative(vertices, b);
            vertices[std::max(a, b)] = std::min(a, b);
        }

        /** @brief Reifies perimeter adjacency before the flat builder verifies the complete edge table. */
        [[nodiscard]] bool JoinEdges(NavigationTileBuildResult &topology, std::vector<Edge> &edges) {
            std::ranges::sort(edges, std::less<>{}, &Edge::Key);
            std::vector<std::uint32_t> vertices(topology.vertices.size());
            std::iota(vertices.begin(), vertices.end(), 0U);
            for (std::size_t index = 0; index < edges.size();) {
                const auto &a = edges[index++];
                if (index == edges.size() || a.Key() != edges[index].Key())
                    continue;
                const auto &b = edges[index++];
                if (index < edges.size() && a.Key() == edges[index].Key())
                    return false;
                if (!Opposes(a, b))
                    continue;
                topology.polygonAdjacencies[a.neighborRow] = b.polygon;
                topology.polygonAdjacencies[b.neighborRow] = a.polygon;
                Weld(vertices, a.lowIndex, b.lowIndex);
                Weld(vertices, a.highIndex, b.highIndex);
            }
            for (auto &vertex : topology.polygonVertexIndices)
                vertex = Representative(vertices, vertex);
            return true;
        }

        struct TileStorage final {
            std::size_t vertices{};
            std::size_t polygons{};
            std::size_t edges{};
            std::size_t links{};

            [[nodiscard]] bool Add(const NavigationTileBuildResult &tile) noexcept {
                if (tile.vertices.size() > RecastDetourProviderHardLimits::Vertices - vertices ||
                    tile.polygons.size() > RecastDetourProviderHardLimits::Polygons - polygons ||
                    tile.offMeshLinks.size() > NavMeshArtifactLimits::MaximumOffMeshLinks - links)
                    return false;
                vertices += tile.vertices.size();
                polygons += tile.polygons.size();
                edges += tile.polygonVertexIndices.size();
                links += tile.offMeshLinks.size();
                return true;
            }

            [[nodiscard]] std::size_t Bytes() const noexcept {
                return vertices * (sizeof(Math::Vec3) + sizeof(std::uint32_t)) + polygons * sizeof(NavMeshPolygon) +
                       edges * (2 * sizeof(std::uint32_t) + sizeof(Edge)) + links * sizeof(NavMeshOffMeshLink);
            }
        };
    }  // namespace

    /** @copydoc JoinRecastDetourCookedTiles */
    Joined JoinRecastDetourCookedTiles(std::span<const std::shared_ptr<const NavigationCookedTile>> tiles, std::size_t maximumOwnedBytes) {
        TileStorage storage;
        for (const auto &tile : tiles)
            if (!storage.Add(tile->Topology()))
                return Joined::Failure(MakeError(NavigationErrors::CapacityExceeded));
        const auto reserved = storage.Bytes();
        if (reserved >= maximumOwnedBytes)
            return Joined::Failure(MakeError(NavigationErrors::CapacityExceeded));
        try {
            NavigationTileBuildResult topology;
            topology.vertices.reserve(storage.vertices);
            topology.polygons.reserve(storage.polygons);
            topology.polygonVertexIndices.reserve(storage.edges);
            topology.polygonAdjacencies.reserve(storage.edges);
            topology.offMeshLinks.reserve(storage.links);
            std::vector<Edge> edges;
            edges.reserve(storage.edges);
            for (const auto &tile : tiles) {
                AppendTile(tile->Topology(), topology, edges);
                AppendLinks(tile->Topology(), topology);
            }
            if (!HasUniqueLinks(topology.offMeshLinks))
                return Joined::Failure(
                    MakeError(NavigationErrors::DescriptorConflict, "Duplicate authored link identity in the tile closure."));
            if (!JoinEdges(topology, edges))
                return Joined::Failure(MakeError(NavigationErrors::OperationUnsupported, "Ambiguous cooked tile perimeter edges."));
            return Joined::Success({std::move(topology), reserved});
        } catch (const std::bad_alloc &) {
            return Joined::Failure(MakeError(NavigationErrors::CapacityExceeded));
        }
    }
}  // namespace Horo::Navigation::Detail
