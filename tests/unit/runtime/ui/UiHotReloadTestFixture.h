#pragma once

#include "Horo/Runtime/Ui/UiHotReload.h"

#include <catch2/catch_test_macros.hpp>
#include <string_view>

namespace Horo::Runtime::Ui::ReloadTests {
    template <typename T> T Stable(std::uint8_t marker) {
        SerializedUiId bytes{};
        bytes.back() = marker;
        return T::Create(bytes).Value();
    }

    template <typename T> T Revision(std::uint64_t value) {
        return T::Create(value).Value();
    }

    inline UiOwnershipGeneration Owner() {
        return UiOwnershipGeneration::Create(704).Value();
    }

    inline RuntimeUiInstanceId Instance() {
        return {Owner(), 1, 1};
    }

    inline UiActionText Text(std::string_view value) {
        return UiActionText::Create(value).Value();
    }

    /** @brief Produces real cooked/provider-loaded closures and actual typed owner compositions. */
    [[nodiscard]] UiReloadGeneration Generation(UiElementSlotAllocator &allocator, std::uint64_t version, std::uint16_t textLimit = 32,
                                                bool secondCanvas = false, std::uint32_t modalHighWater = 0,
                                                std::uint32_t routeHighWater = 0, std::uint32_t concurrentSnapshots = 3);
    [[nodiscard]] UiReloadCanvas &Canvas(UiHotReload &publisher);
    [[nodiscard]] UiControlInput Input(const UiControlStateMachine &control, UiControlInputKind kind, std::uint64_t sequence,
                                       std::string_view text = {});
    /** @brief Enters a real text edit session, then changes its draft without submitting. */
    void Draft(UiReloadCanvas &canvas, std::string_view text, std::uint64_t firstSequence = 1);
}  // namespace Horo::Runtime::Ui::ReloadTests
