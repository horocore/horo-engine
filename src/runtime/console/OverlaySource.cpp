#include "Horo/Debug/OverlaySource.h"

#include <array>

namespace Horo::Debug {
    namespace {
        constexpr std::array<std::string_view, static_cast<std::size_t>(OverlaySource::Count)> Names{"logs",   "frame", "cpu",
                                                                                                     "memory", "jobs",  "renderer",
                                                                                                     "assets", "scene", "input",
                                                                                                     "network"};
    }

    /** @copydoc OverlaySourceName */
    std::string_view OverlaySourceName(OverlaySource source) noexcept {
        const auto index = static_cast<std::size_t>(source);
        return index < Names.size() ? Names[index] : std::string_view{};
    }

    /** @copydoc ParseOverlaySource */
    std::optional<OverlaySource> ParseOverlaySource(std::string_view name) noexcept {
        for (std::size_t i = 0; i < Names.size(); ++i)
            if (Names[i] == name)
                return static_cast<OverlaySource>(i);
        return std::nullopt;
    }
}  // namespace Horo::Debug
