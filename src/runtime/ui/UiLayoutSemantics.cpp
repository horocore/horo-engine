#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiLayout.h"
#include "UiLayoutSemanticsInternal.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>

namespace Horo::Runtime::Ui::UiErrors {
    namespace {
        const ErrorDomainId UiDomain{"horo.runtime_ui"};
    }

    /** @copydoc LayoutConstraintConflict */
    const ErrorCodeDescriptor
        LayoutConstraintConflict{UiDomain,
                                 ErrorCode{"runtime_ui.layout.constraint_conflict"},
                                 ErrorSeverity::Error,
                                 "The Runtime UI layout constraints are ambiguous or cannot be resolved.",
                                 "Remove conflicting anchors, explicit stretch sizing, or non-positive aspect constraints.",
                                 false,
                                 true};
    /** @copydoc LayoutIntrinsicUnavailable */
    const ErrorCodeDescriptor
        LayoutIntrinsicUnavailable{UiDomain,
                                   ErrorCode{"runtime_ui.layout.intrinsic_unavailable"},
                                   ErrorSeverity::Error,
                                   "A required Runtime UI intrinsic metric is unavailable.",
                                   "Publish a revision-matched text or image metric, or declare an optional bounded fallback.",
                                   true,
                                   true};
}  // namespace Horo::Runtime::Ui::UiErrors

namespace Horo::Runtime::Ui {
    namespace {
        using LayoutInternal::CheckedCast;
        using LayoutInternal::MultiplyRatio;
        using LayoutInternal::ResolveLength;
        using LayoutInternal::ResolveOptionalMaximum;

        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool IsExplicit(const UiLength &length) noexcept {
            return length.kind != UiLengthKind::Auto;
        }

        struct AxisRange final {
            std::int32_t minimum{};
            std::int32_t maximum{};
            std::int32_t preferred{};
            bool definite{};
            bool depends{};
            UiLayoutConstraintResult result{UiLayoutConstraintResult::Satisfied};
        };

        [[nodiscard]] Result<AxisRange> ResolveAxisRange(const UiLength &preferred, const UiLength &minimum, const UiLength &maximum,
                                                         const std::int32_t available, const std::int32_t intrinsic) {
            using enum UiLayoutConstraintResult;
            auto minValue = ResolveLength(minimum, available);
            auto maxValue = ResolveOptionalMaximum(maximum, available);
            if (minValue.HasError() || maxValue.HasError())
                return Failure<AxisRange>(UiErrors::LayoutInvalid);
            AxisRange result{minValue.Value(), maxValue.Value(), intrinsic, IsExplicit(preferred), preferred.kind == UiLengthKind::Percent,
                             Satisfied};
            if (result.minimum > result.maximum) {
                result.maximum = result.minimum;
                result.result = Unsatisfiable;
            }
            if (IsExplicit(preferred)) {
                auto explicitValue = ResolveLength(preferred, available);
                if (explicitValue.HasError())
                    return Failure<AxisRange>(UiErrors::LayoutInvalid);
                result.preferred = explicitValue.Value();
            }
            if (result.preferred < result.minimum) {
                result.preferred = result.minimum;
                if (result.result != Unsatisfiable)
                    result.result = Clamped;
            }
            if (result.preferred > result.maximum) {
                result.preferred = result.maximum;
                if (result.result != Unsatisfiable)
                    result.result = Clamped;
            }
            return Result<AxisRange>::Success(result);
        }

        [[nodiscard]] Result<std::int32_t> DeriveWidth(const std::int32_t height, const UiAspectRatio ratio) {
            return MultiplyRatio(height, ratio.width, ratio.height);
        }

        [[nodiscard]] Result<std::int32_t> DeriveHeight(const std::int32_t width, const UiAspectRatio ratio) {
            return MultiplyRatio(width, ratio.height, ratio.width);
        }

