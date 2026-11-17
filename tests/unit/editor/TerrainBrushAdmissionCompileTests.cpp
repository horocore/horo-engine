#include "TerrainBrushAdmission.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace {
    using Horo::Editor::Detail::CanOwnTerrainHeightPatch;
    using Horo::Editor::Detail::TryNarrowTerrainHeight;

    constexpr double MaximumHeight = std::numeric_limits<float>::max();
    constexpr std::size_t PatchBytes = sizeof(Horo::Editor::TerrainRasterPatch);
    constexpr std::size_t SizeMaximum = std::numeric_limits<std::size_t>::max();
    constexpr std::uint64_t MaximumSamples = (SizeMaximum - PatchBytes) / sizeof(float);

    /** @brief Runs the actual production narrowing helper during C++ constant evaluation. */
    constexpr bool NarrowsTo(double input, float expected) {
        float output = 17.0F;
        return TryNarrowTerrainHeight(input, output) && output == expected;
    }

    /** @brief Rejected values must never be cast or overwrite an existing candidate. */
    constexpr bool RejectsUnchanged(double input) {
        float output = 17.0F;
        return !TryNarrowTerrainHeight(input, output) && output == 17.0F;
    }

    static_assert(NarrowsTo(0.0, 0.0F));
    static_assert(NarrowsTo(1.5, 1.5F));
    static_assert(NarrowsTo(MaximumHeight, std::numeric_limits<float>::max()));
    static_assert(NarrowsTo(-MaximumHeight, -std::numeric_limits<float>::max()));
    static_assert(NarrowsTo(std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::denorm_min()));
    static_assert(RejectsUnchanged(MaximumHeight * 2.0));
    static_assert(RejectsUnchanged(-MaximumHeight * 2.0));
    static_assert(RejectsUnchanged(MaximumHeight * (1.0 + std::numeric_limits<double>::epsilon())));
    static_assert(RejectsUnchanged(-MaximumHeight * (1.0 + std::numeric_limits<double>::epsilon())));
    static_assert(RejectsUnchanged(std::numeric_limits<double>::max()));
    static_assert(RejectsUnchanged(std::numeric_limits<double>::infinity()));
    static_assert(RejectsUnchanged(-std::numeric_limits<double>::infinity()));
    static_assert(RejectsUnchanged(std::numeric_limits<double>::quiet_NaN()));

    static_assert(!CanOwnTerrainHeightPatch(1, sizeof(float)));
    static_assert(!CanOwnTerrainHeightPatch(1, PatchBytes + sizeof(float) - 1));
    static_assert(CanOwnTerrainHeightPatch(1, PatchBytes + sizeof(float)));
    static_assert(CanOwnTerrainHeightPatch(9, PatchBytes + 9 * sizeof(float)));
    static_assert(!CanOwnTerrainHeightPatch(9, PatchBytes + 9 * sizeof(float) - 1));
    static_assert(!CanOwnTerrainHeightPatch(0, 0));
    static_assert(!CanOwnTerrainHeightPatch(0, PatchBytes - 1));
    static_assert(CanOwnTerrainHeightPatch(0, PatchBytes));
    static_assert(CanOwnTerrainHeightPatch(MaximumSamples, SizeMaximum));
    static_assert(!CanOwnTerrainHeightPatch(MaximumSamples + 1, SizeMaximum));
    static_assert(!CanOwnTerrainHeightPatch(std::numeric_limits<std::uint64_t>::max(), SizeMaximum));
}  // namespace
