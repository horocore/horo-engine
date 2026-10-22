#include "Horo/Editor/PartitionedWorldWorkspace.h"
#include "Horo/Foundation/Utf8.h"
#include "Horo/WorldStreaming/WorldSpatialAssignment.h"

#include <algorithm>
#include <map>

namespace Horo::Editor {
    namespace {
        namespace WS = WorldStreaming;

        using LayerRows = std::map<WS::StreamingLayerId, WorldBrowserLayer>;
        using CellRows = std::map<WS::StreamingCellId, WorldBrowserCell, WS::StreamingCellCanonicalLess>;

        /** @brief Bounded detached metadata for the shared authoring assignment kernel. */
        struct CoverageInput final {
            LayerRows layers;
            std::vector<WS::WorldSpatialAssignmentCandidate> candidates;
            std::map<WS::WorldAuthoringObjectAddress, bool> openObjects;
        };

        /** @brief Preserves stable typed errors through projection preparation. */
        template <typename T> Result<T> Failure(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Folds only ASCII letters without corrupting non-ASCII UTF-8 bytes. */
        std::string SearchKey(std::string value) {
            for (char &byte : value)
                if (byte >= 'A' && byte <= 'Z')
                    byte = static_cast<char>(byte + ('a' - 'A'));
            return value;
        }

        /** @brief Tests a complete source working-set count against a supported status filter. */
        bool MatchesStatus(const WorldBrowserStatus status, const std::uint32_t total, const std::uint32_t open) {
            switch (status) {
                case WorldBrowserStatus::All:
                    return true;
                case WorldBrowserStatus::Open:
                    return total != 0 && total == open;
                case WorldBrowserStatus::Closed:
                    return open == 0;
                case WorldBrowserStatus::Mixed:
                    return open != 0 && open < total;
            }
            return false;
        }

        /** @brief Captures per-layer source counts and spatial candidates without mutating working-set membership. */
        Result<CoverageInput> PrepareCoverage(const PartitionedWorldSnapshot &source, const std::stop_token cancellation) {
            CoverageInput input;
            for (const auto &layer : source.document.layers)
                input.layers.emplace(layer.id, WorldBrowserLayer{.descriptor = layer});
            for (const auto &page : source.document.pages) {
                if (cancellation.stop_requested())
                    return Failure<CoverageInput>(WorldDocumentErrors::Cancelled);
                const bool open = std::ranges::find(source.openPages, page.head.sourceAsset) != source.openPages.end();
                for (const auto &object : page.objects) {
                    auto &layer = input.layers.at(object.layer);
                    ++layer.objects;
                    layer.openObjects += open ? 1U : 0U;
                    if (object.descriptor.placement == WS::WorldSpatialObjectPlacementClass::AlwaysPresent) {
                        ++layer.alwaysPresentObjects;
                        continue;
                    }
                    input.candidates.push_back({.address = object.descriptor.address,
                                                .revision = object.descriptor.revision,
                                                .bounds = object.descriptor.bounds,
                                                .layer = object.layer,
                                                .lod = object.assignmentLod});
                    input.openObjects.emplace(object.descriptor.address, open);
                }
            }
            return Result<CoverageInput>::Success(std::move(input));
        }

        /** @brief Counts canonical source coverage through the ordinary bounded spatial-assignment kernel. */
        Result<CellRows> DeriveCells(const PartitionedWorldSnapshot &source, const CoverageInput &input,
                                     const std::stop_token cancellation) {
            CellRows cells;
            if (input.candidates.empty())
                return Result<CellRows>::Success(std::move(cells));
            const auto &settings = source.document.settings;
            const auto coverage = WS::WorldSpatialAssignment::CreateForAuthoring({.partition = settings.partition,
                                                                                  .bounds = settings.bounds,
                                                                                  .grid = settings.grid,
                                                                                  .layers = source.document.layers},
                                                                                 input.candidates,
                                                                                 {.maximumObjects = 65536,
                                                                                  .maximumCellsPerObject = 65536,
                                                                                  .maximumTotalAssignments = 65536});
            if (coverage.HasError())
                return Result<CellRows>::Failure(coverage.ErrorValue());
            const auto entries = coverage.Value().Objects();
            for (std::size_t index = 0; index < entries.size(); ++index) {
                if (cancellation.stop_requested())
                    return Failure<CellRows>(WorldDocumentErrors::Cancelled);
                for (const auto &id : coverage.Value().CellsForObject(index)) {
                    auto &cell = cells.try_emplace(id, WorldBrowserCell{.id = id}).first->second;
                    ++cell.objects;
                    cell.openObjects += input.openObjects.at(entries[index].address) ? 1U : 0U;
                }
            }
            return Result<CellRows>::Success(std::move(cells));
        }

        /** @brief Produces stable human-search text without ever using it as a row identity. */
        std::string CellSearchText(const WS::StreamingCellId &id, const LayerRows &layers) {
            return SearchKey(layers.at(id.layer).descriptor.name) + " " + std::to_string(id.layer.Value()) + " " + std::to_string(id.x) +
                   "," + std::to_string(id.y) + "," + std::to_string(id.z) + " " + std::to_string(id.lod);
        }

        /** @brief Applies presentation filters and rejects overflow instead of publishing truncated rows. */
        Result<WorldBrowserSnapshot> FilterRows(PartitionedWorldSnapshotLease source, const LayerRows &layers, const CellRows &cells,
                                                const WorldBrowserQuery &query, const std::stop_token cancellation) {
            WorldBrowserSnapshot output{.source = std::move(source)};
            const auto search = SearchKey(query.search);
            for (const auto &[id, layer] : layers) {
                if (query.layer && *query.layer != id)
                    continue;
                const auto text = SearchKey(layer.descriptor.name) + " " + std::to_string(id.Value());
                if (MatchesStatus(query.status, layer.objects, layer.openObjects) && text.find(search) != std::string::npos)
                    output.layers.push_back(layer);
            }
            for (const auto &[id, cell] : cells) {
                if (query.layer && *query.layer != id.layer)
                    continue;
                if (!MatchesStatus(query.status, cell.objects, cell.openObjects) ||
                    CellSearchText(id, layers).find(search) == std::string::npos)
                    continue;
                if (output.cells.size() == query.maximumCells)
                    return Failure<WorldBrowserSnapshot>(WorldDocumentErrors::Capacity);
                output.cells.push_back(cell);
            }
            if (cancellation.stop_requested())
                return Failure<WorldBrowserSnapshot>(WorldDocumentErrors::Cancelled);
            return Result<WorldBrowserSnapshot>::Success(std::move(output));
        }
    }  // namespace

