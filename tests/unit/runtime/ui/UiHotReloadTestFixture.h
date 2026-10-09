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

    /** @brief Real cooked route and independently scoped owner composition for cross-layer contract coverage. */
    struct LayerOptions final {
        UiRouteMetadata route{Stable<UiRouteId>(9), UiPresentationBand::Screen, 1, false};
        RuntimeUiInstanceId instance{Instance()};
        UiCanvasInstanceId canvas{Owner(), 2, 1};
        std::optional<UiFocusPlayerId> player{UiFocusPlayerId{Owner(), 3, 1}};
        UiFocusPresentationLayerId layer{Owner(), 4, 1};
        UiRenderViewId view{Owner(), 10, 1};
        std::uint8_t childMarker{11}; /**< Change the real authored target ID to exercise removal across cooked reload. */
    };

    /** @brief Produces real cooked/provider-loaded closures and actual typed owner compositions. */
    [[nodiscard]] UiReloadGeneration Generation(UiElementSlotAllocator &allocator, std::uint64_t version, std::uint16_t textLimit = 32,
                                                bool secondCanvas = false, std::uint32_t modalHighWater = 0,
                                                std::uint32_t routeHighWater = 0, std::uint32_t concurrentSnapshots = 3,
                                                const LayerOptions &layer = {});
    [[nodiscard]] UiReloadCanvas &Canvas(UiHotReload &publisher);
    [[nodiscard]] UiControlInput Input(const UiControlStateMachine &control, UiControlInputKind kind, std::uint64_t sequence,
                                       std::string_view text = {});
    /** @brief Enters a real text edit session, then changes its draft without submitting. */
    void Draft(UiReloadCanvas &canvas, std::string_view text, std::uint64_t firstSequence = 1);
}  // namespace Horo::Runtime::Ui::ReloadTests
