#ifndef HORO_EDITOR_SOURCE_COMMAND_ENDPOINT_H
#define HORO_EDITOR_SOURCE_COMMAND_ENDPOINT_H

/** @file EditorSourceCommandEndpoint.h @brief Transport-neutral lease for embedded source presentation. */

#include "Horo/Editor/EditorSurfaceIdentity.h"

#include <functional>
#include <thread>

namespace Horo::Editor {
    /** @brief Zero-based UTF-16 source coordinate admitted from a presentation request. */
    struct EditorSourceCommandPosition final {
        std::uint32_t line{};
        std::uint32_t utf16Column{};
    };

    /** @brief Bound to one workspace owner and borrowed during synchronous editor-lane calls. */
    struct EditorSourceCommandEndpoint final {
        using Present = std::function<Result<void>(const SourceDocumentId &, std::optional<EditorSourceCommandPosition>,
                                                   const std::function<bool()> &)>;
        std::string project;
        std::uint64_t authorizationRevision{};
        std::thread::id owner;
        Present present;
    };
}  // namespace Horo::Editor

#endif  // HORO_EDITOR_SOURCE_COMMAND_ENDPOINT_H
