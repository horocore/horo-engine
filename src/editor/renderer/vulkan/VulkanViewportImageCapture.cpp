#include "VulkanViewportImageCapture.h"

#include "RenderFrontendResourceAccess.h"
#include "VulkanEditorGraphicsBridge.h"
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

    /** @copydoc VulkanViewportImageCapture::Request */
    Result<std::uint64_t> VulkanViewportImageCapture::Request(Source source, EditorViewportImageRegion region,
                                                              std::function<bool()> authorized) {
        using Output = Result<std::uint64_t>;
        const auto now = Clock::now();
        Expire(now);
        if (!source.target.IsValid() || !source.image.IsValid() || !region.Fits(source.extent.width, source.extent.height))
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

    /** @copydoc VulkanViewportImageCapture::Read */
    std::optional<EditorViewportImageSnapshot> VulkanViewportImageCapture::Read(std::uint64_t id, Source source) {
        Reconcile(source);
        if (!started_ || serial_ != id)
            return std::nullopt;
        if (phase_ == EditorViewportImagePhase::Ready)
            return EditorViewportImageSnapshot{phase_, pixels_, publication_, *started_ + Lifetime};
        return EditorViewportImageSnapshot{phase_, {}};
    }

    /** @copydoc VulkanViewportImageCapture::Cancel */
    bool VulkanViewportImageCapture::Cancel(std::uint64_t id) noexcept {
        Expire(Clock::now());
        if (!id || serial_ != id || (phase_ != EditorViewportImagePhase::Pending && phase_ != EditorViewportImagePhase::Ready))
            return false;
        Withdraw(EditorViewportImagePhase::Cancelled);
        return true;
    }

    /** @brief Withdraws both the detached pixels and any queued MCP deliveries. */
    void VulkanViewportImageCapture::Withdraw(EditorViewportImagePhase phase) noexcept {
        publication_.reset();
        pixels_.reset();
        authorized_ = {};
        signal_ = {};
        accepted_ = false;
        phase_ = phase;
    }

    /** @brief Applies TTL before checking the original owner and texture generation. */
    void VulkanViewportImageCapture::Reconcile(Source source) {
        Expire(Clock::now());
        if (phase_ != EditorViewportImagePhase::Pending && phase_ != EditorViewportImagePhase::Ready)
            return;
        if (authorized_ && !authorized_())
            Withdraw(EditorViewportImagePhase::Cancelled);
        else if (source != source_)
            Withdraw(EditorViewportImagePhase::Resized);
    }

    /** @brief Enforces monotonic expiry regardless of frame or transport progress. */
    void VulkanViewportImageCapture::Expire(Clock::time_point now) noexcept {
        if (started_ && now - *started_ >= Lifetime)
            Withdraw(EditorViewportImagePhase::Expired);
    }

    /** @brief Cold-allocates exact coherent staging and detached storage for the issued crop. */
    bool VulkanViewportImageCapture::PrepareStaging(Render::RenderFrontend &frontend) {
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
    void VulkanViewportImageCapture::Poll(Render::Detail::IVulkanEditorGraphicsPort &port) {
        const auto read = port.ReadCompletedColor(nativeStaging_, signal_, pixels_->rgba);
        if (read.HasError())
            Fail();
        else if (read.Value())
            phase_ = EditorViewportImagePhase::Ready;
    }

    /** @brief Drops the frontend owner reference. Accepted submissions retain their own buffer lease. */
    void VulkanViewportImageCapture::ReleaseStaging(Render::RenderFrontend &frontend) noexcept {
        if (auto handle = std::exchange(staging_, {}); handle.IsValid())
            static_cast<void>(frontend.ReleaseBuffer(handle));
        nativeStaging_ = 0;
        signal_ = {};
        accepted_ = false;
    }

    /** @copydoc VulkanViewportImageCapture::Prepare */
    void VulkanViewportImageCapture::Prepare(Render::RenderFrontend &frontend, Render::Detail::IVulkanEditorGraphicsPort &port,
                                             Source source) {
        Reconcile(source);
        if (phase_ == EditorViewportImagePhase::Pending) {
            if (!PrepareStaging(frontend))
                Fail();
            else if (accepted_)
                Poll(port);
        }
        if (phase_ != EditorViewportImagePhase::Pending && staging_.IsValid())
            ReleaseStaging(frontend);
    }

    /** @copydoc VulkanViewportImageCapture::Pending */
    std::optional<VulkanViewportImageCapture::Copy> VulkanViewportImageCapture::Pending() {
        Reconcile(source_);
        if (phase_ != EditorViewportImagePhase::Pending || !nativeStaging_ || signal_.IsValid())
            return std::nullopt;
        return Copy{serial_, region_, staging_};
    }

    /** @copydoc VulkanViewportImageCapture::Recorded */
    void VulkanViewportImageCapture::Recorded(Render::RenderTimelinePoint signal) noexcept {
        signal_ = signal;
    }

    /** @copydoc VulkanViewportImageCapture::Finish */
    void VulkanViewportImageCapture::Finish(Render::Detail::IVulkanEditorGraphicsPort *port, std::uint64_t deviceIncarnation) noexcept {
        if (!signal_.IsValid() || accepted_)
            return;
        if (port) {
            const auto submitted = port->WasSubmissionAccepted(signal_, deviceIncarnation);
            if (submitted.HasValue() && submitted.Value()) {
                accepted_ = true;
                return;
            }
        }
        Fail();
    }

    /** @copydoc VulkanViewportImageCapture::Fail */
    void VulkanViewportImageCapture::Fail() noexcept {
        if (phase_ == EditorViewportImagePhase::Pending)
            Withdraw(EditorViewportImagePhase::Failed);
    }

    /** @copydoc VulkanViewportImageCapture::Shutdown */
    void VulkanViewportImageCapture::Shutdown(Render::RenderFrontend &frontend) noexcept {
        Withdraw(EditorViewportImagePhase::Failed);
        started_.reset();
        ReleaseStaging(frontend);
    }
}  // namespace Horo::Editor
