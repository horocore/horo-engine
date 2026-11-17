#pragma once
#include "Horo/XR/XRSimulatorScenario.h"

namespace Horo::XR::SimulatorInternal {
    struct EncodedMeasure final {
        std::size_t bytes{};
        std::size_t nodes{};
        bool valid{};
    };

    /** @brief Allocation-free exact step encoding measure shared with canonical export. */
    [[nodiscard]] EncodedMeasure Measure(const XRSimulatorStep &step) noexcept;
    inline constexpr std::size_t CaptureManifestCharge = 512;
    inline constexpr std::size_t CaptureManifestNodeCharge = 128;
}  // namespace Horo::XR::SimulatorInternal
