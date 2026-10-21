#include "Horo/Runtime/Render/MultiviewSceneExtraction.h"

#include "Horo/Runtime/Render/MultiviewExtractionErrors.h"
#include "Horo/Runtime/Render/RenderCapabilities.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace Horo::Render {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Reject(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Validates exact owner fences before candidate payload access. */
        [[nodiscard]] Result<void> ValidatePublication(const MultiviewPublication &candidate, const MultiviewPublication &active) {
            if (!candidate.IsValid() || !active.IsValid())
                return Reject<void>(MultiviewExtractionErrors::InvalidInput);
            if (candidate != active)
                return Reject<void>(MultiviewExtractionErrors::StalePublication);
            return Result<void>::Success();
        }

        /** @brief Resolves explicit optimization policy without inventing selected-backend support. */
        [[nodiscard]] Result<MultiviewExecutionMode> SelectMode(const MultiviewExecutionPolicy policy,
                                                                const MultiviewCapabilities &capabilities) {
            if (policy != MultiviewExecutionPolicy::Multipass && policy != MultiviewExecutionPolicy::PreferInstanced &&
                policy != MultiviewExecutionPolicy::RequireInstanced)
                return Reject<MultiviewExecutionMode>(MultiviewExtractionErrors::InvalidInput);
            const bool instanced = capabilities.indexedShaders && capabilities.instancedTargetRouting;
            if (policy != MultiviewExecutionPolicy::Multipass && instanced)
                return Result<MultiviewExecutionMode>::Success(MultiviewExecutionMode::Instanced);
            if (policy == MultiviewExecutionPolicy::RequireInstanced || !capabilities.multipass)
                return Reject<MultiviewExecutionMode>(MultiviewExtractionErrors::Unsupported);
            return Result<MultiviewExecutionMode>::Success(MultiviewExecutionMode::Multipass);
        }

        /** @brief Applies a column-major homogeneous transform without perspective division. */
        [[nodiscard]] Math::Vec4 ClipCorner(const Math::Mat4 &matrix, const Math::Vec3 point) noexcept {
            return {matrix.At(0, 0) * point.x + matrix.At(0, 1) * point.y + matrix.At(0, 2) * point.z + matrix.At(0, 3),
                    matrix.At(1, 0) * point.x + matrix.At(1, 1) * point.y + matrix.At(1, 2) * point.z + matrix.At(1, 3),
                    matrix.At(2, 0) * point.x + matrix.At(2, 1) * point.y + matrix.At(2, 2) * point.z + matrix.At(2, 3),
                    matrix.At(3, 0) * point.x + matrix.At(3, 1) * point.y + matrix.At(3, 2) * point.z + matrix.At(3, 3)};
        }

        /** @brief Returns the six planes outside one finite homogeneous corner. */
        [[nodiscard]] std::uint32_t OutsidePlanes(const Math::Vec4 clip, const Math::ClipDepthRange depthRange) noexcept {
            const float nearBound = depthRange == Math::ClipDepthRange::ZeroToOne ? 0.0F : -clip.w;
            return (clip.x < -clip.w ? 1U : 0U) | (clip.x > clip.w ? 2U : 0U) | (clip.y < -clip.w ? 4U : 0U) | (clip.y > clip.w ? 8U : 0U) |
                   (clip.z < nearBound ? 16U : 0U) | (clip.z > clip.w ? 32U : 0U);
        }

        /** @brief Conservative six-plane box test; straddling and near-plane-touching geometry remains visible. */
        [[nodiscard]] Result<bool> Visible(const MultiviewSceneInstance &instance, const MultiviewSceneView &view) {
            const Math::Mat4 transform = Math::Multiply(view.worldToClip, instance.localToWorld);
            if (!Math::IsFinite(transform))
                return Reject<bool>(MultiviewExtractionErrors::InvalidInput);
            std::uint32_t outsideEveryCorner = 0x3FU;
            for (std::uint32_t corner = 0; corner < 8; ++corner) {
                const auto &bounds = instance.localBounds;
                const Math::Vec3 point{(corner & 1U) ? bounds.maximum.x : bounds.minimum.x,
                                       (corner & 2U) ? bounds.maximum.y : bounds.minimum.y,
                                       (corner & 4U) ? bounds.maximum.z : bounds.minimum.z};
                const Math::Vec4 clip = ClipCorner(transform, point);
                if (!Math::IsFinite(clip))
                    return Reject<bool>(MultiviewExtractionErrors::InvalidInput);
                outsideEveryCorner &= OutsidePlanes(clip, view.depthRange);
            }
            return Result<bool>::Success(outsideEveryCorner == 0);
        }

        /** @brief Computes one source instance's complete visibility without per-view storage. */
        [[nodiscard]] Result<std::uint32_t> VisibilityMask(const MultiviewSceneInstance &instance,
                                                           const std::span<const MultiviewSceneView> views) {
            std::uint32_t mask = 0;
            for (const auto &view : views) {
                const auto visible = Visible(instance, view);
                if (visible.HasError())
                    return Result<std::uint32_t>::Failure(visible.ErrorValue());
                if (visible.Value())
                    mask |= std::uint32_t{1} << view.index;
            }
            return Result<std::uint32_t>::Success(mask);
        }

        /** @brief Checks a finite invertible view and its explicit clip-depth convention. */
        [[nodiscard]] bool ValidView(const MultiviewSceneView &view) {
            return Math::IsFinite(view.worldToClip) &&
                   (view.depthRange == Math::ClipDepthRange::ZeroToOne || view.depthRange == Math::ClipDepthRange::NegativeOneToOne) &&
                   Math::TryInverse(view.worldToClip).HasValue();
        }

        /** @brief Keeps scene model transforms affine rather than accepting projective world geometry. */
        [[nodiscard]] bool FiniteAffine(const Math::Mat4 &matrix) noexcept {
            return Math::IsFinite(matrix) && matrix.At(3, 0) == 0.0F && matrix.At(3, 1) == 0.0F && matrix.At(3, 2) == 0.0F &&
                   matrix.At(3, 3) == 1.0F;
        }

        /** @brief Checks one instance's renderer namespace and geometric invariants. */
        [[nodiscard]] bool ValidInstance(const MultiviewSceneInstance &instance, const RenderResourceOwnerId owner) noexcept {
            return instance.mesh.IsValid() && instance.material.IsValid() && instance.mesh.owner == instance.material.owner &&
                   instance.mesh.owner == owner && FiniteAffine(instance.localToWorld) && instance.localBounds.IsValid();
        }

        /** @brief Checks bounded source metadata and transforms before culling can publish a presentation. */
        [[nodiscard]] Result<void> ValidateSources(const std::span<const MultiviewSceneView> views,
                                                   const std::span<const MultiviewSceneInstance> instances) {
            for (std::size_t index = 0; index < views.size(); ++index) {
                const auto &view = views[index];
                if (view.index != index || !ValidView(view))
                    return Reject<void>(MultiviewExtractionErrors::InvalidInput);
            }
            for (const auto &instance : instances) {
                if (!ValidInstance(instance, instances.front().mesh.owner))
                    return Reject<void>(MultiviewExtractionErrors::InvalidInput);
            }
            return Result<void>::Success();
        }

        /** @brief Charges candidate shape and culling work before any payload access. */
        [[nodiscard]] Result<void> ValidateShape(const std::size_t viewCount, const std::size_t instanceCount,
                                                 const MultiviewExtractionLimits &limits) {
            if (!limits.IsValid() || viewCount == 0)
                return Reject<void>(MultiviewExtractionErrors::InvalidInput);
            if (viewCount > limits.maximumViews || instanceCount > limits.maximumInstances ||
                instanceCount > limits.maximumCullTests / viewCount)
                return Reject<void>(MultiviewExtractionErrors::CapacityExceeded);
            return Result<void>::Success();
        }

        /** @brief Admits current owner evidence, finite source shape and explicit effective-backend policy. */
        [[nodiscard]] Result<MultiviewExecutionMode> AdmitExtraction(const MultiviewPublication &publication,
                                                                     const MultiviewPublication &active, const std::size_t viewCount,
                                                                     const std::size_t instanceCount,
                                                                     const MultiviewCapabilities &capabilities,
                                                                     const MultiviewExecutionPolicy policy,
                                                                     const MultiviewExtractionLimits &limits) {
            if (const auto live = ValidatePublication(publication, active); live.HasError())
                return Result<MultiviewExecutionMode>::Failure(live.ErrorValue());
            if (const auto shape = ValidateShape(viewCount, instanceCount, limits); shape.HasError())
                return Result<MultiviewExecutionMode>::Failure(shape.ErrorValue());
            if (capabilities.maximumViews == 0 || capabilities.maximumViews > MultiviewExtractionLimits::HardMaximumViews ||
                viewCount > capabilities.maximumViews)
                return Reject<MultiviewExecutionMode>(MultiviewExtractionErrors::Unsupported);
            return SelectMode(policy, capabilities);
        }
    }  // namespace

    /** @copydoc ResolveMultiviewCapabilities */
    MultiviewCapabilities ResolveMultiviewCapabilities(const RenderCapabilitySnapshot &capabilities) noexcept {
        if (!capabilities.IsValid())
            return {};
        const bool indexed = capabilities.features.Supports(RenderCapability::ViewIndexedShaders);
        return {capabilities.limits.maxMultiviewViews, indexed, indexed,
                indexed && capabilities.features.Supports(RenderCapability::InstancedViewRouting)};
    }

    /** @copydoc MultiviewPublication::IsValid */
    bool MultiviewPublication::IsValid() const noexcept {
        return sceneIncarnation != 0 && frame != 0 && originRevision != 0 && viewRevision != 0 && deviceIncarnation != 0 &&
               capabilityRevision != 0;
    }

    /** @copydoc MultiviewExtractionLimits::IsValid */
    bool MultiviewExtractionLimits::IsValid() const noexcept {
        constexpr auto maximumWork = HardMaximumViews * HardMaximumInstances;
        return maximumViews > 0 && maximumViews <= HardMaximumViews && maximumInstances > 0 && maximumInstances <= HardMaximumInstances &&
               maximumCullTests > 0 && maximumCullTests <= maximumWork && maximumExpandedDraws > 0 && maximumExpandedDraws <= maximumWork;
    }

    /** @copydoc MultiviewSceneSnapshot::Extract */
    Result<MultiviewSceneSnapshot> MultiviewSceneSnapshot::Extract(
        const MultiviewPublication &publication, const MultiviewPublication &active, const std::span<const MultiviewSceneView> views,
        const std::span<const MultiviewSceneInstance> instances, const MultiviewCapabilities &capabilities,
        const MultiviewExecutionPolicy policy, const MultiviewExtractionLimits &limits) {
        const auto mode = AdmitExtraction(publication, active, views.size(), instances.size(), capabilities, policy, limits);
        if (mode.HasError())
            return Result<MultiviewSceneSnapshot>::Failure(mode.ErrorValue());
        if (const auto valid = ValidateSources(views, instances); valid.HasError())
            return Result<MultiviewSceneSnapshot>::Failure(valid.ErrorValue());
        MultiviewSceneSnapshot snapshot;
        snapshot.publication_ = publication;
        snapshot.mode_ = mode.Value();
        snapshot.viewCount_ = static_cast<std::uint32_t>(views.size());
        snapshot.instanceCount_ = static_cast<std::uint32_t>(instances.size());
        std::ranges::copy(views, snapshot.views_.begin());
        std::ranges::copy(instances, snapshot.instances_.begin());
        for (std::uint32_t index = 0; index < snapshot.instanceCount_; ++index) {
            const auto visibility = VisibilityMask(instances[index], views);
            if (visibility.HasError())
                return Result<MultiviewSceneSnapshot>::Failure(visibility.ErrorValue());
            const auto mask = visibility.Value();
            const auto count = static_cast<std::uint32_t>(std::popcount(mask));
            if (count > limits.maximumExpandedDraws - snapshot.expandedDrawCount_)
                return Reject<MultiviewSceneSnapshot>(MultiviewExtractionErrors::CapacityExceeded);
            snapshot.expandedDrawCount_ += count;
            if (mask != 0)
                snapshot.draws_[snapshot.drawCount_++] = {index, mask};
        }
        return Result<MultiviewSceneSnapshot>::Success(std::move(snapshot));
    }

    /** @copydoc MultiviewSceneSnapshot::Publication */
    const MultiviewPublication &MultiviewSceneSnapshot::Publication() const noexcept {
        return publication_;
    }

    /** @copydoc MultiviewSceneSnapshot::Mode */
    MultiviewExecutionMode MultiviewSceneSnapshot::Mode() const noexcept {
        return mode_;
    }

    /** @copydoc MultiviewSceneSnapshot::Views */
    std::span<const MultiviewSceneView> MultiviewSceneSnapshot::Views() const noexcept {
        return {views_.data(), viewCount_};
    }

    /** @copydoc MultiviewSceneSnapshot::Instances */
    std::span<const MultiviewSceneInstance> MultiviewSceneSnapshot::Instances() const noexcept {
        return {instances_.data(), instanceCount_};
    }

    /** @copydoc MultiviewSceneSnapshot::Draws */
    std::span<const MultiviewDraw> MultiviewSceneSnapshot::Draws() const noexcept {
        return {draws_.data(), drawCount_};
    }

    /** @copydoc MultiviewSceneSnapshot::ExpandedDrawCount */
    std::uint32_t MultiviewSceneSnapshot::ExpandedDrawCount() const noexcept {
        return expandedDrawCount_;
    }

    /** @copydoc MultiviewSceneSnapshot::Validate */
    Result<void> MultiviewSceneSnapshot::Validate(const MultiviewPublication &active) const {
        return ValidatePublication(publication_, active);
    }

    /** @copydoc MultiviewSceneSnapshot::Invocation */
    Result<MultiviewShaderInvocation> MultiviewSceneSnapshot::Invocation(const std::uint32_t drawIndex, std::uint32_t invocation,
                                                                         const MultiviewPublication &active) const {
        if (const auto live = Validate(active); live.HasError())
            return Result<MultiviewShaderInvocation>::Failure(live.ErrorValue());
        if (drawIndex >= drawCount_)
            return Reject<MultiviewShaderInvocation>(MultiviewExtractionErrors::InvalidInput);
        const auto &draw = draws_[drawIndex];
        for (std::uint32_t view = 0; view < viewCount_; ++view) {
            if ((draw.visibleViews & (std::uint32_t{1} << view)) == 0)
                continue;
            if (invocation == 0)
                return Result<MultiviewShaderInvocation>::Success({draw.instanceIndex, view});
            --invocation;
        }
        return Reject<MultiviewShaderInvocation>(MultiviewExtractionErrors::InvalidInput);
    }
}  // namespace Horo::Render
