#pragma once

/**
 * @file ExtensionAbi.h
 * @brief Versioned C ABI shared by native extension modules and the Horo host.
 *
 * All text and arrays are borrowed for the duration of the call that receives
 * them. Modules and hosts must not pass C++ objects, exceptions, or allocator
 * ownership across this boundary.
 */

#include <stdint.h>

#ifdef _WIN32
#ifdef __cplusplus
#define HORO_EXTENSION_EXPORT extern "C" __declspec(dllexport)
#else
#define HORO_EXTENSION_EXPORT __declspec(dllexport)
#endif
#else
#ifdef __cplusplus
#define HORO_EXTENSION_EXPORT extern "C" __attribute__((visibility("default")))
#else
#define HORO_EXTENSION_EXPORT __attribute__((visibility("default")))
#endif
#endif

enum {  // NOSONAR(cpp:S3642) C ABI constant group
    HORO_EXTENSION_ABI_VERSION = 1,
    HORO_EXTENSION_ABI_MINOR_VERSION = 1,
    HORO_ASSET_IMPORTER_ABI_VERSION = 1,
};

enum HoroExtensionStatusCode {  // NOSONAR(cpp:S3642) C ABI constants; wire values use uint32_t.
    HORO_EXTENSION_SUCCESS = 0,
    HORO_EXTENSION_ERROR_VERSION_MISMATCH = 1,
    HORO_EXTENSION_ERROR_INIT_FAILED = 2,
    HORO_EXTENSION_ERROR_INVALID_ARGS = 3,
    HORO_EXTENSION_ERROR_CANCELLED = 4,
    HORO_EXTENSION_ERROR_OUTPUT_REJECTED = 5,
};

typedef uint32_t HoroExtensionStatus;  // NOSONAR(cpp:S5416) Shared C11 ABI requires typedef.

/** @brief Borrowed byte-counted UTF-8 text; it need not be null-terminated. */
struct HoroExtensionStringView {
    const char *data;
    uint32_t length;
};
typedef struct HoroExtensionStringView HoroExtensionStringView;  // NOSONAR(cpp:S5416) Shared C11 ABI requires typedef.

/** @brief Host-owned byte output resized and filled by a module callback. */
struct HoroExtensionByteSink {
    void *context;
    HoroExtensionStatus (*resize)(void *context, uint64_t byteCount, uint8_t **outBytes);
};
typedef struct HoroExtensionByteSink HoroExtensionByteSink;  // NOSONAR(cpp:S5416) Shared C11 ABI requires typedef.

/** @brief Cooperative cancellation query valid only during one callback. */
struct HoroExtensionCancellation {
    const void *context;
    uint8_t (*isCancellationRequested)(const void *context);
};
typedef struct HoroExtensionCancellation HoroExtensionCancellation;  // NOSONAR(cpp:S5416) Shared C11 ABI requires typedef.

/** @brief Host-owned progress callback valid only during one importer invocation. */
struct HoroAssetImportProgressSink {
    void *context;
    HoroExtensionStatus (*report)(void *context, uint64_t completedUnits, uint64_t totalUnits, HoroExtensionStringView message);
};
typedef struct HoroAssetImportProgressSink HoroAssetImportProgressSink;  // NOSONAR(cpp:S5416) Shared C11 ABI requires typedef.

/** @brief Host-owned dependency callback valid only during one importer invocation. */
struct HoroAssetImportDependencySink {
    void *context;
    HoroExtensionStatus (*append)(void *context, HoroExtensionStringView assetId);
};
typedef struct HoroAssetImportDependencySink HoroAssetImportDependencySink;  // NOSONAR(cpp:S5416) Shared C11 ABI requires typedef.

enum HoroAssetImportDiagnosticSeverityCode {  // NOSONAR(cpp:S3642) C ABI constants; wire values use uint32_t.
    HORO_ASSET_IMPORT_DIAGNOSTIC_INFO = 0,
    HORO_ASSET_IMPORT_DIAGNOSTIC_WARNING = 1,
    HORO_ASSET_IMPORT_DIAGNOSTIC_ERROR = 2,
};

typedef uint32_t HoroAssetImportDiagnosticSeverity;  // NOSONAR(cpp:S5416) Shared C11 ABI requires typedef.

/** @brief One borrowed structured diagnostic emitted by an importer. */
struct HoroAssetImportDiagnostic {
    HoroAssetImportDiagnosticSeverity severity;
    HoroExtensionStringView code;
    HoroExtensionStringView message;
    int32_t line;
    uint8_t hasLine;
};
typedef struct HoroAssetImportDiagnostic HoroAssetImportDiagnostic;  // NOSONAR(cpp:S5416) Shared C11 ABI requires typedef.

/** @brief Host-owned structured diagnostic callback valid only during one importer invocation. */
struct HoroAssetImportDiagnosticSink {
    void *context;
    HoroExtensionStatus (*append)(void *context, const HoroAssetImportDiagnostic *diagnostic);
};
typedef struct HoroAssetImportDiagnosticSink HoroAssetImportDiagnosticSink;  // NOSONAR(cpp:S5416) Shared C11 ABI requires typedef.

