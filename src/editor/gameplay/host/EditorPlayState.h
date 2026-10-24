#pragma once
/** @file EditorPlayState.h
 * @brief Existing canonical editor Play state, shared only by non-installed host composition.
 */
#include <cstdint>

namespace Horo::Editor {
    /** @brief Presentation projection of the editor play-session lifecycle; never another state authority. */
    enum class EditorPlayState : std::uint8_t {
        Idle,
        Starting,
        Playing,
        Paused,
        Stopping,
        Failed,
    };
}  // namespace Horo::Editor
