#include "Horo/Terrain/TerrainSourceImport.h"

namespace Horo::Terrain::TerrainSourceErrors {
    namespace {
        const ErrorDomainId Domain{"horo.terrain.import"};
    }

    const ErrorCodeDescriptor UnsupportedFormat{.domain = Domain,
                                                .code = ErrorCode{"terrain.import.unsupported_format"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "The terrain raster format is unsupported for this channel.",
                                                .remediationHint = "Convert height to explicit RAW U16/F32, weights to RAW U8/U16, and "
                                                                   "holes to binary RAW U8; supply byte and row order.",
                                                .retryable = false,
                                                .userActionable = true,
                                                .deprecatedBy = std::nullopt};
    const ErrorCodeDescriptor
        InvalidDimensions{.domain = Domain,
                          .code = ErrorCode{"terrain.import.invalid_dimensions"},
                          .defaultSeverity = ErrorSeverity::Error,
                          .summary = "Terrain source dimensions are empty, mismatched, or exceed the grid ceiling.",
                          .remediationHint = "Use one equal-sized grid of at least two samples per axis within captured project limits.",
                          .retryable = false,
                          .userActionable = true,
                          .deprecatedBy = std::nullopt};
    const ErrorCodeDescriptor
        InvalidBytes{.domain = Domain,
                     .code = ErrorCode{"terrain.import.invalid_bytes"},
                     .defaultSeverity = ErrorSeverity::Error,
                     .summary = "Terrain raster byte length does not match its declared shape and scalar encoding.",
                     .remediationHint = "Provide exact tightly packed single-channel bytes without a header, padding, or trailing data.",
                     .retryable = false,
                     .userActionable = true,
                     .deprecatedBy = std::nullopt};
    const ErrorCodeDescriptor InvalidCoordinates{.domain = Domain,
                                                 .code = ErrorCode{"terrain.import.invalid_coordinates"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary =
                                                     "Terrain coordinate metadata is incomplete, non-finite, or requires reprojection.",
                                                 .remediationHint =
                                                     "Provide finite meter coordinates and positive spacing/scale; project geographic "
                                                     "degrees upstream and name a projected EPSG CRS.",
                                                 .retryable = false,
                                                 .userActionable = true,
                                                 .deprecatedBy = std::nullopt};
    const ErrorCodeDescriptor PrecisionLost{.domain = Domain,
                                            .code = ErrorCode{"terrain.import.precision_lost"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "Canonical float32 meters exceed the requested precision error.",
                                            .remediationHint =
                                                "Adjust source units/offset or explicitly allow a larger finite meter error before import.",
                                            .retryable = false,
                                            .userActionable = true,
                                            .deprecatedBy = std::nullopt};
    const ErrorCodeDescriptor InvalidSample{.domain = Domain,
                                            .code = ErrorCode{"terrain.import.invalid_sample"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary =
                                                "Terrain raster contains a non-finite height, empty weight pixel, or non-binary hole.",
                                            .remediationHint = "Replace no-data/NaN/inf heights, supply positive weight at each pixel, "
                                                               "and encode holes as exact zero or one.",
                                            .retryable = false,
                                            .userActionable = true,
                                            .deprecatedBy = std::nullopt};
    const ErrorCodeDescriptor
        LimitExceeded{.domain = Domain,
                      .code = ErrorCode{"terrain.import.limit_exceeded"},
                      .defaultSeverity = ErrorSeverity::Error,
                      .summary = "Terrain source samples, bytes, layers, or decode work exceed captured limits.",
                      .remediationHint = "Split or reduce the source, or select a compatible larger project import budget explicitly.",
                      .retryable = false,
                      .userActionable = true,
                      .deprecatedBy = std::nullopt};
    const ErrorCodeDescriptor Cancelled{.domain = Domain,
                                        .code = ErrorCode{"terrain.import.cancelled"},
                                        .defaultSeverity = ErrorSeverity::Warning,
                                        .summary = "Terrain source normalization was cancelled before publication.",
                                        .remediationHint = "Start a new import operation when the source is still needed.",
                                        .retryable = true,
                                        .userActionable = false,
                                        .deprecatedBy = std::nullopt};
    const ErrorCodeDescriptor DecoderFailed{.domain = Domain,
                                            .code = ErrorCode{"terrain.import.decoder_failed"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "An optional terrain raster decoder failed outside its typed result contract.",
                                            .remediationHint =
                                                "Disable or repair the exact importer contribution and retry with a supported source.",
                                            .retryable = false,
                                            .userActionable = true,
                                            .deprecatedBy = std::nullopt};
    const ErrorCodeDescriptor
        RevisionStale{.domain = Domain,
                      .code = ErrorCode{"terrain.import.revision_stale"},
                      .defaultSeverity = ErrorSeverity::Warning,
                      .summary = "Terrain source changed since import began or the candidate revision is not newer.",
                      .remediationHint =
                          "Reimport against the current authoring revision; do not overwrite intervening sculpt or paint edits.",
                      .retryable = true,
                      .userActionable = true,
                      .deprecatedBy = std::nullopt};
    const ErrorCodeDescriptor Closed{.domain = Domain,
                                     .code = ErrorCode{"terrain.import.closed"},
                                     .defaultSeverity = ErrorSeverity::Warning,
                                     .summary = "The terrain authoring document is closed to publication.",
                                     .remediationHint = "Open a new document owner before importing.",
                                     .retryable = false,
                                     .userActionable = false,
                                     .deprecatedBy = std::nullopt};
}  // namespace Horo::Terrain::TerrainSourceErrors
