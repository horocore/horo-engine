#include "ExternalAssetImporter.h"

#include "ExternalAssetImporterConversion.h"
#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Foundation/Logging/Logger.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace Horo::Extensions {
    namespace {
        constexpr std::uint32_t kMaxTextBytes = 4096;
        constexpr std::uint32_t kMaxListEntries = 256;
        constexpr std::size_t kMaxImportDependencies = 16384;
        constexpr std::size_t kMaxImportDiagnostics = 1024;
        constexpr std::uint64_t kMaxPayloadBytes = 1024ULL * 1024ULL * 1024ULL;
        constexpr std::uint32_t kMaxPreviewDimension = 4096;

        using Detail::ConvertExternalImportSetting;
        using Detail::CopyExternalText;

        [[nodiscard]] bool IsLowerExtension(const std::string &value) {
            return !value.empty() && std::ranges::all_of(value, [](const unsigned char character) {
                return std::isdigit(character) != 0 || std::islower(character) != 0 || character == '_' || character == '-';
            });
        }

        [[nodiscard]] HoroAssetImportSettingValue ToAbiValue(const Assets::ImportSettingValue &value) {
            HoroAssetImportSettingValue result{};
            std::visit([&result]<typename T>(const T &typed) {
                if constexpr (std::is_same_v<T, bool>) {
                    result.kind = HORO_ASSET_IMPORT_SETTING_BOOLEAN;
                    result.booleanValue = static_cast<std::uint8_t>(typed);
                } else if constexpr (std::is_same_v<T, std::int64_t>) {
                    result.kind = HORO_ASSET_IMPORT_SETTING_INTEGER;
                    result.integerValue = typed;
                } else if constexpr (std::is_same_v<T, double>) {
                    result.kind = HORO_ASSET_IMPORT_SETTING_FLOAT;
                    result.floatValue = typed;
                } else if constexpr (std::is_same_v<T, std::string>) {
                    result.kind = HORO_ASSET_IMPORT_SETTING_TEXT;
                    result.textValue = {typed.data(), static_cast<std::uint32_t>(typed.size())};
                } else {
                    result.kind = HORO_ASSET_IMPORT_SETTING_CHOICE;
                    result.choiceIndex = typed;
                }
            }, value);
            return result;
        }

        struct ExternalImportOutputState final {
            Assets::PreparedAssetImport *prepared{};
            const Assets::AssetImportProgressSink *progress{};
            std::unordered_set<Assets::AssetId, Assets::AssetIdHash> dependencyIds;
            bool rejected{};
        };

        void RejectOutput(ExternalImportOutputState *state) noexcept {
            if (state != nullptr)
                state->rejected = true;
        }

        [[nodiscard]] bool IsDiagnosticInputValid(const ExternalImportOutputState *state,
                                                  const HoroAssetImportDiagnostic *diagnostic) noexcept {
            return state != nullptr && state->prepared != nullptr && diagnostic != nullptr &&
                   state->prepared->diagnostics.size() < kMaxImportDiagnostics;
        }

        [[nodiscard]] bool IsProgressInputValid(const ExternalImportOutputState *state, const std::uint64_t completedUnits,
                                                const std::uint64_t totalUnits, const HoroExtensionStringView message) noexcept {
            return state != nullptr && state->progress != nullptr && totalUnits != 0 && completedUnits <= totalUnits &&
                   message.length <= kMaxTextBytes && (message.data != nullptr || message.length == 0);
        }

        // The opaque pointer type is fixed by the versioned C ABI callback signature.
        [[nodiscard]] uint8_t IsCancelled(const void *context) {  // NOSONAR(cpp:S5008)
            if (context == nullptr) {
                return 0U;
            }
            return static_cast<std::uint8_t>(static_cast<const CancellationToken *>(context)->IsCancellationRequested());
        }

        [[nodiscard]] HoroExtensionStatus ResizeVector(void *context, const std::uint64_t byteCount,  // NOSONAR(cpp:S5008) C ABI callback
                                                       std::uint8_t **outBytes) {                     // NOSONAR(cpp:S5008)
            if (context == nullptr || outBytes == nullptr || byteCount > kMaxPayloadBytes)

                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            try {
                auto &bytes = *static_cast<std::vector<std::uint8_t> *>(context);
                bytes.resize(static_cast<std::size_t>(byteCount));
                *outBytes = bytes.empty() ? nullptr : bytes.data();
                return HORO_EXTENSION_SUCCESS;
            } catch (const std::bad_alloc &) {
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            } catch (const std::length_error &exception) {
                LOG_WARN("extensions.importer", "ResizeVector rejected an oversized payload: %s", exception.what());
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            } catch (const std::exception &exception) {  // NOSONAR(cpp:S1181) C ABI exception barrier.
                LOG_WARN("extensions.importer", "ResizeVector failed: %s", exception.what());
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            }
        }

        [[nodiscard]] HoroExtensionStatus ResizeImportPayload(void *context, const std::uint64_t byteCount,  // NOSONAR(cpp:S5008)
                                                              std::uint8_t **outBytes) {                     // NOSONAR(cpp:S5008)
            auto *state = static_cast<ExternalImportOutputState *>(context);
            if (state == nullptr || state->prepared == nullptr || outBytes == nullptr || byteCount > kMaxPayloadBytes) {
                if (state != nullptr)
                    state->rejected = true;
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            }
            const HoroExtensionStatus status = ResizeVector(&state->prepared->editorPayload, byteCount, outBytes);
            state->rejected |= status != HORO_EXTENSION_SUCCESS;
            return status;
        }

        [[nodiscard]] HoroExtensionStatus AppendImportDependency(void *context,  // NOSONAR(cpp:S5008) C ABI callback
                                                                 const HoroExtensionStringView assetId) {
            auto *state = static_cast<ExternalImportOutputState *>(context);
            if (state == nullptr || state->prepared == nullptr || state->prepared->dependencies.size() >= kMaxImportDependencies) {
                if (state != nullptr)
                    state->rejected = true;
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            }
            try {
                std::string value;
                if (!CopyExternalText(assetId, value))
                    throw std::invalid_argument{"invalid dependency"};
                auto parsed = Assets::AssetId::Parse(value);
                if (parsed.HasError())
                    throw std::invalid_argument{"invalid dependency"};
                if (!state->dependencyIds.insert(parsed.Value()).second)
                    return HORO_EXTENSION_SUCCESS;
                state->prepared->dependencies.push_back(std::move(parsed).Value());
                return HORO_EXTENSION_SUCCESS;
            } catch (...) {  // NOSONAR(cpp:S1181) C ABI exception barrier.
                state->rejected = true;
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            }
        }

        [[nodiscard]] bool ConvertDiagnosticSeverity(const HoroAssetImportDiagnosticSeverity source,
                                                     Assets::ImportDiagnostic::Severity &destination) noexcept {
            using enum Assets::ImportDiagnostic::Severity;
            switch (source) {
                case HORO_ASSET_IMPORT_DIAGNOSTIC_INFO:
                    destination = Info;
                    return true;
                case HORO_ASSET_IMPORT_DIAGNOSTIC_WARNING:
                    destination = Warning;
                    return true;
                case HORO_ASSET_IMPORT_DIAGNOSTIC_ERROR:
                    destination = Error;
                    return true;
                default:
                    return false;
            }
        }

        [[nodiscard]] HoroExtensionStatus AppendImportDiagnostic(void *context,  // NOSONAR(cpp:S5008) C ABI callback
                                                                 const HoroAssetImportDiagnostic *diagnostic) {
            auto *state = static_cast<ExternalImportOutputState *>(context);
            if (!IsDiagnosticInputValid(state, diagnostic)) {
                RejectOutput(state);
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            }
            try {
                Assets::ImportDiagnostic converted;
                if (!ConvertDiagnosticSeverity(diagnostic->severity, converted.severity) ||
                    !CopyExternalText(diagnostic->code, converted.code) || !CopyExternalText(diagnostic->message, converted.message) ||
                    (diagnostic->hasLine != 0 && diagnostic->line < 0))
                    throw std::invalid_argument{"invalid diagnostic"};
                if (diagnostic->hasLine != 0)
                    converted.line = diagnostic->line;
                state->prepared->diagnostics.push_back(std::move(converted));
                return HORO_EXTENSION_SUCCESS;
            } catch (...) {  // NOSONAR(cpp:S1181) C ABI exception barrier.
                RejectOutput(state);
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            }
        }

        [[nodiscard]] HoroExtensionStatus ReportImportProgress(void *context, const std::uint64_t completedUnits,  // NOSONAR(cpp:S5008)
                                                               const std::uint64_t totalUnits,                     // NOSONAR(cpp:S5008)
                                                               const HoroExtensionStringView message) {
            auto *state = static_cast<ExternalImportOutputState *>(context);
            if (!IsProgressInputValid(state, completedUnits, totalUnits, message)) {
                RejectOutput(state);
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            }
            try {
                state->progress->Report(completedUnits, totalUnits,
                                        std::string_view{message.data != nullptr ? message.data : "", message.length});
                return HORO_EXTENSION_SUCCESS;
            } catch (...) {  // NOSONAR(cpp:S1181) C ABI exception barrier.
                RejectOutput(state);
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            }
        }

        void DestroyExternalImporter(const HoroAssetImporterDestroyFunc destroy, void *context) noexcept {
            if (destroy == nullptr)
                return;
            try {
                destroy(context);
            } catch (const std::exception &exception) {  // NOSONAR(cpp:S1181) External destructor is an exception boundary.
                LOG_WARN("extensions.importer", "External importer destroy callback threw: %s", exception.what());
            } catch (...) {  // NOSONAR(cpp:S1181) External destructor is an exception boundary.
                LOG_WARN("extensions.importer", "External importer destroy callback threw an unknown exception.");
            }
        }

        struct ExternalImporterInstance final {
            ExternalImporterInstance() = default;

            ~ExternalImporterInstance() {
                DestroyExternalImporter(destroy, context);
            }

            ExternalImporterInstance(const ExternalImporterInstance &) = delete;
            ExternalImporterInstance &operator=(const ExternalImporterInstance &) = delete;

            ExternalImporterInstance(ExternalImporterInstance &&) = delete;
            ExternalImporterInstance &operator=(ExternalImporterInstance &&) = delete;

            std::shared_ptr<ExtensionModuleLifetime> lifetime;
            void *context{};
            HoroAssetImporterDestroyFunc destroy{};
            HoroAssetImportFunc importFn{};
            HoroAssetPreviewFunc preview{};
        };

        template <typename Invoker> [[nodiscard]] HoroExtensionStatus SafeInvoke(Invoker &&invoker, const char *operation) {
            try {
                return invoker();
            } catch (const std::runtime_error &exception) {  // NOSONAR(cpp:S1181)
                LOG_WARN("extensions.importer", "External %s threw runtime error: %s", operation, exception.what());
            } catch (const std::logic_error &exception) {  // NOSONAR(cpp:S1181)
                LOG_WARN("extensions.importer", "External %s threw logic error: %s", operation, exception.what());
            } catch (const std::bad_alloc &exception) {  // NOSONAR(cpp:S1181)
                LOG_WARN("extensions.importer", "External %s threw bad alloc: %s", operation, exception.what());
            } catch (const std::exception &exception) {  // NOSONAR(cpp:S1181) External code is an exception containment boundary.
                LOG_WARN("extensions.importer", "External %s threw exception: %s", operation, exception.what());
            } catch (...) {  // NOSONAR(cpp:S1181)
                LOG_WARN("extensions.importer", "External %s threw unknown exception.", operation);
            }
            return HORO_EXTENSION_ERROR_INIT_FAILED;
        }

        [[nodiscard]] Result<Assets::PreparedAssetImport> ImportFailure(const std::string_view message) {
            return Result<Assets::PreparedAssetImport>::Failure(MakeError(ExtensionErrors::InvocationFailed, std::string{message}));
        }

        [[nodiscard]] std::vector<HoroAssetImportSettingValue> BuildAbiSettings(const Assets::AssetImportInput &input) {
            std::vector<HoroAssetImportSettingValue> settings;
            settings.reserve(input.settings.size());
            for (const auto &setting : input.settings)
                settings.push_back(ToAbiValue(setting));
            return settings;
        }

        [[nodiscard]] HoroExtensionStatus InvokeExternalImporter(const std::shared_ptr<ExternalImporterInstance> &instance,
                                                                 const Assets::AssetImportInput &input,
                                                                 const CancellationToken &cancellation,
                                                                 Assets::PreparedAssetImport &prepared, bool &outputRejected) {
            const auto settings = BuildAbiSettings(input);
            ExternalImportOutputState output{.prepared = &prepared, .progress = &input.progress};
            HoroAssetImportRequest request{
                .structSize = sizeof(HoroAssetImportRequest),
                .sourceBytes = input.sourceBytes.data(),
                .sourceByteCount = input.sourceBytes.size(),
                .sourceExtension = {input.sourceExtension.data(), static_cast<std::uint32_t>(input.sourceExtension.size())},
                .settings = settings.data(),
                .settingCount = static_cast<std::uint32_t>(settings.size()),
                .cancellation = {&cancellation, IsCancelled},
            };
            HoroAssetImportResponse response{
                .structSize = sizeof(HoroAssetImportResponse),
                .assetType = {},
                .editorPayload = {&output, ResizeImportPayload},
                .dependencies = {&output, AppendImportDependency},
                .diagnostics = {&output, AppendImportDiagnostic},
                .progress = {&output, ReportImportProgress},
            };
            const HoroExtensionStatus status = SafeInvoke([&instance, &request, &response] {
                return instance->importFn(instance->context, &request, &response);
            }, "importer");
            outputRejected = output.rejected;

            std::string assetType;
            if (status == HORO_EXTENSION_SUCCESS && !output.rejected && CopyExternalText(response.assetType, assetType)) {
                auto parsedType = Assets::AssetTypeId::Parse(assetType);
                if (parsedType.HasValue())
                    prepared.type = std::move(parsedType).Value();
            }
            return status;
        }

        class ExternalAssetImporter final : public Assets::IAssetImporter {
        public:
            explicit ExternalAssetImporter(std::shared_ptr<ExternalImporterInstance> instance) : instance_(std::move(instance)) {}

            [[nodiscard]] Result<Assets::PreparedAssetImport> Import(const Assets::AssetImportInput &input,
                                                                     const CancellationToken &cancellation) const override {
                if (cancellation.IsCancellationRequested())
                    return ImportFailure("External asset import was cancelled before provider entry.");
                if (input.settings.size() > kMaxListEntries || input.sourceExtension.size() > kMaxTextBytes)
                    return ImportFailure("External asset import input exceeded ABI bounds.");

                Assets::PreparedAssetImport prepared;
                bool outputRejected{};
                const HoroExtensionStatus status = InvokeExternalImporter(instance_, input, cancellation, prepared, outputRejected);
                if (cancellation.IsCancellationRequested() || status == HORO_EXTENSION_ERROR_CANCELLED)
                    return ImportFailure("External asset importer callback was cancelled.");
                if (status != HORO_EXTENSION_SUCCESS || outputRejected)
                    return ImportFailure("External asset importer callback failed or produced rejected output.");
                if (prepared.type.Value().empty() || prepared.editorPayload.empty())
                    return ImportFailure("External importer returned invalid or empty output.");
                return Result<Assets::PreparedAssetImport>::Success(std::move(prepared));
            }

        private:
            std::shared_ptr<ExternalImporterInstance> instance_;
        };

        class ExternalAssetPreviewProvider final : public Assets::IAssetPreviewProvider {
        public:
            explicit ExternalAssetPreviewProvider(std::shared_ptr<ExternalImporterInstance> instance) : instance_(std::move(instance)) {}

            [[nodiscard]] Result<Assets::AssetPreviewImage> GeneratePreview(const Assets::AssetPreviewInput &input,
                                                                            const CancellationToken &cancellation) const override {
                if (input.width == 0 || input.height == 0 || input.width > kMaxPreviewDimension || input.height > kMaxPreviewDimension)
                    return Result<Assets::AssetPreviewImage>::Failure(
                        MakeError(ExtensionErrors::InvocationFailed, "External preview dimensions are invalid."));

                Assets::AssetPreviewImage image;
                HoroAssetPreviewRequest request{
                    .structSize = sizeof(HoroAssetPreviewRequest),
                    .editorPayload = input.editorPayload.data(),
                    .editorPayloadByteCount = input.editorPayload.size(),
                    .absoluteAssetPath = {input.absoluteAssetPath.data(), static_cast<std::uint32_t>(input.absoluteAssetPath.size())},
                    .assetType = {input.assetType.Value().data(), static_cast<std::uint32_t>(input.assetType.Value().size())},
                    .width = input.width,
                    .height = input.height,
                    .cancellation = {&cancellation, IsCancelled},
                };
                HoroAssetPreviewResponse response{
                    .structSize = sizeof(HoroAssetPreviewResponse),
                    .rgba8Pixels = {&image.pixels, ResizeVector},
                };
                const HoroExtensionStatus status = SafeInvoke([&instance = instance_, &request, &response] {
                    return instance->preview(instance->context, &request, &response);
                }, "preview");

                image.width = response.width;
                image.height = response.height;
                if (status != HORO_EXTENSION_SUCCESS || !image.IsValid())
                    return Result<Assets::AssetPreviewImage>::Failure(
                        MakeError(ExtensionErrors::InvocationFailed, "External preview callback returned invalid output."));
                return Result<Assets::AssetPreviewImage>::Success(std::move(image));
            }

        private:
            std::shared_ptr<ExternalImporterInstance> instance_;
        };

    }  // namespace

    ExtensionModuleLifetime::~ExtensionModuleLifetime() {
        (void)UnloadNow();
    }

    /** @copydoc ExtensionModuleLifetime::UnloadNow */
    bool ExtensionModuleLifetime::UnloadNow() noexcept {
        if (!loaded)
            return true;
        loaded = false;
        if (unload != nullptr && moduleApi.moduleContext != nullptr) {
            try {
                unload(&moduleApi);
            } catch (const std::runtime_error &exception) {
                LOG_WARN("extensions.importer", "Runtime error during module unload: %s", exception.what());
                return false;
            } catch (const std::logic_error &exception) {
                LOG_WARN("extensions.importer", "Logic error during module unload: %s", exception.what());
                return false;
            } catch (const std::bad_alloc &exception) {
                LOG_WARN("extensions.importer", "Bad alloc during module unload: %s", exception.what());
                return false;
            } catch (const std::exception &exception) {  // NOSONAR(cpp:S1181)
                LOG_WARN("extensions.importer", "Exception during module unload: %s", exception.what());
                return false;
            } catch (...) {  // NOSONAR(cpp:S1181)
                LOG_WARN("extensions.importer", "Unknown exception during module unload.");
                return false;
            }
        }
        return true;
    }

    namespace {
        /** @brief Bound each declared span before dereferencing any element. */
        bool HasValidImporterLists(const HoroAssetImporterDescriptor &descriptor) {
            return descriptor.fileExtensionCount > 0 && descriptor.fileExtensionCount <= kMaxListEntries && descriptor.assetTypeCount > 0 &&
                   descriptor.assetTypeCount <= kMaxListEntries && descriptor.settingCount <= kMaxListEntries &&
                   descriptor.fileExtensions != nullptr && descriptor.assetTypes != nullptr &&
                   (descriptor.settings != nullptr || descriptor.settingCount == 0);
        }

        /** @brief Reject incomplete tables and contexts without a matching owner-provided destructor. */
        bool IsValidImporterDescriptor(const HoroAssetImporterDescriptor *descriptor) {
            return descriptor != nullptr && descriptor->structSize >= sizeof(HoroAssetImporterDescriptor) &&
                   descriptor->abiVersion == HORO_ASSET_IMPORTER_ABI_VERSION && descriptor->importAsset != nullptr &&
                   (descriptor->importerContext == nullptr || descriptor->destroyImporter != nullptr) && HasValidImporterLists(*descriptor);
        }

        /** @brief Copy identity and verify the declared owning module before accepting metadata. */
        void CopyContributionMetadata(const AssetImporterRegistrationSession &session, const HoroAssetImporterDescriptor &descriptor,
                                      Assets::AssetImporterContribution &contribution) {
            if (!CopyExternalText(descriptor.contributionId, contribution.contributionId) ||
                !CopyExternalText(descriptor.contributionVersion, contribution.version) ||
                !CopyExternalText(descriptor.subfolderCategory, contribution.subfolderCategory, true) ||
                !CopyExternalText(descriptor.targetExtension, contribution.targetExtension))
                throw std::invalid_argument{"invalid text"};

            if (const bool declared = std::ranges::any_of(session.manifest->contributions,
                                                          [&contribution, &session](const ExtensionContributionManifest &candidate) {
                return candidate.type == "asset.importer" && candidate.id == contribution.contributionId &&
                       candidate.owningModule == session.extensionModule->id;
            });
                !declared) {
                throw std::invalid_argument{"undeclared contribution"};
            }

            contribution.packageId = session.manifest->id;
            contribution.moduleId = session.extensionModule->id;
            contribution.moduleVersion = session.extensionModule->version;
            contribution.supportsMetaSidecar = descriptor.supportsMetaSidecar != 0;
            if (descriptor.previewFallback > static_cast<std::uint8_t>(Assets::AssetPreviewFallback::Generic))
                throw std::invalid_argument{"invalid fallback"};
            contribution.previewFallback = static_cast<Assets::AssetPreviewFallback>(descriptor.previewFallback);
        }

        /** @brief Copy validated list content into host-owned contribution storage. */
        void CopyContributionLists(const HoroAssetImporterDescriptor &descriptor, Assets::AssetImporterContribution &contribution) {
            contribution.fileExtensions.reserve(descriptor.fileExtensionCount);
            for (std::uint32_t index = 0; index < descriptor.fileExtensionCount; ++index) {
                std::string extension;
                if (!CopyExternalText(descriptor.fileExtensions[index], extension) || !IsLowerExtension(extension))
                    throw std::invalid_argument{"invalid file extension"};
                contribution.fileExtensions.push_back(std::move(extension));
            }

            contribution.assetTypes.reserve(descriptor.assetTypeCount);
            for (std::uint32_t index = 0; index < descriptor.assetTypeCount; ++index) {
                std::string assetType;
                if (!CopyExternalText(descriptor.assetTypes[index], assetType))
                    throw std::invalid_argument{"invalid asset type"};
                auto parsed = Assets::AssetTypeId::Parse(assetType);
                if (parsed.HasError())
                    throw std::invalid_argument{"invalid type"};
                contribution.assetTypes.push_back(std::move(parsed).Value());
            }
            contribution.settings.resize(descriptor.settingCount);
            for (std::uint32_t index = 0; index < descriptor.settingCount; ++index) {
                if (!ConvertExternalImportSetting(descriptor.settings[index], contribution.settings[index]))
                    throw std::invalid_argument{"invalid setting"};
            }
        }
    }  // namespace

    HoroExtensionStatus RegisterExternalAssetImporter(void *hostContext,  // NOSONAR(cpp:S5008)
                                                      const HoroAssetImporterDescriptor *descriptor) noexcept {
        auto *session = static_cast<AssetImporterRegistrationSession *>(hostContext);
        if (session == nullptr || session->failed || !IsValidImporterDescriptor(descriptor)) {
            if (session != nullptr) {
                session->failed = true;
                session->error =
                    MakeError(ExtensionErrors::ContributionRejected, "External importer descriptor is incomplete or ABI-incompatible.");
            }
            return HORO_EXTENSION_ERROR_INVALID_ARGS;
        }

        try {
            Assets::AssetImporterContribution contribution;
            CopyContributionMetadata(*session, *descriptor, contribution);
            CopyContributionLists(*descriptor, contribution);

            auto instance = std::make_shared<ExternalImporterInstance>();
            instance->lifetime = session->lifetime;
            instance->context = descriptor->importerContext;
            instance->importFn = descriptor->importAsset;
            instance->preview = descriptor->generatePreview;
            contribution.strategy = std::make_shared<ExternalAssetImporter>(instance);
            if (instance->preview != nullptr)
                contribution.previewProvider = std::make_shared<ExternalAssetPreviewProvider>(instance);
            session->contributions.push_back(std::move(contribution));
            // Only a successful registration transfers the caller-owned context.
            instance->destroy = descriptor->destroyImporter;
            return HORO_EXTENSION_SUCCESS;
        } catch (...) {
            session->failed = true;
            session->error =
                MakeError(ExtensionErrors::ContributionRejected, "External importer descriptor exceeded bounds or contained invalid data.");
            return HORO_EXTENSION_ERROR_INVALID_ARGS;
        }
    }
}  // namespace Horo::Extensions
