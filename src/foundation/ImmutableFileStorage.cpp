#include "Horo/Foundation/ImmutableFileStorage.h"

namespace Horo::FileStorageErrors {
    namespace {
        /** @brief Defines stable storage failures without native object names. */
        ErrorCodeDescriptor Descriptor(const char *code, const char *summary) {
            return {ErrorDomainId{"horo.foundation.file_storage"}, ErrorCode{code}, ErrorSeverity::Error, summary,
                    "Use a current host-owned storage capability and bounded immutable content."};
        }
    }  // namespace

    const ErrorCodeDescriptor InvalidPath =
        Descriptor("file_storage.invalid_path", "The immutable storage path is not portable and relative.");
    const ErrorCodeDescriptor UnsafeObject =
        Descriptor("file_storage.unsafe_object", "The immutable storage object is redirected or unsafe.");
    const ErrorCodeDescriptor Capacity = Descriptor("file_storage.capacity", "The immutable file exceeds a finite storage limit.");
    const ErrorCodeDescriptor Cancelled = Descriptor("file_storage.cancelled", "The immutable storage operation was cancelled.");
    const ErrorCodeDescriptor Conflict = Descriptor("file_storage.conflict", "The immutable file already contains different bytes.");
    const ErrorCodeDescriptor IoFailed =
        Descriptor("file_storage.io_failed", "The immutable storage operation could not complete consistently.");
}  // namespace Horo::FileStorageErrors

namespace Horo {
    /** @copydoc ImmutableFileStorageErrorDomain */
    ModuleErrorDomainDescriptor ImmutableFileStorageErrorDomain() {
        return {ErrorDomainId{"horo.foundation.file_storage"},
                {&FileStorageErrors::InvalidPath, &FileStorageErrors::UnsafeObject, &FileStorageErrors::Capacity,
                 &FileStorageErrors::Cancelled, &FileStorageErrors::Conflict, &FileStorageErrors::IoFailed}};
    }
}  // namespace Horo
