#include "UiPaintProgramInternal.h"

#include <algorithm>

namespace Horo::Render {
    using namespace Runtime::Ui;
    using Detail::NoPaintRole;

    /** @copydoc UiPaintProgram::Storage::IsMaskAncestor */
    bool UiPaintProgram::Storage::IsMaskAncestor(std::uint32_t ancestor, std::uint32_t leaf) const noexcept {
        if (ancestor == NoUiRenderIndex)
            return true;
        const auto masks = source.prototype.Masks();
        for (std::uint32_t depth = 0; leaf != NoUiRenderIndex && depth < MaximumUiRenderMaskDepth; ++depth) {
            if (leaf >= masks.size())
                return false;
            if (leaf == ancestor)
                return true;
            leaf = masks[leaf].parent;
        }
        return false;
    }

    /** @copydoc UiPaintProgram::Storage::AddMaskScope */
    Result<std::uint32_t> UiPaintProgram::Storage::AddMaskScope(std::uint32_t leaf, std::uint32_t boundary, std::uint32_t depth) {
        using Scope = Result<std::uint32_t>;
        if (!IsMaskAncestor(boundary, leaf))
            return Scope::Failure(MakeError(UiPaintProgramErrors::PreparationRequired));
        if (leaf == boundary)
            return Scope::Success(NoPaintRole);
        if (depth >= MaximumUiRenderMaskDepth)
            return Scope::Failure(MakeError(UiPaintProgramErrors::Capacity));
        for (std::uint32_t index = 0; index < source.masks.size(); ++index)
            if (source.masks[index].leaf == leaf && source.masks[index].boundary == boundary)
                return Scope::Success(index);
        if (leaf >= source.prototype.Masks().size())
            return Scope::Failure(MakeError(UiPaintProgramErrors::PreparationRequired));
        const auto parent = AddMaskScope(source.prototype.Masks()[leaf].parent, boundary, depth + 1);
        if (parent.HasError())
            return parent;
        if (source.masks.size() + source.prototype.Groups().size() + 1 >= descriptor.maximumPasses)
            return Scope::Failure(MakeError(UiPaintProgramErrors::Capacity));
        const auto index = static_cast<std::uint32_t>(source.masks.size());
        source.masks.push_back({leaf, boundary, parent.Value()});
        return Scope::Success(index);
    }

    /** @copydoc UiPaintProgram::Storage::MaskScope */
    Result<std::uint32_t> UiPaintProgram::Storage::MaskScope(std::uint32_t leaf, std::uint32_t group) {
        if (leaf == NoUiRenderIndex)
            return Result<std::uint32_t>::Success(NoPaintRole);
        const auto groups = source.prototype.Groups();
        for (std::uint32_t depth = 0; group != NoUiRenderIndex && depth < MaximumUiRenderOpacityGroupDepth; ++depth) {
            if (group >= groups.size())
                return Result<std::uint32_t>::Failure(MakeError(UiPaintProgramErrors::PreparationRequired));
            const auto boundary = groups[group].mask;
            if (boundary != NoUiRenderIndex && IsMaskAncestor(boundary, leaf))
                return AddMaskScope(leaf, boundary);
            group = groups[group].parent;
        }
        if (group != NoUiRenderIndex)
            return Result<std::uint32_t>::Failure(MakeError(UiPaintProgramErrors::Capacity));
        return AddMaskScope(leaf, NoUiRenderIndex);
    }

