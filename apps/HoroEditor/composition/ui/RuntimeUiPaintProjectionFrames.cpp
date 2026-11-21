#include "Horo/Runtime/Ui/UiStyleAsset.h"
#include "RuntimeUiPaintProjectionInternal.h"

#include <algorithm>

namespace Horo::Application::Internal {
    using namespace Runtime::Ui;
    using PaintProjectionDetail::Row;

    namespace {
        /** @brief Resolves paint from the actual computed publication without repeating cascade or layout. */
        Result<UiThemeSurfaceProjection> ComputedPaint(const Row &row, const PaintProjectionDetail::Presentation &frame) {
            auto style = ProjectUiElementStyle(*row.source, row.element);
            auto layout = ProjectUiElementLayout(*row.source, row.element);
            if (style.HasError())
                return Result<UiThemeSurfaceProjection>::Failure(style.ErrorValue());
            if (layout.HasError())
                return Result<UiThemeSurfaceProjection>::Failure(layout.ErrorValue());
            UiThemeSurfaceInput input{.style = style.Value(),
                                      .layout = layout.Value().style,
                                      .intrinsic = layout.Value().intrinsic,
                                      .fill = {0, 0, 0, 0},
                                      .visible = !row.source->presentation || row.source->presentation->state.visible};
            if (const auto *kind = UiElementCatalog::Find(row.source->type.Value()); kind && kind->surface) {
                auto canonical = ProjectUiElementSurface(*row.source, row.element);
                if (canonical.HasError())
                    return Result<UiThemeSurfaceProjection>::Failure(canonical.ErrorValue());
                input = std::move(canonical).Value();
            }
            auto properties = CoreUiStyleSurfaceProperties();
            // Layout has already been published by UiAnimationOwner; this projection consumes paint only.
            properties.width = {};
            properties.height = {};
            properties.padding = {};
            return ProjectUiThemeSurface(frame.Styles(), input, properties);
        }

        /** @brief Intersects two validated clip rectangles using wide endpoint arithmetic. */
        UiLogicalRect IntersectHitClips(const UiLogicalRect &first, const UiLogicalRect &second) noexcept {
            const auto left = std::max(first.origin.x, second.origin.x);
            const auto top = std::max(first.origin.y, second.origin.y);
            const auto right =
                std::min(std::int64_t{first.origin.x} + first.extent.width, std::int64_t{second.origin.x} + second.extent.width);
            const auto bottom =
                std::min(std::int64_t{first.origin.y} + first.extent.height, std::int64_t{second.origin.y} + second.extent.height);
            return {{left, top},
                    {static_cast<std::int32_t>(std::max<std::int64_t>(0, right - left)),
                     static_cast<std::int32_t>(std::max<std::int64_t>(0, bottom - top))}};
        }
    }  // namespace

    /** @copydoc RuntimeUiPaintProjection::Storage::BeginScopes */
    Result<void> RuntimeUiPaintProjection::Storage::BeginScopes(const std::size_t index, const UiDrawCommand &paint) {
        auto &row = rows[index];
        row.mask = row.parent == NoUiRenderIndex ? NoUiRenderIndex : rows[row.parent].mask;
        row.group = row.parent == NoUiRenderIndex ? NoUiRenderIndex : rows[row.parent].group;
        if (row.authoredMask != NoUiRenderIndex) {
            if (masks.size() == descriptor.limits.masks)
                return Result<void>::Failure(MakeError(UiErrors::CapacityExceeded));
            const auto &input = declaration.masks[row.authoredMask];
            masks.emplace_back(paint.rect, input.resource, paint.transform, input.shape, row.mask, input.cornerRadii);
            row.mask = static_cast<std::uint32_t>(masks.size() - 1);
        }
        if (row.isolate) {
            if (groups.size() == descriptor.limits.groups)
                return Result<void>::Failure(MakeError(UiErrors::CapacityExceeded));
            groups.emplace_back(row.element, paint.rect, row.firstCommand, 0, row.group, paint.opacity, paint.transform, paint.clip,
                                row.mask);
            row.group = static_cast<std::uint32_t>(groups.size() - 1);
        }
        return Result<void>::Success();
    }

