#pragma once

/**
 * @file SequenceAsset.h
 * @brief Versioned bounded sequence assets and backend-neutral cook admission.
 */

#include "Horo/Assets/AssetId.h"
#include "Horo/Cinematic/CinematicIdentity.h"
#include "Horo/Foundation/Result.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Cinematic {
    /** @brief Current persisted sequence source schema version. */
    struct SequenceSchemaVersion final {
        std::uint16_t major{1};
        std::uint16_t minor{};
        constexpr auto operator<=>(const SequenceSchemaVersion &) const noexcept = default;
    };

    inline constexpr SequenceSchemaVersion CurrentSequenceSchemaVersion{1, 0};

    /** @brief Compatibility classification used before decoding a sequence source. */
    enum class SequenceSchemaCompatibility : std::uint8_t {
        Exact,
        MigrationRequired,
        Unsupported
    };

    /** @brief Compile-time ceilings that hostile sequence input cannot raise. */
    struct SequenceSchemaHardLimits final {
        static constexpr std::size_t SourceBytes = 8U * 1024U * 1024U;
        static constexpr std::size_t JsonDepth = 32;
        static constexpr std::size_t NameBytes = 255;
        static constexpr std::size_t Tracks = 1'024;
        static constexpr std::size_t References = 4'096;
        static constexpr std::uint32_t KeysPerTrack = 65'536;
    };

    /** @brief Project-lowerable parsing limits bounded by SequenceSchemaHardLimits. */
    struct SequenceSchemaLimits final {
        std::size_t maximumSourceBytes{SequenceSchemaHardLimits::SourceBytes};
        std::size_t maximumJsonDepth{SequenceSchemaHardLimits::JsonDepth};
        std::size_t maximumNameBytes{SequenceSchemaHardLimits::NameBytes};
        std::size_t maximumTracks{SequenceSchemaHardLimits::Tracks};
        std::size_t maximumReferences{SequenceSchemaHardLimits::References};
        std::uint32_t maximumKeysPerTrack{SequenceSchemaHardLimits::KeysPerTrack};
        constexpr auto operator<=>(const SequenceSchemaLimits &) const noexcept = default;
    };

    /** @brief Exact rational authoring frame rate; no floating-point rate is persisted. */
    struct SequenceFrameRate final {
        std::uint32_t numerator{};
        std::uint32_t denominator{};
        constexpr auto operator<=>(const SequenceFrameRate &) const noexcept = default;
    };

    enum class SequenceLoopMode : std::uint8_t {
        Once,
        Loop,
        PingPong,
        Count
    };
    enum class SequenceClockSource : std::uint8_t {
        CommittedSimulation,
        UnscaledFixedControl,
        MonotonicWall,
        External,
        Count
    };
    enum class SequencePausePolicy : std::uint8_t {
        FollowGameplay,
        PlayerOnly,
        Count
    };
    enum class SequenceDilationPolicy : std::uint8_t {
        SourceNative,
        ApplyGameplayScale,
        Count
    };

    /** @brief Persisted playback defaults with independently validated clock policies. */
    struct SequencePlaybackSettings final {
        SequenceLoopMode loopMode{SequenceLoopMode::Once};
        SequenceClockSource clockSource{SequenceClockSource::CommittedSimulation};
        SequencePausePolicy pausePolicy{SequencePausePolicy::FollowGameplay};
        SequenceDilationPolicy dilationPolicy{SequenceDilationPolicy::SourceNative};
        bool pauseGameplay{}; /**< Requests a scoped host gameplay-pause token during playback. */
        bool hideHud{};       /**< Requests a scoped Runtime UI HUD suppression token during playback. */
        constexpr auto operator<=>(const SequencePlaybackSettings &) const noexcept = default;
    };

    enum class SequenceTrackType : std::uint8_t {
        Transform,
        Property,
        CameraCut,
        Event,
        Audio,
        SubSequence,
        Count
    };
    enum class SequenceReferenceKind : std::uint8_t {
        AudioClip,
        SubSequence,
        BindingDescriptor,
        Count
    };

    /** @brief One stable path-independent external dependency declared by a track. */
    struct SequenceAssetReference final {
        Assets::AssetId asset;
        SequenceReferenceKind kind{SequenceReferenceKind::AudioClip};
        constexpr auto operator<=>(const SequenceAssetReference &) const noexcept = default;
    };

    /** @brief Bounded track metadata required before curve payload delivery in CIN-001.4. */
    struct SequenceTrackSchema final {
        TrackId id;
        SequenceTrackType type{SequenceTrackType::Transform};
        std::uint32_t keyframeCount{};
        std::vector<SequenceAssetReference> references;
        bool operator==(const SequenceTrackSchema &) const = default;
    };

    /** @brief Complete mutable carrier accepted only by SequenceAsset::Create. */
    struct SequenceAssetData final {
        SequenceSchemaVersion version{CurrentSequenceSchemaVersion};
        Assets::AssetId asset;
        SequenceId sequence;
        std::string name;
        std::uint64_t durationFrames{};
        SequenceFrameRate frameRate;
        SequencePlaybackSettings playback;
        std::vector<SequenceTrackSchema> tracks;
        bool operator==(const SequenceAssetData &) const = default;
    };

    /** @brief Immutable validated sequence source value with no runtime or editor lifetime. */
    class SequenceAsset final {
    public:
        /**
         * @brief Validates and owns one sequence source value.
         * @param data Candidate schema data.
         * @param limits Active parser/schema ceilings.
         * @return Immutable asset or a typed version, duplicate, malformed, or limit error.
         */
        [[nodiscard]] static Result<SequenceAsset> Create(SequenceAssetData data, const SequenceSchemaLimits &limits = {});

        /** @brief Returns immutable validated data. @return Borrowed data owned by this asset. */
        [[nodiscard]] const SequenceAssetData &Data() const noexcept;

    private:
        explicit SequenceAsset(SequenceAssetData data) noexcept;
        SequenceAssetData data_;
    };

    /**
     * @brief Parses the canonical versioned JSON source format under strict finite limits.
     * @param source Complete UTF-8 JSON source bytes.
     * @param limits Active parser/schema ceilings.
     * @return Immutable asset or a typed malformed, duplicate, version, or limit error.
     */
    [[nodiscard]] Result<SequenceAsset> ParseSequenceAsset(std::string_view source, const SequenceSchemaLimits &limits = {});

    /** @brief Provider-neutral sequence cook tier. */
    enum class SequenceCookTier : std::uint8_t {
        Compact,
        Standard,
        Large,
        Count
    };

    /** @brief Exact finite limits selected for one sequence cook. */
    struct SequenceCookLimits final {
        std::uint32_t maximumTracks{};
        std::uint32_t maximumKeysPerTrack{};
        std::uint32_t maximumNestingDepth{};
        std::uint32_t maximumReferences{};
        constexpr auto operator<=>(const SequenceCookLimits &) const noexcept = default;
    };

    /** @brief Immutable typed cook profile; profile names never hide mutable defaults. */
    struct SequenceCookProfile final {
        SequenceCookTier tier{SequenceCookTier::Compact};
        SequenceCookLimits limits;
        constexpr auto operator<=>(const SequenceCookProfile &) const noexcept = default;
    };

    /** @brief Exact state captured from the Asset Registry/provider snapshot used by cook. */
    enum class SequenceReferenceAvailability : std::uint8_t {
        Available,
        Missing,
        Moved,
        Unloadable,
        TypeMismatch,
        Cycle,
        Count
    };

    /** @brief Resolved dependency evidence consumed only for one bounded cook invocation. */
    struct SequenceResolvedReference final {
        Assets::AssetId asset;
        SequenceReferenceKind kind{SequenceReferenceKind::AudioClip};
        SequenceReferenceAvailability availability{SequenceReferenceAvailability::Missing};
        std::uint32_t rootInclusiveNestingDepth{1};
        constexpr auto operator<=>(const SequenceResolvedReference &) const noexcept = default;
    };

    /** @brief Validated deterministic cook summary; it owns no artifacts, jobs, or provider leases. */
    struct SequenceCookPlan final {
        SequenceCookProfile profile;
        std::uint32_t trackCount{};
        std::uint64_t keyframeCount{};
        std::uint32_t referenceCount{};
        std::uint32_t rootInclusiveNestingDepth{1};
        constexpr auto operator<=>(const SequenceCookPlan &) const noexcept = default;
    };

    /**
     * @brief Classifies persisted schema compatibility without parsing payload fields.
     * @param version Persisted version to classify.
     * @return Exact for the current schema, MigrationRequired for older major-one schemas, otherwise Unsupported.
     */
    [[nodiscard]] constexpr SequenceSchemaCompatibility ClassifySequenceSchemaCompatibility(const SequenceSchemaVersion version) noexcept {
        if (version == CurrentSequenceSchemaVersion)
            return SequenceSchemaCompatibility::Exact;
        if (version.major == CurrentSequenceSchemaVersion.major && version.minor < CurrentSequenceSchemaVersion.minor)
            return SequenceSchemaCompatibility::MigrationRequired;
        return SequenceSchemaCompatibility::Unsupported;
    }

    /** @brief Returns the exact canonical profile for a known tier. @param tier Requested tier. @return Profile or typed malformed error.
     */
    [[nodiscard]] Result<SequenceCookProfile> GetSequenceCookProfile(SequenceCookTier tier);

    /**
     * @brief Validates one sequence and its exact resolved dependencies for deterministic cook admission.
     * @param asset Immutable validated sequence source.
     * @param profile Exact selected cook profile.
     * @param references Snapshot entries for every external reference; duplicate evidence is rejected.
     * @return Cook summary or a typed actionable tier/reference failure.
     * @note A move is admitted only after the Asset Registry publishes the same AssetId as Available in a new snapshot.
     */
    [[nodiscard]] Result<SequenceCookPlan> BuildSequenceCookPlan(const SequenceAsset &asset, const SequenceCookProfile &profile,
                                                                 std::span<const SequenceResolvedReference> references);
}  // namespace Horo::Cinematic
