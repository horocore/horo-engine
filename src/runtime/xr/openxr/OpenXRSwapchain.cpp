#include "host/OpenXRSwapchain.h"

#include <algorithm>
#include <array>
#include <format>
#include <limits>

namespace Horo::XR::OpenXRInternal {
    namespace {
        constexpr std::uint32_t MaximumFormats = 128;
        constexpr std::uint32_t MaximumImages = 64;
        constexpr XrDuration MaximumWait = 100'000'000;

        /** @brief Preserves the official operation and numeric native failure without device identifiers. */
        [[nodiscard]] Error NativeFailure(const char *operation, const XrResult result) {
            const auto &code = result == XR_ERROR_FUNCTION_UNSUPPORTED || result == XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED
                                   ? XRErrors::OperationUnsupported
                                   : XRErrors::OperationUnavailable;
            return MakeError(code, std::format("OpenXR {} failed; native result={}", operation, static_cast<std::int32_t>(result)));
        }

        /** @brief Maps declared Horo uses without introducing additional native uses. */
        [[nodiscard]] XrSwapchainUsageFlags NativeUsage(const NativeSwapchainRequest &request) noexcept {
            using Render::HasTextureUsage;
            using enum Render::RenderTextureUsage;
            XrSwapchainUsageFlags usage{};
            if (HasTextureUsage(request.texture.usage, RenderAttachment))
                usage |= request.role == NativeSwapchainRole::Depth ? XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                                    : XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
            if (HasTextureUsage(request.texture.usage, Sampled))
                usage |= XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            if (HasTextureUsage(request.texture.usage, Storage))
                usage |= XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT;
            if (HasTextureUsage(request.texture.usage, CopySource))
                usage |= XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
            if (HasTextureUsage(request.texture.usage, CopyDestination))
                usage |= XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            return usage;
        }

        /** @brief Enforces an allocation shape the native base swapchain contract can describe. */
        [[nodiscard]] bool ValidRequest(const NativeSwapchainRequest &request) noexcept {
            using enum Render::RenderTextureFormat;
            const auto format = request.texture.format;
            const bool depth =
                format == Depth16Unorm || format == Depth24Stencil8 || format == Depth32Float || format == Depth32FloatStencil8;
            return request.target.IsValid() && request.role < NativeSwapchainRole::Count &&
                   request.foveation < NativeSwapchainFoveation::Count && request.texture.IsValid() &&
                   request.texture.dimension == Render::RenderTextureDimension::TwoD && request.texture.mipCount == 1 &&
                   request.texture.layerCount <= XRViewRenderHardLimits::MaximumArrayLayers &&
                   depth == (request.role == NativeSwapchainRole::Depth);
        }
    }  // namespace

    /** @copydoc OpenXRSwapchain::OpenXRSwapchain */
    OpenXRSwapchain::OpenXRSwapchain(OpenXRNativeSession &session, IOpenXRSwapchainImages &images) noexcept
        : session_(&session), images_(&images) {}

    /** @copydoc OpenXRSwapchain::~OpenXRSwapchain */
    OpenXRSwapchain::~OpenXRSwapchain() {
        HORO_INVARIANT_MSG(native_ == XR_NULL_HANDLE, "OpenXR swapchain requires successful explicit Close before destruction");
    }

    /** @copydoc OpenXRSwapchain::CheckThread */
    Result<void> OpenXRSwapchain::CheckThread() const {
        return thread_ == std::this_thread::get_id()
                   ? Result<void>::Success()
                   : Result<void>::Failure(
                         MakeError(XRErrors::OperationIncompatible, "OpenXR swapchain operation requires its control thread."));
    }

    /** @copydoc OpenXRSwapchain::CheckCurrent */
    Result<void> OpenXRSwapchain::CheckCurrent() const {
        if (auto thread = CheckThread(); thread.HasError())
            return thread;
        if (closing_ || lost_ || native_ == XR_NULL_HANDLE)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        if (auto session = session_->Validate(request_.target.session); session.HasError())
            return session;
        return images_->Validate(request_);
    }

    /** @copydoc OpenXRSwapchain::DispatchFailure */
    Error OpenXRSwapchain::DispatchFailure(const char *name, const XrResult result) {
        return NativeFailure(name, result);
    }

