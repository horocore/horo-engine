#pragma once

#include "Horo/Assets/AssetCook.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

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

    /** @brief Owns a uniquely named temporary project root and removes only that root at teardown. */
    struct OwnedCookTestDirectory {
        std::filesystem::path path;

        OwnedCookTestDirectory(const std::string_view prefix, const bool canonicalRoot) {
            const auto parent = std::filesystem::temp_directory_path() / prefix;
            std::filesystem::create_directories(parent);
            const auto unique = parent / ("test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(unique);
            path = canonicalRoot ? std::filesystem::canonical(unique) : unique;
        }

        ~OwnedCookTestDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    };
}  // namespace Horo::Assets::CookTestValues