    /** @copydoc RuntimeUiPaintProjection::Storage::PaintRow */
    Result<void> RuntimeUiPaintProjection::Storage::PaintRow(const std::size_t index, const PaintProjectionDetail::Presentation &frame) {
        auto &row = rows[index];
        auto computed = ComputedPaint(row, frame);
        const auto layout = frame.Layout().Get(row.element);
        if (computed.HasError())
            return Result<void>::Failure(computed.ErrorValue());
        if (layout.HasError())
            return Result<void>::Failure(layout.ErrorValue());
        auto surface = std::move(computed).Value();
        auto &paint = surface.paint;
        paint.rect = layout.Value().arrangement.borderBox;
        paint.transform = static_cast<std::uint32_t>(index);
        UiLogicalTransform transform;
        bool scrollable = false;
        if (const auto *clipping = frame.Clipping()) {
            const auto clip = clipping->Get(row.element);
            if (clip.HasError())
                return Result<void>::Failure(clip.ErrorValue());
            transform.values[4] = static_cast<float>(clip.Value().scrollTranslation.x);
            transform.values[5] = static_cast<float>(clip.Value().scrollTranslation.y);
            paint.clip = clip.Value().clip;
            scrollable = clip.Value().scroll != NoUiLayoutScroll;
        }
        transforms.push_back(transform);
        row.firstCommand = static_cast<std::uint32_t>(commands.size());
        if (const auto scopes = BeginScopes(index, paint); scopes.HasError())
            return scopes;
        paint.mask = row.mask;
        paint.group = row.group;
        const bool visible = paint.opacity > 0.0F;
        const bool structural = UiElementCatalog::Find(row.source->type.Value())->surface;
        if (hitStore) {
            const auto *parent = row.parent == NoUiRenderIndex ? nullptr : &rows[row.parent];
            row.visibleForInput = visible && (!parent || parent->visibleForInput);
            row.enabledForInput =
                !surface.state.Contains(UiVisualStateMask{UiVisualState::Disabled}) && (!parent || parent->enabledForInput);
            const bool clipped = paint.clip != NoUiRenderIndex;
            hitElements.push_back({.element = row.element,
                                   .transform = transform,
                                   .clip = clipped ? hitClips[paint.clip] : UiLogicalRect{},
                                   .hasClip = clipped,
                                   .visible = row.visibleForInput,
                                   .enabled = row.enabledForInput && (structural || row.stateIndicators || scrollable)});
        }
        if (row.isolate)
            paint.opacity = 1.0F;
        if (structural)
            commands.push_back(paint);
        if (row.component != NoUiRenderIndex) {
            paint.payload = declaration.components[row.component].payload;
            commands.push_back(paint);
        }
        if (row.stateIndicators) {
            // The native program pins this role across frames. An inactive or hidden slot realizes no geometry.
            paint.payload = UiVisualIndicatorDraw{surface.state};
            paint.opacity = visible ? 1.0F : 0.0F;
            commands.push_back(paint);
        }
        return Result<void>::Success();
    }

    /** @copydoc RuntimeUiPaintProjection::Storage::CloseGroups */
    Result<void> RuntimeUiPaintProjection::Storage::CloseGroups() {
        for (const auto &row : rows) {
            if (!row.isolate)
                continue;
            const auto end = row.end == rows.size() ? commands.size() : rows[row.end].firstCommand;
            if (end <= row.firstCommand)
                return Result<void>::Failure(MakeError(UiErrors::HandleMalformed));
            groups[row.group].commandCount = static_cast<std::uint32_t>(end - row.firstCommand);
        }
        return Result<void>::Success();
    }

    /** @copydoc RuntimeUiPaintProjection::Storage::Build */
    Result<void> RuntimeUiPaintProjection::Storage::Build(const PaintProjectionDetail::Presentation &frame) {
        // Admission reserved exact immutable roles, including dormant state indicators.
        commands.clear();
        clips.clear();
        masks.clear();
        groups.clear();
        transforms.clear();
        hitElements.clear();
        hitClips.clear();
        if (const auto *clipping = frame.Clipping()) {
            const auto &layoutSource = frame.Layout().Descriptor();
            if (const auto &derived = clipping->Descriptor();
                layoutSource.instance != derived.instance || layoutSource.canvas != derived.canvas ||
                layoutSource.sources != derived.sources || layoutSource.interaction != derived.interaction ||
                clipping->Clips().size() > descriptor.limits.clips)
                return Result<void>::Failure(MakeError(UiErrors::RevisionStale));
            for (const auto &clip : clipping->Clips()) {
                clips.push_back({clip.rect, clip.parent});
                if (hitStore)
                    hitClips.push_back(clip.parent == NoUiLayoutClip ? clip.rect : IntersectHitClips(clip.rect, hitClips[clip.parent]));
            }
        }
        for (std::size_t index = 0; index < rows.size(); ++index) {
            const auto painted = PaintRow(index, frame);
            if (painted.HasError())
                return painted;
        }
        return CloseGroups();
    }
}  // namespace Horo::Application::Internal