        struct IntrinsicState final {
            UiLayoutIntrinsicMeasurement measurement{{}, NoUiBaseline, false, false};
            bool available{};
        };

        [[nodiscard]] Result<UiLayoutIntrinsicMeasurement> QueryIntrinsic(const UiLayoutIntrinsicSource &source,
                                                                          const UiLayoutIntrinsicRequest &request,
                                                                          const UiLayoutIntrinsicProvider *provider) {
            if (provider == nullptr)
                return Failure<UiLayoutIntrinsicMeasurement>(UiErrors::LayoutIntrinsicUnavailable);
            return source.kind == UiLayoutIntrinsicKind::Text ? provider->MeasureText(request) : provider->MeasureImage(request);
        }

        [[nodiscard]] bool HasAnchor(const UiLayoutAnchorAxis &axis) noexcept {
            return axis.start.has_value() || axis.end.has_value();
        }

        [[nodiscard]] bool IsOutOfFlow(const UiLayoutStyle &style) noexcept {
            return style.positioning == UiLayoutPositioning::Absolute || HasAnchor(style.anchors.horizontal) ||
                   HasAnchor(style.anchors.vertical);
        }

        [[nodiscard]] const UiLayoutElementDescriptor *FindDescriptor(const std::span<const UiLayoutElementDescriptor> descriptors,
                                                                      const UiElementHandle element) noexcept {
            const auto found = std::ranges::lower_bound(descriptors, element, {}, &UiLayoutElementDescriptor::element);
            return found != descriptors.end() && found->element == element ? std::to_address(found) : nullptr;
        }

        [[nodiscard]] Result<std::int64_t> CheckedAdd(const std::int64_t left, const std::int64_t right) {
            if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
                (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right))
                return Failure<std::int64_t>(UiErrors::LayoutInvalid);
            return Result<std::int64_t>::Success(left + right);
        }

        [[nodiscard]] Result<UiLogicalExtent> FinalIntrinsicExtent(const std::int64_t width, const std::int64_t height) {
            const auto checkedWidth = CheckedCast(width);
            const auto checkedHeight = CheckedCast(std::max<std::int64_t>(0, height));
            if (checkedWidth.HasError() || checkedHeight.HasError())
                return Failure<UiLogicalExtent>(UiErrors::LayoutInvalid);
            return Result<UiLogicalExtent>::Success({checkedWidth.Value(), checkedHeight.Value()});
        }

        [[nodiscard]] Result<IntrinsicState> AggregateIntrinsic(const std::span<const UiLayoutElementDescriptor> descriptors,
                                                                const std::span<const UiLayoutChildMeasurement> children) {
            if (children.empty())
                return Result<IntrinsicState>::Success({{}, false});
            std::int64_t width{};
            std::int64_t height{};
            bool available{};
            bool dependsOnParentWidth{};
            bool dependsOnParentHeight{};
            for (const auto &child : children) {
                const auto *descriptor = FindDescriptor(descriptors, child.element);
                if (descriptor == nullptr)
                    return Failure<IntrinsicState>(UiErrors::LayoutSourceStale);
                if (IsOutOfFlow(descriptor->style))
                    continue;

                const auto &margin = descriptor->style.margin;
                const auto outerWidth =
                    std::max<std::int64_t>(0, static_cast<std::int64_t>(child.measurement.desired.width) + margin.left + margin.right);
                const auto outerHeight = static_cast<std::int64_t>(child.measurement.desired.height) + margin.top + margin.bottom;
                const auto nextHeight = CheckedAdd(height, outerHeight);
                if (nextHeight.HasError())
                    return Result<IntrinsicState>::Failure(nextHeight.ErrorValue());
                available = true;
                dependsOnParentWidth = dependsOnParentWidth || child.measurement.dependsOnParentWidth;
                dependsOnParentHeight = dependsOnParentHeight || child.measurement.dependsOnParentHeight;
                width = std::max(width, outerWidth);
                height = nextHeight.Value();
            }
            const auto extent = FinalIntrinsicExtent(width, height);
            if (extent.HasError())
                return Result<IntrinsicState>::Failure(extent.ErrorValue());
            return Result<IntrinsicState>::Success(
                {{extent.Value(), NoUiBaseline, dependsOnParentWidth, dependsOnParentHeight}, available});
        }

