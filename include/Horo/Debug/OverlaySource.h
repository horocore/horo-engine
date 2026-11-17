#pragma once

/** @file OverlaySource.h @brief Canonical inert overlay source identities shared by launch policy and presentation. */
#include <cstdint>
#include <optional>
#include <string_view>

namespace Horo::Debug {
    /** @brief Owning snapshot family; identifying a source never queries or starts its service. */
    enum class OverlaySource : std::uint8_t {
        Logs,
        Frame,
        Cpu,
        Memory,
        Jobs,
        Renderer,
        Assets,
        Scene,
        Input,
        Network,
        Count
    };

    /** @brief Inert presentation intent; the owning host validates capability/permissions and submits through its dispatcher. */
    struct OverlayVisibilityIntent final {
        OverlaySource source{OverlaySource::Logs};
        bool visible{};
    };

    /** @brief Returns the canonical portable launch name.
     * @param source Typed source identity. @return Static name or an empty view for an invalid/sentinel value.
     */
    [[nodiscard]] std::string_view OverlaySourceName(OverlaySource source) noexcept;
    /** @brief Resolves an exact canonical portable name without aliasing or normalization.
     * @param name Untrusted launch spelling. @return Typed identity, or no match.
     */
    [[nodiscard]] std::optional<OverlaySource> ParseOverlaySource(std::string_view name) noexcept;
}  // namespace Horo::Debug
