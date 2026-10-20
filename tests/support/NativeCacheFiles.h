#pragma once

/** @file NativeCacheFiles.h
 * @brief Explicit native cache authority composition for owned filesystem fixtures.
 */

#include "Horo/Platform/NativeImmutableFileStorage.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace Horo::TestSupport {
    /** @brief Grants exactly one fixture cache namespace while leaving missing cache directories uncreated.
     * @param root Fixture-owned absolute cache location; all existing ancestors belong to the test.
     * @return Owned native authority retained by the test request or cache-view owner.
     */
    inline std::shared_ptr<const ImmutableFileStorage> NativeCacheFiles(const std::filesystem::path &root) {
        const auto absolute = std::filesystem::absolute(root).lexically_normal();
        auto anchor = absolute.parent_path();
        while (!std::filesystem::exists(anchor)) {
            REQUIRE(anchor.has_relative_path());
            anchor = anchor.parent_path();
        }
        const auto relative = absolute.lexically_relative(anchor).generic_u8string();
        const std::string name{relative.begin(), relative.end()};
        auto files = Platform::CreateNativeImmutableFileStorage(std::filesystem::canonical(anchor), name);
        REQUIRE(files.HasValue());
        return std::move(files).Value();
    }
}  // namespace Horo::TestSupport
