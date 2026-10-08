#pragma once

#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <thread>

namespace Horo::Runtime::ScenePublicationDetail {
    /** @brief Owner-thread state shared only by Scene queue admission and its no-fail aggregate transfer. */
    struct State final {
        std::thread::id owner{std::this_thread::get_id()};
        ScenePublicationSnapshot snapshot;
    };
}  // namespace Horo::Runtime::ScenePublicationDetail
