#pragma once

/** @file SequenceDocument.h
 * @brief Immutable sequence documents and transient timeline presentation.
 */

#include "Horo/Cinematic/SequenceAsset.h"
#include "Horo/Editor/EditorSurfaceIdentity.h"
#include "Horo/Foundation/Sha256.h"

#include <filesystem>
#include <string_view>

namespace Horo::Editor {
    /** @brief Explicit default suffix for versioned cinematic sequence JSON sources. */
    inline constexpr std::string_view SequenceDocumentExtension = ".hsequence";

    /** @brief Presentation state, never serialized into a sequence asset or source history. */
    struct SequenceTimelineState final {
        std::uint64_t firstFrame{};
        std::uint64_t playhead{};
        double zoom{1.0};

        /** @brief Clamps presentation to the validated sequence range. @param duration Sequence frame count. */
        void Clamp(std::uint64_t duration) noexcept;
        /** @brief Returns the visible frame count. @param duration Sequence frame count. @return Bounded nonzero span. */
        [[nodiscard]] std::uint64_t VisibleFrames(std::uint64_t duration) const noexcept;
        /** @brief Maps a normalized canvas position to a frame. @param duration Frame count. @param position Normalized position.
         * @return Frame inside the visible interval, including its endpoints. */
        [[nodiscard]] std::uint64_t FrameAt(std::uint64_t duration, double position) const noexcept;
    };

    /** @brief Workspace-owned immutable source snapshot; no scene/runtime or presentation state. */
    struct SequenceDocument final {
        DocumentIdentity identity;
        Cinematic::SequenceAsset asset;
        Sha256Digest revision; /**< Exact source bytes admitted for this session. */

        /** @brief Opens a bounded validated sequence document at an admitted project path.
         * @param identity Sequence document identity. @param absolutePath Canonical regular source file.
         * @return Immutable document or the original typed source/schema error. */
        [[nodiscard]] static Result<SequenceDocument> Open(DocumentIdentity identity, const std::filesystem::path &absolutePath);
    };

    /** @brief Reports delivered track types available in the foundation view.
     * @param type Source track type. @return True for implemented transform, property, camera-cut and event tracks. */
    [[nodiscard]] bool IsTimelineTrackAvailable(Cinematic::SequenceTrackType type) noexcept;
}  // namespace Horo::Editor
