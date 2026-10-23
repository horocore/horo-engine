#include "Horo/Runtime/Ui/UiStyleAsset.h"
#include "RuntimeUiPaintProjectionInternal.h"

namespace Horo::Application::Internal {
    using namespace Runtime::Ui;
    using PaintProjectionDetail::Row;

    namespace {
        /** @brief Resolves paint from the actual computed publication without repeating cascade or layout. */
        Result<UiDrawCommand> ComputedPaint(const Row &row, const UiAnimationFrameLease &frame) {
            auto style = ProjectUiElementStyle(*row.source, row.element);
            auto layout = ProjectUiElementLayout(*row.source, row.element);
            if (style.HasError())
                return Result<UiDrawCommand>::Failure(style.ErrorValue());
            if (layout.HasError())
                return Result<UiDrawCommand>::Failure(layout.ErrorValue());
            UiThemeSurfaceInput input{.style = style.Value(),
                                      .layout = layout.Value().style,
                                      .intrinsic = layout.Value().intrinsic,
                                      .fill = {0, 0, 0, 0},
                                      .visible = !row.source->presentation || row.source->presentation->state.visible};
            if (const auto *kind = UiElementCatalog::Find(row.source->type.Value()); kind && kind->surface) {
                auto canonical = ProjectUiElementSurface(*row.source, row.element);
                if (canonical.HasError())
                    return Result<UiDrawCommand>::Failure(canonical.ErrorValue());
                input = std::move(canonical).Value();
            }
            auto properties = CoreUiStyleSurfaceProperties();
            // Layout has already been published by UiAnimationOwner; this projection consumes paint only.
            properties.width = {};
            properties.height = {};
            properties.padding = {};
            auto surface = ProjectUiThemeSurface(frame.Styles(), input, properties);
            if (surface.HasError())
                return Result<UiDrawCommand>::Failure(surface.ErrorValue());
            return Result<UiDrawCommand>::Success(surface.Value().paint);
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
            masks.push_back({paint.rect, input.resource, paint.transform, input.shape, row.mask, input.cornerRadii});
            row.mask = static_cast<std::uint32_t>(masks.size() - 1);
        }
        if (row.isolate) {
            if (groups.size() == descriptor.limits.groups)
                return Result<void>::Failure(MakeError(UiErrors::CapacityExceeded));
            groups.push_back(
                {row.element, paint.rect, row.firstCommand, 0, row.group, paint.opacity, paint.transform, paint.clip, row.mask});
            row.group = static_cast<std::uint32_t>(groups.size() - 1);
        }
        return Result<void>::Success();
    }

    /** @copydoc RuntimeUiPaintProjection::Storage::PaintRow */
    Result<void> RuntimeUiPaintProjection::Storage::PaintRow(const std::size_t index, const UiAnimationFrameLease &frame) {
        auto &row = rows[index];
        auto computed = ComputedPaint(row, frame);
        const auto layout = frame.Layout().Get(row.element);
        if (computed.HasError())
            return Result<void>::Failure(computed.ErrorValue());
        if (layout.HasError())
            return Result<void>::Failure(layout.ErrorValue());
        auto paint = std::move(computed).Value();
        paint.rect = layout.Value().arrangement.borderBox;
        paint.transform = static_cast<std::uint32_t>(index);
        UiLogicalTransform transform;
        if (const auto *clipping = frame.Clipping()) {
            const auto clip = clipping->Get(row.element);
            if (clip.HasError())
                return Result<void>::Failure(clip.ErrorValue());
            transform.values[4] = static_cast<float>(clip.Value().scrollTranslation.x);
            transform.values[5] = static_cast<float>(clip.Value().scrollTranslation.y);
            paint.clip = clip.Value().clip;
        }
        transforms.push_back(transform);
        row.firstCommand = static_cast<std::uint32_t>(commands.size());
        const auto scopes = BeginScopes(index, paint);
        if (scopes.HasError())
            return scopes;
        paint.mask = row.mask;
        paint.group = row.group;
        if (row.isolate)
            paint.opacity = 1.0F;
        if (const auto *kind = UiElementCatalog::Find(row.source->type.Value()); kind && kind->surface)
            commands.push_back(paint);
        if (row.component != NoUiRenderIndex) {
            paint.payload = declaration.components[row.component].payload;
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
    Result<void> RuntimeUiPaintProjection::Storage::Build(const UiAnimationFrameLease &frame) {
        // Worst-case capacity is checked before any push; ordinary successful frames cannot grow a vector.
        if (rows.size() + declaration.components.size() > descriptor.limits.commands)
            return Result<void>::Failure(MakeError(UiErrors::CapacityExceeded));
        commands.clear();
        clips.clear();
        masks.clear();
        groups.clear();
        transforms.clear();
        if (const auto *clipping = frame.Clipping()) {
            const auto &source = frame.Layout().Descriptor();
            const auto &derived = clipping->Descriptor();
            if (source.instance != derived.instance || source.canvas != derived.canvas || source.sources != derived.sources ||
                source.interaction != derived.interaction || clipping->Clips().size() > descriptor.limits.clips)
                return Result<void>::Failure(MakeError(UiErrors::RevisionStale));
            for (const auto &clip : clipping->Clips())
                clips.push_back({clip.rect, clip.parent});
        }
        for (std::size_t index = 0; index < rows.size(); ++index) {
            const auto painted = PaintRow(index, frame);
            if (painted.HasError())
                return painted;
        }
        return CloseGroups();
    }
}  // namespace Horo::Application::Internal