        [[nodiscard]] Result<IntrinsicState> ResolveIntrinsic(const std::span<const UiLayoutElementDescriptor> descriptors,
                                                              const UiLayoutStyle &style, const UiLayoutIntrinsicSource &source,
                                                              const UiLayoutConstraints &constraints,
                                                              const std::span<const UiLayoutChildMeasurement> children,
                                                              const UiLayoutIntrinsicProvider *provider, const UiElementHandle element) {
            if (source.kind == UiLayoutIntrinsicKind::None)
                return AggregateIntrinsic(descriptors, children);
            const UiLayoutIntrinsicRequest request{element, constraints.maximum, IsExplicit(style.width), IsExplicit(style.height)};
            auto provided = QueryIntrinsic(source, request, provider);
            if (provided.HasError()) {
                if (source.required)
                    return Result<IntrinsicState>::Failure(provided.ErrorValue());
                return Result<IntrinsicState>::Success({{source.fallback, NoUiBaseline, false, false}, true});
            }
            if (!provided.Value().IsValid())
                return Failure<IntrinsicState>(UiErrors::LayoutInvalid);
            return Result<IntrinsicState>::Success({provided.Value(), true});
        }

        [[nodiscard]] UiLayoutConstraintResult CombineConstraintResults(const AxisRange width, const AxisRange height) noexcept {
            using enum UiLayoutConstraintResult;
            if (width.result == Unsatisfiable || height.result == Unsatisfiable)
                return Unsatisfiable;
            return width.result == Clamped || height.result == Clamped ? Clamped : Satisfied;
        }

