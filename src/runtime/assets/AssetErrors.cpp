#include "AssetErrors.h"

namespace Horo::Assets::AssetErrors {
    namespace {
        const ErrorDomainId kDomain{"horo.asset"};
        constexpr auto kError = ErrorSeverity::Error;
        constexpr auto kWarning = ErrorSeverity::Warning;
    }  // namespace

    const ErrorCodeDescriptor IdentityInvalid{kDomain, ErrorCode{"asset.identity.invalid"}, kError,
                                              "Asset identity is not a canonical non-zero UUID.",
                                              "Restore the committed identity or explicitly re-import the asset."};
    const ErrorCodeDescriptor TypeInvalid{kDomain, ErrorCode{"asset.type.invalid"}, kError, "Asset type identity is invalid.",
                                          "Use a canonical lowercase dotted type ID."};
    const ErrorCodeDescriptor SidecarMissing{kDomain, ErrorCode{"asset.registry.sidecar_missing"}, kWarning,
                                             "A supported source asset has no identity sidecar.",
                                             "Import the source asset before referencing it."};
    const ErrorCodeDescriptor IdentityMissing{kDomain, ErrorCode{"asset.registry.identity_missing"}, kError,
                                              "An asset sidecar has no identity.", "Repair or re-import the asset."};
    const ErrorCodeDescriptor RegistryIdentityInvalid{kDomain, ErrorCode{"asset.registry.identity_invalid"}, kError,
                                                      "An asset sidecar identity is invalid.",
                                                      "Restore a canonical UUID or explicitly re-import the asset."};
    const ErrorCodeDescriptor SidecarMalformed{kDomain, ErrorCode{"asset.registry.sidecar_malformed"}, kError,
                                               "An asset sidecar is malformed.", "Repair or re-import the asset."};
    const ErrorCodeDescriptor SchemaUnsupported{kDomain, ErrorCode{"asset.registry.schema_unsupported"}, kError,
                                                "The asset metadata schema is unsupported.",
                                                "Use a compatible engine version or migrate the sidecar."};
    const ErrorCodeDescriptor SourceMissing{kDomain, ErrorCode{"asset.registry.source_missing"}, kError,
                                            "An asset sidecar has no source file.", "Restore or remove the orphan sidecar."};
    const ErrorCodeDescriptor DuplicateId{kDomain, ErrorCode{"asset.registry.duplicate_id"}, kError,
                                          "Two assets claim the same stable identity.", "Repair the conflicting sidecars."};
    const ErrorCodeDescriptor DuplicatePath{kDomain, ErrorCode{"asset.registry.duplicate_path"}, kError,
                                            "An asset path is registered more than once.", "Remove the duplicate entry."};
    const ErrorCodeDescriptor PathCollision{kDomain, ErrorCode{"asset.registry.path_collision"}, kError,
                                            "Asset paths collide under portable case folding.",
                                            "Rename one asset to a portable unique path."};
    const ErrorCodeDescriptor RootInvalid{kDomain, ErrorCode{"asset.registry.root_invalid"}, kError,
                                          "The project asset root is missing or unreadable.", "Open a valid project root."};
    const ErrorCodeDescriptor SymlinkAmbiguous{kDomain, ErrorCode{"asset.registry.symlink_ambiguous"}, kError,
                                               "Asset discovery encountered an ambiguous symbolic link.",
                                               "Replace it with a regular project-contained file."};
    const ErrorCodeDescriptor IndexMalformed{kDomain, ErrorCode{"asset.registry.index_malformed"}, kError,
                                             "The derived asset index is malformed.", "Rebuild it from committed sidecars."};
    const ErrorCodeDescriptor IndexIo{kDomain, ErrorCode{"asset.registry.index_io"}, kError,
                                      "The derived asset index could not be read or replaced.", "Check project metadata permissions."};
    const ErrorCodeDescriptor ProviderNotFound{kDomain, ErrorCode{"asset.provider.not_found"}, kError, "The cooked asset is unavailable.",
                                               "Cook or mount the requested asset."};
    const ErrorCodeDescriptor ProviderTooLarge{kDomain, ErrorCode{"asset.provider.too_large"}, kError,
                                               "The cooked asset exceeds the provider limit.",
                                               "Increase the explicit limit or reduce the artifact."};
    const ErrorCodeDescriptor ProviderReadFailed{kDomain, ErrorCode{"asset.provider.read_failed"}, kError,
                                                 "The cooked asset could not be read completely.",
                                                 "Validate or rebuild the cooked artifact."};
    const ErrorCodeDescriptor LoadCancelled{kDomain, ErrorCode{"asset.load.cancelled"}, kError, "The asset load was cancelled.",
                                            "Retry if the owning operation is active."};
    const ErrorCodeDescriptor LoadNotReady{kDomain, ErrorCode{"asset.load.not_ready"}, kError, "The asset load has not completed.",
                                           "Poll or wait before taking its result."};
    const ErrorCodeDescriptor LoadConsumed{kDomain, ErrorCode{"asset.load.consumed"}, kError, "The asset load result was already consumed.",
                                           "Retain the consumed payload."};
    const ErrorCodeDescriptor LoadQueueFull{kDomain, ErrorCode{"asset.load.queue_full"}, kError, "The asset load queue is at capacity.",
                                            "Retry after existing loads complete."};
    const ErrorCodeDescriptor LoadShutdown{kDomain, ErrorCode{"asset.load.shutdown"}, kError, "The asset load service is shutting down.",
                                           "Do not submit new work."};
}  // namespace Horo::Assets::AssetErrors