// Unscoped enumerators are part of the source-compatible C ABI surface.
enum HoroAssetImportSettingKindCode {  // NOSONAR(cpp:S3642) C ABI constants; wire values use uint32_t.
    HORO_ASSET_IMPORT_SETTING_BOOLEAN = 0,
    HORO_ASSET_IMPORT_SETTING_INTEGER = 1,
    HORO_ASSET_IMPORT_SETTING_FLOAT = 2,
    HORO_ASSET_IMPORT_SETTING_TEXT = 3,
    HORO_ASSET_IMPORT_SETTING_CHOICE = 4,
};

typedef uint32_t HoroAssetImportSettingKind;  // NOSONAR(cpp:S5416) Shared C11 ABI requires typedef.

/** @brief One tagged import-setting value. */
struct HoroAssetImportSettingValue {
    HoroAssetImportSettingKind kind;
    uint8_t booleanValue;
    int64_t integerValue;
    double floatValue;
    uint64_t choiceIndex;
    HoroExtensionStringView textValue;
};
typedef struct HoroAssetImportSettingValue HoroAssetImportSettingValue;  // NOSONAR(cpp:S5416) Shared C11 ABI requires typedef.

/** @brief One declarative choice belonging to a choice setting. */
struct HoroAssetImportSettingChoice {
    HoroExtensionStringView id;
    HoroExtensionStringView labelKey;
    HoroAssetImportSettingValue value;
};
typedef struct HoroAssetImportSettingChoice HoroAssetImportSettingChoice;  // NOSONAR(cpp:S5416) Shared C11 ABI requires typedef.

/** @brief One host-rendered declarative importer setting. */
struct HoroAssetImportSettingDescriptor {
    HoroExtensionStringView id;
    HoroExtensionStringView labelKey;
    HoroExtensionStringView descriptionKey;
    HoroAssetImportSettingKind kind;
    HoroAssetImportSettingValue defaultValue;
    uint8_t hasMinimum;
    uint8_t hasMaximum;
    double minimum;
    double maximum;
    const HoroAssetImportSettingChoice *choices;
    uint32_t choiceCount;
    uint8_t includeInPresets;
};
typedef struct HoroAssetImportSettingDescriptor HoroAssetImportSettingDescriptor;  // NOSONAR(cpp:S5416) Shared C11 ABI requires typedef.

/** @brief Borrowed input for one external importer invocation. */
struct HoroAssetImportRequest {
    uint32_t structSize;
    const uint8_t *sourceBytes;
    uint64_t sourceByteCount;
    HoroExtensionStringView sourceExtension;
    const HoroAssetImportSettingValue *settings;
    uint32_t settingCount;
    HoroExtensionCancellation cancellation;
};
typedef struct HoroAssetImportRequest HoroAssetImportRequest;  // NOSONAR(cpp:S5416) Shared C11 ABI requires typedef.

/** @brief Host-owned output surfaces filled by one external importer invocation. */
struct HoroAssetImportResponse {
    uint32_t structSize;
    HoroExtensionStringView assetType;
    HoroExtensionByteSink editorPayload;
    /** @brief Append-only v1 output surfaces; modules must check structSize before using them. */
    HoroAssetImportDependencySink dependencies;
    HoroAssetImportDiagnosticSink diagnostics;
    HoroAssetImportProgressSink progress;
};
typedef struct HoroAssetImportResponse HoroAssetImportResponse;  // NOSONAR(cpp:S5416) Shared C11 ABI requires typedef.

/** @brief Borrowed input for one rendering-neutral preview invocation. */
struct HoroAssetPreviewRequest {
    uint32_t structSize;
    const uint8_t *editorPayload;
    uint64_t editorPayloadByteCount;
    HoroExtensionStringView absoluteAssetPath;
    HoroExtensionStringView assetType;
    uint32_t width;
    uint32_t height;
    HoroExtensionCancellation cancellation;
};
typedef struct HoroAssetPreviewRequest HoroAssetPreviewRequest;  // NOSONAR(cpp:S5416) Shared C11 ABI requires typedef.

/** @brief Host-owned tightly packed RGBA8 preview output. */
struct HoroAssetPreviewResponse {
    uint32_t structSize;
    uint32_t width;
    uint32_t height;
    HoroExtensionByteSink rgba8Pixels;
};
typedef struct HoroAssetPreviewResponse HoroAssetPreviewResponse;  // NOSONAR(cpp:S5416) Shared C11 ABI requires typedef.

typedef HoroExtensionStatus (*HoroAssetImportFunc)(  // NOSONAR(cpp:S5416) Shared C11 ABI requires typedef.
    void *importerContext, const HoroAssetImportRequest *request, HoroAssetImportResponse *response);
