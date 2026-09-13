#pragma once

#include "Horo/Runtime/Render/RenderMemoryBudget.h"

#include <limits>
#include <optional>
#include <vector>

namespace Horo::Render::Detail {
    enum class MemoryRegionState : std::uint8_t {
        Reserved,
        Live,
        Retiring,
    };

    struct MemoryRegion {
        bool active{false};
        std::uint64_t block{0};
        RenderMemoryPoolId pool;
        std::uint64_t reservation{0};
        std::uint64_t allocation{0};
        ResourceOperationId attempt;
        std::uint64_t budgetRevision{0};
        std::size_t offset{0};
        std::size_t requiredBytes{0};
        std::size_t payloadBytes{0};
        RenderMemoryCostProvenance provenance{RenderMemoryCostProvenance::Exact};
        MemoryRegionState state{MemoryRegionState::Reserved};
    };

    struct MemoryPool {
        RenderMemoryPoolId id;
        RenderMemoryScopeId scope;
        RenderMemoryClass memoryClass{RenderMemoryClass::PersistentDevice};
        RenderMemoryCompatibilityId compatibility;
    };

    struct MemoryBlock {
        std::uint64_t id{0};
        RenderMemoryPoolId pool;
        std::size_t capacity{0};
        bool dedicated{false};
        bool committed{false};
    };

    struct MemoryLocation {
        std::size_t block{0};
        std::size_t region{0};
    };

    using SortedMemoryRegions = std::vector<const MemoryRegion *>;
    using Block = MemoryBlock;
    using Pool = MemoryPool;
    using Region = MemoryRegion;
    using RegionState = MemoryRegionState;

    [[nodiscard]] inline Error MemoryBudgetError(const ErrorCodeDescriptor &descriptor, const char *message) {
        return MakeError(descriptor, message);
    }

    [[nodiscard]] inline std::optional<std::size_t> CheckedMemoryAdd(const std::size_t left, const std::size_t right) noexcept {
        if (right > std::numeric_limits<std::size_t>::max() - left)
            return std::nullopt;
        return left + right;
    }

    [[nodiscard]] inline std::optional<std::size_t> AlignMemoryUp(const std::size_t value, const std::size_t alignment) noexcept {
        const std::size_t mask = alignment - 1U;
        const auto sum = CheckedMemoryAdd(value, mask);
        if (!sum.has_value())
            return std::nullopt;
        return *sum & ~mask;
    }

    [[nodiscard]] inline std::optional<std::size_t> FindFirstMemoryFit(const MemoryBlock &block, const std::vector<MemoryRegion> &regions,
                                                                       const std::size_t size, const std::size_t alignment) noexcept {
        std::size_t cursor = 0;
        while (cursor <= block.capacity) {
            const auto aligned = AlignMemoryUp(cursor, alignment);
            if (!aligned.has_value())
                return std::nullopt;
            const MemoryRegion *next = nullptr;
            for (const MemoryRegion &region : regions) {
                if (region.active && region.block == block.id && region.offset >= cursor &&
                    (next == nullptr || region.offset < next->offset))
                    next = &region;
            }
            const std::size_t gapEnd = next == nullptr ? block.capacity : next->offset;
            if (const auto end = CheckedMemoryAdd(*aligned, size); end.has_value() && *end <= gapEnd)
                return aligned;
            if (next == nullptr)
                return std::nullopt;
            const auto nextCursor = CheckedMemoryAdd(next->offset, next->requiredBytes);
            if (!nextCursor.has_value() || *nextCursor <= cursor)
                return std::nullopt;
            cursor = *nextCursor;
        }
        return std::nullopt;
    }
}  // namespace Horo::Render::Detail
