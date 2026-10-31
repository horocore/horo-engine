#pragma once
#include "Horo/Runtime/InputCollection.h"
#include "InputCollectionInternal.h"

#include <algorithm>
#include <bitset>

namespace Horo::Input {
    /** @brief Fixed exact transition ledger avoids frame-hot hash-node allocation and identity collisions. */
    struct GamepadTransitions {
        struct Entry {
            GamepadDeviceId device;
            BindingControlKind kind;
            std::uint64_t control;
            bool operator==(const Entry &) const noexcept = default;
        };

        std::array<Entry, MaximumConsumedGamepadTransitions> entries{};
        std::size_t count{};
        bool capacityExceeded{};

        bool Consume(const Entry &entry) noexcept {
            if (const auto used = std::span(entries).first(count); std::ranges::find(used, entry) != used.end())
                return false;
            if (count == entries.size()) {
                capacityExceeded = true;
                return false;
            }
            entries[count++] = entry;
            return true;
        }

        void clear() noexcept {
            count = 0;
            capacityExceeded = false;
        }
    };

    struct InputRouter::Impl {
        struct Context {
            std::uint64_t token;
            InputContextId id;
            InputContextKind kind;
        };

        struct Capture {
            std::uint64_t token;
            std::uint64_t context;
            PointerButton button;
            IInputCaptureOwner *owner;
        };

        // One owner and one delivery window govern all committed text and pre-edit state.
        struct TextFocusState {
            std::uint64_t focusToken{0};
            std::uint64_t minimumCompositionRevision{0};
            std::uint64_t lastDeliveredCompositionRevision{0};
            bool served{false};
            bool blockedFrame{false};
        };

        std::shared_ptr<InputCollectionLifetime> collection;
        const RawInputSnapshot *snapshot{nullptr};
        const RawInputSnapshot *previousSnapshot{nullptr};
        RawInputSnapshot empty;
        std::vector<Context> contexts;
        std::optional<Capture> capture;
        std::vector<ActionDescriptor> actions;
        InputBindingProfile profile;

        /** @brief Frame-local admission ledger shared by direct reads and action evaluation/rollback. */
        struct ConsumptionState final {
            std::bitset<static_cast<std::size_t>(Key::Count)> keys;
            std::bitset<static_cast<std::size_t>(PointerButton::Count)> pointerButtons;
            std::bitset<MaximumTouchContacts> touchContacts;
            bool pointerMotion{};
            GamepadTransitions gamepadTransitions;
            bool wheelX{false};
            bool wheelY{false};
        };

        ConsumptionState consumed;
        InputDeviceAssignments assignments;
        std::uint64_t configurationRevision{1};
        std::uint64_t assignmentRevision{1};
        ActionReadStatus lastActionStatus{ActionReadStatus::Unavailable};
        std::uint64_t nextToken{1};
        bool modalBarrier{false};
        bool blockNextFrame{false};
        TextFocusState textFocus;
    };

}  // namespace Horo::Input
