#pragma once
/** @file FixtureFileRead.h @brief Bounded bulk reads for immutable binary test fixtures. */

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>

namespace Horo::Tests {
    /**
     * @brief Reads a complete fixture without per-byte stream extraction.
     * @param path Native filesystem path to the fixture.
     * @param maximumBytes Maximum accepted file size before allocating storage.
     * @return The complete binary contents, including embedded null bytes.
     * @throws std::filesystem::filesystem_error If opening, sizing or reading fails,
     * the size exceeds the bound, or the file grows during the read.
     */
    [[nodiscard]] inline std::string ReadFixtureBytes(const std::filesystem::path &path,
                                                      const std::size_t maximumBytes = 256U * 1024U * 1024U) {
        if (!std::filesystem::is_regular_file(path))
            throw std::filesystem::filesystem_error("Fixture is not a regular file", path,
                                                    std::make_error_code(std::errc::invalid_argument));
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input)
            throw std::filesystem::filesystem_error("Cannot open fixture", path, std::make_error_code(std::errc::io_error));
        const std::streamoff length = input.tellg();
        if (length < 0)
            throw std::filesystem::filesystem_error("Cannot size fixture", path, std::make_error_code(std::errc::io_error));
        const auto limit = std::min<std::uintmax_t>(
            {maximumBytes, std::string{}.max_size(), static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())});
        if (static_cast<std::uintmax_t>(length) > limit)
            throw std::filesystem::filesystem_error("Fixture exceeds size bound", path, std::make_error_code(std::errc::file_too_large));
        std::string bytes(static_cast<std::size_t>(length), '\0');
        input.seekg(0);
        input.read(bytes.data(), static_cast<std::streamsize>(length));
        if (!input || input.gcount() != length)
            throw std::filesystem::filesystem_error("Cannot read complete fixture", path, std::make_error_code(std::errc::io_error));
        // A prefix must not hide appended data from the caller's digest validation.
        const auto trailing = input.peek();
        if (input.bad() || trailing != std::char_traits<char>::eof())
            throw std::filesystem::filesystem_error("Fixture changed during read", path, std::make_error_code(std::errc::io_error));
        return bytes;
    }
}  // namespace Horo::Tests
