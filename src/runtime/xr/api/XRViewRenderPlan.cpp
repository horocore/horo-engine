#include "Horo/XR/XRViewRenderPlan.h"

#include "Horo/XR/XRErrors.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace Horo::XR {
    namespace {
        /** @brief Creates a failed result from a stable XR descriptor. */
        template <typename Value> [[nodiscard]] Result<Value> Reject(const ErrorCodeDescriptor &descriptor) {
            return Result<Value>::Failure(MakeError(descriptor));
        }

        /** @brief Checks the closed Horo render-texture usage mask. */
        [[nodiscard]] bool ValidTextureUsage(const Render::RenderTextureUsage usage) noexcept {
            using enum Render::RenderTextureUsage;
            constexpr auto maximumValidUsage = Sampled | RenderAttachment | CopySource | CopyDestination | Storage;
            return usage <= maximumValidUsage && Render::HasTextureUsage(usage, RenderAttachment);
        }

        /** @brief Reports whether a Horo render format has depth semantics. */
        [[nodiscard]] bool IsDepthFormat(const Render::RenderTextureFormat format) noexcept {
            using enum Render::RenderTextureFormat;
            return format == Depth16Unorm || format == Depth24Stencil8 || format == Depth32Float || format == Depth32FloatStencil8;
        }

        /** @brief Checks the closed backend-neutral render-format range. */
        [[nodiscard]] bool ValidTextureFormat(const Render::RenderTextureFormat format) noexcept {
            return format <= Render::RenderTextureFormat::Depth32FloatStencil8;
        }

        /** @brief Validates a known, structurally coherent view-configuration family. */
        [[nodiscard]] bool ValidConfigurationShape(const XRViewConfigurationDescriptor &configuration) noexcept {
            if (configuration.type >= XRViewConfigurationType::Count || configuration.blendMode >= XREnvironmentBlendMode::Count)
                return false;
            using enum XRViewConfigurationType;
            switch (configuration.type) {
                case PrimaryMono:
                    return configuration.viewCount == 1;
                case PrimaryStereo:
                    return configuration.viewCount == 2;
                case QuadView:
                    return configuration.viewCount == 4;
                case Other:
                    return configuration.viewCount > 0;
                case Count:
                    return false;
            }
            return false;
        }

        /** @brief Enforces the implementation's explicit supported configuration without truncation. */
        [[nodiscard]] Result<void> AdmitConfiguration(const XRViewConfigurationDescriptor &configuration,
                                                      const XRViewAdmissionPolicy &policy) {
            if (policy.mode >= XRViewAdmissionMode::Count || policy.maximumViews == 0 || policy.maximumViews > XRHardLimits::MaximumViews)
                return Reject<void>(XRErrors::ViewPlanInvalid);
            if (configuration.viewCount > policy.maximumViews)
                return Reject<void>(XRErrors::OperationUnsupported);

            using enum XRViewAdmissionMode;
            switch (policy.mode) {
                case SimulatorSingleView:
                    return configuration.type == XRViewConfigurationType::PrimaryMono && configuration.viewCount == 1
                               ? Result<void>::Success()
                               : Reject<void>(XRErrors::OperationUnsupported);
                case PrimaryStereo:
                    return configuration.type == XRViewConfigurationType::PrimaryStereo && configuration.viewCount == 2 &&
                                   configuration.blendMode == XREnvironmentBlendMode::Opaque
                               ? Result<void>::Success()
                               : Reject<void>(XRErrors::OperationUnsupported);
                case BoundedNView:
                    return Result<void>::Success();
                case Count:
                    return Reject<void>(XRErrors::ViewPlanInvalid);
            }
            return Reject<void>(XRErrors::ViewPlanInvalid);
        }

        /** @brief Checks view IDs, order, pose, projection and runtime extents. */
        [[nodiscard]] Result<void> ValidateViews(const XRViewRenderPlanDescriptor &descriptor, const XRSessionId &activeSession,
                                                 const XRWorldOriginRevision activeOriginRevision) {
            for (std::size_t index = 0; index < descriptor.views.size(); ++index) {
                const auto &view = descriptor.views[index];
                if (auto identity = ValidateXRSessionObject(view.id, activeSession); identity.HasError())
                    return identity;
                if (view.order != index || view.role >= XRViewRole::Count || !view.projection.IsValid() ||
                    !view.recommendedExtent.IsValid() || !view.maximumExtent.IsValid() ||
                    view.recommendedExtent.width > view.maximumExtent.width || view.recommendedExtent.height > view.maximumExtent.height)
                    return Reject<void>(XRErrors::ViewPlanInvalid);
                for (std::size_t priorIndex = 0; priorIndex < index; ++priorIndex) {
                    if (descriptor.views[priorIndex].id == view.id)
                        return Reject<void>(XRErrors::ViewPlanInvalid);
                }
                if (auto pose = XRPoseSample::Create(view.pose, activeSession, activeOriginRevision); pose.HasError())
                    return Result<void>::Failure(pose.ErrorValue());
                if (view.pose.purpose != XRPosePurpose::PresentationPrediction || view.pose.time.simulation.has_value() ||
                    view.pose.time.renderPrediction != descriptor.predictedDisplayTime)
                    return Reject<void>(XRErrors::TimeDomainIncompatible);
            }
            return Result<void>::Success();
        }

        /** @brief Validates both peripheral/inset pairs while preserving the runtime's full ordering. */
        [[nodiscard]] bool HasQuadRoles(const std::span<const XRViewDescriptor> views) noexcept {
            using enum XRViewRole;
            constexpr std::array roles{PrimaryLeft, PrimaryRight, FoveatedInsetLeft, FoveatedInsetRight};
            return std::ranges::all_of(roles, [views](const XRViewRole role) {
                return std::ranges::find(views, role, &XRViewDescriptor::role) != views.end();
            });
        }

        /** @brief Enforces semantic roles for the admitted view configuration. */
        [[nodiscard]] bool RolesMatchAdmission(const XRViewRenderPlanDescriptor &descriptor, const XRViewAdmissionMode mode) noexcept {
            const auto views = descriptor.views;
            using enum XRViewAdmissionMode;
            if (mode == SimulatorSingleView)
                return views.front().role == XRViewRole::Primary;
            if (mode == PrimaryStereo)
                return views[0].role == XRViewRole::PrimaryLeft && views[1].role == XRViewRole::PrimaryRight;
            return descriptor.configuration.type != XRViewConfigurationType::QuadView || HasQuadRoles(views);
        }

        /** @brief Finds the runtime order associated with one exact view identity. */
        [[nodiscard]] std::size_t FindViewOrder(const std::span<const XRViewDescriptor> views, const XRViewId &view) noexcept {
            for (const auto &candidate : views) {
                if (candidate.id == view)
                    return candidate.order;
            }
            return views.size();
        }

        /** @brief Validates one target's Horo format, subresource, sampling, and render-area contract. */
        [[nodiscard]] Result<void> ValidateTargetStructure(const XRExternalRenderTargetDescriptor &target,
                                                           const XRViewAdmissionPolicy &policy) {
            if (!target.image.IsValid() || target.image.target != target.target || target.role >= XRExternalRenderTargetRole::Count ||
                !ValidTextureFormat(target.format) || !ValidTextureUsage(target.usage) || !target.allocationExtent.IsValid() ||
                !target.renderRectangle.FitsWithin(target.allocationExtent) || target.arrayLayerCount == 0 ||
                target.arrayLayerCount > XRViewRenderHardLimits::MaximumArrayLayers || target.arrayLayer >= target.arrayLayerCount ||
                target.sampleCount == 0 || target.sampleCount > XRViewRenderHardLimits::MaximumSampleCount ||
                (target.sampleCount & (target.sampleCount - 1U)) != 0)
                return Reject<void>(XRErrors::ExternalTargetInvalid);

            const bool depth = target.role == XRExternalRenderTargetRole::ProjectionDepth;
            if (depth != IsDepthFormat(target.format))
                return Reject<void>(XRErrors::ExternalTargetInvalid);
            if (depth && !policy.supportsDepthTargets)
                return Reject<void>(XRErrors::OperationUnsupported);
            return Result<void>::Success();
        }

        /** @brief Validates one target's session-scoped owners before registry or renderer access. */
        [[nodiscard]] Result<void> ValidateTargetBasics(const XRExternalRenderTargetDescriptor &target, const XRSessionId &activeSession,
                                                        const XRViewAdmissionPolicy &policy) {
            if (auto view = ValidateXRSessionObject(target.view, activeSession); view.HasError())
                return view;
            if (auto targetIdentity = ValidateXRSessionObject(target.target, activeSession); targetIdentity.HasError())
                return targetIdentity;
            return ValidateTargetStructure(target, policy);
        }

        /** @brief Compares stable target and image slots without generation equality. */
        [[nodiscard]] bool SameImageSlot(const XRSwapchainImageId &left, const XRSwapchainImageId &right) noexcept {
            return left.target.session == right.target.session && left.target.slot.index == right.target.slot.index &&
                   left.slot.index == right.slot.index;
        }

        /** @brief Detects a stale acquired image while keeping unavailable distinct. */
        [[nodiscard]] bool HasReplacedImage(const std::span<const XRSwapchainImageId> acquiredImages,
                                            const XRSwapchainImageId &image) noexcept {
            std::size_t index = 0;
            while (index < acquiredImages.size()) {
                if (const auto &active = acquiredImages[index]; SameImageSlot(active, image) && active != image)
                    return true;
                ++index;
            }
            return false;
        }

        /** @brief Mutable canonical-order cursor local to one bounded validation pass. */
        struct TargetOrderCursor final {
            std::size_t view{};
            XRExternalRenderTargetRole role{XRExternalRenderTargetRole::ProjectionColor};
            bool initialized{};
        };

        /** @brief Advances one color-before-depth, view-major target-order cursor. */
        [[nodiscard]] bool AdvanceTargetOrder(const std::size_t viewOrder, const XRExternalRenderTargetRole role,
                                              TargetOrderCursor &cursor) noexcept {
            if (cursor.initialized && (viewOrder < cursor.view || (viewOrder == cursor.view && role <= cursor.role)))
                return false;
            cursor = {.view = viewOrder, .role = role, .initialized = true};
            return true;
        }

        /** @brief Records exactly one required projection-color binding per view. */
        [[nodiscard]] bool RecordColorBinding(const std::size_t viewOrder, const XRExternalRenderTargetRole role,
                                              std::array<bool, XRHardLimits::MaximumViews> &hasColor) noexcept {
            if (role == XRExternalRenderTargetRole::ProjectionDepth)
                return true;
            if (hasColor[viewOrder])
                return false;
            hasColor[viewOrder] = true;
            return true;
        }

        /** @brief Detects contradictory images or aliased subresources in prior target bindings. */
        [[nodiscard]] bool InvalidImageAliasing(const std::span<const XRExternalRenderTargetDescriptor> priorTargets,
                                                const XRExternalRenderTargetDescriptor &target) noexcept {
            std::size_t index = 0;
            while (index < priorTargets.size()) {
                const auto &prior = priorTargets[index];
                const bool differentImageForTarget = prior.target == target.target && prior.image != target.image;
                if (const bool duplicateImageLayer = prior.image == target.image && prior.arrayLayer == target.arrayLayer;
                    differentImageForTarget || duplicateImageLayer)
                    return true;
                ++index;
            }
            return false;
        }

        /** @brief Validates target structure and canonical view/role ordering. */
        [[nodiscard]] Result<void> ValidateTargets(const XRViewRenderPlanDescriptor &descriptor, const XRSessionId &activeSession,
                                                   const XRViewAdmissionPolicy &policy) {
            TargetOrderCursor orderCursor;
            std::array<bool, XRHardLimits::MaximumViews> hasColor{};
            for (std::size_t index = 0; index < descriptor.targets.size(); ++index) {
                const auto &target = descriptor.targets[index];
                if (auto basics = ValidateTargetBasics(target, activeSession, policy); basics.HasError())
                    return basics;

                const auto viewOrder = FindViewOrder(descriptor.views, target.view);
                if (viewOrder == descriptor.views.size())
                    return Reject<void>(XRErrors::ExternalTargetInvalid);
                if (!AdvanceTargetOrder(viewOrder, target.role, orderCursor) || !RecordColorBinding(viewOrder, target.role, hasColor) ||
                    InvalidImageAliasing(descriptor.targets.first(index), target))
                    return Reject<void>(XRErrors::ExternalTargetInvalid);
            }
            for (std::size_t viewOrder = 0; viewOrder < descriptor.views.size(); ++viewOrder) {
                if (!hasColor[viewOrder])
                    return Reject<void>(XRErrors::ExternalTargetInvalid);
            }
            return Result<void>::Success();
        }

        /** @brief Checks whether an acquired image was already listed earlier in the same bounded set. */
        [[nodiscard]] bool DuplicateAcquiredImage(const std::span<const XRSwapchainImageId> acquiredImages,
                                                  const std::size_t index) noexcept {
            for (std::size_t priorIndex = 0; priorIndex < index; ++priorIndex) {
                if (acquiredImages[priorIndex] == acquiredImages[index])
                    return true;
            }
            return false;
        }

        /** @brief Checks whether a current image belongs to one declared external-target slot. */
        [[nodiscard]] bool KnownTargetSlot(const std::span<const XRExternalRenderTargetDescriptor> targets,
                                           const XRSwapchainImageId &image) noexcept {
            std::size_t index = 0;
            while (index < targets.size()) {
                if (SameImageSlot(targets[index].image, image))
                    return true;
                ++index;
            }
            return false;
        }

        /** @brief Checks whether an exact target image generation is currently acquired. */
        [[nodiscard]] bool ExactImageAvailable(const std::span<const XRSwapchainImageId> acquiredImages,
                                               const XRSwapchainImageId &image) noexcept {
            std::size_t index = 0;
            while (index < acquiredImages.size()) {
                if (acquiredImages[index] == image)
                    return true;
                ++index;
            }
            return false;
        }

        /** @brief Validates one current acquired image against ownership, uniqueness, and target membership. */
        [[nodiscard]] Result<void> ValidateAcquiredImage(const std::span<const XRExternalRenderTargetDescriptor> targets,
                                                         const std::span<const XRSwapchainImageId> acquiredImages,
                                                         const XRSessionId &activeSession, const std::size_t index) {
            const auto &active = acquiredImages[index];
            if (!active.IsValid())
                return Reject<void>(XRErrors::ExternalTargetInvalid);
            if (auto target = ValidateXRSessionObject(active.target, activeSession); target.HasError())
                return target;
            if (DuplicateAcquiredImage(acquiredImages, index) || !KnownTargetSlot(targets, active))
                return Reject<void>(XRErrors::ExternalTargetInvalid);
            return Result<void>::Success();
        }

        /** @brief Validates the complete current acquired-image set against target descriptors. */
        [[nodiscard]] Result<void> ValidateAcquiredImages(const std::span<const XRExternalRenderTargetDescriptor> targets,
                                                          const std::span<const XRSwapchainImageId> acquiredImages,
                                                          const XRSessionId &activeSession) {
            if (acquiredImages.empty())
                return Reject<void>(XRErrors::OperationUnavailable);
            for (std::size_t index = 0; index < acquiredImages.size(); ++index) {
                if (auto image = ValidateAcquiredImage(targets, acquiredImages, activeSession, index); image.HasError())
                    return image;
            }
            for (const auto &target : targets) {
                if (ExactImageAvailable(acquiredImages, target.image))
                    continue;
                return Reject<void>(HasReplacedImage(acquiredImages, target.image) ? XRErrors::IdentityStale
                                                                                   : XRErrors::OperationUnavailable);
            }
            return Result<void>::Success();
        }

        /** @brief Validates session and configuration replacement fences before owner-registry access. */
        [[nodiscard]] Result<void> ValidateConfigurationLiveness(const XRViewConfigurationDescriptor &configuration,
                                                                 const XRSessionId &activeSession,
                                                                 const XRViewConfigurationId &activeConfiguration,
                                                                 const XRViewConfigurationRevision expectedRevision) {
            if (auto session = ValidateXRSession(configuration.session, activeSession); session.HasError())
                return session;
            if (auto identity = ValidateXRSessionObject(configuration.id, activeSession); identity.HasError())
                return identity;
            if (!activeConfiguration.IsValid() || !expectedRevision.IsValid())
                return Reject<void>(XRErrors::ViewPlanInvalid);
            if (configuration.id != activeConfiguration)
                return Reject<void>(XRErrors::IdentityStale);
            if (configuration.revision != expectedRevision)
                return Reject<void>(XRErrors::ViewConfigurationStale);
            return Result<void>::Success();
        }

        /** @brief Validates complete counts against structural and immutable system ceilings. */
        [[nodiscard]] Result<void> ValidatePlanShape(const XRViewRenderPlanDescriptor &descriptor, const std::uint32_t systemMaximumViews) {
            if (!descriptor.configuration.id.IsValid() || !descriptor.configuration.revision.IsValid() ||
                !descriptor.predictedDisplayTime.IsValid() || !ValidConfigurationShape(descriptor.configuration) ||
                descriptor.views.empty() || descriptor.views.size() != descriptor.configuration.viewCount ||
                descriptor.targets.size() < descriptor.views.size())
                return Reject<void>(XRErrors::ViewPlanInvalid);
            if (descriptor.views.size() > XRHardLimits::MaximumViews ||
                descriptor.targets.size() > XRViewRenderHardLimits::MaximumTargets ||
                descriptor.acquiredImages.size() > XRViewRenderHardLimits::MaximumTargets ||
                descriptor.targets.size() > descriptor.views.size() * 2U || systemMaximumViews == 0 ||
                systemMaximumViews > XRHardLimits::MaximumViews || descriptor.views.size() > systemMaximumViews)
                return Reject<void>(XRErrors::CapacityExceeded);
            return Result<void>::Success();
        }

        /** @brief Current owner state used to revalidate an immutable view plan. */
        struct ViewPlanLiveness final {
            XRSessionId session;
            XRViewConfigurationId configuration;
            XRViewConfigurationRevision configurationRevision;
            XRWorldOriginRevision originRevision;
        };

        /** @brief Validates exact session, configuration, revision, origin, and image liveness. */
        [[nodiscard]] Result<void> ValidateLiveness(const XRViewConfigurationDescriptor &configuration,
                                                    const std::span<const XRViewDescriptor> views,
                                                    const std::span<const XRExternalRenderTargetDescriptor> targets,
                                                    const ViewPlanLiveness &active,
                                                    const std::span<const XRSwapchainImageId> acquiredImages) {
            if (auto configurationState =
                    ValidateConfigurationLiveness(configuration, active.session, active.configuration, active.configurationRevision);
                configurationState.HasError())
                return configurationState;
            if (!active.originRevision.IsValid())
                return Reject<void>(XRErrors::ViewPlanInvalid);
            for (const auto &view : views) {
                if (view.pose.source.worldOriginRevision != active.originRevision ||
                    view.pose.target.worldOriginRevision != active.originRevision)
                    return Reject<void>(XRErrors::OriginRevisionStale);
            }
            return ValidateAcquiredImages(targets, acquiredImages, active.session);
        }
    }  // namespace

    /** @copydoc XRViewProjection::IsValid */
    bool XRViewProjection::IsValid() const noexcept {
        return std::isfinite(leftTangent) && std::isfinite(rightTangent) && std::isfinite(downTangent) && std::isfinite(upTangent) &&
               leftTangent <= 0.0F && rightTangent >= 0.0F && downTangent <= 0.0F && upTangent >= 0.0F && leftTangent < rightTangent &&
               downTangent < upTangent;
    }

    /** @copydoc XRRenderRectangle::FitsWithin */
    bool XRRenderRectangle::FitsWithin(const Render::FramebufferExtent allocation) const noexcept {
        return allocation.IsValid() && width > 0 && height > 0 && x <= allocation.width && y <= allocation.height &&
               width <= allocation.width - x && height <= allocation.height - y;
    }

    /** @copydoc XRViewRenderPlan::Create */
    Result<XRViewRenderPlan> XRViewRenderPlan::Create(const XRViewRenderPlanDescriptor &descriptor, const XRSessionId &activeSession,
                                                      const XRViewConfigurationId &activeConfiguration,
                                                      const XRViewConfigurationRevision expectedRevision,
                                                      const XRWorldOriginRevision activeOriginRevision,
                                                      const std::uint32_t systemMaximumViews, const XRViewAdmissionPolicy &policy) {
        if (auto version = RequireXRContractVersion(CurrentXRContractVersion, descriptor.configuration.contractVersion); version.HasError())
            return Result<XRViewRenderPlan>::Failure(version.ErrorValue());
        if (auto shape = ValidatePlanShape(descriptor, systemMaximumViews); shape.HasError())
            return Result<XRViewRenderPlan>::Failure(shape.ErrorValue());
        if (auto configuration =
                ValidateConfigurationLiveness(descriptor.configuration, activeSession, activeConfiguration, expectedRevision);
            configuration.HasError())
            return Result<XRViewRenderPlan>::Failure(configuration.ErrorValue());
        if (auto admission = AdmitConfiguration(descriptor.configuration, policy); admission.HasError())
            return Result<XRViewRenderPlan>::Failure(admission.ErrorValue());
        if (auto views = ValidateViews(descriptor, activeSession, activeOriginRevision); views.HasError())
            return Result<XRViewRenderPlan>::Failure(views.ErrorValue());
        if (!RolesMatchAdmission(descriptor, policy.mode))
            return Reject<XRViewRenderPlan>(XRErrors::OperationIncompatible);
        if (auto targets = ValidateTargets(descriptor, activeSession, policy); targets.HasError())
            return Result<XRViewRenderPlan>::Failure(targets.ErrorValue());
        if (auto images = ValidateAcquiredImages(descriptor.targets, descriptor.acquiredImages, activeSession); images.HasError())
            return Result<XRViewRenderPlan>::Failure(images.ErrorValue());
        return Result<XRViewRenderPlan>::Success(XRViewRenderPlan{descriptor});
    }

    XRViewRenderPlan::XRViewRenderPlan(const XRViewRenderPlanDescriptor &descriptor) noexcept
        : configuration_(descriptor.configuration), predictedDisplayTime_(descriptor.predictedDisplayTime),
          viewCount_(descriptor.views.size()), targetCount_(descriptor.targets.size()) {
        for (std::size_t index = 0; index < viewCount_; ++index)
            views_[index] = descriptor.views[index];
        for (std::size_t index = 0; index < targetCount_; ++index)
            targets_[index] = descriptor.targets[index];
    }

    /** @copydoc XRViewRenderPlan::Configuration */
    const XRViewConfigurationDescriptor &XRViewRenderPlan::Configuration() const noexcept {
        return configuration_;
    }

    /** @copydoc XRViewRenderPlan::PredictedDisplayTime */
    XRRenderPredictionTime XRViewRenderPlan::PredictedDisplayTime() const noexcept {
        return predictedDisplayTime_;
    }

    /** @copydoc XRViewRenderPlan::Views */
    std::span<const XRViewDescriptor> XRViewRenderPlan::Views() const noexcept {
        return {views_.data(), viewCount_};
    }

    /** @copydoc XRViewRenderPlan::Targets */
    std::span<const XRExternalRenderTargetDescriptor> XRViewRenderPlan::Targets() const noexcept {
        return {targets_.data(), targetCount_};
    }

    /** @copydoc ValidateXRViewRenderPlan */
    Result<void> ValidateXRViewRenderPlan(const XRViewRenderPlan &plan, const XRSessionId &activeSession,
                                          const XRViewConfigurationId &activeConfiguration,
                                          const XRViewConfigurationRevision expectedRevision,
                                          const XRWorldOriginRevision activeOriginRevision,
                                          const std::span<const XRSwapchainImageId> acquiredImages) {
        const ViewPlanLiveness active{
            .session = activeSession,
            .configuration = activeConfiguration,
            .configurationRevision = expectedRevision,
            .originRevision = activeOriginRevision,
        };
        return ValidateLiveness(plan.Configuration(), plan.Views(), plan.Targets(), active, acquiredImages);
    }
}  // namespace Horo::XR
