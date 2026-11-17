#pragma once
/** @file EditorDirectionalShadowOptions.h @brief Explicit startup shadow quality admission without renderer selection. */
#include "Horo/Runtime/Render/DirectionalShadow.h"

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

namespace Horo::Editor::Detail {
    /** @brief Resolves only documented product presets, never a backend-dependent fallback. */
    inline std::optional<Render::DirectionalShadowQuality> ParseDirectionalShadowQuality(const std::string_view value) noexcept {
        using Quality = Render::DirectionalShadowQuality;
        constexpr std::array names{std::pair{"off", Quality::Off}, std::pair{"low", Quality::Low}, std::pair{"medium", Quality::Medium},
                                   std::pair{"high", Quality::High}, std::pair{"ultra", Quality::Ultra}};
        const auto found = std::ranges::find_if(names, [value](const auto &entry) {
            return value == entry.first;
        });
        return found == names.end() ? std::nullopt : std::optional{found->second};
    }

    /** @brief Reads a unique explicit preset; malformed, missing and duplicate values fail before native startup. */
    inline std::optional<Render::DirectionalShadowProfile> ParseDirectionalShadowArguments(
        const std::span<char *const> arguments) noexcept {
        auto profile = Render::MakeDirectionalShadowProfile(Render::DirectionalShadowQuality::High);
        bool seen{};
        for (std::size_t at = 1; at < arguments.size(); ++at) {
            const std::string_view argument{arguments[at]};
            std::string_view value;
            if (argument.starts_with("--shadow-quality="))
                value = argument.substr(std::string_view{"--shadow-quality="}.size());
            else if (argument == "--shadow-quality") {
                if (++at == arguments.size())
                    return std::nullopt;
                value = arguments[at];
            } else
                continue;
            if (seen)
                return std::nullopt;
            const auto quality = ParseDirectionalShadowQuality(value);
            if (!quality)
                return std::nullopt;
            profile = Render::MakeDirectionalShadowProfile(*quality);
            seen = true;
        }
        return profile;
    }
}  // namespace Horo::Editor::Detail
