#pragma once

#include "Horo/Assets/AssetCook.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>

namespace Horo::Assets::CookTestValues {
    inline AssetId Id(const std::string_view value) {
        auto parsed = AssetId::Parse(value);
        REQUIRE((parsed.HasValue()));
        return parsed.Value();
    }

    inline AssetTypeId Type(const std::string_view value) {
        auto parsed = AssetTypeId::Parse(value);
        REQUIRE((parsed.HasValue()));
        return parsed.Value();
    }

    inline AssetCookTargetId Target(const std::string_view value) {
        auto parsed = AssetCookTargetId::Parse(value);
        REQUIRE((parsed.HasValue()));
        return parsed.Value();
    }

    /** @brief Finite collision budget shared by fixture creation and deterministic admission coverage. */
    inline constexpr std::size_t MaximumCookTestDirectoryAttempts = 32;

    /** @brief Exclusively creates one candidate; collisions never transfer cleanup ownership.
     * @param candidate Supplies a candidate path for each bounded attempt; used by deterministic fixture regressions.
     * @param canonicalRoot Whether to canonicalize the newly created root.
     * @return Only a directory created by this call.
     * @throws std::filesystem::filesystem_error For non-collision failures or exhausted candidates.
     */
    template <typename Candidate>
    std::filesystem::path CreateCookTestDirectoryFromCandidates(const Candidate &candidate, const bool canonicalRoot) {
        for (std::size_t attempt = 0; attempt < MaximumCookTestDirectoryAttempts; ++attempt) {
            const auto unique = candidate(attempt);
            std::error_code error;
            if (std::filesystem::create_directory(unique, error))
                return canonicalRoot ? std::filesystem::canonical(unique) : unique;
            if (error && error != std::errc::file_exists)
                throw std::filesystem::filesystem_error("Cannot exclusively create cook test directory", unique, error);
        }
        throw std::filesystem::filesystem_error("Cook test directory collision retry limit reached",
                                                std::make_error_code(std::errc::file_exists));
    }

    /** @brief Exclusively creates a private root before its cleanup owner adopts it; retries bounded name collisions only. */
    inline std::filesystem::path CreateCookTestDirectory(const std::string_view prefix, const bool canonicalRoot) {
        const auto parent = std::filesystem::current_path();
        return CreateCookTestDirectoryFromCandidates([prefix, &parent](const std::size_t attempt) {
            return parent / std::format("{}_test_{}_{}", prefix, std::chrono::steady_clock::now().time_since_epoch().count(), attempt);
        }, canonicalRoot);
    }

    /** @brief Owns a uniquely named temporary project root and removes only that root at teardown. */
    struct OwnedCookTestDirectory {
        std::filesystem::path path;

        OwnedCookTestDirectory(const std::string_view prefix, const bool canonicalRoot)
            : path(CreateCookTestDirectory(prefix, canonicalRoot)) {}

        OwnedCookTestDirectory(const OwnedCookTestDirectory &) = delete;
        OwnedCookTestDirectory &operator=(const OwnedCookTestDirectory &) = delete;
        OwnedCookTestDirectory(OwnedCookTestDirectory &&) = delete;
        OwnedCookTestDirectory &operator=(OwnedCookTestDirectory &&) = delete;

        ~OwnedCookTestDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    };
}  // namespace Horo::Assets::CookTestValues