        [[nodiscard]] Result<void> ApplyDefiniteAspect(const UiAspectRatio ratio, AxisRange &width, AxisRange &height) {
            if (width.definite == height.definite)
                return Result<void>::Success();
            const auto derived = width.definite ? DeriveHeight(width.preferred, ratio) : DeriveWidth(height.preferred, ratio);
            if (derived.HasError())
                return Failure<void>(UiErrors::LayoutConstraintConflict);
            if (width.definite)
                height.preferred = derived.Value();
            else
                width.preferred = derived.Value();
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ApplyIntrinsicAspect(const UiAspectRatio ratio, AxisRange &width, AxisRange &height) {
            if (width.preferred == 0 || height.preferred == 0) {
                width.preferred = 0;
                height.preferred = 0;
                return Result<void>::Success();
            }
            const auto widthFromHeight = DeriveWidth(height.preferred, ratio);
            if (widthFromHeight.HasError())
                return Failure<void>(UiErrors::LayoutConstraintConflict);
            if (widthFromHeight.Value() <= width.preferred) {
                width.preferred = widthFromHeight.Value();
                return Result<void>::Success();
            }
            const auto heightFromWidth = DeriveHeight(width.preferred, ratio);
            if (heightFromWidth.HasError())
                return Failure<void>(UiErrors::LayoutConstraintConflict);
            height.preferred = heightFromWidth.Value();
            return Result<void>::Success();
        }

        [[nodiscard]] bool IsOutsideAspectBounds(const AxisRange &width, const AxisRange &height) noexcept {
            return width.preferred < width.minimum || width.preferred > width.maximum || height.preferred < height.minimum ||
                   height.preferred > height.maximum;
        }

        [[nodiscard]] Result<void> ApplyAspectGeometry(const UiAspectRatio ratio, AxisRange &width, AxisRange &height,
                                                       const bool intrinsicAvailable) {
            if (width.definite != height.definite)
                return ApplyDefiniteAspect(ratio, width, height);
            if (!width.definite && intrinsicAvailable)
                return ApplyIntrinsicAspect(ratio, width, height);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ApplyAspect(const UiAspectRatio ratio, AxisRange &width, AxisRange &height,
                                               const bool intrinsicAvailable, UiLayoutConstraintResult &result) {
            if (!ratio.IsValid() || ratio.width == 0)
                return Result<void>::Success();
            if (auto applied = ApplyAspectGeometry(ratio, width, height, intrinsicAvailable); applied.HasError())
                return applied;
            if (IsOutsideAspectBounds(width, height))
                result = UiLayoutConstraintResult::Unsatisfiable;
            return Result<void>::Success();
        }

        struct MeasurementAxes final {
            AxisRange width;
            AxisRange height;
        };

        [[nodiscard]] Result<MeasurementAxes> ResolveMeasurementAxes(const UiLayoutStyle &style, const UiLayoutConstraints &constraints,
                                                                     const IntrinsicState &intrinsic) {
            const auto width = ResolveAxisRange(style.width, style.minimumWidth, style.maximumWidth, constraints.maximum.width,
                                                intrinsic.available ? intrinsic.measurement.preferred.width : 0);
            const auto height = ResolveAxisRange(style.height, style.minimumHeight, style.maximumHeight, constraints.maximum.height,
                                                 intrinsic.available ? intrinsic.measurement.preferred.height : 0);
            if (width.HasError() || height.HasError())
                return Failure<MeasurementAxes>(UiErrors::LayoutInvalid);
            return Result<MeasurementAxes>::Success({width.Value(), height.Value()});
        }

        [[nodiscard]] Result<UiLayoutMeasurement> FinishMeasurement(const MeasurementAxes &axes,
                                                                    const UiLayoutIntrinsicMeasurement &intrinsic,
                                                                    const UiLayoutConstraints &constraints,
                                                                    UiLayoutConstraintResult result) {
            const auto width = std::clamp(axes.width.preferred, constraints.minimum.width, constraints.maximum.width);
            const auto height = std::clamp(axes.height.preferred, constraints.minimum.height, constraints.maximum.height);
            if (width != axes.width.preferred || height != axes.height.preferred)
                result = UiLayoutConstraintResult::Unsatisfiable;
            return Result<UiLayoutMeasurement>::Success({{width, height},
                                                         axes.width.depends || intrinsic.dependsOnParentWidth,
                                                         axes.height.depends || intrinsic.dependsOnParentHeight,
                                                         result,
                                                         intrinsic.baseline});
        }

        [[nodiscard]] Result<UiLayoutMeasurement> MeasureElement(const std::span<const UiLayoutElementDescriptor> descriptors,
                                                                 const UiLayoutElementDescriptor &descriptor,
                                                                 const UiLayoutMeasureRequest &request,
                                                                 const UiLayoutIntrinsicProvider *provider) {
            auto intrinsic = ResolveIntrinsic(descriptors, descriptor.style, descriptor.intrinsic, request.constraints, request.children,
                                              provider, request.element);
            if (intrinsic.HasError())
                return Result<UiLayoutMeasurement>::Failure(intrinsic.ErrorValue());
            auto axes = ResolveMeasurementAxes(descriptor.style, request.constraints, intrinsic.Value());
            if (axes.HasError())
                return Result<UiLayoutMeasurement>::Failure(axes.ErrorValue());
            auto resolvedAxes = axes.Value();
            auto result = CombineConstraintResults(resolvedAxes.width, resolvedAxes.height);
            if (auto aspect =
                    ApplyAspect(descriptor.style.aspectRatio, resolvedAxes.width, resolvedAxes.height, intrinsic.Value().available, result);
                aspect.HasError())
                return Result<UiLayoutMeasurement>::Failure(aspect.ErrorValue());
            return FinishMeasurement(resolvedAxes, intrinsic.Value().measurement, request.constraints, result);
        }
    }  // namespace

    /** @copydoc UiDeclarativeLayoutEvaluator::Create */
    Result<UiDeclarativeLayoutEvaluator> UiDeclarativeLayoutEvaluator::Create(const std::span<const UiLayoutElementDescriptor> descriptors,
                                                                              const UiLayoutIntrinsicProvider *intrinsic) {
        if (descriptors.empty())
            return Failure<UiDeclarativeLayoutEvaluator>(UiErrors::LayoutInvalid);
        bool requiresProvider = false;
        for (std::size_t index = 0; index < descriptors.size(); ++index) {
            if (!descriptors[index].IsValid())
                return Failure<UiDeclarativeLayoutEvaluator>(UiErrors::LayoutInvalid);
            if (index > 0 && descriptors[index - 1].element >= descriptors[index].element)
                return Failure<UiDeclarativeLayoutEvaluator>(UiErrors::LayoutInvalid);
            requiresProvider = requiresProvider ||
                               (descriptors[index].intrinsic.kind != UiLayoutIntrinsicKind::None && descriptors[index].intrinsic.required);
        }
        if (requiresProvider && intrinsic == nullptr)
            return Failure<UiDeclarativeLayoutEvaluator>(UiErrors::LayoutIntrinsicUnavailable);
        return Result<UiDeclarativeLayoutEvaluator>::Success(UiDeclarativeLayoutEvaluator{descriptors, intrinsic});
    }

    /** @copydoc UiDeclarativeLayoutEvaluator::Find */
    const UiLayoutElementDescriptor *UiDeclarativeLayoutEvaluator::Find(const UiElementHandle element) const noexcept {
        const auto found = std::ranges::lower_bound(descriptors_, element, {}, &UiLayoutElementDescriptor::element);
        return found != descriptors_.end() && found->element == element ? std::to_address(found) : nullptr;
    }

    /** @copydoc UiDeclarativeLayoutEvaluator::ResolveChildConstraints */
    Result<void> UiDeclarativeLayoutEvaluator::ResolveChildConstraints(const UiLayoutChildConstraintRequest &request,
                                                                       const std::span<UiLayoutConstraints> output) const {
        if (!request.element.IsValid() || !request.constraints.IsValid() || output.size() != request.children.size())
            return Failure(UiErrors::LayoutInvalid);
        return LayoutInternal::ResolveChildConstraints(descriptors_, request, output);
    }

    /** @copydoc UiDeclarativeLayoutEvaluator::Measure */
    Result<UiLayoutMeasurement> UiDeclarativeLayoutEvaluator::Measure(const UiLayoutMeasureRequest &request) const {
        if (!request.element.IsValid() || !request.constraints.IsValid())
            return Failure<UiLayoutMeasurement>(UiErrors::LayoutInvalid);
        const auto *descriptor = Find(request.element);
        if (descriptor == nullptr)
            return Failure<UiLayoutMeasurement>(UiErrors::LayoutSourceStale);
        return MeasureElement(descriptors_, *descriptor, request, intrinsic_);
    }

    /** @copydoc UiDeclarativeLayoutEvaluator::Arrange */
    Result<UiLayoutArrangement> UiDeclarativeLayoutEvaluator::Arrange(const UiLayoutArrangeRequest &request,
                                                                      const std::span<UiLogicalRect> childContent) const {
        if (!request.element.IsValid() || !request.assignedContent.IsValid() || childContent.size() != request.children.size())
            return Failure<UiLayoutArrangement>(UiErrors::LayoutInvalid);
        const auto *descriptor = Find(request.element);
        if (descriptor == nullptr)
            return Failure<UiLayoutArrangement>(UiErrors::LayoutSourceStale);
        return LayoutInternal::Arrange(descriptors_, *descriptor, request, childContent);
    }
}  // namespace Horo::Runtime::Ui
