#include "Horo/WorldStreaming/WorldPartitionRegistry.h"

#include "Horo/WorldStreaming/WorldStreamingErrors.h"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <numeric>
#include <utility>
#include <vector>

namespace Horo::WorldStreaming {
    namespace {
        constexpr std::size_t SpatialIndexLeafCapacity = 8;
        constexpr std::size_t SpatialIndexTraversalCapacity = 64;

        /** @brief Flat immutable BVH node; leaves address a contiguous cell-slot range. */
        struct SpatialIndexNode final {
            WorldPartitionBounds bounds{};
            std::uint32_t first{};
            std::uint32_t count{};
            std::uint32_t left{};
            std::uint32_t right{};

            [[nodiscard]] bool IsLeaf() const noexcept {
                return count != 0;
            }
        };

        /** @brief Publication-owned cell bounds, BVH permutation, and flat traversal nodes. */
        struct SpatialIndex final {
            std::vector<WorldPartitionBounds> cellBounds;
            std::vector<std::uint32_t> cellSlots;
            std::vector<SpatialIndexNode> nodes;
        };
    }  // namespace

    struct WorldPartitionRegistrySnapshot::State final {
        WorldPartitionRegistryBinding binding{};
        WorldPartitionRegistryLimits limits{};
        WorldPartitionDescriptor descriptor;
        SpatialIndex spatialIndex;

        State(WorldPartitionRegistryBinding bindingValue, const WorldPartitionRegistryLimits limitsValue,
              WorldPartitionDescriptor descriptorValue, SpatialIndex spatialIndexValue) noexcept
            : binding(std::move(bindingValue)), limits(limitsValue), descriptor(std::move(descriptorValue)),
              spatialIndex(std::move(spatialIndexValue)) {}
    };

    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool Ordered(const WorldPartitionBounds &bounds) noexcept {
            const auto minimum = bounds.minimum.Millimeters();
            const auto maximum = bounds.maximum.Millimeters();
            return minimum[0] <= maximum[0] && minimum[1] <= maximum[1] && minimum[2] <= maximum[2];
        }

        struct BoundsAxes final {
            std::array<std::int64_t, 3> minimum{};
            std::array<std::int64_t, 3> maximum{};
        };

        /** @brief Projects exact coordinate endpoints once for spatial predicates and BVH construction. */
        [[nodiscard]] BoundsAxes Axes(const WorldPartitionBounds &bounds) noexcept {
            return {bounds.minimum.Millimeters(), bounds.maximum.Millimeters()};
        }

        [[nodiscard]] bool MultiplyChecked(const std::int64_t value, const std::int64_t factor, std::int64_t &product) noexcept {
            if (factor <= 0)
                return false;
            if (value > 0 && value > std::numeric_limits<std::int64_t>::max() / factor)
                return false;
            if (value < 0 && value < std::numeric_limits<std::int64_t>::min() / factor)
                return false;
            product = value * factor;
            return true;
        }

