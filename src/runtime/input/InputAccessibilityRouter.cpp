#include "InputAccessibilityRouter.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Input::Detail {
    /** @copydoc PrepareAccessibilityIdentities */
    Result<std::vector<AccessibilityActionIdentity>> PrepareAccessibilityIdentities(const std::span<const ActionDescriptor> actions,
                                                                                    const InputBindingProfile &profile) {
        try {
            std::vector<AccessibilityActionIdentity> result(actions.size());
            for (std::size_t index = 0; index < actions.size(); ++index) {
                const auto overrideValue = std::ranges::find(profile.overrides, actions[index].id, &BindingOverride::action);
                const auto &bindings = overrideValue == profile.overrides.end() ? actions[index].defaultBindings : overrideValue->bindings;
                if (std::ranges::any_of(bindings, [](const auto &binding) {
                    return binding.accessibility.IsEnabled();
                }))
                    result[index] = std::make_shared<const ActionId>(actions[index].id);
            }
            return Result<std::vector<AccessibilityActionIdentity>>::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Result<std::vector<AccessibilityActionIdentity>>::Failure(MakeError(InputAccessibilityErrors::CapacityExceeded));
        }
    }

    /** @copydoc InputAccessibilityRouter::Cancel */
    void InputAccessibilityRouter::Cancel(Cell &cell, const ActionReadStatus cause) noexcept {
        if (!cell.action || cell.context == 0 || cell.retired)
            return;
        const bool released = cell.processor && cell.processor->Cancel().button.released;
        cell.pendingRelease = cell.pendingRelease || released;
        if (released)
            cell.releaseStatus = cause;
        cell.retired = true;
        cell.chordHeld = false;
        if (!cell.pendingRelease)
            cell.context = 0;
    }

    /** @copydoc InputAccessibilityRouter::BeginFrame */
    bool InputAccessibilityRouter::BeginFrame(const RawInputSnapshot &raw, const RawInputSnapshot *previous,
                                              const InputDeviceAssignments &assignments) noexcept {
        EndFrame();
        const bool reversed = previousTime_ && raw.sampleTime && *raw.sampleTime < *previousTime_;
        const bool deviceBoundary = previous && (raw.gamepads.size() != previous->gamepads.size() ||
                                                 std::ranges::any_of(previous->gamepads, [&](const GamepadState &pad) {
            return !raw.FindGamepad(pad.id);
        }));
        exhausted_ = exhausted_ || serial_ == std::numeric_limits<std::uint64_t>::max();
        serial_ = exhausted_ ? 0 : serial_ + 1;
        if (reversed || serial_ == 0)
            CancelAll();
        previousTime_ = raw.sampleTime;
        for (auto &cell : cells_) {
            const bool missingClock = !raw.sampleTime && (cell.policy.digital.holdMicroseconds != 0 || cell.policy.digital.repeat);
            const bool lostDevice =
                cell.device && (!raw.FindGamepad(*cell.device) || (cell.player && assignments.PlayerFor(*cell.device) != cell.player));
            if (missingClock || lostDevice)
                Cancel(cell, missingClock ? ActionReadStatus::ClockUnavailable : ActionReadStatus::Resolved);
            if (cell.retired || !cell.action || cell.context == 0)
                continue;
            const auto physical = PeekAccessibilityPhysical(cell.source, raw, previous, cell.device);
            const bool chordHeld = std::ranges::all_of(std::span(cell.source.chord).first(cell.source.chordSize), [&](const Key key) {
                return raw.State(key).down;
            });
            const bool modifiersHeld = cell.policy.stickyModifiers || raw.modifiers == cell.source.requiredModifiers;
            if (!cell.policy.digital.toggle && cell.physicalHeld && (!physical.down || physical.pressed || !chordHeld || !modifiersHeld)) {
                const auto cancelled = cell.processor->Cancel();
                cell.pendingRelease = cell.pendingRelease || cancelled.button.released;
                cell.chordHeld = false;
                cell.cached = {};
                cell.serial = 0;
            }
            cell.physicalHeld = physical.down;
        }
        return reversed || deviceBoundary || serial_ == 0;
    }

    /** @copydoc InputAccessibilityRouter::CancelAll */
    void InputAccessibilityRouter::CancelAll() noexcept {
        for (auto &cell : cells_)
            Cancel(cell);
    }

    /** @copydoc InputAccessibilityRouter::CancelDevices */
    void InputAccessibilityRouter::CancelDevices() noexcept {
        for (auto &cell : cells_)
            if (cell.device)
                Cancel(cell);
    }

    /** @copydoc InputAccessibilityRouter::CancelContext */
    void InputAccessibilityRouter::CancelContext(const std::uint64_t context, const bool ownerRemoved) noexcept {
        for (auto &cell : cells_) {
            if (cell.context != context)
                continue;
            Cancel(cell);
            if (ownerRemoved) {
                cell.ownerRemoved = true;
            }
        }
    }

    /** @copydoc InputAccessibilityRouter::CancelAction */
    void InputAccessibilityRouter::CancelAction(const std::uint64_t context, const ActionId &action,
                                                const std::optional<PlayerId> player) noexcept {
        for (auto &cell : cells_)
            if (cell.context == context && cell.action && *cell.action == action && cell.player == player)
                Cancel(cell, ActionReadStatus::ClockUnavailable);
    }

    /** @copydoc InputAccessibilityRouter::TakeReleases */
    AccessibilityRelease InputAccessibilityRouter::TakeReleases(const std::uint64_t context, const ActionId &action,
                                                                const std::optional<PlayerId> player) noexcept {
        AccessibilityRelease release;
        for (auto &cell : cells_) {
            if (cell.context != context || !cell.pendingRelease || !cell.action || *cell.action != action || cell.player != player)
                continue;
            release.released = true;
            if (cell.releaseStatus == ActionReadStatus::ClockUnavailable)
                release.status = cell.releaseStatus;
            cell.pendingRelease = false;
            cell.releaseStatus = ActionReadStatus::Resolved;
            if (cell.retired)
                cell.context = 0;
        }
        return release;
    }

    /** @copydoc InputAccessibilityRouter::EndFrame */
    void InputAccessibilityRouter::EndFrame() noexcept {
        for (auto &cell : cells_)
            if (cell.context == 0 && cell.action)
                cell = {};
    }

    /** @copydoc InputAccessibilityRouter::TryDequeueRetired */
    bool InputAccessibilityRouter::TryDequeueRetired(RetiredInputActionRelease &release) noexcept {
        for (auto &cell : cells_) {
            if (!cell.ownerRemoved || !cell.pendingRelease)
                continue;
            release = {cell.context, cell.action, cell.player};
            for (auto &matching : cells_) {
                if (matching.ownerRemoved && matching.pendingRelease && matching.context == release.contextIdentity && matching.action &&
                    *matching.action == *release.action && matching.player == release.player) {
                    matching.pendingRelease = false;
                    matching.context = 0;
                }
            }
            return true;
        }
        return false;
    }

    /** @copydoc InputAccessibilityRouter::Transaction::Process */
    DigitalAffordanceValue InputAccessibilityRouter::Transaction::Process(const AccessibilityMappingKey &key, const InputBinding &source,
                                                                          const AccessibilityMappingSample &sample) noexcept {
        const auto &[context, action, binding, player] = key;
        const auto &[device, physical, time, eligible, chordEligible, deviceHadNeutral] = sample;
        const auto &policy = source.accessibility;
        if (failed_ || !action || count_ == candidates_.size() || owner_.serial_ == 0) {
            failed_ = true;
            return {};
        }
        auto found = std::ranges::find_if(owner_.cells_, [&](const auto &cell) {
            return !cell.retired && cell.context == context && cell.action == action && cell.binding == binding && cell.player == player;
        });
        if (found == owner_.cells_.end()) {
            found = std::ranges::find_if(owner_.cells_, [&](const auto &cell) {
                const auto slot = static_cast<std::size_t>(&cell - owner_.cells_.data());
                return cell.context == 0 && !cell.action &&
                       std::ranges::none_of(std::span(candidates_).first(count_), [&](const auto &candidate) {
                    return candidate.slot == slot;
                });
            });
        }
        if (found == owner_.cells_.end()) {
            failed_ = true;
            return {};
        }
        auto &candidate = candidates_[count_++];
        candidate.slot = static_cast<std::size_t>(found - owner_.cells_.begin());
        candidate.cell = *found;
        auto &cell = candidate.cell;
        if (!cell.action) {
            cell.context = context;
            cell.action = action;
            cell.binding = binding;
            cell.player = player;
            cell.policy = policy;
            cell.source = source;
            cell.processor.emplace(DigitalAffordance::Create(policy.digital).Value());
            cell.device = device;
        }
        if (cell.serial == owner_.serial_)
            return {{cell.cached.button.down, false, false}, false};
        DigitalAffordanceValue result;
        if (cell.device != device) {
            if (cell.physicalHeld || cell.cached.button.down || !deviceHadNeutral) {
                result = cell.processor->Cancel();
                cell.awaitingNeutral = true;
                cell.chordHeld = false;
            }
            cell.device = device;
        }
        if (cell.awaitingNeutral) {
            const bool gamepad = source.kind == BindingControlKind::GamepadButton || source.kind == BindingControlKind::GamepadAxis ||
                                 source.kind == BindingControlKind::RawGamepadButton || source.kind == BindingControlKind::RawGamepadAxis;
            if (!physical.down && !physical.pressed && (!gamepad || device.has_value()))
                cell.awaitingNeutral = false;
        } else {
            if (physical.pressed)
                cell.chordHeld = false;
            const bool admitted = eligible || (policy.stickyModifiers && cell.chordHeld && physical.down && chordEligible);
            if (admitted && physical.pressed && policy.stickyModifiers) {
                cell.chordHeld = physical.down;
                consumeSticky_ = true;
            }
            if (!physical.down)
                cell.chordHeld = false;
            const bool timed = policy.digital.holdMicroseconds != 0 || policy.digital.repeat;
            result = cell.processor->Update(admitted ? physical : ButtonState{}, timed && time ? time->microseconds : 0);
        }
        cell.cached = result;
        cell.physicalHeld = physical.down;
        cell.serial = owner_.serial_;
        return result;
    }

    /** @copydoc InputAccessibilityRouter::Transaction::PreferredDevice */
    std::optional<GamepadDeviceId> InputAccessibilityRouter::Transaction::PreferredDevice(
        const AccessibilityMappingKey &key) const noexcept {
        const auto &[context, action, binding, player] = key;
        const auto found = std::ranges::find_if(owner_.cells_, [&](const auto &cell) {
            return !cell.retired && cell.context == context && cell.action == action && cell.binding == binding && cell.player == player &&
                   (cell.physicalHeld || cell.cached.button.down);
        });
        return found == owner_.cells_.end() ? std::nullopt : found->device;
    }

    /** @copydoc InputAccessibilityRouter::Transaction::Commit */
    void InputAccessibilityRouter::Transaction::Commit() noexcept {
        if (!failed_)
            for (const auto &candidate : std::span(candidates_).first(count_))
                owner_.cells_[candidate.slot] = candidate.cell;
    }
}  // namespace Horo::Input::Detail