    /** @copydoc OpenXRSwapchain::SelectRuntimeFormat */
    Result<std::int64_t> OpenXRSwapchain::SelectRuntimeFormat(const XrSession session) const {
        std::array<std::int64_t, MaximumFormats> formats{};
        std::uint32_t count{};
        auto result = dispatch_.formats(session, 0, &count, nullptr);
        if (XR_FAILED(result))
            return Result<std::int64_t>::Failure(NativeFailure("enumerate swapchain formats", result));
        if (count == 0 || count > formats.size())
            return Result<std::int64_t>::Failure(MakeError(XRErrors::CapacityExceeded));
        const std::uint32_t capacity = count;
        result = dispatch_.formats(session, capacity, &count, formats.data());
        if (XR_FAILED(result))
            return Result<std::int64_t>::Failure(NativeFailure("enumerate swapchain formats", result));
        if (count == 0 || count > capacity)
            return Result<std::int64_t>::Failure(MakeError(XRErrors::CapacityExceeded));
        const auto available = std::span{formats}.first(count);
        const auto format = images_->SelectFormat(request_, available);
        if (format.HasError())
            return Result<std::int64_t>::Failure(format.ErrorValue());
        if (std::ranges::find(available, format.Value()) == available.end())
            return Result<std::int64_t>::Failure(
                MakeError(XRErrors::OperationIncompatible, "Renderer selected an unadvertised runtime format."));
        return format;
    }

    /** @copydoc OpenXRSwapchain::Prepare */
    Result<void> OpenXRSwapchain::Prepare(const NativeSessionBorrow &session) {
        const auto format = SelectRuntimeFormat(session.session);
        if (format.HasError())
            return Result<void>::Failure(format.ErrorValue());
        if (auto sessionState = session_->Validate(request_.target.session); sessionState.HasError())
            return sessionState;
        if (auto device = images_->Validate(request_); device.HasError())
            return device;
        const XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO,
                                         nullptr,
                                         0,
                                         NativeUsage(request_),
                                         format.Value(),
                                         request_.texture.sampleCount,
                                         request_.texture.extent.width,
                                         request_.texture.extent.height,
                                         1,
                                         request_.texture.layerCount,
                                         request_.texture.mipCount};
        auto result = dispatch_.create(session.session, &info, &native_);
        if (XR_FAILED(result))
            return Result<void>::Failure(NativeFailure("create swapchain", result));
        if (native_ == XR_NULL_HANDLE)
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        result = dispatch_.enumerate(native_, 0, &count_, nullptr);
        if (XR_FAILED(result))
            return Result<void>::Failure(NativeFailure("enumerate swapchain images", result));
        if (count_ == 0 || count_ > MaximumImages)
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        const OpenXRSwapchainImageEnumeration enumeration{native_, dispatch_.enumerate};
        if (auto images = images_->Enumerate(request_, count_, enumeration); images.HasError())
            return images;
        if (auto sessionState = session_->Validate(request_.target.session); sessionState.HasError())
            return sessionState;
        if (auto device = images_->Validate(request_); device.HasError())
            return device;
        releasedImage_ = {};
        phase_ = NativeSwapchainPhase::Available;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRSwapchain::AdmitCreate */
    Result<NativeSessionBorrow> OpenXRSwapchain::AdmitCreate(const NativeSwapchainRequest &request) const {
        using Outcome = Result<NativeSessionBorrow>;
        if (native_ != XR_NULL_HANDLE || !ValidRequest(request))
            return Outcome::Failure(MakeError(XRErrors::OperationInvalid));
        if (lastTarget_.IsValid() && request.target.session == lastTarget_.session && request.target.slot.index == lastTarget_.slot.index &&
            request.target.slot.generation <= lastTarget_.slot.generation)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        auto session = session_->Borrow(request.target.session);
        if (session.HasError())
            return session;
        if (request.role == NativeSwapchainRole::Depth && !session_->HasExtension("XR_KHR_composition_layer_depth"))
            return Outcome::Failure(
                MakeError(XRErrors::OperationUnsupported, "Projection depth requires negotiated XR_KHR_composition_layer_depth."));
        if (auto device = images_->Validate(request); device.HasError())
            return Outcome::Failure(std::move(device).ErrorValue());
        return session;
    }

