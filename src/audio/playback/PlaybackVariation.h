#pragma once

/** @file PlaybackVariation.h
 * @brief Target-private versioned candidate selection; committed only by the repeated-play owner.
 */
#include "Horo/Audio/AudioRepeatedPlayback.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace Horo::Audio {
    namespace Detail {
        static_assert(std::numeric_limits<float>::is_iec559 && std::numeric_limits<double>::is_iec559);

        /** @brief Compare clock targets without relying on padding or object representation. */
        inline bool SameTarget(const AudioCommandTarget &a, const AudioCommandTarget &b) noexcept {
            return a.kind == b.kind && a.sampleFrame == b.sampleFrame && a.clockGeneration == b.clockGeneration &&
                   a.discontinuityRevision == b.discontinuityRevision;
        }

        /** @brief Preserve exactly one authored rule for every shared group identity. */
        inline bool SameGroup(const AudioConcurrencyGroup &a, const AudioConcurrencyGroup &b) noexcept {
            return a.group == b.group && a.scope == b.scope && a.maximumInstances == b.maximumInstances &&
                   a.retriggerFrames == b.retriggerFrames && a.countPaused == b.countPaused && a.countVirtual == b.countVirtual;
        }

        /** @brief Version-1 unsigned SplitMix64 arithmetic is identical on every supported platform. */
        inline std::uint64_t Next(std::uint64_t &state) noexcept {
            state += UINT64_C(0x9e3779b97f4a7c15);
            auto word = state;
            word = (word ^ (word >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
            word = (word ^ (word >> 27)) * UINT64_C(0x94d049bb133111eb);
            return word ^ (word >> 31);
        }

        /** @brief Rejection sampling removes modulo bias; exhaustion rejects without committing candidate state. */
        inline bool Draw(std::uint64_t &state, const std::uint64_t bound, std::uint64_t &value) noexcept {
            if (bound == 1) {
                value = 0;
                return true;
            }
            const auto threshold = (std::uint64_t{0} - bound) % bound;
            for (std::uint32_t attempt = 0; attempt < 128; ++attempt) {
                const auto word = Next(state);
                if (word >= threshold) {
                    value = word % bound;
                    return true;
                }
            }
            return false;
        }

        /** @brief Fixed consumption, inclusive dyadic grid and double intermediate avoid overflow in range subtraction. */
        inline float Range(std::uint64_t &state, const AudioVariationRange range) noexcept {
            const auto numerator = Next(state) >> 40;
            const double fraction = static_cast<double>(numerator) / 16777215.0;
            return static_cast<float>(static_cast<double>(range.minimum) + (static_cast<double>(range.maximum) - range.minimum) * fraction);
        }

        /** @brief Candidate replay state is copied before any fallible selection/preparation. */
        struct VariationState final {
            std::uint64_t random{};
            std::array<std::uint16_t, MaximumAudioVariationEntries> bag{};
            std::uint32_t position{};
            std::optional<std::uint32_t> last;
        };

        /** @brief Build an unbiased Fisher-Yates bag and avoid repeating the preceding bag's last entry. */
        inline bool ShuffleBag(VariationState &state, const std::uint32_t count) noexcept {
            for (std::uint32_t index = 0; index < count; ++index)
                state.bag[index] = static_cast<std::uint16_t>(index);
            for (std::uint32_t remaining = count; remaining > 1; --remaining) {
                std::uint64_t index{};
                if (!Draw(state.random, remaining, index))
                    return false;
                std::swap(state.bag[remaining - 1], state.bag[index]);
            }
            if (count > 1 && state.last == state.bag[0]) {
                std::uint64_t index{};
                if (!Draw(state.random, count - 1, index))
                    return false;
                std::swap(state.bag[0], state.bag[index + 1]);
            }
            state.position = 0;
            return true;
        }

        /** @brief Select canonical entries using bounded integer tickets and conditional no-repeat weighting. */
        inline bool Weighted(VariationState &state, const std::vector<AudioVariationEntry> &entries, std::uint32_t &selected) noexcept {
            float maximum{};
            for (const auto &entry : entries)
                maximum = std::max(maximum, entry.weight);
            std::array<std::uint64_t, MaximumAudioVariationEntries> tickets{};
            std::uint64_t total{};
            for (std::size_t index = 0; index < entries.size(); ++index) {
                if (entries.size() > 1 && state.last == index)
                    continue;
                tickets[index] = static_cast<std::uint64_t>(std::ceil(static_cast<double>(entries[index].weight) / maximum * 4294967296.0));
                total += tickets[index];
            }
            std::uint64_t draw{};
            if (!Draw(state.random, total, draw))
                return false;
            for (std::uint32_t index = 0; index < entries.size(); ++index) {
                if (draw < tickets[index]) {
                    selected = index;
                    return true;
                }
                draw -= tickets[index];
            }
            return false;
        }

        /** @brief Map a uniform draw around the preceding canonical entry without consuming retry draws. */
        inline bool RandomEntry(VariationState &state, const std::uint32_t count, std::uint32_t &selected) noexcept {
            const bool excludeLast = count > 1 && state.last.has_value();
            std::uint64_t index{};
            if (!Draw(state.random, count - (excludeLast ? 1U : 0U), index))
                return false;
            selected = static_cast<std::uint32_t>(index);
            if (excludeLast && selected >= *state.last)
                ++selected;
            return true;
        }

        /** @brief Execute selection on candidate state; only a successful voice admission publishes it. */
        inline bool Select(VariationState &state, const AudioVariationAssetSchema &variation, std::uint32_t &selected) noexcept {
            const auto count = static_cast<std::uint32_t>(variation.entries.size());
            using enum AudioVariationSelection;
            switch (variation.selection) {
                case Shuffle:
                    if (state.position == count && !ShuffleBag(state, count))
                        return false;
                    selected = state.bag[state.position++];
                    break;
                case RoundRobin:
                    selected = state.position;
                    state.position = (state.position + 1) % count;
                    break;
                case WeightedRandom:
                    if (!Weighted(state, variation.entries, selected))
                        return false;
                    break;
                case Random:
                    if (!RandomEntry(state, count, selected))
                        return false;
                    break;
            }
            state.last = selected;
            return true;
        }
    }  // namespace Detail

}  // namespace Horo::Audio
