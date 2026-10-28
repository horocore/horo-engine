#pragma once
/** @file VulkanExternalRegistrationStorage.h
 * @brief Cold backing for the existing Vulkan registries, never a second live-resource authority.
 */
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <thread>
#include <unordered_map>
#include <utility>

namespace Horo::Render::Detail {
    /** @brief Owner-lane fixed shared-control-block cells, retained through the last native queued reference.
     * @details Construction is cold. Successful allocation/deallocation never allocates or locks; exhaustion
     * throws bad_alloc to the existing typed admission boundary with no upstream fallback. The allocator's
     * shared owner keeps these cells alive even after the runtime releases its own pool reference.
     * Every pooled object's FINAL shared release AND FINAL weak control-block release must occur on this
     * same render owner. No pooled shared/weak owner may escape to a foreign-thread retirement path.
     * Current private registries, material generations and command retention obey this existing owner-lane
     * contract; retaining the pool prevents dangling storage but does not grant cross-thread deallocation.
     */
    class VulkanExternalAllocationPool final {
    public:
        static constexpr std::size_t Capacity = 128;
        static constexpr std::size_t CellBytes = 1024;

        [[nodiscard]] std::byte *Allocate(const std::size_t bytes, const std::size_t alignment) {
            if (std::this_thread::get_id() != owner_ || bytes > CellBytes || alignment > alignof(std::max_align_t))
                throw std::bad_alloc{};
            for (auto &cell : cells_)
                if (!cell.used) {
                    cell.used = true;
                    return cell.bytes.data();
                }
            throw std::bad_alloc{};
        }

        void Deallocate(const std::byte *address) noexcept {
            // Destruction follows the same render-owner completion poll lane as native retirement.
            if (std::this_thread::get_id() != owner_)
                std::terminate();
            for (auto &cell : cells_)
                if (cell.bytes.data() == address && cell.used) {
                    cell.used = false;
                    return;
                }
            std::terminate();
        }

    private:
        struct alignas(std::max_align_t) Cell {
            std::array<std::byte, CellBytes> bytes;
            bool used{};
        };

        std::array<Cell, Capacity> cells_;
        std::thread::id owner_{std::this_thread::get_id()};
    };

    /** @brief Allocator retaining the cold backing through allocate_shared's final weak reference. */
    template <typename T> struct VulkanExternalAllocator {
        using value_type = T;
        std::shared_ptr<VulkanExternalAllocationPool> pool;

        explicit VulkanExternalAllocator(std::shared_ptr<VulkanExternalAllocationPool> owner) noexcept : pool(std::move(owner)) {}

        template <typename U> explicit VulkanExternalAllocator(const VulkanExternalAllocator<U> &other) noexcept : pool(other.pool) {}

        [[nodiscard]] T *allocate(const std::size_t count) {
            if (!pool || count > VulkanExternalAllocationPool::CellBytes / sizeof(T))
                throw std::bad_alloc{};
            return reinterpret_cast<T *>(pool->Allocate(count * sizeof(T), alignof(T)));
        }

        void deallocate(T *address, std::size_t) const noexcept {
            pool->Deallocate(reinterpret_cast<std::byte *>(address));
        }

        template <typename U> bool operator==(const VulkanExternalAllocator<U> &other) const noexcept {
            return pool == other.pool;
        }
    };

    /** @brief Reusable nodes belonging to the same canonical map, with no resident wrapper cache.
     * @details Prepare is cold. Registration refuses bucket growth; ordinary resources can consume spare
     * bucket capacity, in which case external admission fails rather than allocating in the frame.
     */
    template <typename Resource> class VulkanExternalRegistryNodes final {
    public:
        using Registry = std::unordered_map<std::uint64_t, std::shared_ptr<Resource>>;
        static constexpr std::size_t Capacity = 64;

        void Prepare(Registry &registry) {
            registry.reserve(registry.size() + Capacity);
            Registry scratch;
            scratch.reserve(Capacity);
            for (std::size_t index = 0; index < Capacity; ++index) {
                scratch.emplace(index + 1, nullptr);
                nodes_[index] = scratch.extract(index + 1);
            }
        }

        [[nodiscard]] bool Insert(Registry &registry, const std::uint64_t identity, std::shared_ptr<Resource> resource) {
            if (static_cast<double>(registry.size() + 1) > static_cast<double>(registry.bucket_count()) * registry.max_load_factor())
                return false;
            for (auto &node : nodes_)
                if (!node.empty()) {
                    node.key() = identity;
                    node.mapped() = std::move(resource);
                    if (const auto inserted = registry.insert(std::move(node)); !inserted.inserted)
                        std::terminate();  // Fresh backend identities cannot collide.
                    return true;
                }
            return false;
        }

        void Erase(Registry &registry, const std::uint64_t identity) noexcept {
            auto node = registry.extract(identity);
            if (node.empty())
                return;
            node.mapped().reset();  // Queued shared owners, not the node cache, retain native backing.
            for (auto &slot : nodes_)
                if (slot.empty()) {
                    slot = std::move(node);
                    return;
                }
            std::terminate();
        }

    private:
        std::array<typename Registry::node_type, Capacity> nodes_;
    };
}  // namespace Horo::Render::Detail
