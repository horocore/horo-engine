#pragma once

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>

namespace Horo::Character::PropertyTest {
    /** @brief Portable SplitMix64 corpus; each recipe is a complete independently replayable input. */
    struct Recipe final {
        std::array<std::uint32_t, 8> words{};
    };

    class Generator final {
    public:
        explicit Generator(const std::uint64_t seed) : state_(seed) {}

        [[nodiscard]] std::uint64_t Next() noexcept {
            state_ += 0x9e3779b97f4a7c15ULL;
            auto value = state_;
            value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
            value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
            return value ^ (value >> 31U);
        }

        [[nodiscard]] Recipe Input(const std::uint32_t index) noexcept {
            Recipe result;
            for (auto &word : result.words)
                word = static_cast<std::uint32_t>(Next());
            result.words[0] = index;
            return result;
        }

    private:
        std::uint64_t state_;
    };

    /** @brief Repeatedly reduces individual words, preserving the original failure class within 128 evaluations. */
    template <typename Check> Recipe Minimize(Recipe input, const std::string &failure, Check check) {
        unsigned attempts{};
        for (std::size_t field = 0; field < input.words.size() && attempts < 128; ++field) {
            auto candidate = input;
            candidate.words[field] = 0;
            ++attempts;
            if (check(candidate) == failure) {
                input = candidate;
                continue;
            }
            for (auto reduced = input.words[field] / 2; reduced != 0 && attempts < 128; reduced /= 2) {
                candidate = input;
                candidate.words[field] = reduced;
                ++attempts;
                if (check(candidate) == failure)
                    input = candidate;
            }
        }
        return input;
    }

    /** @brief Runs bounded recipes or one eight-word decimal replay supplied through HORO_CHARACTER_REPLAY. */
    template <typename Check> void Corpus(const char *stage, Check check) {
        const auto evaluate = [&](const Recipe &input, const std::uint64_t seed, const unsigned index) {
            const auto failure = check(input);
            if (failure.empty())
                return;
            const auto minimized = Minimize(input, failure, check);
            std::ostringstream replay;
            for (const auto word : minimized.words)
                replay << word << ' ';
            INFO("stage=" << stage << " seed=" << seed << " case=" << index);
            INFO("HORO_CHARACTER_REPLAY='" << replay.str() << "' (select this exact test name)");
            FAIL(failure);
        };
        if (const auto *replay = std::getenv("HORO_CHARACTER_REPLAY")) {
            Recipe input;
            std::istringstream stream(replay);
            for (auto &word : input.words)
                REQUIRE(static_cast<bool>(stream >> word));
            std::string extra;
            REQUIRE_FALSE(static_cast<bool>(stream >> extra));
            evaluate(input, 0, 0);
            return;
        }
        for (const auto seed : {0x994ULL, 0x434852ULL, 0xdeadbeefULL}) {
            Generator generator(seed);
            for (unsigned index = 0; index < 48; ++index)
                evaluate(generator.Input(index), seed, index);
        }
    }
}  // namespace Horo::Character::PropertyTest
