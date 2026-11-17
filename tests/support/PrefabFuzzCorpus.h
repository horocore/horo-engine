#pragma once
/** @file PrefabFuzzCorpus.h @brief Bounded deterministic prefab mutation recipes and exact failure replay. */

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <sstream>
#include <string>
#include <typeinfo>
#include <utility>

namespace Horo::Prefab::Fuzz {
    inline constexpr std::size_t MaximumShrinkAttempts = 96;
    inline constexpr std::size_t MaximumInputBytes = 1024U * 1024U + 1;

    /** @brief Word zero selects a mutation family; remaining words parameterize the exact portable input. */
    struct Recipe final {
        std::array<std::uint32_t, 8> words{};
        [[nodiscard]] bool operator==(const Recipe &) const noexcept = default;
    };

    /** @brief Portable fixed-algorithm PRNG independent of standard-library distributions. */
    class Generator final {
    public:
        explicit Generator(const std::uint64_t seed) : state_(seed) {}

        [[nodiscard]] Recipe Next(const std::uint32_t ordinal) noexcept {
            Recipe recipe;
            for (auto &word : recipe.words) {
                state_ += 0x9e3779b97f4a7c15ULL;
                auto value = state_;
                value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
                value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
                word = static_cast<std::uint32_t>(value ^ (value >> 31U));
            }
            recipe.words[0] = ordinal % 8;
            return recipe;
        }

    private:
        std::uint64_t state_;
    };

    /** @brief Exact decimal replay encoding; no native byte ordering or locale-sensitive floating point. */
    inline std::string Encode(const Recipe &recipe) {
        std::ostringstream stream;
        for (const auto word : recipe.words)
            stream << word << ' ';
        return stream.str();
    }

    /** @brief Bounded replay parser rejects excess fields and never silently chooses a different recipe. */
    inline Recipe Decode(const std::string &text) {
        REQUIRE(text.size() <= 128);
        Recipe recipe;
        std::istringstream stream{text};
        for (auto &word : recipe.words)
            REQUIRE(static_cast<bool>(stream >> word));
        std::string extra;
        REQUIRE_FALSE(static_cast<bool>(stream >> extra));
        REQUIRE(recipe.words[0] < 8);
        return recipe;
    }

    /** @brief Shrinks scalar recipe parameters while preserving exact failure identity and mutation family. */
    template <class Check> Recipe Minimize(Recipe recipe, const std::string &failure, Check check, std::size_t &attempts) {
        attempts = 0;
        for (std::size_t field = 1; field < recipe.words.size() && attempts < MaximumShrinkAttempts; ++field) {
            auto candidate = recipe;
            candidate.words[field] = 0;
            ++attempts;
            if (check(candidate) == failure) {
                recipe = candidate;
                continue;
            }
            for (auto value = recipe.words[field] / 2; value > 0 && attempts < MaximumShrinkAttempts; value /= 2) {
                candidate = recipe;
                candidate.words[field] = value;
                ++attempts;
                if (check(candidate) == failure)
                    recipe = candidate;
            }
        }
        return recipe;
    }

    /** @brief Removes byte ranges while retaining the same production failure signature within a fixed evaluation budget. */
    template <class Check> std::string MinimizeBytes(std::string bytes, const std::string &failure, Check check, std::size_t &attempts) {
        REQUIRE(bytes.size() <= MaximumInputBytes);
        attempts = 0;
        for (auto width = bytes.size() / 2; width > 0 && attempts < MaximumShrinkAttempts; width /= 2) {
            for (std::size_t offset = 0; offset < bytes.size() && attempts < MaximumShrinkAttempts;) {
                auto candidate = bytes;
                candidate.erase(offset, width);
                ++attempts;
                if (check(candidate) == failure)
                    bytes = std::move(candidate);
                else
                    offset += width;
            }
        }
        return bytes;
    }

    /** @brief One bounded source mutation; format-specific parser bounds remain explicit operation inputs. */
    struct SourceInput final {
        std::string bytes;
        bool accepts{};
        bool lowerByteLimit{};
    };

    /** @brief Replaces one required baseline token, failing the fixture rather than weakening its oracle. */
    inline void Replace(std::string &bytes, const std::string &before, const std::string &after) {
        const auto offset = bytes.find(before);
        REQUIRE(offset != std::string::npos);
        bytes.replace(offset, before.size(), after);
    }

    /** @brief Produces all source mutation families from a real canonical source without duplicating its parser. */
    inline SourceInput Source(std::string baseline, const Recipe &recipe) {
        SourceInput result{std::move(baseline), false, false};
        switch (recipe.words[0]) {
            case 0:
                result.bytes.insert(0, recipe.words[1] % 17, ' ');
                result.accepts = true;
                break;
            case 1:
                Replace(result.bytes, "0.0", "1e" + std::to_string(999U + recipe.words[1] % 100));
                break;
            case 2:
                result.bytes.insert(1, "\"assetId\":\"00000000-0000-0000-0000-000000000002\",");
                break;
            case 3:
                result.bytes.resize(recipe.words[1] % (result.bytes.size() / 2));
                break;
            case 4:
                result.lowerByteLimit = true;
                break;
            case 5:
                result.bytes.insert(recipe.words[1] % result.bytes.size(), 1, static_cast<char>(0xff));
                break;
            case 6:
                Replace(result.bytes, "\"0.2.0\"", "\"0.2.1\"");
                break;
            case 7:
                Replace(result.bytes, "\"components\": []",
                        "\"components\": [{\"instanceId\":1,\"typeId\":\"game.fuzz.opaque\",\"schemaVersion\":1,"
                        "\"encoding\":\"canonicalJson\",\"payload\":{\"bytes\":[123,125]}}]");
                result.accepts = true;
                break;
        }
        REQUIRE(result.bytes.size() <= MaximumInputBytes);
        return result;
    }

    /** @brief Runs three fixed seed cohorts, or one explicit replay; unexpected failures shrink before reporting. */
    template <class Check> void Run(const char *format, Check check, const unsigned casesPerSeed = 32) {
        REQUIRE(casesPerSeed > 0);
        REQUIRE(casesPerSeed <= 32);
        const auto guarded = [&](const Recipe &recipe) {
            try {
                return check(recipe);
            } catch (const std::exception &error) {
                return std::string{"unexpected exception/"} + typeid(error).name();
            }
        };
        const auto evaluate = [&](const Recipe &recipe, const std::uint64_t seed, const unsigned ordinal) {
            const auto failure = guarded(recipe);
            if (failure.empty())
                return;
            std::size_t attempts{};
            const auto minimized = Minimize(recipe, failure, guarded, attempts);
            INFO("format=" << format << " seed=" << seed << " ordinal=" << ordinal << " shrink evaluations=" << attempts);
            INFO("HORO_PREFAB_FUZZ_REPLAY='" << Encode(minimized) << "' (select this exact test case)");
            FAIL(failure);
        };
        if (const auto *replay = std::getenv("HORO_PREFAB_FUZZ_REPLAY")) {
            evaluate(Decode(replay), 0, 0);
            return;
        }
        for (const auto seed : {0x1081ULL, 0x504642ULL, 0xdeadbeefULL}) {
            Generator generator{seed};
            for (unsigned ordinal = 0; ordinal < casesPerSeed; ++ordinal)
                evaluate(generator.Next(ordinal), seed, ordinal);
        }
    }
}  // namespace Horo::Prefab::Fuzz
