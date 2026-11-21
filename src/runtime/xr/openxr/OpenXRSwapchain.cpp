#include "host/OpenXRSwapchain.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>

namespace Horo::XR::OpenXRInternal {
    namespace {
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
        [[nodiscard]] XrSwapchainUsageFlags NativeUsage(const Render::RenderTextureDescriptor &texture,
                                                        const NativeSwapchainRole role) noexcept {
            using Render::HasTextureUsage;
            using enum Render::RenderTextureUsage;
            XrSwapchainUsageFlags usage{};
            if (HasTextureUsage(texture.usage, RenderAttachment))
                usage |= IsDepthSwapchain(role) ? XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
            if (HasTextureUsage(texture.usage, Sampled))
                usage |= XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            if (HasTextureUsage(texture.usage, Storage))
                usage |= XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT;
            if (HasTextureUsage(texture.usage, CopySource))
                usage |= XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
            if (HasTextureUsage(texture.usage, CopyDestination))
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
                   request.texture.layerCount <= XRViewRenderHardLimits::MaximumArrayLayers && depth == IsDepthSwapchain(request.role) &&
                   request.reprojection.has_value() == IsReprojectionSwapchain(request.role);
        }

        /** @brief Checks the bounded motion/depth allocation class before calling native discovery. */
        bool ValidReprojectionDescriptor(const Render::RenderTextureDescriptor &descriptor, const NativeSwapchainRole role) noexcept {
            using Render::RenderTextureUsage;
            using enum Render::RenderTextureFormat;
            const bool format = role == NativeSwapchainRole::Motion
                                    ? descriptor.format == Rgba16Float || descriptor.format == Rgba32Float
                                    : descriptor.format == Depth16Unorm || descriptor.format == Depth32Float;
            return IsReprojectionSwapchain(role) && format && descriptor.IsValid() &&
                   descriptor.dimension == Render::RenderTextureDimension::TwoD && descriptor.mipCount == 1 &&
                   descriptor.sampleCount == 1 &&
                   Render::HasTextureUsage(descriptor.usage, RenderTextureUsage::RenderAttachment | RenderTextureUsage::Sampled);
        }

        /** @brief Admits the concrete motion/depth allocation shape for an observed enabled native provider. */
        Result<void> ValidateReprojectionTuple(const Render::RenderTextureDescriptor &descriptor,
                                               const NativeReprojectionAllocation &selection, const NativeReprojectionOffer &observed) {
            if (selection.route == NativeReprojectionRoute::None || selection.route != observed.route ||
                !std::ranges::all_of(observed.extents, &Render::FramebufferExtent::IsValid))
                return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported));
            const auto &left = observed.extents.front();
            const auto &right = observed.extents.back();
            const Render::FramebufferExtent storage{std::max(left.width, right.width), std::max(left.height, right.height)};
            if (descriptor.layerCount != observed.extents.size() || descriptor.extent != storage ||
                selection.eyeExtents != observed.extents)
                return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc OpenXRSwapchain::OpenXRSwapchain */
    OpenXRSwapchain::OpenXRSwapchain(OpenXRNativeSession &session, IOpenXRSwapchainImages &images,
                                     const NativeSwapchainMetricHandles &metrics) noexcept
        : session_(&session), images_(&images), metrics_(metrics) {}

