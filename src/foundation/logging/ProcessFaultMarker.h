#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace Horo::Log::Internal {
    /** @brief Scalar fault evidence from an exact, completed process marker. */
    struct ProcessFault {
        std::array<std::uint32_t, 4> id;
        std::uint32_t code;
        std::uint32_t thread;
    };

    /** @brief Reads only a complete platform marker from the supplied session's file. */
    [[nodiscard]] std::optional<ProcessFault> ReadProcessFault(const std::filesystem::path &path);

    /** @brief Removes a consumed or incomplete staging file on the platform that owns it. */
    void DiscardProcessFaultMarker(const std::filesystem::path &path) noexcept;

    /** @brief Prepares local fault capture and installs the host's native process hook when supported. */
    [[nodiscard]] bool ArmProcessFaultMarker(const std::filesystem::path &path);

    /** @brief Removes an unused marker and restores native handling, or yields ownership to the fault callback. */
    [[nodiscard]] bool DisarmProcessFaultMarker(const std::filesystem::path &path) noexcept;
}
