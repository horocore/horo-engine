#pragma once

#include "Horo/Audio/AudioIdentity.h"

#include <cstddef>
#include <format>
#include <limits>
#include <new>
#include <vector>

namespace Horo::Audio::Detail {
    struct AudioHandleRegistryLimits {
        std::uint32_t maximumSlots{65'535};
        std::uint32_t maximumGeneration{std::numeric_limits<std::uint32_t>::max()};

        [[nodiscard]] bool IsValid() const noexcept {
            return maximumSlots > 0 && maximumSlots <= MaximumAudioHandleSlots && maximumGeneration > 0;
        }
    };

    template <typename Handle> class AudioHandleRegistry final {
    public:
        [[nodiscard]] static Result<AudioHandleRegistry> Create(const AudioRuntimeId owner, const AudioHandleRegistryLimits limits = {}) {
            if (!owner.IsValid() || !limits.IsValid())
                return Result<AudioHandleRegistry>::Failure(MakeError(AudioErrors::IdentityInvalid));
            try {
                return Result<AudioHandleRegistry>::Success(AudioHandleRegistry{owner, limits});
            } catch (const std::bad_alloc &) {
                return Result<AudioHandleRegistry>::Failure(MakeError(AudioErrors::HandleCapacityExhausted));
            }
        }

        AudioHandleRegistry(const AudioHandleRegistry &) = delete;
        AudioHandleRegistry &operator=(const AudioHandleRegistry &) = delete;
        AudioHandleRegistry(AudioHandleRegistry &&) noexcept = default;
        AudioHandleRegistry &operator=(AudioHandleRegistry &&) noexcept = default;

        [[nodiscard]] Result<Handle> Acquire() {
            if (!freeSlots_.empty()) {
                const std::uint32_t slot = freeSlots_.back();
                freeSlots_.pop_back();
                entries_[slot].active = true;
                ++activeCount_;
                return Result<Handle>::Success(Handle{owner_, slot, entries_[slot].generation});
            }
            if (entries_.size() - 1 >= limits_.maximumSlots) {
                const ErrorCodeDescriptor &error =
                    exhaustedSlots_ == limits_.maximumSlots ? AudioErrors::HandleGenerationExhausted : AudioErrors::HandleCapacityExhausted;
                return Result<Handle>::Failure(MakeError(error));
            }
            entries_.emplace_back();
            ++activeCount_;
            const auto slot = static_cast<std::uint32_t>(entries_.size() - 1);
            return Result<Handle>::Success(Handle{owner_, slot, entries_.back().generation});
        }

        [[nodiscard]] Result<std::uint32_t> Resolve(const Handle &handle) const {
            if (!handle.IsValid())
                return Result<std::uint32_t>::Failure(MakeError(AudioErrors::HandleMalformed));
            if (const auto *error = Check(handle))
                return Result<std::uint32_t>::Failure(HandleError(*error, handle));
            return Result<std::uint32_t>::Success(handle.slot);
        }

        /** @brief Validate identity without formatting or allocating on a processing owner. */
        [[nodiscard]] const ErrorCodeDescriptor *Check(const Handle &handle) const noexcept {
            if (!handle.IsValid())
                return &AudioErrors::HandleMalformed;
            if (handle.owner != owner_)
                return &AudioErrors::HandleOwnerMismatch;
            if (handle.slot >= entries_.size())
                return &AudioErrors::HandleStale;
            if (const Entry &entry = entries_[handle.slot]; !entry.active || entry.generation != handle.generation)
                return &AudioErrors::HandleStale;
            return nullptr;
        }

        [[nodiscard]] Result<void> Release(const Handle &handle) {
            Result<std::uint32_t> resolved = Resolve(handle);
            if (resolved.HasError())
                return Result<void>::Failure(resolved.ErrorValue());
            Entry &entry = entries_[resolved.Value()];
            entry.active = false;
            --activeCount_;
            if (entry.generation == limits_.maximumGeneration) {
                ++exhaustedSlots_;
                return Result<void>::Success();
            }
            ++entry.generation;
            freeSlots_.push_back(handle.slot);
            return Result<void>::Success();
        }

        [[nodiscard]] std::size_t ActiveCount() const noexcept {
            return activeCount_;
        }

    private:
        struct Entry {
            std::uint32_t generation{1};
            bool active{true};
        };

        AudioHandleRegistry(const AudioRuntimeId owner, const AudioHandleRegistryLimits limits) : owner_(owner), limits_(limits) {
            entries_.reserve(static_cast<std::size_t>(limits.maximumSlots) + 1);
            freeSlots_.reserve(limits.maximumSlots);
            entries_.push_back({.generation = 0, .active = false});
        }

        [[nodiscard]] static Error HandleError(const ErrorCodeDescriptor &descriptor, const Handle &handle) {
            return MakeError(descriptor, std::format("Audio handle owner {}, slot {}, generation {} is not current.", handle.owner.Value(),
                                                     handle.slot, handle.generation));
        }

        AudioRuntimeId owner_;
        AudioHandleRegistryLimits limits_;
        std::vector<Entry> entries_;
        std::vector<std::uint32_t> freeSlots_;
        std::size_t activeCount_{};
        std::uint32_t exhaustedSlots_{};
    };
}  // namespace Horo::Audio::Detail
