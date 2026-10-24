#pragma once
/** @file EditorPlayState.h
 * @brief Existing canonical editor Play state and view policy, shared only by non-installed host composition.
 */
#include <cstdint>

namespace Horo::Editor {
    /** @brief Existing explicit private Play view policy, projected by the actual selected factory/host.
     * @details Values are unchanged; this non-installed declaration is shared with the Game panel only.
     * SelectedXr is selection intent, never physical renderer or native session readiness.
     */
    enum class EditorPlayViewSource {
        AuthoredCamera,
        SelectedXr
    };
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