namespace Horo::Assets::CookErrors {
    namespace {
        const ErrorDomainId kCookDomain{"horo.asset"};
        constexpr auto kCookError = ErrorSeverity::Error;
    }  // namespace

    const ErrorCodeDescriptor UnsupportedFormat{kCookDomain, ErrorCode{"asset.cook.unsupported_format"}, kCookError,
                                                "Cooked artifact format version is unsupported.",
                                                "Use a compatible engine version or re-cook the asset."};
    const ErrorCodeDescriptor MalformedArtifact{kCookDomain, ErrorCode{"asset.cook.malformed_artifact"}, kCookError,
                                                "Cooked artifact is malformed.", "Re-cook the asset from its authoritative source."};
    const ErrorCodeDescriptor TooLarge{kCookDomain, ErrorCode{"asset.cook.too_large"}, kCookError,
                                       "Cooked artifact exceeds the configured size limit.",
                                       "Increase the limit or reduce the source asset."};
    const ErrorCodeDescriptor HashMismatch{kCookDomain, ErrorCode{"asset.cook.hash_mismatch"}, kCookError,
                                           "Cooked artifact payload digest does not match.",
                                           "Re-cook the asset from its authoritative source."};
    const ErrorCodeDescriptor CookerMissing{kCookDomain, ErrorCode{"asset.cook.cooker_missing"}, kCookError,
                                            "No cooker contribution is registered for this asset type and target.",
                                            "Install or enable a matching cooker extension."};
    const ErrorCodeDescriptor DuplicateCooker{kCookDomain, ErrorCode{"asset.cook.duplicate_cooker"}, kCookError,
                                              "Two cooker contributions claim the same asset type and target.",
                                              "Resolve the conflict in project policy or disable one contribution."};
    const ErrorCodeDescriptor CatalogSealed{kCookDomain, ErrorCode{"asset.cook.catalog_sealed"}, kCookError,
                                            "The cooker catalog is already sealed.", "Register cookers before sealing the catalog."};
    const ErrorCodeDescriptor Cancelled{kCookDomain, ErrorCode{"asset.cook.cancelled"}, kCookError, "The cook operation was cancelled.",
                                        "Retry if the owning operation is still active."};
    const ErrorCodeDescriptor DependencyUnsupported{kCookDomain, ErrorCode{"asset.cook.dependency_unsupported"}, kCookError,
                                                    "The cook operation does not support non-empty dependency sets in this slice.",
                                                    "Remove dependencies or use a dependency-aware cook target."};
    const ErrorCodeDescriptor SourceReadFailed{kCookDomain, ErrorCode{"asset.cook.source_read_failed"}, kCookError,
                                               "The source asset could not be read.", "Verify the source file exists and is readable."};
    const ErrorCodeDescriptor OutputIdentityExhausted{kCookDomain, ErrorCode{"asset.cook.output_identity_exhausted"}, kCookError,
                                                      "The build-output session identity space is exhausted.",
                                                      "Restart the owning project session before submitting more cook work."};
}  // namespace Horo::Assets::CookErrors

namespace Horo::Assets::ImportErrors {
    namespace {
        const ErrorDomainId kImportDomain{"horo.asset"};
        constexpr auto kImportError = ErrorSeverity::Error;
    }  // namespace

    const ErrorCodeDescriptor NoImporter{kImportDomain, ErrorCode{"asset.import.no_importer"}, kImportError,
                                         "No importer registered for this file extension.",
                                         "Install or enable a matching importer extension."};
    const ErrorCodeDescriptor ObjNoVertices{kImportDomain, ErrorCode{"asset.import.obj.no_vertices"}, kImportError,
                                            "OBJ file contains no vertex positions.", "Ensure the file has valid vertex data."};
    const ErrorCodeDescriptor FbxMalformed{kImportDomain, ErrorCode{"asset.import.fbx.malformed"}, kImportError,
                                           "FBX mesh geometry is missing, malformed, or uses an unsupported array encoding.",
                                           "Export the source as a standard ASCII or binary FBX mesh and retry."};
    const ErrorCodeDescriptor ImportCancelled{kImportDomain, ErrorCode{"asset.import.cancelled"}, kImportError,
                                              "The import operation was cancelled.", "Retry if the owning operation is still active."};
    const ErrorCodeDescriptor ObjParseWarning{kImportDomain, ErrorCode{"asset.import.obj.warning"}, ErrorSeverity::Warning,
                                              "OBJ parse warning.", "Review the import diagnostics."};
}  // namespace Horo::Assets::ImportErrors
