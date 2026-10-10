#pragma once
/** @file AnimationGraphSource.h
 * @brief Bounded canonical animation graph source bytes, isolated from runtime evaluation.
 */
#include "Horo/Animation/AnimationGraph.h"

#include <string_view>

namespace Horo::Animation {
    /** @brief Independent source-parser and encoded-byte safety ceilings. */
    struct AnimationGraphSourceHardLimits final {
        static constexpr std::size_t Bytes = 4U * 1024U * 1024U;
        static constexpr std::size_t JsonValues = 1000000;
        static constexpr std::size_t Depth = 16;
        static constexpr std::size_t StringBytes = 64;
    };

    /** @brief Caller-lowerable source codec policy. */
    struct AnimationGraphSourceLimits final {
        std::size_t bytes{AnimationGraphSourceHardLimits::Bytes};
        std::size_t jsonValues{AnimationGraphSourceHardLimits::JsonValues};
        std::size_t depth{AnimationGraphSourceHardLimits::Depth};
    };
    /** @brief Explicit permission to upgrade legacy untyped connections during source decode. */
    enum class AnimationGraphSourceMigration : std::uint8_t {
        RequireCurrent,
        MigrateVersion1
    };
    /** @brief Serializes a valid current-schema candidate as canonical UTF-8 JSON with one trailing newline.
     * @param data Current typed source; stable asset identity is supplied by the sidecar and is not duplicated in bytes.
     * @param context Captured authoring limits, cancellation and replacement identity; runtime dependencies are not needed.
     * @param limits Encoded-byte and parser policy; hard ceilings cannot be raised.
     * @return Canonical complete bytes or stable graph failure. No files or registry state are mutated.
     * @throws std::bad_alloc When bounded load/tool-boundary storage cannot be allocated.
     */
    [[nodiscard]] Result<std::string> SerializeAnimationGraphSource(const AnimationGraphData &data,
                                                                    const AnimationGraphCompileContext &context = {},
                                                                    const AnimationGraphSourceLimits &limits = {});
    /** @brief Decodes detached graph source, rejecting duplicate/unknown keys, malformed identities and unsupported versions.
     * @param source Complete bounded UTF-8 JSON input; trailing whitespace is admitted, trailing data is rejected.
     * @param identity Exact sidecar-owned graph identity injected into the detached candidate.
     * @param context Captured authoring validation, cancellation and replacement identity.
     * @param limits Byte, nesting and parser-value limits applied before/between parsing work.
     * @param migration Explicit policy for version-1 connection type migration.
     * @return Fully validated canonical current-schema candidate or stable failure; no partial result or publication.
     * @throws std::bad_alloc When bounded load/tool-boundary storage cannot be allocated.
     */
    [[nodiscard]] Result<AnimationGraphData> DeserializeAnimationGraphSource(
        std::string_view source, AnimationGraphId identity, const AnimationGraphCompileContext &context = {},
        const AnimationGraphSourceLimits &limits = {},
        AnimationGraphSourceMigration migration = AnimationGraphSourceMigration::RequireCurrent);
}  // namespace Horo::Animation