    /** @copydoc PartitionedWorldWorkspace::Browse */
    Result<WorldBrowserSnapshot> PartitionedWorldWorkspace::Browse(const WorldBrowserQuery &query, const WorldDocumentFence expected,
                                                                   const std::stop_token cancellation) const {
        const auto captured = Snapshot();
        if (captured.HasError())
            return Result<WorldBrowserSnapshot>::Failure(captured.ErrorValue());
        const auto fail = [](const ErrorCodeDescriptor &error) {
            return Result<WorldBrowserSnapshot>::Failure(MakeError(error));
        };
        if (captured.Value()->fence != expected)
            return fail(WorldDocumentErrors::Stale);
        if (cancellation.stop_requested())
            return fail(WorldDocumentErrors::Cancelled);
        if (query.search.size() > 256 || query.maximumCells == 0 || query.maximumCells > 65536)
            return fail(WorldDocumentErrors::Capacity);
        if (!IsValidUtf8ScalarSequence(query.search) || query.search.find('\0') != std::string::npos ||
            (query.layer && !query.layer->IsValid()))
            return fail(WorldDocumentErrors::Invalid);
        if (query.status > WorldBrowserStatus::Mixed)
            return fail(WorldDocumentErrors::Unsupported);
        const auto &source = *captured.Value();
        if (query.layer && std::ranges::none_of(source.document.layers, [&](const auto &layer) {
            return layer.id == *query.layer;
        }))
            return fail(WorldDocumentErrors::Conflict);

        auto input = PrepareCoverage(source, cancellation);
        if (input.HasError())
            return Result<WorldBrowserSnapshot>::Failure(input.ErrorValue());
        auto coverage = DeriveCells(source, input.Value(), cancellation);
        if (coverage.HasError())
            return Result<WorldBrowserSnapshot>::Failure(coverage.ErrorValue());
        return FilterRows(captured.Value(), input.Value().layers, coverage.Value(), query, cancellation);
    }
}  // namespace Horo::Editor
