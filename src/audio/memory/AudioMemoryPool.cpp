#include "AlignedAudioStorage.h"
#include "Horo/Audio/AudioMemory.h"
#include "Horo/Audio/Internal/AudioCallbackSafetyHooks.h"

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <ranges>
#include <utility>
#include <vector>

namespace Horo::Audio {
    namespace {
        enum class SlotState : std::uint8_t {
            Free,
            Live,
            Retired
        };

        /** @brief Preallocated freelist and lifetime facts; only the processing owner mutates these fields. */
        struct Slot {
            std::uint64_t generation{1};
            std::uint64_t lastUse{};
            std::uint32_t next{};
            SlotState state{SlotState::Free};
        };

        /** @brief Validate dimensions before rounded-size arithmetic or allocations. */
        bool ValidDescriptor(const AudioMemoryPoolDescriptor &descriptor) noexcept {
            const std::array requirements{descriptor.owner.IsValid(),
                                          descriptor.identity.IsValid(),
                                          descriptor.purpose <= AudioMemoryPurpose::EventStorage,
                                          descriptor.slots > 0,
                                          descriptor.slots <= MaximumAudioHandleSlots,
                                          descriptor.blockBytes > 0,
                                          descriptor.blockBytes <= MaximumAudioMemoryBytes,
                                          descriptor.budgetBytes > 0,
                                          descriptor.budgetBytes <= MaximumAudioMemoryBytes};
            return std::ranges::all_of(requirements, std::identity{});
        }
    }  // namespace

    /** @brief Own all callback storage; free-list updates and bounded scans never allocate or free heap memory. */
    struct AudioMemoryPool::State {
        AudioMemoryPoolDescriptor descriptor;
        std::size_t stride;
        std::vector<Slot> slots;
        Detail::AlignedAudioStorage storage;
        AudioMemoryStats stats;
        std::uint64_t completedEpoch{};
        std::uint32_t freeHead{1};
        std::uint32_t scan{};

        /** @brief Allocate only after total reservation admission; RAII rolls back either failed allocation. */
        State(const AudioMemoryPoolDescriptor &description, const std::size_t blockStride)
            : descriptor(description), stride(blockStride), slots(description.slots),
              storage(Detail::AllocateAudioStorage(blockStride * description.slots)),
              stats{.reservedBytes = (blockStride + sizeof(Slot)) * description.slots} {
            std::ranges::for_each(std::views::iota(std::uint32_t{0}, descriptor.slots - 1), [&](const std::uint32_t index) {
                slots[index].next = index + 2;
            });
        }

        /** @brief Check every identity field before indexing or exposing a live borrow. */
        Slot *Find(const AudioMemoryHandle &handle) noexcept {
            if (const std::array invalidIdentity{handle.owner != descriptor.owner, handle.pool != descriptor.identity, handle.slot == 0,
                                                 handle.slot > descriptor.slots};
                std::ranges::any_of(invalidIdentity, std::identity{})) {
                return nullptr;
            }
            auto &slot = slots[handle.slot - 1];
            if (const std::array matches{slot.state == SlotState::Live, slot.generation == handle.generation};
                !std::ranges::all_of(matches, std::identity{})) {
                return nullptr;
            }
            return &slot;
        }

        /** @brief Return an admitted slot's exact payload, never its private alignment padding. */
        std::span<std::byte> Bytes(const std::uint32_t slot) const noexcept {
            return {storage.get() + (slot - 1) * stride, descriptor.blockBytes};
        }

        /** @brief Reuse only completed retired slots whose identity can advance without wrapping. */
        bool Recycle(const std::uint32_t index) noexcept {
            auto &slot = slots[index];
            if (const std::array recyclable{slot.state == SlotState::Retired, slot.lastUse <= completedEpoch,
                                            slot.generation != std::numeric_limits<std::uint64_t>::max()};
                !std::ranges::all_of(recyclable, std::identity{})) {
                return false;
            }
            ++slot.generation;
            slot.state = SlotState::Free;
            slot.next = freeHead;
            freeHead = index + 1;
            stats.usedBytes -= stride;
            return true;
        }

        /** @brief Advance the bounded round-robin cursor without division on the callback-adjacent path. */
        void AdvanceScan() noexcept {
            if (++scan == descriptor.slots) {
                scan = 0;
            }
        }
    };

