#pragma once

/**
 * @file MixerAssetSchema.h
 * @brief Versioned backend-neutral mixer buses, routes, effects and migrations.
 */

#include "Horo/Audio/AudioFormat.h"
#include "Horo/Audio/AudioIdentity.h"
#include "Horo/Foundation/Result.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace Horo::Audio {
    /** @brief Compiled ceiling for buses in one persisted mixer asset. */
    inline constexpr std::size_t MaximumMixerAssetBuses = 256;
    /** @brief Compiled ceiling for primary and send routes in one persisted mixer asset. */
    inline constexpr std::size_t MaximumMixerAssetRoutes = 1'024;
    /** @brief Compiled ceiling for effect descriptors across one persisted mixer asset. */
    inline constexpr std::size_t MaximumMixerAssetEffects = 2'048;
    /** @brief Compiled byte ceiling for one persisted bus display label. */
    inline constexpr std::size_t MaximumMixerBusDisplayNameBytes = 255;

    /** @brief Persisted mixer schema version. */
    struct MixerAssetSchemaVersion final {
        std::uint16_t major{1};
        std::uint16_t minor{1};
        constexpr auto operator<=>(const MixerAssetSchemaVersion &) const noexcept = default;
    };

    inline constexpr MixerAssetSchemaVersion CurrentMixerAssetSchemaVersion{1, 1};

    /** @brief Compatibility classification made before migration or validation. */
    enum class MixerAssetSchemaCompatibility : std::uint8_t {
        Exact,
        MigrationRequired,
        Unsupported
    };

    /** @brief Project-lowerable limits that cannot exceed compiled mixer ceilings. */
    struct MixerAssetSchemaLimits final {
        std::size_t maximumBuses{MaximumMixerAssetBuses};
        std::size_t maximumRoutes{MaximumMixerAssetRoutes};
        std::size_t maximumEffects{MaximumMixerAssetEffects};
        std::size_t maximumBusDisplayNameBytes{MaximumMixerBusDisplayNameBytes};
        constexpr auto operator<=>(const MixerAssetSchemaLimits &) const noexcept = default;
    };

    /** @brief Semantic role of one persisted bus. */
    enum class MixerBusRole : std::uint8_t {
        MasterOutput,
        Bus,
        Return
    };

    /** @brief Runtime-affecting defaults authored for a bus. */
    struct MixerBusDefaults final {
        float gainDb{};
        bool muted{};
        bool paused{};
        bool operator==(const MixerBusDefaults &) const = default;
    };

    /** @brief Closed set of backend-neutral core effect descriptors. */
    enum class MixerEffectKind : std::uint8_t {
        Gain,
        LowPass,
        HighPass,
        Reverb
    };

    /** @brief Parameters for a gain effect. */
    struct MixerGainEffectParameters final {
        float gainDb{};
        bool operator==(const MixerGainEffectParameters &) const = default;
    };

    /** @brief Parameters shared by low-pass and high-pass effects. */
    struct MixerFilterEffectParameters final {
        float cutoffHz{};
        float q{1.0F};
        bool operator==(const MixerFilterEffectParameters &) const = default;
    };

    /** @brief Parameters for the bounded core reverb descriptor. */
    struct MixerReverbEffectParameters final {
        float decaySeconds{1.0F};
        float damping{0.5F};
        float mix{1.0F};
        bool operator==(const MixerReverbEffectParameters &) const = default;
    };

    /** @brief Closed typed parameter payload for a core mixer effect. */
    using MixerEffectParameters = std::variant<MixerGainEffectParameters, MixerFilterEffectParameters, MixerReverbEffectParameters>;

    /** @brief One ordered, stable-identity effect in a bus insert chain. */
    struct MixerEffectDescriptor final {
        AudioEffectId id;
        MixerEffectKind kind{MixerEffectKind::Gain};
        bool bypassed{};
        MixerEffectParameters parameters{MixerGainEffectParameters{}};
        bool operator==(const MixerEffectDescriptor &) const = default;
    };

    /** @brief One persisted bus with a stable identity and ordered effect chain. */
    struct MixerBusDescriptor final {
        AudioBusId id;
        MixerBusRole role{MixerBusRole::Bus};
        std::string displayName;
        AudioChannelLayout layout;
        MixerBusDefaults defaults;
        std::vector<MixerEffectDescriptor> effects;
        bool operator==(const MixerBusDescriptor &) const = default;
    };

    /** @brief Signal tap exposed by a send route. */
    enum class MixerSendTap : std::uint8_t {
        PostInsertPreFader,
        PostFader
    };

    /** @brief Distinguishes a rooted parent edge from an authored send edge. */
    enum class MixerRouteKind : std::uint8_t {
        Primary,
        Send
    };

    /** @brief One stable-identity bus-to-bus signal edge. */
    struct MixerRouteDescriptor final {
        AudioRouteId id;
        AudioBusId source;
        AudioBusId destination;
        MixerRouteKind kind{MixerRouteKind::Primary};
        MixerSendTap tap{MixerSendTap::PostFader};
        float gainDb{};
        bool enabled{true}; /**< Disabled routes remain persisted but still undergo structural validation. */
        bool operator==(const MixerRouteDescriptor &) const = default;
    };

    /** @brief Complete persisted mixer source data; array order is presentation only. */
    struct MixerAssetSchema final {
        MixerAssetSchemaVersion version{CurrentMixerAssetSchemaVersion};
        std::vector<MixerBusDescriptor> buses;
        std::vector<MixerRouteDescriptor> routes;
        bool operator==(const MixerAssetSchema &) const = default;
    };

    /**
     * @brief Classifies a persisted mixer schema without inspecting its payload.
     * @param version Persisted version to classify.
     * @return Exact, migration-required, or unsupported compatibility.
     */
    [[nodiscard]] constexpr MixerAssetSchemaCompatibility ClassifyMixerAssetSchemaCompatibility(
        const MixerAssetSchemaVersion version) noexcept {
        if (version == CurrentMixerAssetSchemaVersion)
            return MixerAssetSchemaCompatibility::Exact;
        if (version.major == CurrentMixerAssetSchemaVersion.major && version.minor < CurrentMixerAssetSchemaVersion.minor)
            return MixerAssetSchemaCompatibility::MigrationRequired;
        return MixerAssetSchemaCompatibility::Unsupported;
    }

    /**
     * @brief Creates the six-bus rooted mixer template used by a new project.
     * @return Detached mixer data with stable identities and deterministic primary routes.
     * @throws std::bad_alloc When constructing the template containers fails.
     */
    [[nodiscard]] MixerAssetSchema MakeDefaultMixerAsset();

    /**
     * @brief Validates project limits against immutable compiled mixer ceilings.
     * @param limits Candidate active limits.
     * @return Success or a stable limit error.
     */
    [[nodiscard]] Result<void> ValidateMixerAssetSchemaLimits(const MixerAssetSchemaLimits &limits);

    /**
     * @brief Validates one current-version mixer asset without mutation or name-based resolution.
     * @param asset Candidate persisted mixer data.
     * @param limits Active project-lowered limits.
     * @return Success or a stable version, limit, or structural error.
     */
    [[nodiscard]] Result<void> ValidateMixerAssetSchema(const MixerAssetSchema &asset, const MixerAssetSchemaLimits &limits = {});

    /**
     * @brief Migrates supported older mixer data to the current version transactionally.
     * @param source Persisted source; it is never mutated.
     * @param limits Active project-lowered limits applied to the migrated copy.
     * @return A validated current-version copy or a stable migration/version/structure error.
     */
    [[nodiscard]] Result<MixerAssetSchema> MigrateMixerAssetSchema(const MixerAssetSchema &source,
                                                                   const MixerAssetSchemaLimits &limits = {});
}  // namespace Horo::Audio
