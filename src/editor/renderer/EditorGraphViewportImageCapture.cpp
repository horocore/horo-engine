#include "editor/renderer/EditorGraphViewportImageCapture.h"

#include "RenderFrontendResourceAccess.h"
#include "editor/renderer/EditorRenderMemoryScopes.h"
#include "editor/renderer/EditorRendererErrors.h"

#include <limits>
#include <new>
#include <utility>

namespace Horo::Editor {
    namespace {
        constexpr auto Lifetime = std::chrono::seconds{30};
        constexpr auto StartSpacing = std::chrono::seconds{1};
        static_assert(EditorViewportImageRegion::MaximumDimension == Render::RenderGraphColorReadback::MaximumAxis);
        static_assert(EditorViewportImageRegion::Channels == Render::RenderGraphColorReadback::BytesPerTexel);
    }  // namespace

    /** @copydoc EditorGraphViewportImageCapture::Request */
    Result<std::uint64_t> EditorGraphViewportImageCapture::Request(Source source, EditorViewportImageRegion region,
                                                                   std::function<bool()> authorized) {
        using Output = Result<std::uint64_t>;
        const auto now = Clock::now();
        Expire(now);
        if (!source.target.IsValid() || !source.image.IsValid() || !region.Fits(source.extent.width, source.extent.height) || !authorized ||
            !authorized())
            return Output::Failure(MakeError(RendererErrors::ViewportCapabilityUnavailable));
        if (phase_ == EditorViewportImagePhase::Pending || staging_.IsValid() || (started_ && now - *started_ < StartSpacing) ||
            serial_ == std::numeric_limits<std::uint64_t>::max())
            return Output::Failure(MakeError(RendererErrors::ViewportCapabilityUnavailable, "Viewport readback is busy."));
        Withdraw(EditorViewportImagePhase::Pending);
        source_ = source;
        region_ = region;
        started_ = now;
        authorized_ = std::move(authorized);
        return Output::Success(++serial_);
    }

    /** @copydoc EditorGraphViewportImageCapture::Read */
    std::optional<EditorViewportImageSnapshot> EditorGraphViewportImageCapture::Read(std::uint64_t id, Source source) {
        Reconcile(source);
        if (!started_ || serial_ != id)
            return std::nullopt;
        if (phase_ == EditorViewportImagePhase::Ready)
            return EditorViewportImageSnapshot{phase_, pixels_, publication_, *started_ + Lifetime};
        return EditorViewportImageSnapshot{phase_, {}};
    }

    /** @copydoc EditorGraphViewportImageCapture::Cancel */
    bool EditorGraphViewportImageCapture::Cancel(std::uint64_t id) noexcept {
        Expire(Clock::now());
        if (!id || serial_ != id || (phase_ != EditorViewportImagePhase::Pending && phase_ != EditorViewportImagePhase::Ready))
            return false;
        Withdraw(EditorViewportImagePhase::Cancelled);
        return true;
    }

    /** @brief Withdraws both the detached pixels and any queued MCP deliveries. */
    void EditorGraphViewportImageCapture::Withdraw(EditorViewportImagePhase phase) noexcept {
        publication_.reset();
        pixels_.reset();
        authorized_ = {};
        signal_ = {};
        recorded_ = false;
        accepted_ = false;
        phase_ = phase;
    }

    /** @brief Applies TTL before checking the original owner and texture generation. */
    void EditorGraphViewportImageCapture::Reconcile(Source source) {
        Expire(Clock::now());
        if (phase_ != EditorViewportImagePhase::Pending && phase_ != EditorViewportImagePhase::Ready)
            return;
        if (authorized_ && !authorized_())
            Withdraw(EditorViewportImagePhase::Cancelled);
        else if (source != source_)
            Withdraw(EditorViewportImagePhase::Resized);
    }

    /** @brief Enforces monotonic expiry regardless of frame or transport progress. */
    void EditorGraphViewportImageCapture::Expire(Clock::time_point now) noexcept {
        if (started_ && now - *started_ >= Lifetime)
            Withdraw(EditorViewportImagePhase::Expired);
    }

