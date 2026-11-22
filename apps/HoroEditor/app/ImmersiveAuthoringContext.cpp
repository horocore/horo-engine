#include "ImmersiveAuthoringContext.h"

#include <algorithm>
#include <atomic>
#include <limits>

namespace Horo::Editor {
    struct ImmersiveContextEpoch final {
        std::atomic<std::uint64_t> current{};
    };

    namespace {
        /** @brief Admits only a complete observation and a space owned by the exact XR session. */
        bool ValidIdentity(const ImmersiveContextIdentity &identity) noexcept {
            using enum ImmersiveAuthoringMode;
            const bool validMode = identity.mode == Edit || identity.mode == Simulate || identity.mode == Play;
            const bool validWorld = (identity.worldGeneration == 0) == (identity.mode == Edit);
            return identity.projectOpening && identity.documentOpening && identity.documentRevision && identity.user &&
                   identity.conversation && identity.toolContext && identity.capabilities.IsValid() && identity.xrSession.IsValid() &&
                   identity.referenceSpace.IsValid() && identity.referenceSpace.session == identity.xrSession && validMode &&
                   validWorld;
        }

        /** @brief Verifies selection without allocations or reordering the owner projection. */
        bool ValidSelection(const std::span<const SceneSource::SceneObjectId> selection) noexcept {
            if (selection.size() > ImmersivePlanningContext::MaximumSelection)
                return false;
            for (std::size_t index = 0; index < selection.size(); ++index) {
                const auto previous = selection.first(index);
                if (!selection[index].IsValid() || std::ranges::find(previous, selection[index]) != previous.end())
                    return false;
            }
            return true;
        }
    }  // namespace

    bool ImmersivePlanningContext::IsCurrent() const noexcept {
        return epoch_ && generation_ != 0 && epoch_->current.load(std::memory_order_acquire) == generation_;
    }

    ImmersiveAuthoringContext::ImmersiveAuthoringContext() : epoch_(std::make_shared<ImmersiveContextEpoch>()) {}

    ImmersiveAuthoringContext::~ImmersiveAuthoringContext() {
        Revoke();
    }

    std::optional<ImmersivePlanningContext> ImmersiveAuthoringContext::Publish(
        const ImmersiveContextIdentity &identity, const std::span<const SceneSource::SceneObjectId> selection) noexcept {
        if (!ValidIdentity(identity) || !ValidSelection(selection)) {
            Revoke();
            return std::nullopt;
        }
        if (current_ && identity == current_->Identity() && std::ranges::equal(selection, current_->Selection()))
            return current_;
        Revoke();
        if (generation_ == std::numeric_limits<std::uint64_t>::max())
            return std::nullopt;
        auto &context = current_.emplace();
        context.identity_ = identity;
        std::ranges::copy(selection, context.selection_.begin());
        context.selectionCount_ = selection.size();
        context.generation_ = ++generation_;
        context.epoch_ = epoch_;
        epoch_->current.store(context.generation_, std::memory_order_release);
        return current_;
    }

    void ImmersiveAuthoringContext::Revoke() noexcept {
        epoch_->current.store(0, std::memory_order_release);
        current_.reset();
    }

    ImmersiveAdmission ImmersiveAuthoringContext::Admit(const ImmersivePlanningContext &context,
                                                        const ImmersiveRequestKind kind) const noexcept {
        if (context.epoch_ != epoch_ || !context.IsCurrent())
            return ImmersiveAdmission::StaleContext;
        const auto &identity = context.Identity();
        switch (kind) {
            case ImmersiveRequestKind::Plan:
                if (identity.mode == ImmersiveAuthoringMode::Play)
                    return ImmersiveAdmission::ModeDenied;
                return identity.planningAllowed ? ImmersiveAdmission::Admitted : ImmersiveAdmission::PermissionDenied;
            case ImmersiveRequestKind::Diagnostics:
                return identity.diagnosticsAllowed ? ImmersiveAdmission::Admitted : ImmersiveAdmission::PermissionDenied;
        }
        return ImmersiveAdmission::InvalidRequest;
    }
}  // namespace Horo::Editor
