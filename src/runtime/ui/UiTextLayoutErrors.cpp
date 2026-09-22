#include "Horo/Runtime/Ui/UiErrors.h"

namespace Horo::Runtime::Ui::UiErrors {
    namespace {
        const ErrorDomainId UiDomain{"horo.runtime_ui"};
    }
}  // namespace Horo::Runtime::Ui::UiErrors

namespace Horo::Runtime::Ui::UiErrors {
    /** @copydoc TextLayoutInputInvalid */
    const ErrorCodeDescriptor
        TextLayoutInputInvalid{UiDomain,
                               ErrorCode{"runtime_ui.text_layout.input_invalid"},
                               ErrorSeverity::Error,
                               "The Runtime UI text layout input or result is invalid.",
                               "Provide complete shaped cluster evidence, closed text policies, and finite logical bounds.",
                               false,
                               true};
    /** @copydoc TextLayoutSourceStale */
    const ErrorCodeDescriptor TextLayoutSourceStale{UiDomain,
                                                    ErrorCode{"runtime_ui.text_layout.source_stale"},
                                                    ErrorSeverity::Error,
                                                    "The Runtime UI text layout source belongs to another or older generation.",
                                                    "Prepare text from the active element, tree, content, intrinsic, and policy revisions.",
                                                    true,
                                                    false};
    /** @copydoc TextLayoutCapacityExceeded */
    const ErrorCodeDescriptor
        TextLayoutCapacityExceeded{UiDomain,
                                   ErrorCode{"runtime_ui.text_layout.capacity_exceeded"},
                                   ErrorSeverity::Error,
                                   "The Runtime UI text layout candidate exceeds its fixed capacity.",
                                   "Reduce source text, line, glyph, or face-run count within the declared owner limits.",
                                   true,
                                   false};
    /** @copydoc TextLayoutEllipsisInvalid */
    const ErrorCodeDescriptor
        TextLayoutEllipsisInvalid{UiDomain,
                                  ErrorCode{"runtime_ui.text_layout.ellipsis_invalid"},
                                  ErrorSeverity::Error,
                                  "The Runtime UI text layout needs a valid pre-shaped ellipsis view.",
                                  "Shape the declared ellipsis with the same font and content generation before layout.",
                                  false,
                                  true};
    /** @copydoc TextLayoutStorageExhausted */
    const ErrorCodeDescriptor
        TextLayoutStorageExhausted{UiDomain,
                                   ErrorCode{"runtime_ui.text_layout.storage_exhausted"},
                                   ErrorSeverity::Error,
                                   "Every bounded Runtime UI text layout result slot is still leased.",
                                   "Retire an in-flight text layout result before retrying; never overwrite or allocate fallback storage.",
                                   true,
                                   false};
    /** @copydoc TextLayoutLifecycleUnavailable */
    const ErrorCodeDescriptor
        TextLayoutLifecycleUnavailable{UiDomain,
                                       ErrorCode{"runtime_ui.text_layout.lifecycle_unavailable"},
                                       ErrorSeverity::Error,
                                       "The Runtime UI text layout engine is closed.",
                                       "Create a new text layout engine for the active element generation before submitting work.",
                                       false,
                                       false};
    /** @copydoc GlyphAtlasInputInvalid */
    const ErrorCodeDescriptor
        GlyphAtlasInputInvalid{UiDomain,
                               ErrorCode{"runtime_ui.glyph_atlas.input_invalid"},
                               ErrorSeverity::Error,
                               "The Runtime UI glyph-atlas input is invalid.",
                               "Provide a valid face/glyph variant, page tile, raster payload, and generation evidence.",
                               false,
                               true};
    /** @copydoc GlyphAtlasSourceStale */
    const ErrorCodeDescriptor GlyphAtlasSourceStale{UiDomain,
                                                    ErrorCode{"runtime_ui.glyph_atlas.source_stale"},
                                                    ErrorSeverity::Error,
                                                    "The Runtime UI glyph-atlas identity belongs to an older generation.",
                                                    "Resolve the glyph against the current atlas revision and upload generation.",
                                                    true,
                                                    false};
    /** @copydoc GlyphAtlasCapacityExceeded */
    const ErrorCodeDescriptor GlyphAtlasCapacityExceeded{UiDomain,
                                                         ErrorCode{"runtime_ui.glyph_atlas.capacity_exceeded"},
                                                         ErrorSeverity::Error,
                                                         "The Runtime UI glyph-atlas representation exceeds a fixed bound.",
                                                         "Reduce page, tile, raster, frame, or entry bounds before creating the atlas.",
                                                         false,
                                                         true};
    /** @copydoc GlyphAtlasPressure */
    const ErrorCodeDescriptor GlyphAtlasPressure{UiDomain,
                                                 ErrorCode{"runtime_ui.glyph_atlas.pressure"},
                                                 ErrorSeverity::Warning,
                                                 "No unpinned glyph-atlas tile is available for the request.",
                                                 "Retire in-flight frames or apply a bounded eviction pass before retrying.",
                                                 true,
                                                 false};
    /** @copydoc GlyphAtlasFallbackUnavailable */
    const ErrorCodeDescriptor
        GlyphAtlasFallbackUnavailable{UiDomain,
                                      ErrorCode{"runtime_ui.glyph_atlas.fallback_unavailable"},
                                      ErrorSeverity::Error,
                                      "The Runtime UI terminal fallback glyph is not resident.",
                                      "Upload and complete the declared fallback glyph before rendering missing content.",
                                      true,
                                      true};
    /** @copydoc GlyphAtlasUploadInvalid */
    const ErrorCodeDescriptor GlyphAtlasUploadInvalid{UiDomain,
                                                      ErrorCode{"runtime_ui.glyph_atlas.upload_invalid"},
                                                      ErrorSeverity::Error,
                                                      "The Runtime UI glyph-atlas upload descriptor or payload is invalid.",
                                                      "Provide a tile-sized payload with matching format, stride, and byte count.",
                                                      false,
                                                      true};
    /** @copydoc GlyphAtlasUploadCapacityExceeded */
    const ErrorCodeDescriptor GlyphAtlasUploadCapacityExceeded{UiDomain,
                                                               ErrorCode{"runtime_ui.glyph_atlas.upload_capacity_exceeded"},
                                                               ErrorSeverity::Error,
                                                               "The bounded Runtime UI glyph-atlas upload queue is full.",
                                                               "Retire or discard terminal uploads and reduce the pending byte burst.",
                                                               true,
                                                               false};
    /** @copydoc GlyphAtlasUploadStale */
    const ErrorCodeDescriptor GlyphAtlasUploadStale{UiDomain,
                                                    ErrorCode{"runtime_ui.glyph_atlas.upload_stale"},
                                                    ErrorSeverity::Error,
                                                    "The Runtime UI glyph-atlas upload identity is stale or foreign.",
                                                    "Discard the old upload identity and request work for the current atlas generation.",
                                                    true,
                                                    false};
    /** @copydoc GlyphAtlasUploadInvalidTransition */
    const ErrorCodeDescriptor
        GlyphAtlasUploadInvalidTransition{UiDomain,
                                          ErrorCode{"runtime_ui.glyph_atlas.upload_invalid_transition"},
                                          ErrorSeverity::Error,
                                          "The glyph-atlas upload cannot perform that lifecycle transition.",
                                          "Follow Pending, Submitted, Ready/Failed/Cancelled, Retired, and Discard ordering.",
                                          false,
                                          false};
    /** @copydoc GlyphAtlasFrameInvalid */
    const ErrorCodeDescriptor GlyphAtlasFrameInvalid{UiDomain,
                                                     ErrorCode{"runtime_ui.glyph_atlas.frame_invalid"},
                                                     ErrorSeverity::Error,
                                                     "The Runtime UI glyph-atlas frame identity is invalid.",
                                                     "Use an active frame issued by the same atlas owner.",
                                                     false,
                                                     false};
    /** @copydoc GlyphAtlasFrameCapacityExceeded */
    const ErrorCodeDescriptor GlyphAtlasFrameCapacityExceeded{UiDomain,
                                                              ErrorCode{"runtime_ui.glyph_atlas.frame_capacity_exceeded"},
                                                              ErrorSeverity::Error,
                                                              "A glyph-atlas frame exceeded its fixed distinct-use bound.",
                                                              "Reduce visible glyph demand or increase the load-time frame-use limit.",
                                                              false,
                                                              true};
    /** @copydoc GlyphAtlasFrameInFlight */
    const ErrorCodeDescriptor GlyphAtlasFrameInFlight{UiDomain,
                                                      ErrorCode{"runtime_ui.glyph_atlas.frame_in_flight"},
                                                      ErrorSeverity::Error,
                                                      "Every configured glyph-atlas frame slot remains in flight.",
                                                      "Retire a completed frame before admitting another frame pin set.",
                                                      true,
                                                      false};
    /** @copydoc GlyphAtlasEvictionInvalid */
    const ErrorCodeDescriptor GlyphAtlasEvictionInvalid{UiDomain,
                                                        ErrorCode{"runtime_ui.glyph_atlas.eviction_invalid"},
                                                        ErrorSeverity::Error,
                                                        "The glyph-atlas eviction request is invalid.",
                                                        "Use a positive bounded eviction count owned by the Runtime UI atlas.",
                                                        false,
                                                        true};
    /** @copydoc GlyphAtlasResetInvalid */
    const ErrorCodeDescriptor GlyphAtlasResetInvalid{UiDomain,
                                                     ErrorCode{"runtime_ui.glyph_atlas.reset_invalid"},
                                                     ErrorSeverity::Error,
                                                     "The glyph-atlas reset reason or generation cannot be represented.",
                                                     "Use Reload or DeviceLost at a valid owner safe point.",
                                                     false,
                                                     true};
    /** @copydoc GlyphAtlasResetBusy */
    const ErrorCodeDescriptor GlyphAtlasResetBusy{UiDomain,
                                                  ErrorCode{"runtime_ui.glyph_atlas.reset_busy"},
                                                  ErrorSeverity::Error,
                                                  "The glyph-atlas generation still has frame or submitted-upload leases.",
                                                  "Retire every in-flight frame and submitted upload before resetting the generation.",
                                                  true,
                                                  false};
    /** @copydoc GlyphAtlasLifecycleUnavailable */
    const ErrorCodeDescriptor GlyphAtlasLifecycleUnavailable{UiDomain,
                                                             ErrorCode{"runtime_ui.glyph_atlas.lifecycle_unavailable"},
                                                             ErrorSeverity::Error,
                                                             "The Runtime UI glyph atlas is closed.",
                                                             "Create a new atlas for the active Runtime UI owner generation.",
                                                             false,
                                                             false};
    /** @copydoc GlyphAtlasUploadInFlight */
    const ErrorCodeDescriptor GlyphAtlasUploadInFlight{UiDomain,
                                                       ErrorCode{"runtime_ui.glyph_atlas.upload_in_flight"},
                                                       ErrorSeverity::Error,
                                                       "Submitted glyph-atlas upload work still retains a renderer lease.",
                                                       "Publish completion or retire the submitted work before reset or shutdown drain.",
                                                       true,
                                                       false};
}  // namespace Horo::Runtime::Ui::UiErrors