    /** @copydoc AudioMemoryPool::Create */
    Result<AudioMemoryPool> AudioMemoryPool::Create(const AudioMemoryPoolDescriptor &descriptor) {
        if (!ValidDescriptor(descriptor)) {
            return Result<AudioMemoryPool>::Failure(MakeError(AudioErrors::MemoryInvalid));
        }
        const auto stride = Detail::AudioMemoryStride(descriptor.blockBytes);
        if (descriptor.slots > descriptor.budgetBytes / (stride + sizeof(Slot))) {
            return Result<AudioMemoryPool>::Failure(MakeError(AudioErrors::MemoryBudgetExceeded));
        }
        Safety::OnAudioAllocationAttempt();
        try {
            return Result<AudioMemoryPool>::Success(AudioMemoryPool{std::make_unique<State>(descriptor, stride)});
        } catch (const std::bad_alloc &) {
            return Result<AudioMemoryPool>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
    }

    /** @copydoc AudioMemoryPool::AudioMemoryPool */
    AudioMemoryPool::AudioMemoryPool(std::unique_ptr<State> state) : state_(std::move(state)) {}

    /** @copydoc AudioMemoryPool::~AudioMemoryPool */
    AudioMemoryPool::~AudioMemoryPool() = default;
    /** @copydoc AudioMemoryPool::AudioMemoryPool */
    AudioMemoryPool::AudioMemoryPool(AudioMemoryPool &&) noexcept = default;
    /** @copydoc AudioMemoryPool::operator= */
    AudioMemoryPool &AudioMemoryPool::operator=(AudioMemoryPool &&) noexcept = default;

    /** @copydoc AudioMemoryPool::Acquire */
    AudioMemoryAllocation AudioMemoryPool::Acquire() noexcept {
        if (!state_) {
            return {.status = AudioMemoryStatus::Inactive, .handle = {}, .bytes = {}};
        }
        auto &state = *state_;
        if (state.freeHead == 0) {
            state.stats.failedAllocations +=
                static_cast<std::uint64_t>(state.stats.failedAllocations != std::numeric_limits<std::uint64_t>::max());
            return {.status = AudioMemoryStatus::Exhausted, .handle = {}, .bytes = {}};
        }
        const auto index = state.freeHead;
        auto &slot = state.slots[index - 1];
        state.freeHead = slot.next;
        slot.state = SlotState::Live;
        auto *data = state.storage.get() + (index - 1) * state.stride;
        std::fill_n(data, state.stride, std::byte{0});
        const auto bytes = std::span<std::byte>{data, state.descriptor.blockBytes};
        state.stats.usedBytes += state.stride;
        state.stats.peakBytes = std::max(state.stats.peakBytes, state.stats.usedBytes);
        return {.status = AudioMemoryStatus::Ok,
                .handle = {.owner = state.descriptor.owner,
                           .pool = state.descriptor.identity,
                           .slot = index,
                           .generation = slot.generation},
                .bytes = bytes};
    }

    /** @copydoc AudioMemoryPool::Resolve */
    std::span<std::byte> AudioMemoryPool::Resolve(const AudioMemoryHandle &handle) noexcept {
        return state_ && state_->Find(handle) ? state_->Bytes(handle.slot) : std::span<std::byte>{};
    }

    /** @copydoc AudioMemoryPool::Retire */
    AudioMemoryStatus AudioMemoryPool::Retire(const AudioMemoryHandle &handle, const std::uint64_t lastUseEpoch) noexcept {
        using enum AudioMemoryStatus;
        if (!state_) {
            return Inactive;
        }
        auto *slot = state_->Find(handle);
        if (!slot) {
            return InvalidHandle;
        }
        if (lastUseEpoch <= state_->completedEpoch) {
            return InvalidEpoch;
        }
        slot->lastUse = lastUseEpoch;
        slot->state = SlotState::Retired;
        return Ok;
    }

    /** @copydoc AudioMemoryPool::Reclaim */
    AudioMemoryReclamation AudioMemoryPool::Reclaim(const AudioRuntimeId owner, const std::uint64_t completedEpoch,
                                                    const std::uint32_t visitBudget) noexcept {
        if (!state_) {
            return {};
        }
        if (const std::array invalidEpoch{owner != state_->descriptor.owner, completedEpoch == 0, completedEpoch < state_->completedEpoch};
            std::ranges::any_of(invalidEpoch, std::identity{})) {
            return {.status = AudioMemoryStatus::InvalidEpoch};
        }
        if (visitBudget == 0) {
            return {.status = AudioMemoryStatus::InvalidRequest};
        }
        auto &state = *state_;
        state.completedEpoch = completedEpoch;
        AudioMemoryReclamation result{.status = AudioMemoryStatus::Ok, .visited = std::min(visitBudget, state.descriptor.slots)};
        std::ranges::for_each(std::views::iota(std::uint32_t{0}, result.visited), [&](const std::uint32_t) {
            result.reclaimed += static_cast<std::uint32_t>(state.Recycle(state.scan));
            state.AdvanceScan();
        });
        return result;
    }

    /** @copydoc AudioMemoryPool::Stats */
    AudioMemoryStats AudioMemoryPool::Stats() const noexcept {
        return state_ ? state_->stats : AudioMemoryStats{};
    }
}  // namespace Horo::Audio
