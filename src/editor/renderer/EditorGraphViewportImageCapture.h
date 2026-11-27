#pragma once

/** @file EditorGraphViewportImageCapture.h @brief One bounded capture owned by a graph-backed viewport. */
#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/RenderResource.h"
#include "Horo/Runtime/Render/RenderSubmission.h"
#include "editor/renderer/EditorViewportImageCapture.h"

#include <functional>
#include <optional>

namespace Horo::Render {
    class RenderFrontend;

}  // namespace Horo::Render

namespace Horo::Editor {
    /** @brief Retains one exact capture and never waits on native completion. */
    class EditorGraphViewportImageCapture final {
    public:
        /** @brief Exact resident viewport generation and size fixed at request admission. */
        struct Source {
            Render::RenderTargetHandle target;
            Render::RenderTextureHandle image;
            Render::FramebufferExtent extent;
            bool operator==(const Source &) const = default;
        };

        /** @brief Pending copy with ready staging, borrowed by one cold raster graph variant. */
        struct Copy {
            std::uint64_t id;
            EditorViewportImageRegion region;
            Render::RenderBufferHandle staging;
        };

        /** @brief Synchronous, borrowed port for observing this renderer's accepted copy. */
        struct Readback {
            void *owner;
            Result<bool> (*poll)(void *, std::uint64_t, Render::RenderTimelinePoint, std::span<std::byte>);
        };

        /** @brief Admits one capture of the selected generation and checks the start rate. */
        [[nodiscard]] Result<std::uint64_t> Request(Source source, EditorViewportImageRegion region, std::function<bool()> authorized);
        /** @brief Observes only the requested capture and revalidates the owner's authority. */
        [[nodiscard]] std::optional<EditorViewportImageSnapshot> Read(std::uint64_t id, Source source);
        /** @brief Withdraws an exact pending or ready capture. Native staging retires on the next cold boundary. */
        bool Cancel(std::uint64_t id) noexcept;
        /** @brief Resolves staging or polls the exact completed timeline before any new frame is admitted. */
        void Prepare(Render::RenderFrontend &frontend, Source source, Readback readback);
        /** @brief Returns only an exact, authorized, ready-to-record copy. */
        [[nodiscard]] std::optional<Copy> Pending();
        /** @brief Records the graph's signal until the existing frame owner reports queue acceptance. */
        void Recorded(Render::RenderTimelinePoint signal = {}) noexcept;
        /** @brief Returns the signal whose native submission has yet to be accepted. */
        [[nodiscard]] Render::RenderTimelinePoint UnconfirmedSignal() const noexcept;
        /** @brief Withdraws only a recorded copy whose queue rejected the frame. */
        void Finish(bool accepted) noexcept;
        /** @brief Withdraws the capture without failing normal viewport rendering. */
        void Fail() noexcept;
        /** @brief Withdraws any published pixels and releases the frontend staging handle. */
        void Shutdown(Render::RenderFrontend &frontend) noexcept;

    private:
        using Clock = std::chrono::steady_clock;

        struct Publication {};

        void Reconcile(Source source);
        void Expire(Clock::time_point now) noexcept;
        void Withdraw(EditorViewportImagePhase phase) noexcept;
        bool PrepareStaging(Render::RenderFrontend &frontend);
        void Poll(Readback readback);
        void ReleaseStaging(Render::RenderFrontend &frontend) noexcept;

        std::uint64_t serial_{};
        std::optional<Clock::time_point> started_;
        Source source_;
        EditorViewportImageRegion region_;
        EditorViewportImagePhase phase_{EditorViewportImagePhase::Failed};
        std::function<bool()> authorized_;
        Render::RenderBufferHandle staging_;
        std::uint64_t nativeStaging_{};
        Render::RenderTimelinePoint signal_;
        bool recorded_{};
        bool accepted_{};
        std::shared_ptr<EditorViewportImageArtifact> pixels_;
        std::shared_ptr<const Publication> publication_;
    };
}  // namespace Horo::Editor
