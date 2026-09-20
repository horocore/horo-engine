#include "Horo/Editor/EditorSurfaceIdentity.h"

#include <algorithm>
#include <limits>
#include <ranges>
#include <utility>

namespace Horo::Editor {
    namespace {
        const ErrorDomainId SurfaceIdentityDomain{"horo.editor.surface_identity"};

        using enum SurfaceCapability;

        [[nodiscard]] constexpr std::byte CapabilityByte(const SurfaceCapability capability) noexcept {
            return std::byte{static_cast<unsigned char>(capability)};
        }

        constexpr std::byte KnownSurfaceCapabilityBits =
            CapabilityByte(Pinned) | CapabilityByte(Conditional) | CapabilityByte(Closable) | CapabilityByte(Restorable);

        [[nodiscard]] bool IsAsciiAlphaNumeric(const char value) noexcept {
            const auto character = static_cast<unsigned char>(value);
            return (character >= static_cast<unsigned char>('a') && character <= static_cast<unsigned char>('z')) ||
                   (character >= static_cast<unsigned char>('0') && character <= static_cast<unsigned char>('9'));
        }

        [[nodiscard]] bool IsKnownDocumentKind(const DocumentKind kind) noexcept {
            using enum DocumentKind;
            switch (kind) {
                case None:
                case Scene:
                case Source:
                case Shader:
                case Asset:
                case Project:
                case Custom:
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool IsValidSurfaceTypeCharacter(const char value) noexcept {
            return IsAsciiAlphaNumeric(value) || value == '_' || value == '-';
        }

        [[nodiscard]] bool IsValidSurfaceTypeText(const std::string_view value) noexcept {
            if (value.empty()) {
                return false;
            }
            if (value.size() > MaximumSurfaceTypeIdBytes) {
                return false;
            }
            if (value.front() == '.') {
                return false;
            }
            if (value.back() == '.') {
                return false;
            }

            std::size_t segmentStart = 0;
            for (std::size_t index = 0; index < value.size(); ++index) {
                if (value[index] == '.') {
                    if (index == segmentStart) {
                        return false;
                    }
                    segmentStart = index + 1;
                    continue;
                }
                if (!IsValidSurfaceTypeCharacter(value[index])) {
                    return false;
                }
            }
            return segmentStart < value.size();
        }

        [[nodiscard]] bool IsValidSourceDocumentCharacter(const char value) noexcept {
            const auto character = static_cast<unsigned char>(value);
            if (value == '\\' || value == ':' || value == '*' || value == '?' || value == '"' || value == '<' || value == '>' ||
                value == '|') {
                return false;
            }
            return character >= 0x20U && character != 0x7FU;
        }

        [[nodiscard]] bool IsValidSourceDocumentSegment(const std::string_view segment) noexcept {
            if (segment.empty()) {
                return false;
            }
            if (segment == ".") {
                return false;
            }
            return segment != "..";
        }

        [[nodiscard]] bool IsValidSourceDocumentText(const std::string_view value) noexcept {
            if (value.empty()) {
                return false;
            }
            if (value.size() > MaximumSourceDocumentIdBytes) {
                return false;
            }
            if (value.front() == '/') {
                return false;
            }
            if (value.front() == '\\') {
                return false;
            }
            if (value.back() == '/') {
                return false;
            }

            std::size_t segmentStart = 0;
            for (std::size_t index = 0; index < value.size(); ++index) {
                if (value[index] == '/') {
                    if (!IsValidSourceDocumentSegment(value.substr(segmentStart, index - segmentStart))) {
                        return false;
                    }
                    segmentStart = index + 1;
                    continue;
                }
                if (!IsValidSourceDocumentCharacter(value[index])) {
                    return false;
                }
            }
            return IsValidSourceDocumentSegment(value.substr(segmentStart));
        }

        template <typename Range> [[nodiscard]] auto FindDocumentInstance(Range &documents, const DocumentInstanceId instance) noexcept {
            return std::ranges::find_if(documents, [instance](const DocumentIdentity &identity) {
                return identity.instance == instance;
            });
        }

        [[nodiscard]] Error MakeInvalidSerializedKeyError() {
            return MakeError(EditorSurfaceErrors::SerializedKeyInvalid);
        }
    }  // namespace

    namespace EditorSurfaceErrors {
        const ErrorCodeDescriptor InvalidSurfaceType{
            .domain = SurfaceIdentityDomain,
            .code = ErrorCode{"editor.surface_identity.surface_type_invalid"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Editor surface type identity is invalid.",
            .remediationHint = "Use a lowercase dotted surface identity with non-empty segments.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor InvalidDocumentKind{
            .domain = SurfaceIdentityDomain,
            .code = ErrorCode{"editor.surface_identity.document_kind_invalid"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Editor document kind is invalid.",
            .remediationHint = "Use one of the supported document kind names.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor InvalidSourceDocument{
            .domain = SurfaceIdentityDomain,
            .code = ErrorCode{"editor.surface_identity.source_document_invalid"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Source document identity is invalid.",
            .remediationHint = "Use a canonical project-relative path with forward slashes.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor InvalidDocumentInstance{
            .domain = SurfaceIdentityDomain,
            .code = ErrorCode{"editor.surface_identity.instance_invalid"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Document instance identity is invalid.",
            .remediationHint = "Use a non-zero session-local document instance value.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor InvalidDocumentKey{
            .domain = SurfaceIdentityDomain,
            .code = ErrorCode{"editor.surface_identity.document_key_invalid"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Document open identity is invalid.",
            .remediationHint = "Provide a supported document kind and project-relative source identity.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor InstanceUnknown{
            .domain = SurfaceIdentityDomain,
            .code = ErrorCode{"editor.surface_identity.instance_unknown"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Document instance is not open.",
            .remediationHint = "Refresh the workspace document state before closing the instance.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor InstanceExhausted{
            .domain = SurfaceIdentityDomain,
            .code = ErrorCode{"editor.surface_identity.instance_exhausted"},
            .defaultSeverity = ErrorSeverity::Critical,
            .summary = "No document instance identities remain in this session.",
            .remediationHint = "Restart the editor session before opening another document.",
            .retryable = true,
            .userActionable = false,
        };
        const ErrorCodeDescriptor SerializedKeyInvalid{
            .domain = SurfaceIdentityDomain,
            .code = ErrorCode{"editor.surface_identity.serialized_key_invalid"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Persisted document open identity is invalid.",
            .remediationHint = "Discard the invalid workspace entry and restore a valid project-relative identity.",
            .retryable = false,
            .userActionable = true,
        };
    }  // namespace EditorSurfaceErrors

    Result<SurfaceTypeId> SurfaceTypeId::Parse(const std::string_view value) {
        if (!IsValidSurfaceTypeText(value)) {
            return Result<SurfaceTypeId>::Failure(MakeError(EditorSurfaceErrors::InvalidSurfaceType));
        }
        return Result<SurfaceTypeId>::Success(SurfaceTypeId{std::string{value}});
    }

    const std::string &SurfaceTypeId::Value() const noexcept {
        return value_;
    }

    bool SurfaceTypeId::IsValid() const noexcept {
        return IsValidSurfaceTypeText(value_);
    }

    std::string_view ToString(const DocumentKind kind) noexcept {
        using enum DocumentKind;
        switch (kind) {
            case None:
                return {};
            case Scene:
                return "scene";
            case Source:
                return "source";
            case Shader:
                return "shader";
            case Asset:
                return "asset";
            case Project:
                return "project";
            case Custom:
                return "custom";
        }
        return {};
    }

    Result<DocumentKind> ParseDocumentKind(const std::string_view value) {
        using enum DocumentKind;
        for (const DocumentKind kind : {Scene, Source, Shader, Asset, Project, Custom}) {
            if (ToString(kind) == value) {
                return Result<DocumentKind>::Success(kind);
            }
        }
        return Result<DocumentKind>::Failure(MakeError(EditorSurfaceErrors::InvalidDocumentKind));
    }

    Result<SourceDocumentId> SourceDocumentId::Parse(const std::string_view value) {
        if (!IsValidSourceDocumentText(value)) {
            return Result<SourceDocumentId>::Failure(MakeError(EditorSurfaceErrors::InvalidSourceDocument));
        }
        return Result<SourceDocumentId>::Success(SourceDocumentId{std::string{value}});
    }

    const std::string &SourceDocumentId::Value() const noexcept {
        return value_;
    }

    bool SourceDocumentId::IsValid() const noexcept {
        return IsValidSourceDocumentText(value_);
    }

    Result<DocumentInstanceId> DocumentInstanceId::Create(const std::uint64_t value) {
        if (value == 0) {
            return Result<DocumentInstanceId>::Failure(MakeError(EditorSurfaceErrors::InvalidDocumentInstance));
        }
        return Result<DocumentInstanceId>::Success(DocumentInstanceId{value});
    }

    bool SurfaceDescriptor::IsValid() const noexcept {
        return type.IsValid() && IsKnownDocumentKind(documentKind) && (capabilities.bits & ~KnownSurfaceCapabilityBits) == std::byte{0};
    }

    Result<SurfaceDescriptor> SurfaceDescriptor::MakeViewport() {
        using enum DocumentKind;
        using enum SurfaceCapability;
        const auto type = SurfaceTypeId::Parse("horo.viewport");
        if (type.HasError()) {
            return Result<SurfaceDescriptor>::Failure(type.ErrorValue());
        }
        return Result<SurfaceDescriptor>::Success(SurfaceDescriptor{
            .type = type.Value(),
            .documentKind = DocumentKind::None,
            .capabilities = SurfaceCapabilities{CapabilityByte(Pinned) | CapabilityByte(Restorable)},
        });
    }

    Result<SurfaceDescriptor> SurfaceDescriptor::MakeGame() {
        using enum DocumentKind;
        using enum SurfaceCapability;
        const auto type = SurfaceTypeId::Parse("horo.game");
        if (type.HasError()) {
            return Result<SurfaceDescriptor>::Failure(type.ErrorValue());
        }
        return Result<SurfaceDescriptor>::Success(SurfaceDescriptor{
            .type = type.Value(),
            .documentKind = DocumentKind::None,
            .capabilities = SurfaceCapabilities{CapabilityByte(Conditional) | CapabilityByte(Closable) | CapabilityByte(Restorable)},
        });
    }

    bool DocumentOpenKey::IsValid() const noexcept {
        using enum DocumentKind;
        return kind != None && IsKnownDocumentKind(kind) && source.IsValid();
    }

    bool DocumentIdentity::IsValid() const noexcept {
        return key.IsValid() && instance.IsValid();
    }

    Result<SerializedDocumentOpenKey> SerializeDocumentOpenKey(const DocumentOpenKey &key) {
        if (!key.IsValid()) {
            return Result<SerializedDocumentOpenKey>::Failure(MakeError(EditorSurfaceErrors::InvalidDocumentKey));
        }
        return Result<SerializedDocumentOpenKey>::Success(SerializedDocumentOpenKey{
            .kind = std::string{ToString(key.kind)},
            .source = key.source.Value(),
        });
    }

    Result<DocumentOpenKey> DeserializeDocumentOpenKey(const SerializedDocumentOpenKey &serialized) {
        const auto kind = ParseDocumentKind(serialized.kind);
        const auto source = SourceDocumentId::Parse(serialized.source);
        if (kind.HasError() || source.HasError()) {
            return Result<DocumentOpenKey>::Failure(MakeInvalidSerializedKeyError());
        }

        DocumentOpenKey key{.kind = kind.Value(), .source = source.Value()};
        if (!key.IsValid()) {
            return Result<DocumentOpenKey>::Failure(MakeInvalidSerializedKeyError());
        }
        return Result<DocumentOpenKey>::Success(std::move(key));
    }

    Result<DocumentOpenResult> DocumentIdentityRegistry::Open(const DocumentOpenKey &key) {
        if (!key.IsValid()) {
            return Result<DocumentOpenResult>::Failure(MakeError(EditorSurfaceErrors::InvalidDocumentKey));
        }

        if (const auto existing = Find(key); existing.has_value()) {
            return Result<DocumentOpenResult>::Success(DocumentOpenResult{
                .identity = *existing,
                .disposition = DocumentOpenDisposition::FocusExisting,
            });
        }

        if (nextInstanceValue_ == 0) {
            return Result<DocumentOpenResult>::Failure(MakeError(EditorSurfaceErrors::InstanceExhausted));
        }

        const auto instance = DocumentInstanceId::Create(nextInstanceValue_);
        if (instance.HasError()) {
            return Result<DocumentOpenResult>::Failure(instance.ErrorValue());
        }

        const DocumentIdentity identity{.key = key, .instance = instance.Value()};
        openDocuments_.push_back(identity);
        if (nextInstanceValue_ == std::numeric_limits<std::uint64_t>::max()) {
            nextInstanceValue_ = 0;
        } else {
            ++nextInstanceValue_;
        }
        return Result<DocumentOpenResult>::Success(DocumentOpenResult{
            .identity = identity,
            .disposition = DocumentOpenDisposition::Opened,
        });
    }

    auto DocumentIdentityRegistry::FindInstanceIterator(const DocumentInstanceId instance) noexcept
        -> std::vector<DocumentIdentity>::iterator {
        return FindDocumentInstance(openDocuments_, instance);
    }

    auto DocumentIdentityRegistry::FindInstanceIterator(const DocumentInstanceId instance) const noexcept
        -> std::vector<DocumentIdentity>::const_iterator {
        return FindDocumentInstance(openDocuments_, instance);
    }

    Result<void> DocumentIdentityRegistry::Close(const DocumentInstanceId instance) {
        if (!instance.IsValid()) {
            return Result<void>::Failure(MakeError(EditorSurfaceErrors::InvalidDocumentInstance));
        }

        const auto iterator = FindInstanceIterator(instance);
        if (iterator == openDocuments_.end()) {
            return Result<void>::Failure(MakeError(EditorSurfaceErrors::InstanceUnknown));
        }

        openDocuments_.erase(iterator);
        return Result<void>::Success();
    }

    std::optional<DocumentIdentity> DocumentIdentityRegistry::Find(const DocumentOpenKey &key) const {
        if (!key.IsValid()) {
            return std::nullopt;
        }
        const auto iterator = std::ranges::find_if(openDocuments_, [&key](const DocumentIdentity &identity) {
            return identity.key == key;
        });
        if (iterator == openDocuments_.end()) {
            return std::nullopt;
        }
        return *iterator;
    }

    std::optional<DocumentIdentity> DocumentIdentityRegistry::Find(const DocumentInstanceId instance) const {
        if (!instance.IsValid()) {
            return std::nullopt;
        }
        const auto iterator = FindInstanceIterator(instance);
        if (iterator == openDocuments_.end()) {
            return std::nullopt;
        }
        return *iterator;
    }

    std::size_t DocumentIdentityRegistry::Size() const noexcept {
        return openDocuments_.size();
    }
}  // namespace Horo::Editor