typedef HoroExtensionStatus (*HoroAssetPreviewFunc)(  // NOSONAR(cpp:S5416) Shared C11 ABI requires typedef.
    void *importerContext, const HoroAssetPreviewRequest *request, HoroAssetPreviewResponse *response);
typedef void (*HoroAssetImporterDestroyFunc)(void *importerContext);  // NOSONAR(cpp:S5416) Shared C11 ABI requires typedef.

/** @brief Complete versioned descriptor registered at the asset.importer extension point. */
struct HoroAssetImporterDescriptor {
    uint32_t structSize;
    uint32_t abiVersion;
    HoroExtensionStringView contributionId;
    HoroExtensionStringView contributionVersion;
    const HoroExtensionStringView *fileExtensions;
    uint32_t fileExtensionCount;
    const HoroExtensionStringView *assetTypes;
    uint32_t assetTypeCount;
    HoroExtensionStringView subfolderCategory;
    HoroExtensionStringView targetExtension;
    uint8_t supportsMetaSidecar;
    uint8_t previewFallback;
    const HoroAssetImportSettingDescriptor *settings;
    uint32_t settingCount;
    void *importerContext;
    HoroAssetImportFunc importAsset;
    HoroAssetPreviewFunc generatePreview;
    HoroAssetImporterDestroyFunc destroyImporter;
};
typedef struct HoroAssetImporterDescriptor HoroAssetImporterDescriptor;  // NOSONAR(cpp:S5416) Shared C11 ABI requires typedef.

/** @brief Host callback used only while the module load transaction is active. */
typedef HoroExtensionStatus (*HoroRegisterAssetImporterFunc)(  // NOSONAR(cpp:S5416) Shared C11 ABI requires typedef.
    void *hostContext, const HoroAssetImporterDescriptor *descriptor);

struct HoroExtensionHostApi {
    /** @brief Size of this struct for append-only ABI negotiation. */
    uint32_t structSize;
    uint32_t abiVersion;
    HoroExtensionStringView engineVersion;
    void *hostContext;
    HoroRegisterAssetImporterFunc registerAssetImporter;
    /** @brief Appended in 1.1; read only when structSize covers this field. */
    uint32_t abiMinorVersion;
    /** @brief Reserved for append-only negotiation; must be zero. */
    uint32_t reserved;
};
typedef struct HoroExtensionHostApi HoroExtensionHostApi;  // NOSONAR(cpp:S5416) Shared C11 ABI requires typedef.

struct HoroExtensionModuleApi {
    /** @brief Size of this struct for append-only ABI negotiation. */
    uint32_t structSize;
    void *moduleContext;
    HoroExtensionStringView moduleId;
    HoroExtensionStringView moduleVersion;
};
typedef struct HoroExtensionModuleApi HoroExtensionModuleApi;  // NOSONAR(cpp:S5416) Shared C11 ABI requires typedef.

enum {  // NOSONAR(cpp:S3642) C ABI function requirement bits.
    HORO_EXTENSION_REQUIRES_ASSET_IMPORTER = 1,
};

/**
 * @brief Inert requirements returned by the optional horo_extension_query entry point.
 *
 * The host initializes structSize to its writable capacity and all other fields
 * to zero. The module writes every field and returns its complete table size.
 * Major versions must match; minimumHostMinor must not exceed the host minor.
 * Unknown requirement bits and nonzero reserved fields are rejected. The query
 * must not allocate lifecycle state, register contributions, or retain pointers.
 * Without a query entry point, a module uses the legacy 1.0 bootstrap contract.
 */
struct HoroExtensionRequirements {
    uint32_t structSize;
    uint32_t abiMajorVersion;
    uint32_t minimumHostMinor;
    uint32_t requiredHostApiSize;
    uint32_t requiredFunctions;
    uint32_t reserved;
};
typedef struct HoroExtensionRequirements HoroExtensionRequirements;  // NOSONAR(cpp:S5416) Shared C11 ABI requires typedef.

/** @brief Populate requirements without receiving host capabilities; return a status, never throw. */
typedef HoroExtensionStatus (*HoroExtensionQueryFunc)(HoroExtensionRequirements *requirements);  // NOSONAR(cpp:S5416) Shared C11 ABI.

/**
 * @brief Standard native extension entry point.
 * @param host Host API valid only for the duration of this call.
 * @param outModule Module lifecycle state populated by the extension.
 * @return HORO_EXTENSION_SUCCESS when initialization and registrations succeed.
 */
typedef HoroExtensionStatus (*HoroExtensionLoadFunc)(  // NOSONAR(cpp:S5416) Shared C11 ABI requires typedef.
    const HoroExtensionHostApi *host, HoroExtensionModuleApi *outModule);

/**
 * @brief Optional native extension unload point.
 * @param extensionModule Module state originally returned by the load entry point.
 */
typedef void (*HoroExtensionUnloadFunc)(HoroExtensionModuleApi *extensionModule);  // NOSONAR(cpp:S5416) Shared C11 ABI requires typedef.
