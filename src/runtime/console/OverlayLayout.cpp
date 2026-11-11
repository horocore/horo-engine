#include "Horo/Debug/OverlayLayout.h"

#include <cmath>

namespace Horo::Debug {
    namespace {
        /** @brief Rejects NaN/Inf, negative or empty panels and overflow beyond the logical viewport. */
        bool ValidBounds(const OverlayBounds bounds) {
            return std::isfinite(bounds.x) && std::isfinite(bounds.y) && std::isfinite(bounds.width) && std::isfinite(bounds.height) &&
                   bounds.x >= 0 && bounds.y >= 0 && bounds.width > 0 && bounds.height > 0 && bounds.x + bounds.width <= 1 &&
                   bounds.y + bounds.height <= 1;
        }
    }  // namespace

    /** @copydoc OverlayLayout::Create */
    Result<OverlayLayout> OverlayLayout::Create(const std::span<const OverlayDescriptor> descriptors, RegistryPolicy policy) {
        if (descriptors.size() > 32 || static_cast<unsigned>(policy.profile) > 5)
            return Result<OverlayLayout>::Failure(MakeCommandError(CommandError::LimitExceeded));
        if (!descriptors.empty() && (policy.profile == ProductProfile::Shipping || policy.profile == ProductProfile::Server))
            return Result<OverlayLayout>::Failure(MakeCommandError(CommandError::PermissionDenied));
        OverlayLayout layout;
        layout.profile_ = policy.profile;
        for (std::size_t i = 0; i < descriptors.size(); ++i) {
            const auto &descriptor = descriptors[i];
            const auto permission = static_cast<unsigned>(descriptor.permission);
            if (!descriptor.id || !IsValidConsoleIdentifier(descriptor.name) || descriptor.name.size() > 128 ||
                static_cast<unsigned>(descriptor.source) > 9 || static_cast<unsigned>(descriptor.availability) > 3 ||
                (permission != 1 && permission != 2 && permission != 4 && permission != 8) ||
                (descriptor.availability == CommandAvailability::ShippingAllowlist && permission != 1) ||
                (descriptor.permission == CommandPermission::AdminCheat && descriptor.availability != CommandAvailability::DevelopmentOnly))
                return Result<OverlayLayout>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
            for (const auto &earlier : descriptors.first(i))
                if (earlier.id == descriptor.id || earlier.name == descriptor.name)
                    return Result<OverlayLayout>::Failure(MakeCommandError(CommandError::Duplicate));
            if (!IsConsoleDescriptorIncluded(descriptor.id, descriptor.availability, policy))
                continue;
            auto &panel = layout.panels_[layout.count_];
            panel.id = descriptor.id;
            panel.name = descriptor.name;
            panel.source = descriptor.source;
            panel.permission = descriptor.permission;
            panel.order = static_cast<std::uint8_t>(layout.count_++);
        }
        return Result<OverlayLayout>::Success(std::move(layout));
    }

    /** @copydoc OverlayLayout::Configure */
    Result<void> OverlayLayout::Configure(std::uint64_t id, const OverlayBounds bounds, const bool visible,
                                          const DebugCommandContext &principal) {
        if (std::this_thread::get_id() != owner_)
            return Result<void>::Failure(MakeCommandError(CommandError::WrongThread));
        if (!ValidBounds(bounds))
            return Result<void>::Failure(MakeCommandError(CommandError::Usage));
        for (auto &panel : std::span(panels_).first(count_)) {
            if (panel.id != id)
                continue;
            if (!IsConsolePermissionGranted(panel.permission, principal, profile_))
                return Result<void>::Failure(MakeCommandError(CommandError::PermissionDenied));
            panel.bounds = bounds;
            panel.visible = visible;
            return Result<void>::Success();
        }
        return Result<void>::Failure(MakeCommandError(CommandError::UnknownCommand));
    }

    /** @copydoc OverlayLayout::Focus */
    Result<void> OverlayLayout::Focus(std::uint64_t id, const DebugCommandContext &principal) {
        if (std::this_thread::get_id() != owner_)
            return Result<void>::Failure(MakeCommandError(CommandError::WrongThread));
        for (auto &panel : std::span(panels_).first(count_)) {
            if (panel.id != id)
                continue;
            if (!panel.visible || !IsConsolePermissionGranted(panel.permission, principal, profile_))
                return Result<void>::Failure(MakeCommandError(CommandError::PermissionDenied));
            const auto previous = panel.order;
            for (auto &other : std::span(panels_).first(count_))
                if (other.order > previous)
                    --other.order;
            panel.order = static_cast<std::uint8_t>(count_ - 1);
            return Result<void>::Success();
        }
        return Result<void>::Failure(MakeCommandError(CommandError::UnknownCommand));
    }

    /** @copydoc OverlayLayout::Snapshot */
    Result<std::size_t> OverlayLayout::Snapshot(const DebugCommandContext &principal, std::span<OverlayPanelSnapshot> output) const {
        if (std::this_thread::get_id() != owner_)
            return Result<std::size_t>::Failure(MakeCommandError(CommandError::WrongThread));
        std::array<OverlayPanelSnapshot, 32> candidate;
        std::size_t count{};
        for (const auto &panel : std::span(panels_).first(count_)) {
            if (!panel.visible || !IsConsolePermissionGranted(panel.permission, principal, profile_))
                continue;
            candidate[count++] = {panel.id, panel.source, panel.bounds, panel.order};
        }
        if (count > output.size())
            return Result<std::size_t>::Failure(MakeCommandError(CommandError::LimitExceeded));
        for (std::size_t i = 0; i < count; ++i)
            output[i] = candidate[i];
        return Result<std::size_t>::Success(count);
    }
}  // namespace Horo::Debug
