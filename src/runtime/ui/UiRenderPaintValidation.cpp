#include "UiRenderPaintValidation.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace Horo::Runtime::Ui::PaintInternal {
    namespace {
        Result<void> Invalid() {
            return Result<void>::Failure(MakeError(UiErrors::RenderSnapshotInvalid));
        }

        bool OptionalIndex(const std::uint32_t index, const std::size_t count) noexcept {
            return index == NoUiRenderIndex || index < count;
        }

        /** @brief Canonical radii cannot overlap; native encoders do not invent backend-specific normalization. */
        bool ValidRadii(const UiMask &mask) noexcept {
            const auto &r = mask.cornerRadii;
            if (std::ranges::any_of(r, [&mask](const auto radius) {
                return radius < 0 || (mask.shape != UiMaskShape::RoundedRectangle && radius != 0);
            }))
                return false;
            const auto fits = [](const auto first, const auto second, const auto extent) {
                return static_cast<std::int64_t>(first) + second <= extent;
            };
            return fits(r[0], r[1], mask.rect.extent.width) && fits(r[3], r[2], mask.rect.extent.width) &&
                   fits(r[0], r[3], mask.rect.extent.height) && fits(r[1], r[2], mask.rect.extent.height);
        }

        /** @brief Parent-first ancestry gives finite traversal without recursion or scratch allocation. */
        Result<void> ValidateMasks(const UiRenderProjection &projection) {
            for (std::uint32_t index = 0; index < projection.masks.size(); ++index) {
                const auto &mask = projection.masks[index];
                if (!mask.rect.extent.IsValid() || mask.transform >= projection.transforms.size() ||
                    (mask.parent != NoUiRenderIndex && mask.parent >= index))
                    return Invalid();
                std::uint32_t depth = 1;
                for (auto parent = mask.parent; parent != NoUiRenderIndex; parent = projection.masks[parent].parent)
                    if (++depth > MaximumUiRenderMaskDepth)
                        return Invalid();
                if (mask.shape > UiMaskShape::RoundedRectangle)
                    return Invalid();
                if (mask.shape == UiMaskShape::AlphaTexture) {
                    if (mask.resource >= projection.resources.size() ||
                        projection.resources[mask.resource].role != UiRenderResourceRole::Mask)
                        return Result<void>::Failure(MakeError(UiErrors::RenderResourceReferenceInvalid));
                } else if (mask.resource != NoUiRenderIndex) {
                    return Invalid();
                }
                if (!ValidRadii(mask))
                    return Invalid();
            }
            return Result<void>::Success();
        }

        bool ValidGroup(const UiElementTree &tree, const UiRenderProjection &projection, const UiOpacityGroup &group) {
            return !tree.Get(group.element).HasError() && group.rect.extent.IsValid() && group.commandCount > 0 &&
                   group.firstCommand < projection.commands.size() &&
                   group.commandCount <= projection.commands.size() - group.firstCommand && std::isfinite(group.opacity) &&
                   group.opacity >= 0.0F && group.opacity <= 1.0F && group.transform < projection.transforms.size() &&
                   OptionalIndex(group.clip, projection.clips.size()) && OptionalIndex(group.mask, projection.masks.size());
        }

        /** @brief A nested group's full command interval must fit within its already validated parent. */
        bool FitsParent(const UiRenderProjection &projection, const UiOpacityGroup &group, const std::uint32_t parent,
                        const std::uint32_t command) noexcept {
            return parent == NoUiRenderIndex ||
                   group.commandCount <= projection.groups[parent].firstCommand + projection.groups[parent].commandCount - command;
        }

        /** @brief Ordered intervals form a properly nested forest; each command names exactly its deepest group. */
        Result<void> ValidateGroups(const UiElementTree &tree, const UiRenderProjection &projection) {
            std::array<std::uint32_t, MaximumUiRenderOpacityGroupDepth> stack{};
            std::size_t depth = 0;
            std::uint32_t next = 0;
            for (std::uint32_t command = 0; command < projection.commands.size(); ++command) {
                while (depth != 0) {
                    if (const auto &active = projection.groups[stack[depth - 1]]; command < active.firstCommand + active.commandCount)
                        break;
                    --depth;
                }
                while (next < projection.groups.size() && projection.groups[next].firstCommand == command) {
                    const auto &group = projection.groups[next];
                    const auto parent = depth == 0 ? NoUiRenderIndex : stack[depth - 1];
                    if (!ValidGroup(tree, projection, group) || group.parent != parent || depth == stack.size())
                        return Invalid();
                    if (!FitsParent(projection, group, parent, command))
                        return Invalid();
                    stack[depth++] = next++;
                }
                if (projection.commands[command].group != (depth == 0 ? NoUiRenderIndex : stack[depth - 1]))
                    return Invalid();
            }
            return next == projection.groups.size() ? Result<void>::Success() : Invalid();
        }
    }  // namespace

    /** @copydoc ValidateEffects */
    Result<void> ValidateEffects(const UiElementTree &tree, const UiRenderProjection &projection) {
        if (const auto masks = ValidateMasks(projection); masks.HasError())
            return masks;
        return ValidateGroups(tree, projection);
    }
}  // namespace Horo::Runtime::Ui::PaintInternal