    /** @copydoc UiPaintProgram::Storage::BuildRoles */
    Result<void> UiPaintProgram::Storage::BuildRoles() {
        const auto groups = source.prototype.Groups();
        const auto commands = source.prototype.Commands();
        if (groups.size() + 1 > descriptor.maximumPasses)
            return Result<void>::Failure(MakeError(UiPaintProgramErrors::Capacity));
        source.masks.reserve(descriptor.maximumPasses - groups.size() - 1);
        source.commandScopes.resize(commands.size());
        source.groupScopes.resize(groups.size());
        for (std::size_t index = 0; index < commands.size(); ++index) {
            const auto scope = MaskScope(commands[index].mask, commands[index].group);
            if (scope.HasError())
                return Result<void>::Failure(scope.ErrorValue());
            source.commandScopes[index] = scope.Value();
        }
        for (std::size_t index = 0; index < groups.size(); ++index) {
            const auto scope = MaskScope(groups[index].mask, groups[index].parent);
            if (scope.HasError())
                return Result<void>::Failure(scope.ErrorValue());
            source.groupScopes[index] = scope.Value();
        }
        const auto passes = source.masks.size() + groups.size() + 1;
        const auto resources = 4 + source.external.size() + source.masks.size() + groups.size();
        if (passes > descriptor.maximumPasses || resources > RenderGraphLimits::HardMaxResources ||
            source.external.size() + source.masks.size() + groups.size() > UiPaintLimits::MaximumSampledResources)
            return Result<void>::Failure(MakeError(UiPaintProgramErrors::Capacity));
        std::size_t usages{};
        for (const auto &mask : source.masks)
            usages += 4 + source.external.size() + (mask.parent != NoPaintRole ? 1 : 0);
        for (std::uint32_t parent = 0; parent <= groups.size(); ++parent) {
            const auto group = parent == groups.size() ? NoPaintRole : parent;
            const auto children = std::ranges::count(groups, group, &UiOpacityGroup::parent);
            usages += 3 + (group != NoPaintRole ? 1 : 0) + 1 + source.external.size() + source.masks.size() + children;
        }
        if (usages > RenderGraphLimits::HardMaxUsages)
            return Result<void>::Failure(MakeError(UiPaintProgramErrors::Capacity));
        return Result<void>::Success();
    }

    /** @copydoc UiPaintProgram::Storage::ValidateSource */
    Result<void> UiPaintProgram::Storage::ValidateSource(const UiRenderSubmission &submission) const {
        const auto changed = [] {
            return Result<void>::Failure(MakeError(UiPaintProgramErrors::PreparationRequired));
        };
        if (!submission.geometry || !submission.geometry->IsValid())
            return changed();
        const auto &snapshot = submission.geometry->SourceSnapshot();
        const auto &expected = source.prototype.Descriptor();
        const auto &actual = snapshot.Descriptor();
        if (actual.instance != expected.instance || actual.canvas != expected.canvas || actual.document != expected.document ||
            actual.documentRevision != expected.documentRevision || actual.treeRevision != expected.treeRevision ||
            actual.view != expected.view || snapshot.Commands().size() != source.prototype.Commands().size() ||
            snapshot.Groups().size() != source.prototype.Groups().size() || snapshot.Masks().size() != source.prototype.Masks().size() ||
            snapshot.Resources().size() != source.prototype.Resources().size())
            return changed();
        const auto &presentation = submission.geometry->Descriptor().presentation;
        if (presentation && *presentation != descriptor.projection)
            return changed();
        for (std::size_t index = 0; index < snapshot.Commands().size(); ++index) {
            const auto &command = snapshot.Commands()[index];
            const auto &prototype = source.prototype.Commands()[index];
            if (command.element != prototype.element || command.group != prototype.group || command.mask != prototype.mask ||
                command.payload.index() != prototype.payload.index())
                return changed();
        }
        for (std::size_t index = 0; index < snapshot.Groups().size(); ++index) {
            const auto &group = snapshot.Groups()[index];
            const auto &prototype = source.prototype.Groups()[index];
            if (group.element != prototype.element || group.parent != prototype.parent || group.firstCommand != prototype.firstCommand ||
                group.commandCount != prototype.commandCount || group.mask != prototype.mask)
                return changed();
        }
        for (std::size_t index = 0; index < snapshot.Masks().size(); ++index)
            if (snapshot.Masks()[index].parent != source.prototype.Masks()[index].parent)
                return changed();
        for (std::size_t index = 0; index < snapshot.Resources().size(); ++index) {
            const auto &reference = snapshot.Resources()[index];
            const auto &prototype = source.prototype.Resources()[index];
            if (reference.asset != prototype.asset || reference.revision != prototype.revision || reference.role != prototype.role ||
                reference.colorSpace != prototype.colorSpace || reference.sampling != prototype.sampling ||
                reference.fallback != prototype.fallback || reference.residency != prototype.residency)
                return changed();
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Render
