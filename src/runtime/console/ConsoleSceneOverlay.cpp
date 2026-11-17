#include "ConsoleSceneOverlayInternal.h"

#include <algorithm>

namespace Horo::Debug::Internal {
    /** @copydoc ConsoleSceneOverlay::ConsoleSceneOverlay */
    ConsoleSceneOverlay::ConsoleSceneOverlay(const ConsoleSceneInspectionLimits limits)
        : limits_(limits), owner_(std::this_thread::get_id()) {}

    /** @copydoc ConsoleSceneOverlay::Create */
    Result<ConsoleSceneOverlay> ConsoleSceneOverlay::Create(const ProductProfile profile, const DebugCommandContext &principal,
                                                            const ConsoleSceneInspectionLimits limits) {
        if (profile != ProductProfile::Editor && profile != ProductProfile::Development)
            return Result<ConsoleSceneOverlay>::Failure(MakeCommandError(CommandError::MissingCapability));
        if (!IsConsolePermissionGranted(CommandPermission::Developer, principal, profile))
            return Result<ConsoleSceneOverlay>::Failure(MakeCommandError(CommandError::PermissionDenied));
        if (limits.maximumVisitedSlots == 0 || limits.maximumVisitedSlots > 4096 || limits.maximumReportedEntities == 0 ||
            limits.maximumReportedEntities > 256)
            return Result<ConsoleSceneOverlay>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        return Result<ConsoleSceneOverlay>::Success(ConsoleSceneOverlay{limits});
    }

    /** @copydoc ConsoleSceneOverlay::Clear */
    bool ConsoleSceneOverlay::Clear() noexcept {
        const bool changed = available_ || count_ != 0 || truncated_;
        count_ = 0;
        runtime_ = {};
        structuralRevision_ = 0;
        available_ = false;
        truncated_ = false;
        if (changed)
            ++revision_;
        return changed;
    }

    /** @copydoc ConsoleSceneOverlay::Publish */
    Result<void> ConsoleSceneOverlay::Publish(const Runtime::RuntimeSceneView *view) {
        if (std::this_thread::get_id() != owner_)
            return Result<void>::Failure(MakeCommandError(CommandError::WrongThread));
        view_ = view ? std::optional{*view} : std::nullopt;
        if (!view)
            Clear();
        return Result<void>::Success();
    }

    /** @copydoc ConsoleSceneOverlay::SetVisible */
    Result<void> ConsoleSceneOverlay::SetVisible(const bool visible) {
        if (std::this_thread::get_id() != owner_)
            return Result<void>::Failure(MakeCommandError(CommandError::WrongThread));
        if (visible_ == visible)
            return Result<void>::Success();
        visible_ = visible;
        nextRefresh_.reset();
        if (!visible)
            Clear();
        return Result<void>::Success();
    }

    /** @copydoc ConsoleSceneOverlay::Refresh */
    Result<bool> ConsoleSceneOverlay::Refresh(const std::chrono::steady_clock::time_point now) {
        if (std::this_thread::get_id() != owner_)
            return Result<bool>::Failure(MakeCommandError(CommandError::WrongThread));
        if (!visible_ || (nextRefresh_ && now < *nextRefresh_))
            return Result<bool>::Success(false);
        nextRefresh_ = now + std::chrono::milliseconds{250};
        if (!view_ || !view_->IsCurrent())
            return Result<bool>::Success(Clear());
        const auto runtime = view_->RuntimeId();
        const auto structural = view_->StructuralRevision();
        const auto slots = view_->SlotCount();
        const auto visited = std::min(slots, limits_.maximumVisitedSlots);
        std::size_t count{}, scanned{};
        bool changed = !available_ || runtime_ != runtime || structuralRevision_ != structural;
        for (; scanned < visited && count < limits_.maximumReportedEntities; ++scanned) {
            const auto entity = view_->EntityAt(scanned);
            if (!entity)
                continue;
            const ConsoleSceneOverlayRow row{entity->entity, entity->parent};
            changed |= count >= count_ || rows_[count] != row;
            rows_[count++] = row;
        }
        const bool truncated = scanned < slots;
        changed |= count != count_ || truncated_ != truncated;
        count_ = count;
        runtime_ = runtime;
        structuralRevision_ = structural;
        available_ = true;
        truncated_ = truncated;
        if (changed)
            ++revision_;
        return Result<bool>::Success(changed);
    }

    /** @copydoc ConsoleSceneOverlay::Rows */
    std::span<const ConsoleSceneOverlayRow> ConsoleSceneOverlay::Rows() const noexcept {
        return std::span(rows_).first(count_);
    }

    /** @copydoc ConsoleSceneOverlay::Available */
    bool ConsoleSceneOverlay::Available() const noexcept {
        return available_;
    }

    /** @copydoc ConsoleSceneOverlay::Truncated */
    bool ConsoleSceneOverlay::Truncated() const noexcept {
        return truncated_;
    }

    /** @copydoc ConsoleSceneOverlay::RuntimeId */
    Runtime::SceneRuntimeId ConsoleSceneOverlay::RuntimeId() const noexcept {
        return runtime_;
    }

    /** @copydoc ConsoleSceneOverlay::Revision */
    std::uint64_t ConsoleSceneOverlay::Revision() const noexcept {
        return revision_;
    }
}  // namespace Horo::Debug::Internal