    void OpenXRSwapchain::RecordOutcome(const NativeSwapchainMetricOutcome outcome) const noexcept {
        metrics_.outcomes[static_cast<std::size_t>(outcome)].Add();
    }

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
        if (closing_ || lost_ || native_ == XR_NULL_HANDLE || phase_ == NativeSwapchainPhase::Prepared)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        if (auto session = session_->Validate(request_.target.session); session.HasError())
            return session;
        return images_->Validate(request_);
    }

    /** @copydoc OpenXRSwapchain::DispatchFailure */
    Error OpenXRSwapchain::DispatchFailure(const char *name, const XrResult result) {
        return NativeFailure(name, result);
    }

    /** @copydoc OpenXRSwapchain::ReadRuntimeFormats */
    Result<OpenXRSwapchain::RuntimeFormats> OpenXRSwapchain::ReadRuntimeFormats(const XrSession session) const {
        using Outcome = Result<RuntimeFormats>;
        RuntimeFormats formats{};
        auto result = dispatch_.formats(session, 0, &formats.count, nullptr);
        if (XR_FAILED(result))
            return Outcome::Failure(NativeFailure("enumerate swapchain formats", result));
        if (formats.count == 0 || formats.count > formats.entries.size())
            return Outcome::Failure(MakeError(XRErrors::CapacityExceeded));
        const std::uint32_t capacity = formats.count;
        result = dispatch_.formats(session, capacity, &formats.count, formats.entries.data());
        if (XR_FAILED(result))
            return Outcome::Failure(NativeFailure("enumerate swapchain formats", result));
        if (formats.count == 0 || formats.count > capacity)
            return Outcome::Failure(MakeError(XRErrors::CapacityExceeded));
        return Outcome::Success(std::move(formats));
    }

    /** @copydoc OpenXRSwapchain::SelectRuntimeFormat */
    Result<std::int64_t> OpenXRSwapchain::SelectRuntimeFormat(const XrSession session) const {
        const auto formats = ReadRuntimeFormats(session);
        if (formats.HasError())
            return Result<std::int64_t>::Failure(formats.ErrorValue());
        const auto available = std::span{formats.Value().entries}.first(formats.Value().count);
        const auto format = images_->SelectFormat(request_, available);
        if (format.HasError())
            return Result<std::int64_t>::Failure(format.ErrorValue());
        if (std::ranges::find(available, format.Value()) == available.end())
            return Result<std::int64_t>::Failure(
                MakeError(XRErrors::OperationIncompatible, "Renderer selected an unadvertised runtime format."));
        return format;
    }

    /** @copydoc OpenXRSwapchain::CreateAllocation */
    Result<void> OpenXRSwapchain::CreateAllocation(const XrSession session, const Render::RenderTextureDescriptor &texture,
                                                   const NativeSwapchainRole role, const std::int64_t format) {
        const XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO,
                                         nullptr,
                                         0,
                                         NativeUsage(texture, role),
                                         format,
                                         texture.sampleCount,
                                         texture.extent.width,
                                         texture.extent.height,
                                         1,
                                         texture.layerCount,
                                         texture.mipCount};
        auto result = dispatch_.create(session, &info, &native_);
        if (XR_FAILED(result))
            return Result<void>::Failure(NativeFailure("create swapchain", result));
        if (native_ == XR_NULL_HANDLE)
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        result = dispatch_.enumerate(native_, 0, &count_, nullptr);
        if (XR_FAILED(result))
            return Result<void>::Failure(NativeFailure("enumerate swapchain images", result));
        if (count_ == 0 || count_ > MaximumImages)
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        return Result<void>::Success();
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
        if (auto created = CreateAllocation(session.session, request_.texture, request_.role, format.Value()); created.HasError())
            return created;
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

    /** @copydoc OpenXRSwapchain::AdmitReprojectionAllocation */
    Result<void> OpenXRSwapchain::AdmitReprojectionAllocation(const NativeSwapchainRequest &request) const {
        if (request.foveation != NativeSwapchainFoveation::None || !ValidReprojectionDescriptor(request.texture, request.role))
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        const auto &selection = *request.reprojection;
        const auto offer = session_->DiscoverReprojection(selection.attempt, request.target.session);
        if (offer.HasError())
            return Result<void>::Failure(offer.ErrorValue());
        return ValidateReprojectionTuple(request.texture, selection, offer.Value());
    }

    /** @copydoc OpenXRSwapchain::PrepareReprojection */
    Result<void> OpenXRSwapchain::PrepareReprojection(const NativeSessionPreparation &session, const NativeSessionBorrow &owner,
                                                      const NativePreparedReprojectionSwapchain &allocation) {
        const auto validate = [&]() -> Result<void> {
            if (auto device = images_->ValidatePrepared(allocation); device.HasError())
                return device;
            const auto current = session_->BorrowPreparedSession(session);
            if (current.HasError())
                return Result<void>::Failure(current.ErrorValue());
            if (current.Value().incarnation != owner.incarnation)
                return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
            return Result<void>::Success();
        };
        if (auto current = validate(); current.HasError())
            return current;
        const auto formats = ReadRuntimeFormats(owner.session);
        if (formats.HasError())
            return Result<void>::Failure(formats.ErrorValue());
        const auto available = std::span{formats.Value().entries}.first(formats.Value().count);
        const auto selected = images_->SelectPreparedFormat(allocation, available);
        if (selected.HasError())
            return Result<void>::Failure(selected.ErrorValue());
        if (std::ranges::find(available, selected.Value()) == available.end())
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (auto current = validate(); current.HasError())
            return current;
        if (auto created = CreateAllocation(owner.session, allocation.texture, allocation.role, selected.Value()); created.HasError())
            return created;
        const OpenXRSwapchainImageEnumeration enumeration{native_, dispatch_.enumerate};
        if (auto enumerated = images_->EnumeratePrepared(allocation, count_, enumeration); enumerated.HasError())
            return enumerated;
        if (auto current = validate(); current.HasError())
            return current;
        stagedReprojection_.emplace(allocation, owner.incarnation);
        phase_ = NativeSwapchainPhase::Prepared;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRSwapchain::StageReprojection */
    Result<void> OpenXRSwapchain::StageReprojection(const NativeSessionPreparation &session,
                                                    const NativePreparedReprojectionSwapchain &allocation,
                                                    const PFN_xrGetInstanceProcAddr getProc) {
        if (auto thread = CheckThread(); thread.HasError())
            return thread;
        if (!getProc || phase_ != NativeSwapchainPhase::Empty || native_ != XR_NULL_HANDLE || !allocation.slot.IsValid() ||
            allocation.slot.generation == 0 || allocation.selection.attempt != session.activeAttempt ||
            !ValidReprojectionDescriptor(allocation.texture, allocation.role))
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        const auto owner = session_->BorrowPreparedSession(session);
        if (owner.HasError())
            return Result<void>::Failure(owner.ErrorValue());
        const auto offer = session_->DiscoverReprojection(session.activeAttempt);
        if (offer.HasError())
            return Result<void>::Failure(offer.ErrorValue());
        if (auto admitted = ValidateReprojectionTuple(allocation.texture, allocation.selection, offer.Value()); admitted.HasError())
            return admitted;
        if (auto device = images_->ValidatePrepared(allocation); device.HasError())
            return device;
        if (auto dispatch = Resolve(getProc, owner.Value().instance); dispatch.HasError())
            return dispatch;
        closing_ = false;
        lost_ = false;
        malformedAcquisition_ = false;
        try {
            auto prepared = PrepareReprojection(session, owner.Value(), allocation);
            if (prepared.HasError())
                if (auto rollback = Close(); rollback.HasError())
                    return Result<void>::Failure(rollback.ErrorValue());
            return prepared;
        } catch (...) {
            (void)Close();
            throw;
        }
    }

    /** @copydoc OpenXRSwapchain::AdoptReprojection */
    Result<void> OpenXRSwapchain::AdoptReprojection(const XRSessionId &session) {
        if (auto thread = CheckThread(); thread.HasError())
            return thread;
        if (phase_ != NativeSwapchainPhase::Prepared || !stagedReprojection_ || closing_ || lost_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        const auto owner = session_->Borrow(session);
        if (owner.HasError())
            return Result<void>::Failure(owner.ErrorValue());
        const auto &staged = *stagedReprojection_;
        if (owner.Value().incarnation != staged.incarnation)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        const auto &allocation = staged.allocation;
        const NativeSwapchainRequest request{{session, allocation.slot},
                                             allocation.texture,
                                             allocation.role,
                                             NativeSwapchainFoveation::None,
                                             allocation.selection};
        if (auto admitted = AdmitReprojectionAllocation(request); admitted.HasError())
            return admitted;
        if (auto adopted = images_->AdoptPrepared(request); adopted.HasError())
            return adopted;
        const auto current = session_->Borrow(session);
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        if (current.Value().incarnation != staged.incarnation)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        request_ = request;
        lastTarget_ = request.target;
        releasedImage_ = {};
        stagedReprojection_.reset();
        phase_ = NativeSwapchainPhase::Available;
        return Result<void>::Success();
    }

    Result<bool> OpenXRSwapchain::StageFixedProjection(const NativeSessionPreparation &session,
                                                       const Render::RenderTextureDescriptor &texture, const std::int64_t format,
                                                       const std::array<Render::FramebufferExtent, 2> eyes,
                                                       PFN_xrGetInstanceProcAddr getProc) {
        using Staged = Result<bool>;
        if (auto thread = CheckThread(); thread.HasError())
            return Staged::Failure(thread.ErrorValue());
        if (!getProc || native_ != XR_NULL_HANDLE || phase_ != NativeSwapchainPhase::Empty || !texture.IsValid() ||
            texture.layerCount != eyes.size() || texture.sampleCount != 1 || texture.mipCount != 1 ||
            texture.dimension != Render::RenderTextureDimension::TwoD)
            return Staged::Failure(MakeError(XRErrors::OperationIncompatible));
        const auto extension = session_->ReadExtensionAdmission(session.activeAttempt, XR_HTC_FOVEATION_EXTENSION_NAME);
        if (extension.HasError())
            return Staged::Failure(extension.ErrorValue());
        if (extension.Value().state != NativeExtensionState::Enabled)
            return Staged::Success(false);
        const auto owner = session_->BorrowPreparedSession(session);
        if (owner.HasError())
            return Staged::Failure(owner.ErrorValue());
        const auto &native = owner.Value();
        PFN_xrApplyFoveationHTC apply{};
        if (auto route = Load(getProc, native.instance, "xrApplyFoveationHTC", apply); route.HasError())
            return Staged::Success(false);
        if (auto resolved = Resolve(getProc, native.instance); resolved.HasError())
            return Staged::Failure(resolved.ErrorValue());
        std::array<XrSwapchainSubImage, 2> subImages{};
        for (std::uint32_t eye = 0; eye < eyes.size(); ++eye) {
            const auto extent = eyes[eye];
            constexpr auto maximum = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
            if (!extent.IsValid() || extent.width > texture.extent.width || extent.height > texture.extent.height ||
                extent.width > maximum || extent.height > maximum)
                return Staged::Failure(MakeError(XRErrors::ExternalTargetInvalid));
            subImages[eye].imageRect.extent = {static_cast<std::int32_t>(extent.width), static_cast<std::int32_t>(extent.height)};
            subImages[eye].imageArrayIndex = eye;
        }
        const auto formats = ReadRuntimeFormats(native.session);
        if (formats.HasError())
            return Staged::Failure(formats.ErrorValue());
        const auto offered = std::span{formats.Value().entries}.first(formats.Value().count);
        if (std::ranges::find(offered, format) == offered.end())
            return Staged::Success(false);
        closing_ = false;
        lost_ = false;
        if (auto allocated = CreateAllocation(native.session, texture, NativeSwapchainRole::Color, format); allocated.HasError())
            return Staged::Failure(allocated.ErrorValue());
        for (auto &view : subImages)
            view.swapchain = native_;
        const XrFoveationApplyInfoHTC info{XR_TYPE_FOVEATION_APPLY_INFO_HTC, nullptr, XR_FOVEATION_MODE_FIXED_HTC,
                                           static_cast<std::uint32_t>(subImages.size()), subImages.data()};
        const auto applied = apply(native.session, &info);
        if (applied != XR_SUCCESS) {
            const auto closed = Close();
            if (closed.HasError())
                return Staged::Failure(closed.ErrorValue());
            if (!closed.Value() || applied == XR_SESSION_LOSS_PENDING || applied == XR_ERROR_SESSION_LOST ||
                applied == XR_ERROR_INSTANCE_LOST)
                return Staged::Failure(NativeFailure("apply fixed foveation", applied));
            return Staged::Success(false);
        }
        const auto current = session_->BorrowPreparedSession(session);
        if (current.HasError() || current.Value().incarnation != native.incarnation)
            return Staged::Failure(MakeError(XRErrors::IdentityStale));
        applyFoveation_ = apply;
        foveationExtents_ = eyes;
        stagedFixedProjection_.emplace(texture, native.incarnation);
        phase_ = NativeSwapchainPhase::Prepared;
        return Staged::Success(true);
    }

    Result<void> OpenXRSwapchain::AdoptFixedProjection(const XRSwapchainTargetId &target) {
        if (auto thread = CheckThread(); thread.HasError())
            return thread;
        if (!target.IsValid() || phase_ != NativeSwapchainPhase::Prepared || !stagedFixedProjection_ || closing_ || lost_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        const auto owner = session_->Borrow(target.session);
        if (owner.HasError())
            return Result<void>::Failure(owner.ErrorValue());
        if (owner.Value().incarnation != stagedFixedProjection_->incarnation)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));

        // HTC modifies peripheral density inside the runtime. The renderer still imports ordinary stereo color views.
        const NativeSwapchainRequest request{target, stagedFixedProjection_->texture};
        request_ = request;
        lastTarget_ = target;
        if (auto device = images_->Validate(request); device.HasError())
            return device;
        const OpenXRSwapchainImageEnumeration enumeration{native_, dispatch_.enumerate};
        if (auto images = images_->Enumerate(request, count_, enumeration); images.HasError())
            return images;
        if (auto current = session_->Validate(target.session); current.HasError())
            return current;
        stagedFixedProjection_.reset();
        phase_ = NativeSwapchainPhase::Available;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRSwapchain::SetProjectionFoveation */
    Result<void> OpenXRSwapchain::SetProjectionFoveation(const std::optional<std::array<Math::Vec2, 2>> &centers) {
        using Outcome = Result<void>;
        if (auto current = CheckCurrent(); current.HasError())
            return current;
        if (phase_ != NativeSwapchainPhase::Available || !applyFoveation_)
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible));
        if (!centers && !gazeProfileApplied_)
            return Outcome::Success();
        if (centers && !std::ranges::all_of(*centers, [](const auto &point) {
                return std::abs(point.x) <= 1.0F && std::abs(point.y) <= 1.0F;
            }))
            return Outcome::Failure(MakeError(XRErrors::OperationInvalid));
        const auto owner = session_->Borrow(request_.target.session);
        if (owner.HasError())
            return Outcome::Failure(owner.ErrorValue());
        std::array<XrSwapchainSubImage, 2> subImages{};
        std::array<XrFoveationConfigurationHTC, 2> configurations{};
        for (std::uint32_t eye = 0; eye < subImages.size(); ++eye) {
            const auto &extent = foveationExtents_[eye];
            subImages[eye] = {native_, {{0, 0}, {static_cast<std::int32_t>(extent.width), static_cast<std::int32_t>(extent.height)}}, eye};
            if (centers) {
                const auto &point = (*centers)[eye];
                configurations[eye] = {XR_FOVEATION_LEVEL_LOW_HTC, 40.0F, {point.x, point.y}};
            }
        }
        const auto count = static_cast<std::uint32_t>(subImages.size());
        const XrFoveationCustomModeInfoHTC custom{XR_TYPE_FOVEATION_CUSTOM_MODE_INFO_HTC, nullptr, count, configurations.data()};
        const XrFoveationApplyInfoHTC info{XR_TYPE_FOVEATION_APPLY_INFO_HTC, centers ? &custom : nullptr,
                                           centers ? XR_FOVEATION_MODE_CUSTOM_HTC : XR_FOVEATION_MODE_FIXED_HTC,
                                           count, subImages.data()};
        // A failed native update may have changed runtime state. Keep rollback debt until fixed is positively restored.
        gazeProfileApplied_ = gazeProfileApplied_ || centers.has_value();
        const auto applied = applyFoveation_(owner.Value().session, &info);
        if (applied != XR_SUCCESS)
            return Outcome::Failure(NativeFailure("update gaze foveation", applied));
        if (auto current = CheckCurrent(); current.HasError())
            return current;
        gazeProfileApplied_ = centers.has_value();
        return Outcome::Success();
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
        if (request.reprojection) {
            if (auto synthesis = AdmitReprojectionAllocation(request); synthesis.HasError())
                return Outcome::Failure(std::move(synthesis).ErrorValue());
        }
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
        RecordOutcome(XR_FAILED(result) ? NativeSwapchainMetricOutcome::AcquireFailed : NativeSwapchainMetricOutcome::AcquireCompleted);
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
        if (result == XR_TIMEOUT_EXPIRED) {
            RecordOutcome(NativeSwapchainMetricOutcome::WaitTimeout);
            return Result<bool>::Success(false);
        }
        if (XR_FAILED(result)) {
            RecordOutcome(NativeSwapchainMetricOutcome::WaitFailed);
            lost_ = result == XR_ERROR_SESSION_LOST;
            return Result<bool>::Failure(NativeFailure("wait swapchain image", result));
        }
        RecordOutcome(NativeSwapchainMetricOutcome::WaitReady);
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

    /** @copydoc OpenXRSwapchain::CurrentReprojection */
    Result<NativeReprojectionAcquisition> OpenXRSwapchain::CurrentReprojection() const {
        using Outcome = Result<NativeReprojectionAcquisition>;
        const auto current = Current();
        if (current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (!IsReprojectionSwapchain(request_.role))
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible));
        const auto &acquisition = current.Value();
        const auto kind = request_.role == NativeSwapchainRole::Motion ? XRReprojectionImageKind::Motion : XRReprojectionImageKind::Depth;
        return Outcome::Success({{acquisition.image, kind, request_.texture}, acquisition.texture, request_.reprojection->eyeExtents});
    }

    /** @copydoc OpenXRSwapchain::ReleaseReady */
    Result<bool> OpenXRSwapchain::ReleaseReady() {
        if (!lost_) {
            const XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            const XrResult result = dispatch_.release(native_, &info);
            RecordOutcome(XR_FAILED(result) ? NativeSwapchainMetricOutcome::ReleaseFailed : NativeSwapchainMetricOutcome::ReleaseCompleted);
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
        stagedReprojection_.reset();
        stagedFixedProjection_.reset();
        applyFoveation_ = nullptr;
        foveationExtents_ = {};
        gazeProfileApplied_ = false;
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

    /** @copydoc OpenXRSwapchain::CloseAfterSessionLoss */
    Result<bool> OpenXRSwapchain::CloseAfterSessionLoss(const XRSessionId &owner) {
        if (auto thread = CheckThread(); thread.HasError())
            return Result<bool>::Failure(thread.ErrorValue());
        if (!owner.IsValid() || owner != request_.target.session)
            return Result<bool>::Failure(MakeError(XRErrors::IdentityStale));
        lost_ = true;
        releasedImage_ = {};
        return Close();
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
