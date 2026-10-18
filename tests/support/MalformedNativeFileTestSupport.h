#ifndef HORO_TESTS_SUPPORT_MALFORMED_NATIVE_FILE_TEST_SUPPORT_H
#define HORO_TESTS_SUPPORT_MALFORMED_NATIVE_FILE_TEST_SUPPORT_H

/** @file MalformedNativeFileTestSupport.h
 * @brief Test-only admission of invalid native filenames without confusing filesystem denial with data loss.
 */

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <vector>

namespace Horo::TestSupport {
#ifndef _WIN32
    /** @brief Creates and verifies actual malformed-name bytes, or accepts only macOS encoding/name rejection.
     * @param path Native invalid-byte filename beneath an already created parent.
     * @param bytes Exact initial content, verified before returning a created fixture.
     * @return True for an actual file; false only for EILSEQ/EINVAL denial on macOS. Other failures fail the test.
     */
    inline bool WriteMalformedNativeFile(const std::filesystem::path &path, const std::span<const std::uint8_t> bytes) {
        errno = 0;
        std::ofstream output{path, std::ios::binary | std::ios::trunc};
        const int creationError = errno;
        INFO("Malformed native filename creation errno: " << creationError);
        if (!output) {
#ifdef __APPLE__
            REQUIRE((creationError == EILSEQ || creationError == EINVAL));
            std::error_code error;
            CHECK_FALSE(std::filesystem::exists(path, error));
            return false;
#else
            REQUIRE(static_cast<bool>(output));
#endif
        }
        for (const auto byte : bytes)
            output.put(static_cast<char>(byte));
        output.close();
        REQUIRE(static_cast<bool>(output));
        std::ifstream before{path, std::ios::binary};
        REQUIRE(before.is_open());
        const std::vector<std::uint8_t> retained{std::istreambuf_iterator<char>{before}, std::istreambuf_iterator<char>{}};
        REQUIRE(retained.size() == bytes.size());
        REQUIRE(std::equal(retained.begin(), retained.end(), bytes.begin()));
        return true;
    }
#endif
}  // namespace Horo::TestSupport

#endif
