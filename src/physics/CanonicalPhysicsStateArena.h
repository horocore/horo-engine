#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <span>

namespace Horo::Physics::Detail {
    /** @brief Owner-thread scratch with actual allocation provenance and no heap
     * fallback.
     * @details Only the joined, nonreentrant pinned SaveState operation may run
     * while this arena is installed. Its complete temporary containers are
     * destroyed before scope exit. Free/reallocate on foreign pointers must
     * delegate unchanged to the existing installed allocator. Ordinary budget
     * rejection occurs before entering native code through
     * PlanCanonicalPhysicsStateBounds, never through allocation failure here.
     */
    class CanonicalPhysicsStateArena final {
        struct alignas(16) Header final {
            std::uintptr_t owner{};
            std::size_t size{};
            std::uint64_t tag{};
            bool live{};
        };

        static_assert(sizeof(Header) == 32);
        static constexpr std::uint64_t Tag = 0x485053414c4c4f43ULL;

    public:
        explicit CanonicalPhysicsStateArena(std::span<std::byte> storage) noexcept : storage_(storage) {}

        CanonicalPhysicsStateArena(const CanonicalPhysicsStateArena &) = delete;
        CanonicalPhysicsStateArena &operator=(const CanonicalPhysicsStateArena &) = delete;

        /** @brief Actual range membership, independent of allocator calling phase. */
        bool Contains(const void *memory) const noexcept {
            const auto pointer = reinterpret_cast<std::uintptr_t>(memory);
            const auto begin = reinterpret_cast<std::uintptr_t>(storage_.data());
            return pointer >= begin && pointer - begin < storage_.size();
        }

        /** @brief Allocates with exact private header provenance; impossible
         * exhaustion is an adapter invariant failure. */
        void *Allocate(std::size_t size, std::size_t alignment) noexcept {
            if (alignment == 0 || alignment > 16 || (alignment & (alignment - 1)) != 0)
                std::abort();
            const auto base = reinterpret_cast<std::uintptr_t>(storage_.data());
            const auto padding = (16 - ((base + used_) & 15U)) & 15U;
            if (padding > storage_.size() - used_)
                std::abort();
            const auto begin = used_ + padding;
            if (sizeof(Header) > storage_.size() - begin || size > storage_.size() - begin - sizeof(Header))
                std::abort();
            auto *header = ::new (storage_.data() + begin) Header{reinterpret_cast<std::uintptr_t>(this), size, Tag, true};
            used_ = begin + sizeof(Header) + size;
            ++live_;
            ++allocations_;
            return header + 1;
        }

        /** @brief Rejects stale/double/interior frees instead of passing arena memory
         * to the host allocator. */
        void Free(void *memory) noexcept {
            auto &header = Lookup(memory);
            header.live = false;
            --live_;
        }

        /** @brief Preserves all old bytes through geometric native vector growth,
         * using only the admitted arena. */
        void *Reallocate(void *memory, std::size_t oldSize, std::size_t newSize) noexcept {
            if (!memory)
                return Allocate(newSize, 16);
            if (const auto &header = Lookup(memory); header.size != oldSize)
                std::abort();
            void *next = Allocate(newSize, 16);
            std::memcpy(next, memory, std::min(oldSize, newSize));
            Free(memory);
            return next;
        }

        /** @brief All native local containers must be gone before removing the
         * allocator route. */
        bool Quiescent() const noexcept {
            return live_ == 0;
        }

        /** @brief Actual routed native temporary allocations, not a guessed no-heap
         * evidence counter. */
        std::size_t Allocations() const noexcept {
            return allocations_;
        }

        /** @brief Actual scratch high-water usage, including all alignment/provenance
         * bytes. */
        std::size_t UsedBytes() const noexcept {
            return used_;
        }

        /** @brief Records an actual foreign reallocation forwarded unchanged by the
         * existing allocator hook. */
        void RecordForeignReallocation() noexcept {
            ++foreignReallocations_;
        }

        /** @brief Zero is required for the pinned synchronous SaveState path's native
         * no-heap claim. */
        std::size_t ForeignReallocations() const noexcept {
            return foreignReallocations_;
        }

        /** @brief Resets only after quiescence; an escaped pointer is a lifetime
         * invariant failure. */
        void Reset() noexcept {
            if (!Quiescent())
                std::abort();
            used_ = 0;
            allocations_ = foreignReallocations_ = 0;
        }

    private:
        /** @brief Requires an exact live allocation from this actual scratch owner.
         */
        Header &Lookup(void *memory) noexcept {
            if (!Contains(memory))
                std::abort();
            const auto begin = reinterpret_cast<std::uintptr_t>(storage_.data());
            if (const auto address = reinterpret_cast<std::uintptr_t>(memory); address - begin < sizeof(Header) || (address & 15U) != 0)
                std::abort();
            auto &header = *(static_cast<Header *>(memory) - 1);
            if (header.owner != reinterpret_cast<std::uintptr_t>(this) || header.tag != Tag || !header.live)
                std::abort();
            return header;
        }

        std::span<std::byte> storage_;
        std::size_t used_{};
        std::size_t live_{};
        std::size_t allocations_{};
        std::size_t foreignReallocations_{};
    };
}  // namespace Horo::Physics::Detail
