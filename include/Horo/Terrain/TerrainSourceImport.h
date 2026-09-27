#pragma once

/**
 * @file TerrainSourceImport.h
 * @brief Assets-owned, backend-neutral normalization of external terrain rasters into authoring source.
 */

#include "Horo/Assets/AssetId.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Terrain/TerrainDescriptor.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Horo::Terrain {
    namespace Detail {
        struct TerrainSourceRevisionTag;
    }

    /** @brief Non-zero semantic authoring revision, independent of cooked content and runtime revisions. */
    using TerrainSourceRevision = Foundation::Detail::NonZeroId64<Detail::TerrainSourceRevisionTag, TerrainErrors::IdentityInvalid>;

    /** @brief Exact external scalar encoding; container and codec details never enter runtime contracts. */
    enum class TerrainRasterFormat : std::uint8_t {
        RawU8,
        RawU16,
        RawF32,
        PngGray,
        External,
        Count
    };
    /** @brief Explicit multi-byte source order. */
    enum class TerrainByteOrder : std::uint8_t {
        Little,
        Big,
        Count
    };
    /** @brief Source row mapping into the canonical increasing-Z grid. */
    enum class TerrainRowOrder : std::uint8_t {
        IncreasingZ,
        DecreasingZ,
        Count
    };
    /** @brief Explicit coordinate interpretation; geographic coordinates require an upstream projection. */
    enum class TerrainCoordinateSpace : std::uint8_t {
        LocalMeters,
        ProjectedMeters,
        GeographicDegrees,
        Count
    };

    /** @brief Borrowed exact bytes and interpretation for one single-channel raster. */
    struct TerrainRasterInput final {
        TerrainRasterFormat format{TerrainRasterFormat::Count}; /**< Source scalar encoding. */
        std::span<const std::byte> bytes{};                     /**< Immutable bytes valid throughout NormalizeTerrainSource. */
        std::uint32_t width{};                                  /**< Sample columns. */
        std::uint32_t height{};                                 /**< Sample rows. */
        TerrainByteOrder byteOrder{TerrainByteOrder::Little};   /**< Explicit order for 16/32-bit formats. */
        TerrainRowOrder rowOrder{TerrainRowOrder::IncreasingZ}; /**< Source-to-canonical row mapping. */
        std::string formatId{};                                 /**< Stable registered format ID, required only for External. */
    };

    /** @brief Exact output shape claimed by an optional decoder before host allocation. */
    struct TerrainRasterDecodeInfo final {
        TerrainRasterFormat format{TerrainRasterFormat::Count}; /**< Must resolve to RawU8, RawU16, or RawF32. */
        std::uint64_t decodedBytes{};                           /**< Exact tightly packed output byte count. */
        TerrainByteOrder byteOrder{TerrainByteOrder::Little};   /**< Explicit decoded multi-byte order. */
    };

    /** @brief Optional host-pinned decoder contribution for external formats. */
    class ITerrainRasterDecoder {
    public:
        virtual ~ITerrainRasterDecoder() = default;

        /**
         * @brief Reports exact output shape without decoding or allocating output.
         * @param source Borrowed source raster and stable format ID.
         * @param cancellation Host-owned cancellation state.
         * @return Raw scalar format, byte order, and exact decoded byte count.
         */
        [[nodiscard]] virtual Result<TerrainRasterDecodeInfo> Probe(const TerrainRasterInput &source,
                                                                    const CancellationToken &cancellation) const = 0;

        /**
         * @brief Decodes one explicitly identified external raster into host-owned bounded storage.
         * @param source Borrowed source raster and stable format ID.
         * @param output Exact-sized writable output admitted and owned by the import operation.
         * @param cancellation Host-owned cancellation state.
         * @return Success or typed decoder error; never publishes source state.
         */
        [[nodiscard]] virtual Result<void> DecodeInto(const TerrainRasterInput &source, std::span<std::byte> output,
                                                      const CancellationToken &cancellation) const = 0;
    };

    /** @brief Finite meter-space placement and explicitly preserved projected-coordinate provenance. */
    struct TerrainSourceCoordinates final {
        TerrainCoordinateSpace space{TerrainCoordinateSpace::LocalMeters}; /**< No implicit reprojection. */
        std::string projectedCrs{};                                        /**< Required for projected meters; empty for local meters. */
        double originX{};                                                  /**< World-space first-column X in meters. */
        double originZ{};                                                  /**< World-space first-row Z in meters. */
        double spacingX{1.0};                                              /**< Positive X sample spacing in meters. */
        double spacingZ{1.0};                                              /**< Positive Z sample spacing in meters. */
        double heightScale{1.0};                                           /**< Meters per source height unit; must be positive. */
        double heightOffset{};                                             /**< Meter elevation added after scaling. */
        double maximumPrecisionError{0.01};                                /**< Maximum admitted float32 conversion error in meters. */
    };

    /** @brief Explicit finite work and allocation ceilings captured by one import invocation. */
    struct TerrainSourceImportLimits final {
        std::uint32_t maximumSamplesPerAxis{TerrainDescriptorHardLimits::SamplesPerAxis}; /**< Grid limit. */
        std::uint8_t maximumLayers{TerrainDescriptorHardLimits::LayersPerTile};           /**< Weight-layer limit. */
        std::uint64_t maximumSamples{TerrainDescriptorHardLimits::WorkItems};             /**< Total grid samples. */
        std::uint64_t maximumSourceBytes{TerrainDescriptorHardLimits::StagingBytes};      /**< Aggregate input bytes. */
        std::uint64_t maximumCanonicalBytes{TerrainDescriptorHardLimits::StagingBytes};   /**< Candidate owned bytes. */
        std::uint64_t maximumWorkItems{TerrainDescriptorHardLimits::WorkItems};           /**< Per-layer sample visits. */
    };

    /** @brief Fully explicit detached import request; an absent hole mask means all samples are solid. */
    struct TerrainSourceImportRequest final {
        TerrainDatasetId dataset{};                             /**< Stable dataset identity supplied by project metadata. */
        Assets::AssetId sourceAsset{};                          /**< Tracked canonical authoring asset identity supplied by Assets. */
        TerrainSourceRevision revision{};                       /**< New non-zero authoring revision. */
        TerrainCapabilityRevision capability{};                 /**< Exact captured import capability publication. */
        TerrainRasterInput height{};                            /**< Required U16 or F32 height raster. */
        std::vector<TerrainRasterInput> weights{};              /**< Optional U8/U16 layer rasters in authored layer order. */
        std::optional<TerrainRasterInput> holes{};              /**< Optional U8 binary visibility mask: zero solid, one hole. */
        TerrainSourceCoordinates coordinates{};                 /**< Exact coordinate and precision policy. */
        TerrainSourceImportLimits limits{};                     /**< Captured project/work limits. */
        std::shared_ptr<const ITerrainRasterDecoder> decoder{}; /**< Optional lifetime-pinned external decoder. */
    };

    /** @brief One validated, detached canonical authoring source candidate, never a runtime payload. */
    struct TerrainCanonicalSource final {
        TerrainDatasetId dataset{};             /**< Stable authored dataset identity. */
        Assets::AssetId sourceAsset{};          /**< Tracked canonical authoring asset identity. */
        TerrainSourceRevision revision{};       /**< Exact semantic source revision. */
        TerrainCapabilityRevision capability{}; /**< Exact captured import capability publication. */
        std::uint32_t width{};                  /**< Canonical increasing-X columns. */
        std::uint32_t height{};                 /**< Canonical increasing-Z rows. */
        TerrainSourceCoordinates coordinates{}; /**< Preserved finite meter-space placement and CRS. */
        std::vector<float> heightsMeters{};     /**< Canonical row-major meter elevations. */
        std::vector<std::uint16_t> weights{};   /**< Row-major interleaved layer weights; each pixel sums to 65535. */
        std::vector<std::uint8_t> holes{};      /**< Row-major binary hole values. */
        std::uint8_t layerCount{};              /**< Zero when there are no weight layers. */

        /** @brief Reports exact optional source features. @return Whether an imported hole mask is present. */
        [[nodiscard]] bool HasHoles() const noexcept {
            return !holes.empty();
        }

        /** @brief Reports exact optional source features. @return Whether imported weight layers are present. */
        [[nodiscard]] bool HasWeights() const noexcept {
            return layerCount != 0;
        }
    };

    namespace TerrainSourceErrors {
        extern const ErrorCodeDescriptor UnsupportedFormat;  /**< Format or scalar role is unsupported. */
        extern const ErrorCodeDescriptor InvalidDimensions;  /**< Mismatched, zero, or out-of-range grid shape. */
        extern const ErrorCodeDescriptor InvalidBytes;       /**< Truncated or excess raster bytes. */
        extern const ErrorCodeDescriptor InvalidCoordinates; /**< Non-finite/unsupported coordinate metadata. */
        extern const ErrorCodeDescriptor PrecisionLost;      /**< Requested float32 precision cannot be preserved. */
        extern const ErrorCodeDescriptor InvalidSample;      /**< NaN, infinity, or invalid binary/weight sample. */
        extern const ErrorCodeDescriptor LimitExceeded;      /**< Captured byte/sample/work ceiling exceeded. */
        extern const ErrorCodeDescriptor Cancelled;          /**< Caller cancelled before candidate completion. */
        extern const ErrorCodeDescriptor DecoderFailed;      /**< Optional decoder violated its typed result boundary. */
        extern const ErrorCodeDescriptor RevisionStale;      /**< Publication expectation or new revision is stale. */
        extern const ErrorCodeDescriptor Closed;             /**< Document owner no longer admits publication. */
    }  // namespace TerrainSourceErrors

    /**
     * @brief Decodes explicit raw rasters into one all-or-nothing canonical source candidate.
     * @param request Stable identity, source raster bytes, coordinate policy, and finite limits.
     * @param cancellation Borrowed cooperative cancellation state checked throughout bounded work.
     * @return Detached candidate or actionable typed error; no document or runtime state changes on failure.
     */
    [[nodiscard]] Result<TerrainCanonicalSource> NormalizeTerrainSource(const TerrainSourceImportRequest &request,
                                                                        const CancellationToken &cancellation);

    /** @brief Owner-thread authoring publication fence for explicit insert, replacement, and shutdown. */
    class TerrainSourceDocument final {
    public:
        /**
         * @brief Atomically publishes a fully normalized candidate against an exact current revision.
         * @param candidate Detached normalized source to transfer on success.
         * @param expectedCurrentRevision Absent for first insert; exact current revision for replacement.
         * @return Success or typed closed/stale result; failure preserves the prior source.
         */
        [[nodiscard]] Result<void> Publish(TerrainCanonicalSource candidate, std::optional<TerrainSourceRevision> expectedCurrentRevision);

        /** @brief Stops new publication while retaining the last source for caller-owned retirement. */
        void Close() noexcept {
            closed_ = true;
        }

        /** @brief Returns the current immutable source, if any. @return Borrowed source tied to this document lifetime. */
        [[nodiscard]] const TerrainCanonicalSource *Current() const noexcept {
            return source_ ? &*source_ : nullptr;
        }

        /** @brief Reports owner lifecycle. @return True after Close. */
        [[nodiscard]] bool IsClosed() const noexcept {
            return closed_;
        }

    private:
        std::optional<TerrainCanonicalSource> source_;
        bool closed_{false};
    };
}  // namespace Horo::Terrain