    /** @brief Cold-allocates exact coherent staging and detached storage for the issued crop. */
    bool EditorGraphViewportImageCapture::PrepareStaging(Render::RenderFrontend &frontend) {
        using namespace Render;
        if (staging_.IsValid())
            return nativeStaging_ != 0;
        const auto created =
            frontend.CreateBuffer(RenderMemoryScopes::ViewportResources,
                                  {region_.Bytes(), RenderBufferUsage::CopyDestination, RenderBufferAccess::HostVisible}, {});
        if (created.HasError())
            return false;
        staging_ = created.Value().handle;
        if (frontend.ProcessResourceRequests().HasError() || frontend.ResourceOperationResult(created.Value().operation).HasError())
            return false;
        const auto instance = Detail::RenderFrontendResourceAccess::BackendInstance(frontend, staging_);
        if (instance.HasError())
            return false;
        try {
            pixels_ = std::make_shared<EditorViewportImageArtifact>();
            pixels_->region = region_;
            pixels_->rgba.resize(region_.Bytes());
            publication_ = std::make_shared<const Publication>();
        } catch (const std::bad_alloc &) {
            return false;
        }
        nativeStaging_ = instance.Value();
        return true;
    }

    /** @brief Observes only accepted completion and publishes the detached image without waiting. */
    void EditorGraphViewportImageCapture::Poll(Readback readback) {
        const auto read = readback.poll(readback.owner, nativeStaging_, signal_, pixels_->rgba);
        if (read.HasError())
            Fail();
        else if (read.Value())
            phase_ = EditorViewportImagePhase::Ready;
    }

    /** @brief Drops the frontend owner reference. Accepted submissions retain their own buffer lease. */
    void EditorGraphViewportImageCapture::ReleaseStaging(Render::RenderFrontend &frontend) noexcept {
        if (auto handle = std::exchange(staging_, {}); handle.IsValid())
            static_cast<void>(frontend.ReleaseBuffer(handle));
        nativeStaging_ = 0;
        signal_ = {};
        recorded_ = false;
        accepted_ = false;
    }

    /** @copydoc EditorGraphViewportImageCapture::Prepare */
    void EditorGraphViewportImageCapture::Prepare(Render::RenderFrontend &frontend, Source source, Readback readback) {
        Reconcile(source);
        if (phase_ == EditorViewportImagePhase::Pending) {
            if (!PrepareStaging(frontend))
                Fail();
            else if (accepted_)
                Poll(readback);
        }
        if (phase_ != EditorViewportImagePhase::Pending && staging_.IsValid())
            ReleaseStaging(frontend);
    }

    /** @copydoc EditorGraphViewportImageCapture::Pending */
    std::optional<EditorGraphViewportImageCapture::Copy> EditorGraphViewportImageCapture::Pending() {
        Reconcile(source_);
        if (phase_ != EditorViewportImagePhase::Pending || !nativeStaging_ || recorded_)
            return std::nullopt;
        return Copy{serial_, region_, staging_};
    }

    /** @copydoc EditorGraphViewportImageCapture::Recorded */
    void EditorGraphViewportImageCapture::Recorded(Render::RenderTimelinePoint signal) noexcept {
        signal_ = signal;
        recorded_ = true;
    }

    /** @copydoc EditorGraphViewportImageCapture::UnconfirmedSignal */
    Render::RenderTimelinePoint EditorGraphViewportImageCapture::UnconfirmedSignal() const noexcept {
        return recorded_ && !accepted_ ? signal_ : Render::RenderTimelinePoint{};
    }

    /** @copydoc EditorGraphViewportImageCapture::Finish */
    void EditorGraphViewportImageCapture::Finish(bool accepted) noexcept {
        if (!recorded_ || accepted_)
            return;
        if (accepted)
            accepted_ = true;
        else
            Fail();
    }

    /** @copydoc EditorGraphViewportImageCapture::Fail */
    void EditorGraphViewportImageCapture::Fail() noexcept {
        if (phase_ == EditorViewportImagePhase::Pending)
            Withdraw(EditorViewportImagePhase::Failed);
    }

    /** @copydoc EditorGraphViewportImageCapture::Shutdown */
    void EditorGraphViewportImageCapture::Shutdown(Render::RenderFrontend &frontend) noexcept {
        Withdraw(EditorViewportImagePhase::Failed);
        started_.reset();
        ReleaseStaging(frontend);
    }
}  // namespace Horo::Editor
