#include "Horo/Editor/UiCanvasDocument.h"

#include "Horo/Foundation/PathUtils.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

namespace Horo::Editor {
    namespace {
        using Json = nlohmann::json;
        constexpr std::string_view DocumentFormat = "horo.ui_canvas";

        const ErrorDomainId UiCanvasDocumentDomain{"horo.editor.ui_canvas_document"};

        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor, std::string message = {}) {
            return Result<T>::Failure(MakeError(descriptor, std::move(message)));
        }

        [[nodiscard]] std::string LowercaseAscii(std::string value) {
            for (char &character : value)
                character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            return value;
        }

        [[nodiscard]] bool IsUiCanvasPath(const std::filesystem::path &path) {
            return LowercaseAscii(path.extension().string()) == UiCanvasDocumentExtension;
        }

        [[nodiscard]] std::filesystem::path CanonicalPath(const std::filesystem::path &path, bool &valid) {
            valid = false;
            if (path.empty() || !path.is_absolute())
                return {};
            std::error_code error;
            const std::filesystem::path absolute = path.lexically_normal();
            const std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, error);
            if (error || canonical.empty())
                return {};
            valid = true;
            return canonical;
        }

        [[nodiscard]] Result<std::filesystem::path> ValidateProjectPath(const std::filesystem::path &projectRoot,
                                                                        const std::filesystem::path &documentPath) {
            bool rootValid = false;
            const std::filesystem::path root = CanonicalPath(projectRoot, rootValid);
            bool documentValid = false;
            const std::filesystem::path path = CanonicalPath(documentPath, documentValid);
            if (!rootValid || !documentValid || !IsUiCanvasPath(path) || !Horo::Foundation::Paths::HasPathPrefix(root, path))
                return Failure<std::filesystem::path>(UiCanvasDocumentErrors::InvalidPath);
            return Result<std::filesystem::path>::Success(path);
        }

        [[nodiscard]] Result<std::string> ReadContents(const std::filesystem::path &path) {
            std::error_code error;
            const std::filesystem::file_status status = std::filesystem::status(path, error);
            if (error) {
                if (error == std::errc::no_such_file_or_directory)
                    return Failure<std::string>(UiCanvasDocumentErrors::Missing);
                return Failure<std::string>(UiCanvasDocumentErrors::ReadFailed, error.message());
            }
            if (status.type() == std::filesystem::file_type::not_found)
                return Failure<std::string>(UiCanvasDocumentErrors::Missing);
            if (!std::filesystem::is_regular_file(status))
                return Failure<std::string>(UiCanvasDocumentErrors::ReadFailed, "UI Canvas path is not a regular file.");

            const std::uintmax_t size = std::filesystem::file_size(path, error);
            if (error)
                return Failure<std::string>(UiCanvasDocumentErrors::ReadFailed, error.message());
            if (size > MaximumUiCanvasDocumentBytes)
                return Failure<std::string>(UiCanvasDocumentErrors::ReadFailed, "UI Canvas source exceeds the bounded file size.");

            std::ifstream input(path, std::ios::binary);
            if (!input)
                return Failure<std::string>(UiCanvasDocumentErrors::ReadFailed, "UI Canvas source could not be opened.");
            std::string contents(static_cast<std::size_t>(size), '\0');
            if (size != 0)
                input.read(contents.data(), static_cast<std::streamsize>(size));
            if (!input && !input.eof())
                return Failure<std::string>(UiCanvasDocumentErrors::ReadFailed, "UI Canvas source could not be read completely.");
            return Result<std::string>::Success(std::move(contents));
        }

        [[nodiscard]] UiCanvasFileFingerprint Fingerprint(const std::string_view contents) {
            const auto bytes = std::as_bytes(std::span{contents.data(), contents.size()});
            return UiCanvasFileFingerprint{
                .exists = true,
                .byteSize = contents.size(),
                .checksum = FormatSha256(ComputeSha256(bytes)),
            };
        }

        [[nodiscard]] std::string EncodeUiId(const Runtime::Ui::SerializedUiId &bytes) {
            constexpr std::array<char, 16> Hex{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
            std::string result;
            result.reserve(bytes.size() * 2U);
            for (const std::uint8_t byte : bytes) {
                result.push_back(Hex[byte >> 4U]);
                result.push_back(Hex[byte & 0x0fU]);
            }
            return result;
        }

        [[nodiscard]] std::optional<Runtime::Ui::SerializedUiId> DecodeUiId(const std::string_view text) {
            if (text.size() != 32U)
                return std::nullopt;
            Runtime::Ui::SerializedUiId bytes{};
            for (std::size_t index = 0; index < bytes.size(); ++index) {
                const auto nibble = [](const char value) -> std::optional<std::uint8_t> {
                    if (value >= '0' && value <= '9')
                        return static_cast<std::uint8_t>(value - '0');
                    if (value >= 'a' && value <= 'f')
                        return static_cast<std::uint8_t>(value - 'a' + 10);
                    return std::nullopt;
                };
                const auto high = nibble(text[index * 2U]);
                const auto low = nibble(text[index * 2U + 1U]);
                if (!high.has_value() || !low.has_value())
                    return std::nullopt;
                bytes[index] = static_cast<std::uint8_t>((*high << 4U) | *low);
            }
            return bytes;
        }

        template <typename Id> [[nodiscard]] Result<Id> ParseUiId(const Json &value, const std::string_view field) {
            if (!value.is_string())
                return Failure<Id>(UiCanvasDocumentErrors::Malformed, "UI Canvas identity field is not text: " + std::string(field));
            const auto bytes = DecodeUiId(value.get<std::string>());
            if (!bytes.has_value())
                return Failure<Id>(UiCanvasDocumentErrors::Malformed, "UI Canvas identity field is not canonical: " + std::string(field));
            const Result<Id> parsed = Id::Create(*bytes);
            if (parsed.HasError())
                return Failure<Id>(UiCanvasDocumentErrors::Malformed, "UI Canvas identity field is zero: " + std::string(field));
            return parsed;
        }

        [[nodiscard]] Result<std::uint32_t> ParseUInt32(const Json &value, const std::string_view field) {
            if (!value.is_number_unsigned() || value.get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())
                return Failure<std::uint32_t>(UiCanvasDocumentErrors::Malformed,
                                              "UI Canvas numeric field is invalid: " + std::string(field));
            return Result<std::uint32_t>::Success(value.get<std::uint32_t>());
        }

        [[nodiscard]] Result<Runtime::Ui::UiRenderMode> ParseRenderMode(const Json &value) {
            if (!value.is_string())
                return Failure<Runtime::Ui::UiRenderMode>(UiCanvasDocumentErrors::Malformed, "UI Canvas render mode is invalid.");
            const std::string mode = value.get<std::string>();
            using enum Runtime::Ui::UiRenderMode;
            if (mode == "screen_space_overlay")
                return Result<Runtime::Ui::UiRenderMode>::Success(ScreenSpaceOverlay);
            if (mode == "screen_space_camera")
                return Result<Runtime::Ui::UiRenderMode>::Success(ScreenSpaceCamera);
            if (mode == "world_space")
                return Result<Runtime::Ui::UiRenderMode>::Success(WorldSpace);
            return Failure<Runtime::Ui::UiRenderMode>(UiCanvasDocumentErrors::Malformed, "UI Canvas render mode is unsupported.");
        }

        [[nodiscard]] Result<Runtime::Ui::UiScaleMode> ParseScaleMode(const Json &value) {
            if (!value.is_string())
                return Failure<Runtime::Ui::UiScaleMode>(UiCanvasDocumentErrors::Malformed, "UI Canvas scale mode is invalid.");
            const std::string mode = value.get<std::string>();
            using enum Runtime::Ui::UiScaleMode;
            if (mode == "scale_with_screen_size")
                return Result<Runtime::Ui::UiScaleMode>::Success(ScaleWithScreenSize);
            if (mode == "constant_pixel_size")
                return Result<Runtime::Ui::UiScaleMode>::Success(ConstantPixelSize);
            if (mode == "constant_physical_size")
                return Result<Runtime::Ui::UiScaleMode>::Success(ConstantPhysicalSize);
            return Failure<Runtime::Ui::UiScaleMode>(UiCanvasDocumentErrors::Malformed, "UI Canvas scale mode is unsupported.");
        }

        [[nodiscard]] std::string RenderModeText(const Runtime::Ui::UiRenderMode mode) {
            using enum Runtime::Ui::UiRenderMode;
            switch (mode) {
                case ScreenSpaceOverlay:
                    return "screen_space_overlay";
                case ScreenSpaceCamera:
                    return "screen_space_camera";
                case WorldSpace:
                    return "world_space";
            }
            return {};
        }

        [[nodiscard]] std::string ScaleModeText(const Runtime::Ui::UiScaleMode mode) {
            using enum Runtime::Ui::UiScaleMode;
            switch (mode) {
                case ScaleWithScreenSize:
                    return "scale_with_screen_size";
                case ConstantPixelSize:
                    return "constant_pixel_size";
                case ConstantPhysicalSize:
                    return "constant_physical_size";
            }
            return {};
        }

        [[nodiscard]] Result<Runtime::Ui::UiDocument> ParseDocument(const Json &root) {
            if (!root.is_object() || root.value("format", "") != DocumentFormat)
                return Failure<Runtime::Ui::UiDocument>(UiCanvasDocumentErrors::Malformed, "UI Canvas document format is invalid.");
            if (!root.contains("formatVersion") || !root["formatVersion"].is_number_unsigned())
                return Failure<Runtime::Ui::UiDocument>(UiCanvasDocumentErrors::Malformed, "UI Canvas format version is missing.");
            if (root["formatVersion"].get<std::uint64_t>() != UiCanvasDocumentFormatVersion)
                return Failure<Runtime::Ui::UiDocument>(UiCanvasDocumentErrors::UnsupportedVersion,
                                                        "UI Canvas format version is not supported by this editor.");
            if (!root.contains("documentId") || !root.contains("revision") || !root.contains("canvases") ||
                !root.contains("dependencies") || !root["canvases"].is_array() || !root["dependencies"].is_array())
                return Failure<Runtime::Ui::UiDocument>(UiCanvasDocumentErrors::Malformed, "UI Canvas document fields are incomplete.");

            const Result<Runtime::Ui::UiDocumentId> documentId = ParseUiId<Runtime::Ui::UiDocumentId>(root["documentId"], "documentId");
            if (!root["revision"].is_number_unsigned() || documentId.HasError())
                return Failure<Runtime::Ui::UiDocument>(UiCanvasDocumentErrors::Malformed, "UI Canvas document identity is invalid.");
            const Result<Runtime::Ui::UiDocumentRevision> revision =
                Runtime::Ui::UiDocumentRevision::Create(root["revision"].get<std::uint64_t>());
            if (revision.HasError())
                return Failure<Runtime::Ui::UiDocument>(UiCanvasDocumentErrors::Malformed, "UI Canvas document revision is invalid.");

            Runtime::Ui::UiDocumentBuilder builder{documentId.Value(), revision.Value()};
            if (root["canvases"].empty() || root["canvases"].size() > Runtime::Ui::MaximumUiDocumentCanvases)
                return Failure<Runtime::Ui::UiDocument>(UiCanvasDocumentErrors::InvalidDocument,
                                                        "UI Canvas count is outside the supported bound.");
            for (const Json &canvas : root["canvases"]) {
                if (!canvas.is_object() || !canvas.contains("id") || !canvas.contains("rootElement") || !canvas.contains("renderMode") ||
                    !canvas.contains("referenceResolution") || !canvas.contains("scaleMode") || !canvas["referenceResolution"].is_object())
                    return Failure<Runtime::Ui::UiDocument>(UiCanvasDocumentErrors::Malformed, "UI Canvas descriptor is incomplete.");
                const Result<Runtime::Ui::UiCanvasId> id = ParseUiId<Runtime::Ui::UiCanvasId>(canvas["id"], "canvas.id");
                const Result<Runtime::Ui::UiElementId> rootElement =
                    ParseUiId<Runtime::Ui::UiElementId>(canvas["rootElement"], "canvas.rootElement");
                const Result<Runtime::Ui::UiRenderMode> renderMode = ParseRenderMode(canvas["renderMode"]);
                const Result<Runtime::Ui::UiScaleMode> scaleMode = ParseScaleMode(canvas["scaleMode"]);
                const Json &resolution = canvas["referenceResolution"];
                if (id.HasError() || rootElement.HasError() || renderMode.HasError() || scaleMode.HasError() ||
                    !resolution.contains("width") || !resolution.contains("height"))
                    return Failure<Runtime::Ui::UiDocument>(UiCanvasDocumentErrors::Malformed,
                                                            "UI Canvas descriptor identity or mode is invalid.");
                const Result<std::uint32_t> width = ParseUInt32(resolution["width"], "canvas.referenceResolution.width");
                const Result<std::uint32_t> height = ParseUInt32(resolution["height"], "canvas.referenceResolution.height");
                if (width.HasError() || height.HasError())
                    return Failure<Runtime::Ui::UiDocument>(UiCanvasDocumentErrors::Malformed,
                                                            "UI Canvas reference resolution is invalid.");
                if (const Result<void> added = builder.AddCanvas(Runtime::Ui::UiCanvasDescriptor{
                        .id = id.Value(),
                        .rootElement = rootElement.Value(),
                        .renderMode = renderMode.Value(),
                        .referenceResolution = {.width = width.Value(), .height = height.Value()},
                        .scaleMode = scaleMode.Value(),
                    });
                    added.HasError())
                    return Failure<Runtime::Ui::UiDocument>(UiCanvasDocumentErrors::InvalidDocument, added.ErrorValue().message);
            }

            if (root["dependencies"].size() > Runtime::Ui::MaximumUiDocumentDependencies)
                return Failure<Runtime::Ui::UiDocument>(UiCanvasDocumentErrors::InvalidDocument,
                                                        "UI Canvas dependency count exceeds the supported bound.");
            for (const Json &dependency : root["dependencies"]) {
                if (!dependency.is_object() || !dependency.contains("asset") || !dependency.contains("expectedType") ||
                    !dependency.contains("required") || !dependency["asset"].is_string() || !dependency["expectedType"].is_string() ||
                    !dependency["required"].is_boolean())
                    return Failure<Runtime::Ui::UiDocument>(UiCanvasDocumentErrors::Malformed, "UI Canvas dependency is incomplete.");
                const Result<Assets::AssetId> asset = Assets::AssetId::Parse(dependency["asset"].get<std::string>());
                const Result<Assets::AssetTypeId> expectedType = Assets::AssetTypeId::Parse(dependency["expectedType"].get<std::string>());
                if (asset.HasError() || expectedType.HasError())
                    return Failure<Runtime::Ui::UiDocument>(UiCanvasDocumentErrors::Malformed, "UI Canvas dependency identity is invalid.");
                if (const Result<void> required = builder.RequireAsset(Runtime::Ui::UiAssetDependency{
                        .asset = asset.Value(),
                        .expectedType = expectedType.Value(),
                        .required = dependency["required"].get<bool>(),
                    });
                    required.HasError())
                    return Failure<Runtime::Ui::UiDocument>(UiCanvasDocumentErrors::InvalidDocument, required.ErrorValue().message);
            }

            const Result<Runtime::Ui::UiDocument> built = std::move(builder).Build();
            if (built.HasError())
                return Failure<Runtime::Ui::UiDocument>(UiCanvasDocumentErrors::InvalidDocument, built.ErrorValue().message);
            return built;
        }

        [[nodiscard]] Json SerializeDocument(const Runtime::Ui::UiDocument &document) {
            Json canvases = Json::array();
            for (const Runtime::Ui::UiCanvasDescriptor &canvas : document.Canvases()) {
                canvases.push_back({
                    {"id", EncodeUiId(canvas.id.Bytes())},
                    {"rootElement", EncodeUiId(canvas.rootElement.Bytes())},
                    {"renderMode", RenderModeText(canvas.renderMode)},
                    {"referenceResolution", {{"width", canvas.referenceResolution.width}, {"height", canvas.referenceResolution.height}}},
                    {"scaleMode", ScaleModeText(canvas.scaleMode)},
                });
            }
            Json dependencies = Json::array();
            for (const Runtime::Ui::UiAssetDependency &dependency : document.Dependencies()) {
                dependencies.push_back({
                    {"asset", dependency.asset.ToString()},
                    {"expectedType", dependency.expectedType.Value()},
                    {"required", dependency.required},
                });
            }
            return Json{
                {"format", DocumentFormat},
                {"formatVersion", UiCanvasDocumentFormatVersion},
                {"documentId", EncodeUiId(document.Id().Bytes())},
                {"revision", document.Revision().Value()},
                {"canvases", std::move(canvases)},
                {"dependencies", std::move(dependencies)},
            };
        }

        [[nodiscard]] std::vector<std::byte> Bytes(const std::string_view value) {
            std::vector<std::byte> bytes(value.size());
            if (!value.empty())
                std::memcpy(bytes.data(), value.data(), value.size());
            return bytes;
        }
    }  // namespace

    namespace UiCanvasDocumentErrors {
        const ErrorCodeDescriptor InvalidPath{UiCanvasDocumentDomain,
                                              ErrorCode{"editor.ui_canvas_document.path_invalid"},
                                              ErrorSeverity::Error,
                                              "The UI Canvas path is invalid.",
                                              "Use an absolute project-contained .uicanvas file.",
                                              false,
                                              true};
        const ErrorCodeDescriptor Missing{UiCanvasDocumentDomain,
                                          ErrorCode{"editor.ui_canvas_document.missing"},
                                          ErrorSeverity::Error,
                                          "The UI Canvas source file is missing.",
                                          "Restore the file before opening it.",
                                          true,
                                          true};
        const ErrorCodeDescriptor ReadFailed{UiCanvasDocumentDomain,
                                             ErrorCode{"editor.ui_canvas_document.read_failed"},
                                             ErrorSeverity::Error,
                                             "The UI Canvas source file could not be read.",
                                             "Check the file and project permissions.",
                                             true,
                                             true};
        const ErrorCodeDescriptor Malformed{UiCanvasDocumentDomain,
                                            ErrorCode{"editor.ui_canvas_document.malformed"},
                                            ErrorSeverity::Error,
                                            "The UI Canvas source is malformed.",
                                            "Repair the file or restore a valid authored revision.",
                                            false,
                                            true};
        const ErrorCodeDescriptor UnsupportedVersion{UiCanvasDocumentDomain,
                                                     ErrorCode{"editor.ui_canvas_document.version_unsupported"},
                                                     ErrorSeverity::Error,
                                                     "The UI Canvas source uses a newer format.",
                                                     "Open the project with an editor that supports this authored format.",
                                                     false,
                                                     true};
        const ErrorCodeDescriptor InvalidDocument{UiCanvasDocumentDomain,
                                                  ErrorCode{"editor.ui_canvas_document.invalid"},
                                                  ErrorSeverity::Error,
                                                  "The UI Canvas authored document is invalid.",
                                                  "Fix the canvas identity, mode, resolution, or dependency values.",
                                                  false,
                                                  true};
        const ErrorCodeDescriptor WriteFailed{UiCanvasDocumentDomain,
                                              ErrorCode{"editor.ui_canvas_document.write_failed"},
                                              ErrorSeverity::Error,
                                              "The UI Canvas source could not be saved.",
                                              "Check the project filesystem and try again.",
                                              true,
                                              true};
        const ErrorCodeDescriptor Conflict{UiCanvasDocumentDomain,
                                           ErrorCode{"editor.ui_canvas_document.conflict"},
                                           ErrorSeverity::Warning,
                                           "The UI Canvas source changed outside the editor.",
                                           "Reload, compare, or explicitly overwrite the external revision.",
                                           false,
                                           true};
        const ErrorCodeDescriptor DirtyDocument{UiCanvasDocumentDomain,
                                                ErrorCode{"editor.ui_canvas_document.dirty"},
                                                ErrorSeverity::Warning,
                                                "The UI Canvas has unsaved changes.",
                                                "Save or explicitly discard the changes.",
                                                false,
                                                true};
        const ErrorCodeDescriptor Closed{UiCanvasDocumentDomain,
                                         ErrorCode{"editor.ui_canvas_document.closed"},
                                         ErrorSeverity::Error,
                                         "The UI Canvas document session is closed.",
                                         "Open a new document session before editing.",
                                         false,
                                         true};
        const ErrorCodeDescriptor StateExhausted{UiCanvasDocumentDomain,
                                                 ErrorCode{"editor.ui_canvas_document.state_exhausted"},
                                                 ErrorSeverity::Critical,
                                                 "The UI Canvas state revision is exhausted.",
                                                 "Close and reopen the project before editing further.",
                                                 true,
                                                 false};
    }  // namespace UiCanvasDocumentErrors

    /** @copydoc InspectUiCanvasDocumentFingerprint */
    Result<UiCanvasFileFingerprint> InspectUiCanvasDocumentFingerprint(const std::filesystem::path &absolutePath) {
        bool valid = false;
        const std::filesystem::path path = CanonicalPath(absolutePath, valid);
        if (!valid || !IsUiCanvasPath(path))
            return Failure<UiCanvasFileFingerprint>(UiCanvasDocumentErrors::InvalidPath);
        const Result<std::string> contents = ReadContents(path);
        if (contents.HasError()) {
            if (contents.ErrorValue().code.Value() == UiCanvasDocumentErrors::Missing.code.Value())
                return Result<UiCanvasFileFingerprint>::Success({});
            return Result<UiCanvasFileFingerprint>::Failure(contents.ErrorValue());
        }
        return Result<UiCanvasFileFingerprint>::Success(Fingerprint(contents.Value()));
    }

    /** @copydoc LoadUiCanvasDocument */
    Result<UiCanvasDocumentSnapshot> LoadUiCanvasDocument(const std::filesystem::path &absolutePath) {
        bool valid = false;
        const std::filesystem::path path = CanonicalPath(absolutePath, valid);
        if (!valid || !IsUiCanvasPath(path))
            return Failure<UiCanvasDocumentSnapshot>(UiCanvasDocumentErrors::InvalidPath);
        const Result<std::string> contents = ReadContents(path);
        if (contents.HasError())
            return Result<UiCanvasDocumentSnapshot>::Failure(contents.ErrorValue());

        try {
            const Json root = Json::parse(contents.Value());
            const Result<Runtime::Ui::UiDocument> document = ParseDocument(root);
            if (document.HasError())
                return Failure<UiCanvasDocumentSnapshot>(document.ErrorValue().code.Value() ==
                                                                 UiCanvasDocumentErrors::UnsupportedVersion.code.Value()
                                                             ? UiCanvasDocumentErrors::UnsupportedVersion
                                                         : document.ErrorValue().code.Value() ==
                                                                 UiCanvasDocumentErrors::InvalidDocument.code.Value()
                                                             ? UiCanvasDocumentErrors::InvalidDocument
                                                             : UiCanvasDocumentErrors::Malformed,
                                                         document.ErrorValue().message);
            return Result<UiCanvasDocumentSnapshot>::Success({.document = std::move(document).Value(), .state = {1}});
        } catch (const Json::exception &exception) {
            return Failure<UiCanvasDocumentSnapshot>(UiCanvasDocumentErrors::Malformed, exception.what());
        }
    }

    /** @copydoc SaveUiCanvasDocument */
    Result<UiCanvasDocumentSaveResult> SaveUiCanvasDocument(const std::filesystem::path &absoluteProjectRoot,
                                                            const std::filesystem::path &absolutePath,
                                                            const UiCanvasDocumentSnapshot &snapshot,
                                                            const UiCanvasFileFingerprint &expectedFingerprint,
                                                            const bool overwriteConflict, const ProjectMutationCoordinator &mutations,
                                                            DurableFileSystem &files) {
        const Result<std::filesystem::path> destination = ValidateProjectPath(absoluteProjectRoot, absolutePath);
        if (destination.HasError())
            return Result<UiCanvasDocumentSaveResult>::Failure(destination.ErrorValue());
        if (!snapshot.state.IsValid() || !snapshot.document.Id().IsValid() || !snapshot.document.Revision().IsValid() ||
            snapshot.document.Canvases().empty())
            return Failure<UiCanvasDocumentSaveResult>(UiCanvasDocumentErrors::InvalidDocument);

        const Result<ProjectMutationLease> lease = mutations.TryAcquire(ProjectMutationRequest{
            .projectRoot = absoluteProjectRoot,
            .owner = ProjectMutationOwner::Save,
            .operationId = "ui-canvas-save",
        });
        if (lease.HasError())
            return Result<UiCanvasDocumentSaveResult>::Failure(lease.ErrorValue());

        const std::string serialized = SerializeDocument(snapshot.document).dump(2) + '\n';
        const Result<UiCanvasFileFingerprint> current = InspectUiCanvasDocumentFingerprint(destination.Value());
        if (current.HasError())
            return Result<UiCanvasDocumentSaveResult>::Failure(current.ErrorValue());
        if (!overwriteConflict && current.Value() != expectedFingerprint)
            return Result<UiCanvasDocumentSaveResult>::Success(
                {.status = UiCanvasDocumentSaveStatus::Conflict, .fingerprint = current.Value()});

        std::filesystem::path prepared = destination.Value();
        prepared += ".save.tmp";
        const std::vector<std::byte> bytes = Bytes(serialized);
        if (const Result<void> written = files.WriteDurable(prepared, bytes); written.HasError()) {
            static_cast<void>(files.RemoveDurable(prepared));
            return Result<UiCanvasDocumentSaveResult>::Failure(written.ErrorValue());
        }
        const Result<UiCanvasFileFingerprint> rechecked = InspectUiCanvasDocumentFingerprint(destination.Value());
        if (rechecked.HasError()) {
            static_cast<void>(files.RemoveDurable(prepared));
            return Result<UiCanvasDocumentSaveResult>::Failure(rechecked.ErrorValue());
        }
        if (!overwriteConflict && rechecked.Value() != expectedFingerprint) {
            static_cast<void>(files.RemoveDurable(prepared));
            return Result<UiCanvasDocumentSaveResult>::Success(
                {.status = UiCanvasDocumentSaveStatus::Conflict, .fingerprint = rechecked.Value()});
        }
        if (const Result<void> replaced = files.AtomicReplace(prepared, destination.Value()); replaced.HasError()) {
            static_cast<void>(files.RemoveDurable(prepared));
            return Result<UiCanvasDocumentSaveResult>::Failure(replaced.ErrorValue());
        }
        return Result<UiCanvasDocumentSaveResult>::Success(
            {.status = UiCanvasDocumentSaveStatus::Saved, .fingerprint = Fingerprint(serialized)});
    }

    /** @copydoc UiCanvasDocument::Open */
    Result<UiCanvasDocument> UiCanvasDocument::Open(const DocumentIdentity identity, const std::filesystem::path &absolutePath) {
        if (!identity.IsValid() || identity.key.kind != DocumentKind::UiCanvas)
            return Failure<UiCanvasDocument>(UiCanvasDocumentErrors::InvalidDocument, "The UI Canvas session identity is invalid.");
        const Result<UiCanvasDocumentSnapshot> loaded = LoadUiCanvasDocument(absolutePath);
        if (loaded.HasError())
            return Result<UiCanvasDocument>::Failure(loaded.ErrorValue());
        bool valid = false;
        const std::filesystem::path path = CanonicalPath(absolutePath, valid);
        if (!valid)
            return Failure<UiCanvasDocument>(UiCanvasDocumentErrors::InvalidPath);
        const Result<UiCanvasFileFingerprint> fingerprint = InspectUiCanvasDocumentFingerprint(path);
        if (fingerprint.HasError())
            return Result<UiCanvasDocument>::Failure(fingerprint.ErrorValue());
        return Result<UiCanvasDocument>::Success(UiCanvasDocument{identity, path, std::move(loaded).Value().document, fingerprint.Value()});
    }

    UiCanvasDocument::UiCanvasDocument(DocumentIdentity identity, std::filesystem::path path, Runtime::Ui::UiDocument document,
                                       UiCanvasFileFingerprint fingerprint) noexcept
        : identity_(std::move(identity)), path_(std::move(path)), document_(std::move(document)), fingerprint_(std::move(fingerprint)) {}

    /** @copydoc UiCanvasDocument::Document */
    const Runtime::Ui::UiDocument &UiCanvasDocument::Document() const noexcept {
        assert(document_.has_value());
        return *document_;
    }

    /** @copydoc UiCanvasDocument::AdvanceState */
    Result<UiCanvasDocumentStateId> UiCanvasDocument::AdvanceState() const {
        if (currentState_.value == std::numeric_limits<std::uint64_t>::max())
            return Failure<UiCanvasDocumentStateId>(UiCanvasDocumentErrors::StateExhausted);
        return Result<UiCanvasDocumentStateId>::Success({currentState_.value + 1U});
    }

    /** @copydoc UiCanvasDocument::Replace */
    Result<void> UiCanvasDocument::Replace(Runtime::Ui::UiDocument document) {
        if (closed_)
            return Failure(UiCanvasDocumentErrors::Closed);
        if (!document.Id().IsValid() || !document.Revision().IsValid() || document.Canvases().empty())
            return Failure(UiCanvasDocumentErrors::InvalidDocument);
        if (document.Id() != Document().Id())
            return Failure(UiCanvasDocumentErrors::InvalidDocument);
        if (document.Revision() <= Document().Revision())
            return Failure(Runtime::Ui::UiErrors::RevisionStale);
        const Result<UiCanvasDocumentStateId> nextState = AdvanceState();
        if (nextState.HasError())
            return Result<void>::Failure(nextState.ErrorValue());
        document_ = std::move(document);
        currentState_ = nextState.Value();
        return Result<void>::Success();
    }

    /** @copydoc UiCanvasDocument::Save */
    Result<UiCanvasDocumentSaveResult> UiCanvasDocument::Save(const std::filesystem::path &absoluteProjectRoot,
                                                              const ProjectMutationCoordinator &mutations, DurableFileSystem &files,
                                                              const bool overwriteConflict) {
        if (closed_)
            return Failure<UiCanvasDocumentSaveResult>(UiCanvasDocumentErrors::Closed);
        const Result<UiCanvasDocumentSaveResult> saved =
            SaveUiCanvasDocument(absoluteProjectRoot, path_, UiCanvasDocumentSnapshot{.document = Document(), .state = currentState_},
                                 fingerprint_, overwriteConflict, mutations, files);
        if (saved.HasError() || saved.Value().status != UiCanvasDocumentSaveStatus::Saved)
            return saved;
        fingerprint_ = saved.Value().fingerprint;
        savedState_ = currentState_;
        return saved;
    }

    /** @copydoc UiCanvasDocument::Reload */
    Result<void> UiCanvasDocument::Reload(const UiCanvasReloadPolicy policy) {
        if (closed_)
            return Failure(UiCanvasDocumentErrors::Closed);
        if (IsDirty() && policy == UiCanvasReloadPolicy::RequireClean)
            return Failure(UiCanvasDocumentErrors::DirtyDocument);
        const Result<UiCanvasDocumentSnapshot> loaded = LoadUiCanvasDocument(path_);
        if (loaded.HasError())
            return Result<void>::Failure(loaded.ErrorValue());
        if (loaded.Value().document.Id() != Document().Id())
            return Failure(UiCanvasDocumentErrors::InvalidDocument);
        const Result<UiCanvasDocumentStateId> nextState = AdvanceState();
        if (nextState.HasError())
            return Result<void>::Failure(nextState.ErrorValue());
        const Result<UiCanvasFileFingerprint> fingerprint = InspectUiCanvasDocumentFingerprint(path_);
        if (fingerprint.HasError())
            return Result<void>::Failure(fingerprint.ErrorValue());
        document_ = std::move(loaded).Value().document;
        currentState_ = nextState.Value();
        savedState_ = currentState_;
        fingerprint_ = fingerprint.Value();
        return Result<void>::Success();
    }

    /** @copydoc UiCanvasDocument::Close */
    Result<void> UiCanvasDocument::Close(const UiCanvasClosePolicy policy) {
        if (closed_)
            return Failure(UiCanvasDocumentErrors::Closed);
        if (IsDirty() && policy == UiCanvasClosePolicy::RequireClean)
            return Failure(UiCanvasDocumentErrors::DirtyDocument);
        document_.reset();
        closed_ = true;
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
