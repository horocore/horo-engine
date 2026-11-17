#pragma once
/** @file AnimationCookedFormat.h
 * @brief One private checked container for explicit Animation payload sections inside the Assets envelope.
 */
#include "AnimationCookedFields.h"

namespace Horo::Animation::CookedInternal {
    inline constexpr std::uint32_t HeaderBytes = 240;
    inline constexpr std::uint32_t SectionBytes = 32;
    inline constexpr std::uint32_t SectionCount = 2;
    inline constexpr std::uint32_t BodyOffset = HeaderBytes + SectionBytes * SectionCount;

    /** @brief Borrowed sections remain within the exact checked input span; no trusted asset publication is issued. */
    struct Sections final {
        AnimationCookedKind kind{};
        Assets::AssetId asset;
        std::array<std::uint16_t, 3> version{};
        std::span<const std::uint8_t> body;
        std::span<const std::uint8_t> dependencies;
        std::uint32_t dependencyCount{};
        std::uint64_t work{};
    };

    /** @brief Checks finite hard-clamped policy before parser/encoder storage admission. */
    [[nodiscard]] Result<void> ValidateContext(const AnimationCookedContext &context);
    /** @brief Verifies complete header/table/checksum and returns only exact canonical section borrows. */
    [[nodiscard]] Result<Sections> ReadSections(std::span<const std::uint8_t> bytes, AnimationCookedKind expected, Assets::AssetId asset,
                                                const AnimationCookedContext &context);
    /** @brief Constructs the one canonical header/table/section arrangement, binding exact typed dependency bytes. */
    [[nodiscard]] Result<std::vector<std::uint8_t>> WriteSections(AnimationCookedKind kind, Assets::AssetId asset,
                                                                  std::array<std::uint16_t, 3> version, std::span<const std::uint8_t> body,
                                                                  std::span<const std::uint8_t> dependencies, std::uint32_t dependencyCount,
                                                                  const AnimationCookedContext &context);
    /** @brief Compares exact canonical dependency declarations, with no registry or cache authority. */
    [[nodiscard]] Result<void> ValidateDependencies(std::span<const Assets::AssetDependency> expected,
                                                    std::span<const Assets::AssetDependency> actual);
}  // namespace Horo::Animation::CookedInternal
