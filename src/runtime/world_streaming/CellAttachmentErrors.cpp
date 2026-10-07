#include "Horo/WorldStreaming/CellAttachmentManifest.h"
#include "WorldStreamingErrorDescriptor.h"

namespace Horo::WorldStreaming::CellAttachmentErrors {
    using Internal::Describe;
    const ErrorCodeDescriptor Invalid =
        Describe("world_streaming.attachment.invalid", ErrorSeverity::Error, "Attachment membership or policy is malformed.",
                 "Supply complete exact references without duplicates or required-policy downgrades.", true);
    const ErrorCodeDescriptor Stale =
        Describe("world_streaming.attachment.stale", ErrorSeverity::Error, "Attachment content or attempt was replaced.",
                 "Capture the exact current content and provider revisions.", true);
    const ErrorCodeDescriptor CapacityExceeded =
        Describe("world_streaming.attachment.capacity_exceeded", ErrorSeverity::Error,
                 "Complete attachment membership exceeds admission limits.", "Supply an explicitly admitted bounded manifest.", true);
    const ErrorCodeDescriptor Unsupported = Describe("world_streaming.attachment.unsupported", ErrorSeverity::Error,
                                                     "A required feature implementation or schema is unsupported.",
                                                     "Compose the exact required feature provider before admission.", true);
    const ErrorCodeDescriptor NotReady =
        Describe("world_streaming.attachment.not_ready", ErrorSeverity::Info, "A required attachment is not prepared.",
                 "Retain the attempt until the owning provider has prepared its resources.", true);
    const ErrorCodeDescriptor Closed =
        Describe("world_streaming.attachment.closed", ErrorSeverity::Error, "Attachment admission or publication is closed.",
                 "Retire the attempt and admit a fresh owner if needed.", false);
}  // namespace Horo::WorldStreaming::CellAttachmentErrors