        [[nodiscard]] bool AddChecked(const std::int64_t left, const std::int64_t right, std::int64_t &sum) noexcept {
            if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
                (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right))
                return false;
            sum = left + right;
            return true;
        }

        [[nodiscard]] bool CellAxisBounds(const std::int64_t origin, const std::int32_t coordinate, const std::int64_t size,
                                          std::int64_t &minimum, std::int64_t &maximum) noexcept {
            std::int64_t offset{};
            return MultiplyChecked(coordinate, size, offset) && AddChecked(origin, offset, minimum) &&
                   AddChecked(minimum, size - 1, maximum);
        }

        [[nodiscard]] bool CellBounds(const WorldCellQuantizationPolicy &grid, const StreamingCellId &cell,
                                      WorldPartitionBounds &bounds) noexcept {
            const std::int64_t size = grid.BaseCellSizeMillimeters() << cell.lod;
            const auto origin = grid.Origin().Millimeters();
            std::array<std::int64_t, 3> minimum{};
            std::array<std::int64_t, 3> maximum{};
            if (!CellAxisBounds(origin[0], cell.x, size, minimum[0], maximum[0]) ||
                !CellAxisBounds(origin[1], cell.y, size, minimum[1], maximum[1]) ||
                !CellAxisBounds(origin[2], cell.z, size, minimum[2], maximum[2]))
                return false;
            bounds = {Math::WorldCoordinate64::FromMillimeters(minimum[0], minimum[1], minimum[2]),
                      Math::WorldCoordinate64::FromMillimeters(maximum[0], maximum[1], maximum[2])};
            return true;
        }

        [[nodiscard]] bool Intersects(const WorldPartitionBounds &left, const WorldPartitionBounds &right) noexcept {
            const auto leftAxes = Axes(left);
            const auto rightAxes = Axes(right);
            return leftAxes.minimum[0] <= rightAxes.maximum[0] && leftAxes.maximum[0] >= rightAxes.minimum[0] &&
                   leftAxes.minimum[1] <= rightAxes.maximum[1] && leftAxes.maximum[1] >= rightAxes.minimum[1] &&
                   leftAxes.minimum[2] <= rightAxes.maximum[2] && leftAxes.maximum[2] >= rightAxes.minimum[2];
        }

        [[nodiscard]] WorldPartitionBounds MergeBounds(const WorldPartitionBounds &left, const WorldPartitionBounds &right) noexcept {
            const auto leftAxes = Axes(left);
            const auto rightAxes = Axes(right);
            return {Math::WorldCoordinate64::FromMillimeters(std::min(leftAxes.minimum[0], rightAxes.minimum[0]),
                                                             std::min(leftAxes.minimum[1], rightAxes.minimum[1]),
                                                             std::min(leftAxes.minimum[2], rightAxes.minimum[2])),
                    Math::WorldCoordinate64::FromMillimeters(std::max(leftAxes.maximum[0], rightAxes.maximum[0]),
                                                             std::max(leftAxes.maximum[1], rightAxes.maximum[1]),
                                                             std::max(leftAxes.maximum[2], rightAxes.maximum[2]))};
        }

        [[nodiscard]] std::int64_t Center(const WorldPartitionBounds &bounds, const std::size_t axis) noexcept {
            const auto minimum = bounds.minimum.Millimeters()[axis];
            const auto maximum = bounds.maximum.Millimeters()[axis];
            return std::midpoint(minimum, maximum);
        }

        [[nodiscard]] std::size_t LongestAxis(const WorldPartitionBounds &bounds) noexcept {
            const auto minimum = bounds.minimum.Millimeters();
            const auto maximum = bounds.maximum.Millimeters();
            const std::array extents{static_cast<std::uint64_t>(maximum[0]) - static_cast<std::uint64_t>(minimum[0]),
                                     static_cast<std::uint64_t>(maximum[1]) - static_cast<std::uint64_t>(minimum[1]),
                                     static_cast<std::uint64_t>(maximum[2]) - static_cast<std::uint64_t>(minimum[2])};
            if (extents[1] > extents[0] && extents[1] >= extents[2])
                return 1;
            return extents[2] > extents[0] ? 2 : 0;
        }

        [[nodiscard]] bool Matches(const WorldPartitionCellDescriptor &cell, const WorldPartitionBounds &cellBounds,
                                   const WorldPartitionSpatialQuery &query) noexcept {
            if ((query.layer.has_value() && cell.id.layer != *query.layer) || (query.lod.has_value() && cell.id.lod != *query.lod))
                return false;
            return Intersects(cellBounds, query.bounds);
        }

        [[nodiscard]] auto FindCell(const std::span<const WorldPartitionCellDescriptor> cells, const StreamingCellId cell) noexcept {
            return std::ranges::lower_bound(cells, cell, StreamingCellCanonicalLess{}, &WorldPartitionCellDescriptor::id);
        }

        [[nodiscard]] bool IsDeclaredLayer(const WorldPartitionDescriptor &descriptor, const StreamingLayerId layer) noexcept {
            return std::ranges::binary_search(descriptor.Layers(), layer, {}, &WorldLayerDescriptor::id);
        }

        /** @brief Recursively partitions one non-empty slot range into a deterministic balanced BVH. */
        [[nodiscard]] std::uint32_t BuildNode(SpatialIndex &index, const std::size_t begin, const std::size_t end) {
            WorldPartitionBounds bounds = index.cellBounds[index.cellSlots[begin]];
            for (std::size_t slot = begin + 1; slot < end; ++slot)
                bounds = MergeBounds(bounds, index.cellBounds[index.cellSlots[slot]]);

            const auto nodeSlot = static_cast<std::uint32_t>(index.nodes.size());
            index.nodes.emplace_back();
            if (end - begin <= SpatialIndexLeafCapacity) {
                index.nodes[nodeSlot] = {bounds, static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(end - begin), 0, 0};
                return nodeSlot;
            }

            const auto axis = LongestAxis(bounds);
            std::sort(index.cellSlots.begin() + static_cast<std::ptrdiff_t>(begin),
                      index.cellSlots.begin() + static_cast<std::ptrdiff_t>(end), [&index, axis](const auto left, const auto right) {
                const auto leftCenter = Center(index.cellBounds[left], axis);
                if (const auto rightCenter = Center(index.cellBounds[right], axis); leftCenter != rightCenter)
                    return leftCenter < rightCenter;
                return left < right;
            });
            const auto middle = std::midpoint(begin, end);
            const auto left = BuildNode(index, begin, middle);
            const auto right = BuildNode(index, middle, end);
            index.nodes[nodeSlot] = {bounds, 0, 0, left, right};
            return nodeSlot;
        }

        /** @brief Transactionally builds all immutable query acceleration data for one publication. */
        [[nodiscard]] Result<SpatialIndex> BuildSpatialIndex(const WorldPartitionDescriptor &descriptor,
                                                             const WorldPartitionRegistryLimits limits) {
            if (descriptor.Cells().size() > limits.cells)
                return Failure<SpatialIndex>(WorldStreamingErrors::PartitionRegistryCapacityExceeded);
            SpatialIndex index;
            try {
                const auto cellCount = descriptor.Cells().size();
                const auto leafCount = (cellCount + SpatialIndexLeafCapacity - 1) / SpatialIndexLeafCapacity;
                index.cellBounds.reserve(cellCount);
                index.cellSlots.resize(cellCount);
                index.nodes.reserve(leafCount == 0 ? 0 : leafCount * 2 - 1);
            } catch (const std::bad_alloc &) {
                return Failure<SpatialIndex>(WorldStreamingErrors::PartitionRegistryStorageUnavailable);
            }
            for (const auto &cell : descriptor.Cells()) {
                WorldPartitionBounds cellBounds{};
                if (!CellBounds(descriptor.Grid(), cell.id, cellBounds))
                    return Failure<SpatialIndex>(WorldStreamingErrors::PartitionRegistryUnsupported);
                index.cellBounds.emplace_back(cellBounds);
            }
            std::iota(index.cellSlots.begin(), index.cellSlots.end(), 0U);
            try {
                if (!index.cellSlots.empty())
                    static_cast<void>(BuildNode(index, 0, index.cellSlots.size()));
            } catch (const std::bad_alloc &) {
                return Failure<SpatialIndex>(WorldStreamingErrors::PartitionRegistryStorageUnavailable);
            }
            return Result<SpatialIndex>::Success(std::move(index));
        }

        /** @brief Internal traversal counters used for capacity preflight and returned work evidence. */
        struct QueryWork final {
            std::size_t matches{};
            std::size_t candidates{};
            std::size_t nodes{};
        };

        /** @brief Tests one bounded leaf and optionally writes matching generation-fenced handles. */
        void VisitLeaf(const SpatialIndexNode &node, const SpatialIndex &index, const std::span<const WorldPartitionCellDescriptor> cells,
                       const WorldPartitionSpatialQuery &query, const std::span<WorldPartitionCellHandle> output,
                       const WorldPartitionRegistryBinding &binding, QueryWork &work) noexcept {
            for (std::size_t offset = 0; offset < node.count; ++offset) {
                const auto cellSlot = index.cellSlots[node.first + offset];
                ++work.candidates;
                if (!Matches(cells[cellSlot], index.cellBounds[cellSlot], query))
                    continue;
                if (!output.empty())
                    output[work.matches] = {binding, cellSlot, cells[cellSlot].id};
                ++work.matches;
            }
        }

        /** @brief Traverses intersecting nodes with fixed stack storage and no allocation. */
        [[nodiscard]] Result<QueryWork> TraverseIndex(const SpatialIndex &index, const std::span<const WorldPartitionCellDescriptor> cells,
                                                      const WorldPartitionSpatialQuery &query,
                                                      const std::span<WorldPartitionCellHandle> output,
                                                      const WorldPartitionRegistryBinding &binding) noexcept {
            QueryWork work{};
            if (index.nodes.empty())
                return Result<QueryWork>::Success(work);
            std::array<std::uint32_t, SpatialIndexTraversalCapacity> stack{};
            std::size_t pending = 1;
            stack[0] = 0;
            while (pending != 0) {
                const auto &node = index.nodes[stack[--pending]];
                ++work.nodes;
                if (!Intersects(node.bounds, query.bounds))
                    continue;
                if (node.IsLeaf()) {
                    VisitLeaf(node, index, cells, query, output, binding, work);
                    continue;
                }
                if (pending > stack.size() - 2)
                    return Failure<QueryWork>(WorldStreamingErrors::PartitionRegistryUnsupported);
                stack[pending++] = node.right;
                stack[pending++] = node.left;
            }
            return Result<QueryWork>::Success(work);
        }
    }  // namespace

    /** @copydoc WorldPartitionRegistryBinding::IsValid */
    bool WorldPartitionRegistryBinding::IsValid() const noexcept {
        return registry.IsValid() && revision.IsValid() && owner.IsValid();
    }

    /** @copydoc WorldPartitionCellHandle::IsValid */
    bool WorldPartitionCellHandle::IsValid() const noexcept {
        return binding.IsValid() && cell.IsValid();
    }

    /** @copydoc WorldPartitionRegistryLimits::IsValid */
    bool WorldPartitionRegistryLimits::IsValid() const noexcept {
        return cells > 0 && cells <= MaximumCells && queryResults > 0 && queryResults <= MaximumQueryResults && queryResults <= cells;
    }

    /** @copydoc WorldPartitionRegistrySnapshot::IsValid */
    bool WorldPartitionRegistrySnapshot::IsValid() const noexcept {
        return state_ != nullptr && state_->binding.IsValid();
    }

    /** @copydoc WorldPartitionRegistrySnapshot::Binding */
    const WorldPartitionRegistryBinding &WorldPartitionRegistrySnapshot::Binding() const noexcept {
        static const WorldPartitionRegistryBinding InvalidBinding{};
        return state_ == nullptr ? InvalidBinding : state_->binding;
    }

    /** @copydoc WorldPartitionRegistrySnapshot::Cells */
    std::span<const WorldPartitionCellDescriptor> WorldPartitionRegistrySnapshot::Cells() const noexcept {
        return state_ == nullptr ? std::span<const WorldPartitionCellDescriptor>{} : state_->descriptor.Cells();
    }

    /** @copydoc WorldPartitionRegistrySnapshot::Find */
    Result<WorldPartitionCellHandle> WorldPartitionRegistrySnapshot::Find(const StreamingCellId cell) const {
        if (!IsValid() || !cell.IsValid())
            return Failure<WorldPartitionCellHandle>(WorldStreamingErrors::PartitionRegistryInvalid);
        const auto cells = Cells();
        const auto found = FindCell(cells, cell);
        if (found == cells.end() || found->id != cell)
            return Failure<WorldPartitionCellHandle>(WorldStreamingErrors::PartitionRegistryUnavailable);
        return Result<WorldPartitionCellHandle>::Success({state_->binding, static_cast<std::uint32_t>(found - cells.begin()), cell});
    }

    /** @copydoc WorldPartitionRegistrySnapshot::Resolve */
    Result<const WorldPartitionCellDescriptor *> WorldPartitionRegistrySnapshot::Resolve(const WorldPartitionCellHandle &handle) const {
        if (!IsValid() || !handle.IsValid() || handle.slot >= Cells().size())
            return Failure<const WorldPartitionCellDescriptor *>(WorldStreamingErrors::PartitionRegistryInvalid);
        if (handle.binding != state_->binding)
            return Failure<const WorldPartitionCellDescriptor *>(WorldStreamingErrors::PartitionRegistryStale);
        const auto &record = Cells()[handle.slot];
        if (record.id != handle.cell)
            return Failure<const WorldPartitionCellDescriptor *>(WorldStreamingErrors::PartitionRegistryStale);
        return Result<const WorldPartitionCellDescriptor *>::Success(&record);
    }

    /** @copydoc WorldPartitionRegistrySnapshot::Query */
    Result<WorldPartitionSpatialQueryResult> WorldPartitionRegistrySnapshot::Query(const WorldPartitionSpatialQuery &query,
                                                                                   const std::span<WorldPartitionCellHandle> output) const {
        if (!IsValid() || !Ordered(query.bounds) || output.empty())
            return Failure<WorldPartitionSpatialQueryResult>(WorldStreamingErrors::PartitionRegistryInvalid);
        if (query.layer.has_value() && !query.layer->IsValid())
            return Failure<WorldPartitionSpatialQueryResult>(WorldStreamingErrors::PartitionRegistryInvalid);
        if (query.layer.has_value() && !IsDeclaredLayer(state_->descriptor, *query.layer))
            return Failure<WorldPartitionSpatialQueryResult>(WorldStreamingErrors::PartitionRegistryUnsupported);
        if (query.lod.has_value() && *query.lod >= state_->descriptor.Grid().LodLevels())
            return Failure<WorldPartitionSpatialQueryResult>(WorldStreamingErrors::PartitionRegistryUnsupported);
        const auto cells = Cells();
        const auto countedResult = TraverseIndex(state_->spatialIndex, cells, query, {}, state_->binding);
        if (countedResult.HasError())
            return Result<WorldPartitionSpatialQueryResult>::Failure(countedResult.ErrorValue());
        const auto &counted = countedResult.Value();
        if (counted.matches > state_->limits.queryResults || counted.matches > output.size())
            return Failure<WorldPartitionSpatialQueryResult>(WorldStreamingErrors::PartitionRegistryCapacityExceeded);

        const auto writtenResult = TraverseIndex(state_->spatialIndex, cells, query, output, state_->binding);
        if (writtenResult.HasError())
            return Result<WorldPartitionSpatialQueryResult>::Failure(writtenResult.ErrorValue());
        const auto &written = writtenResult.Value();
        std::sort(output.begin(), output.begin() + static_cast<std::ptrdiff_t>(written.matches), [](const auto &left, const auto &right) {
            return left.slot < right.slot;
        });
        return Result<WorldPartitionSpatialQueryResult>::Success({state_->binding, written.matches, counted.candidates, counted.nodes});
    }

    WorldPartitionRegistry::WorldPartitionRegistry(ConstructionKey, const WorldPartitionRegistryId registry,
                                                   const StreamingRuntimeOwnerToken &owner,
                                                   const WorldPartitionRegistryLimits limits) noexcept
        : registry_(registry), owner_(owner), limits_(limits) {}

    /** @copydoc WorldPartitionRegistry::Create */
    Result<std::unique_ptr<WorldPartitionRegistry>> WorldPartitionRegistry::Create(const WorldPartitionRegistryId registry,
                                                                                   const StreamingRuntimeOwnerToken &owner,
                                                                                   const WorldPartitionRegistryLimits limits) {
        if (!registry.IsValid() || !owner.IsValid() || !limits.IsValid())
            return Failure<std::unique_ptr<WorldPartitionRegistry>>(WorldStreamingErrors::PartitionRegistryInvalid);
        try {
            return Result<std::unique_ptr<WorldPartitionRegistry>>::Success(
                std::make_unique<WorldPartitionRegistry>(ConstructionKey{}, registry, owner, limits));
        } catch (const std::bad_alloc &) {
            return Failure<std::unique_ptr<WorldPartitionRegistry>>(WorldStreamingErrors::PartitionRegistryStorageUnavailable);
        }
    }

    /** @copydoc WorldPartitionRegistrySnapshot::Grid */
    const WorldCellQuantizationPolicy *WorldPartitionRegistrySnapshot::Grid() const noexcept {
        return state_ ? &state_->descriptor.Grid() : nullptr;
    }

    /** @copydoc WorldPartitionRegistrySnapshot::Layers */
    std::span<const WorldLayerDescriptor> WorldPartitionRegistrySnapshot::Layers() const noexcept {
        return state_ ? state_->descriptor.Layers() : std::span<const WorldLayerDescriptor>{};
    }

    /** @copydoc WorldPartitionRegistrySnapshot::BeginQuery */
    Result<WorldPartitionSpatialCursor> WorldPartitionRegistrySnapshot::BeginQuery(const WorldPartitionSpatialQuery &query) const {
        if (!IsValid() || !Ordered(query.bounds) || (query.layer && !query.layer->IsValid()))
            return Failure<WorldPartitionSpatialCursor>(WorldStreamingErrors::PartitionRegistryInvalid);
        if ((query.layer && !IsDeclaredLayer(state_->descriptor, *query.layer)) ||
            (query.lod && *query.lod >= state_->descriptor.Grid().LodLevels()))
            return Failure<WorldPartitionSpatialCursor>(WorldStreamingErrors::PartitionRegistryUnsupported);
        WorldPartitionSpatialCursor cursor;
        cursor.binding_ = state_->binding;
        cursor.query_ = query;
        cursor.complete_ = state_->spatialIndex.nodes.empty();
        cursor.pendingCount_ = cursor.complete_ ? 0 : 1;
        return Result<WorldPartitionSpatialCursor>::Success(cursor);
    }

    /** @copydoc WorldPartitionRegistrySnapshot::QueryNext */
    Result<WorldPartitionSpatialBatch> WorldPartitionRegistrySnapshot::QueryNext(WorldPartitionSpatialCursor &cursor,
                                                                                 const WorldPartitionSpatialWorkBudget budget,
                                                                                 const std::span<WorldPartitionCellHandle> output) const {
        if (!IsValid() || !cursor.binding_.IsValid() || budget.nodes == 0 || budget.candidates == 0 || output.empty())
            return Failure<WorldPartitionSpatialBatch>(WorldStreamingErrors::PartitionRegistryInvalid);
        if (cursor.binding_ != state_->binding)
            return Failure<WorldPartitionSpatialBatch>(WorldStreamingErrors::PartitionRegistryStale);
        WorldPartitionSpatialBatch batch{};
        const auto &index = state_->spatialIndex;
        const auto cells = Cells();
        while (cursor.pendingCount_ != 0 && batch.matches < output.size()) {
            const auto &node = index.nodes[cursor.pending_[cursor.pendingCount_ - 1]];
            if (cursor.leafOffset_ == 0) {
                if (batch.nodesVisited == budget.nodes)
                    break;
                ++batch.nodesVisited;
                if (!Intersects(node.bounds, cursor.query_.bounds)) {
                    --cursor.pendingCount_;
                    continue;
                }
                if (!node.IsLeaf()) {
                    --cursor.pendingCount_;
                    cursor.pending_[cursor.pendingCount_++] = node.right;
                    cursor.pending_[cursor.pendingCount_++] = node.left;
                    continue;
                }
            }
            while (cursor.leafOffset_ < node.count && batch.candidatesExamined < budget.candidates && batch.matches < output.size()) {
                const auto slot = index.cellSlots[node.first + cursor.leafOffset_++];
                ++batch.candidatesExamined;
                if (Matches(cells[slot], index.cellBounds[slot], cursor.query_))
                    output[batch.matches++] = {state_->binding, slot, cells[slot].id};
            }
            if (cursor.leafOffset_ != node.count)
                break;
            cursor.leafOffset_ = 0;
            --cursor.pendingCount_;
            if (batch.candidatesExamined == budget.candidates)
                break;
        }
        cursor.complete_ = cursor.pendingCount_ == 0;
        batch.complete = cursor.complete_;
        return Result<WorldPartitionSpatialBatch>::Success(batch);
    }

    /** @copydoc WorldPartitionRegistry::Publish */
    Result<void> WorldPartitionRegistry::Publish(WorldPartitionDescriptor &&descriptor, const WorldPartitionRegistryRevision revision) {
        if (Lifecycle() != WorldPartitionRegistryState::Active)
            return Failure<void>(WorldStreamingErrors::PartitionRegistryLifecycleUnavailable);
        if (!revision.IsValid() || descriptor.Partition() != owner_.partition)
            return Failure<void>(WorldStreamingErrors::PartitionRegistryInvalid);
        auto spatialIndex = BuildSpatialIndex(descriptor, limits_);
        if (spatialIndex.HasError())
            return Result<void>::Failure(spatialIndex.ErrorValue());

        const auto current = std::atomic_load(&state_);
        if (current != nullptr && current->binding.revision.Value() == std::numeric_limits<std::uint64_t>::max())
            return Failure<void>(WorldStreamingErrors::GenerationExhausted);
        if ((current == nullptr && revision.Value() != 1) ||
            (current != nullptr && (current->binding.owner != owner_ || revision.Value() != current->binding.revision.Value() + 1)))
            return Failure<void>(WorldStreamingErrors::PartitionRegistryStale);

        try {
            auto next =
                std::make_shared<WorldPartitionRegistrySnapshot::State>(WorldPartitionRegistryBinding{registry_, revision, owner_}, limits_,
                                                                        std::move(descriptor), std::move(spatialIndex).Value());
            std::atomic_store(&state_, std::shared_ptr<const WorldPartitionRegistrySnapshot::State>{std::move(next)});
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Failure<void>(WorldStreamingErrors::PartitionRegistryStorageUnavailable);
        }
    }

    /** @copydoc WorldPartitionRegistry::Snapshot */
    Result<WorldPartitionRegistrySnapshot> WorldPartitionRegistry::Snapshot() const {
        if (Lifecycle() != WorldPartitionRegistryState::Active)
            return Failure<WorldPartitionRegistrySnapshot>(WorldStreamingErrors::PartitionRegistryLifecycleUnavailable);
        auto state = std::atomic_load(&state_);
        if (state == nullptr)
            return Failure<WorldPartitionRegistrySnapshot>(WorldStreamingErrors::PartitionRegistryUnavailable);
        return Result<WorldPartitionRegistrySnapshot>::Success(WorldPartitionRegistrySnapshot{std::move(state)});
    }

    /** @copydoc WorldPartitionRegistry::BeginCancellation */
    void WorldPartitionRegistry::BeginCancellation() noexcept {
        auto expected = WorldPartitionRegistryState::Active;
        static_cast<void>(lifecycle_.compare_exchange_strong(expected, WorldPartitionRegistryState::Cancelling));
    }

    /** @copydoc WorldPartitionRegistry::Shutdown */
    void WorldPartitionRegistry::Shutdown() noexcept {
        lifecycle_.store(WorldPartitionRegistryState::Closed);
        std::atomic_store(&state_, std::shared_ptr<const WorldPartitionRegistrySnapshot::State>{});
    }

    /** @copydoc WorldPartitionRegistry::~WorldPartitionRegistry */
    WorldPartitionRegistry::~WorldPartitionRegistry() noexcept {
        Shutdown();
    }

    /** @copydoc WorldPartitionRegistry::Lifecycle */
    WorldPartitionRegistryState WorldPartitionRegistry::Lifecycle() const noexcept {
        return lifecycle_.load();
    }

    /** @copydoc NextWorldPartitionRegistryRevision */
    Result<WorldPartitionRegistryRevision> NextWorldPartitionRegistryRevision(const WorldPartitionRegistryRevision current) {
        if (!current.IsValid())
            return Failure<WorldPartitionRegistryRevision>(WorldStreamingErrors::IdentityInvalid);
        if (current.Value() == std::numeric_limits<std::uint64_t>::max())
            return Failure<WorldPartitionRegistryRevision>(WorldStreamingErrors::GenerationExhausted);
        return WorldPartitionRegistryRevision::Create(current.Value() + 1);
    }
}  // namespace Horo::WorldStreaming