    /** @copydoc OpenXRSwapchain::Acquire */
    Result<void> OpenXRSwapchain::Acquire() {
        if (auto current = CheckCurrent(); current.HasError())
            return current;
        if (phase_ != NativeSwapchainPhase::Available)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (generation_ == std::numeric_limits<std::uint32_t>::max())
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        const XrSwapchainImageAcquireInfo info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        const XrResult result = dispatch_.acquire(native_, &info, &index_);
        if (XR_FAILED(result)) {
            lost_ = result == XR_ERROR_SESSION_LOST;
            return Result<void>::Failure(NativeFailure("acquire swapchain image", result));
        }
        releasedImage_ = {};
        phase_ = NativeSwapchainPhase::Acquired;
        ++generation_;
        malformedAcquisition_ = index_ >= count_;
        if (malformedAcquisition_)
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "Runtime returned an out-of-bounds swapchain image."));
        acquisition_.image = {request_.target, {.index = index_, .generation = generation_}};
        return Result<void>::Success();
    }

    /** @copydoc OpenXRSwapchain::Wait */
    Result<bool> OpenXRSwapchain::Wait(const XrDuration timeout) {
        if (auto thread = CheckThread(); thread.HasError())
            return Result<bool>::Failure(thread.ErrorValue());
        if (phase_ != NativeSwapchainPhase::Acquired || lost_ || timeout < 0 || timeout > MaximumWait)
            return Result<bool>::Failure(MakeError(XRErrors::OperationInvalid));
        const XrSwapchainImageWaitInfo info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, timeout};
        const XrResult result = dispatch_.wait(native_, &info);
        if (result == XR_TIMEOUT_EXPIRED)
            return Result<bool>::Success(false);
        if (XR_FAILED(result)) {
            lost_ = result == XR_ERROR_SESSION_LOST;
            return Result<bool>::Failure(NativeFailure("wait swapchain image", result));
        }
        phase_ = NativeSwapchainPhase::Ready;
        return Result<bool>::Success(true);
    }

    /** @copydoc OpenXRSwapchain::Import */
    Result<NativeSwapchainAcquisition> OpenXRSwapchain::Import() {
        using Outcome = Result<NativeSwapchainAcquisition>;
        if (auto current = CheckCurrent(); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (phase_ != NativeSwapchainPhase::Ready || malformedAcquisition_)
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible));
        auto imported = images_->Import(request_, acquisition_.image, index_);
        if (imported.HasError())
            return Outcome::Failure(imported.ErrorValue());
        acquisition_.texture = imported.Value();
        phase_ = NativeSwapchainPhase::Imported;
        // A valid registration must be returned even if admission was revoked inside the adapter.
        // Retain its retirement obligation before reporting any publication failure.
        if (!acquisition_.texture.IsValid()) {
            closing_ = true;
            return Outcome::Failure(MakeError(XRErrors::OperationInvalid, "Renderer returned an invalid external registration."));
        }
        return Current();
    }

    /** @copydoc OpenXRSwapchain::Current */
    Result<NativeSwapchainAcquisition> OpenXRSwapchain::Current() const {
        if (auto current = CheckCurrent(); current.HasError())
            return Result<NativeSwapchainAcquisition>::Failure(current.ErrorValue());
        if (phase_ != NativeSwapchainPhase::Imported)
            return Result<NativeSwapchainAcquisition>::Failure(MakeError(XRErrors::OperationUnavailable));
        return Result<NativeSwapchainAcquisition>::Success(acquisition_);
    }

    /** @copydoc OpenXRSwapchain::ReleaseReady */
    Result<bool> OpenXRSwapchain::ReleaseReady() {
        if (!lost_) {
            const XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            const XrResult result = dispatch_.release(native_, &info);
            if (XR_FAILED(result)) {
                lost_ = result == XR_ERROR_SESSION_LOST;
                return Result<bool>::Failure(NativeFailure("release swapchain image", result));
            }
        }
        releasedImage_ = lost_ ? XRSwapchainImageId{} : acquisition_.image;
        acquisition_ = {};
        retireRequested_ = false;
        phase_ = NativeSwapchainPhase::Available;
        return Result<bool>::Success(true);
    }

    /** @copydoc OpenXRSwapchain::PollRelease */
    Result<bool> OpenXRSwapchain::PollRelease() {
        if (auto thread = CheckThread(); thread.HasError())
            return Result<bool>::Failure(thread.ErrorValue());
        using enum NativeSwapchainPhase;
        if (phase_ == Empty || phase_ == Available || phase_ == Closing)
            return Result<bool>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (phase_ == Acquired && !lost_)
            return Result<bool>::Failure(MakeError(XRErrors::OperationUnavailable, "Wait must complete before native release."));
        if (phase_ == Imported) {
            // Revoke retained plans before calling out; a failed retirement remains retryable but unpublishable.
            phase_ = Retiring;
            if (const auto retired = images_->Retire(acquisition_.texture); retired.HasError()) {
                retireRequested_ = false;
                return Result<bool>::Failure(retired.ErrorValue());
            }
            retireRequested_ = true;
        }
        if (phase_ == Retiring) {
            if (!retireRequested_) {
                if (const auto retired = images_->Retire(acquisition_.texture); retired.HasError())
                    return Result<bool>::Failure(retired.ErrorValue());
                retireRequested_ = true;
            }
            const auto complete = images_->RetirementComplete(acquisition_.texture);
            if (complete.HasError())
                return Result<bool>::Failure(complete.ErrorValue());
            if (!complete.Value())
                return Result<bool>::Success(false);
        }
        return ReleaseReady();
    }

    /** @copydoc OpenXRSwapchain::FinishClose */
    Result<bool> OpenXRSwapchain::FinishClose() {
        if (auto reset = images_->Reset(); reset.HasError())
            return Result<bool>::Failure(reset.ErrorValue());
        phase_ = NativeSwapchainPhase::Closing;
        if (native_ != XR_NULL_HANDLE) {
            const XrResult result = dispatch_.destroy(native_);
            if (XR_FAILED(result))
                return Result<bool>::Failure(NativeFailure("destroy swapchain", result));
            native_ = XR_NULL_HANDLE;
        }
        count_ = 0;
        phase_ = NativeSwapchainPhase::Empty;
        return Result<bool>::Success(true);
    }

    /** @copydoc OpenXRSwapchain::Close */
    Result<bool> OpenXRSwapchain::Close() {
        using enum NativeSwapchainPhase;
        if (auto thread = CheckThread(); thread.HasError())
            return Result<bool>::Failure(thread.ErrorValue());
        closing_ = true;
        if (phase_ == Acquired && !lost_) {
            auto waited = Wait(0);
            if (waited.HasError() || !waited.Value())
                return waited;
        }
        if (phase_ == Acquired || phase_ == Ready || phase_ == Imported || phase_ == Retiring) {
            auto released = PollRelease();
            if (released.HasError() || !released.Value())
                return released;
        }
        return FinishClose();
    }

    /** @copydoc OpenXRSwapchain::ValidateAcquisition */
    Result<void> OpenXRSwapchain::ValidateAcquisition(const XRSwapchainImageId &image) const {
        if (auto current = CheckCurrent(); current.HasError())
            return current;
        if ((phase_ != NativeSwapchainPhase::Ready && phase_ != NativeSwapchainPhase::Imported) || malformedAcquisition_ ||
            image != acquisition_.image)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        return Result<void>::Success();
    }

    /** @copydoc OpenXRSwapchain::BorrowReleased */
    Result<NativeReleasedSwapchainImage> OpenXRSwapchain::BorrowReleased(const XRSwapchainImageId &image) const {
        using Outcome = Result<NativeReleasedSwapchainImage>;
        if (auto current = CheckCurrent(); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (!image.IsValid() || image != releasedImage_ || phase_ != NativeSwapchainPhase::Available)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        return Outcome::Success({native_, request_});
    }

    /** @copydoc OpenXRSwapchain::Phase */
    NativeSwapchainPhase OpenXRSwapchain::Phase() const noexcept {
        return phase_;
    }
}  // namespace Horo::XR::OpenXRInternal
