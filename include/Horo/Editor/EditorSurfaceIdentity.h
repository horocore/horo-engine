#pragma once

/**
 * @file EditorSurfaceIdentity.h
 * @brief Typed editor surface, document, and source-identity contracts.
 */

#include "Horo/Foundation/ErrorCode.h"
#include "Horo/Foundation/Result.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::Editor {
    inline constexpr std::size_t MaximumSurfaceTypeIdBytes = 128;
    inline constexpr std::size_t MaximumSourceDocumentIdBytes = 1024;

    namespace EditorSurfaceErrors {
        extern const ErrorCodeDescriptor InvalidSurfaceType;
        extern const ErrorCodeDescriptor InvalidDocumentKind;
        extern const ErrorCodeDescriptor InvalidSourceDocument;
        extern const ErrorCodeDescriptor InvalidDocumentInstance;
        extern const ErrorCodeDescriptor InvalidDocumentKey;
        extern const ErrorCodeDescriptor InstanceUnknown;
        extern const ErrorCodeDescriptor InstanceExhausted;
        extern const ErrorCodeDescriptor SerializedKeyInvalid;
    }  // namespace EditorSurfaceErrors

    /** @brief Stable namespaced identity of an editor surface type, such as `horo.viewport`. */
    class SurfaceTypeId final {
    public:
        SurfaceTypeId() = default;

        /**
         * @brief Parses a lowercase dotted surface identity.
         * @param value Namespaced identity to validate.
         * @return Validated identity or EditorSurfaceErrors::InvalidSurfaceType.
         */
        [[nodiscard]] static Result<SurfaceTypeId> Parse(std::string_view value);

        /** @brief Returns the canonical persistent identity text. */
        [[nodiscard]] const std::string &Value() const noexcept;
        /** @brief Reports whether this value contains a parsed identity. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const SurfaceTypeId &) const noexcept = default;

    private:
        explicit SurfaceTypeId(std::string value) : value_(std::move(value)) {}

        std::string value_;
    };

    /** @brief Closed set of document semantics used by editor surface routing. */
    enum class DocumentKind : std::uint8_t {
        None,
        Scene,
        Source,
        Shader,
        Asset,
        Project,
        Custom,
    };

    /** @brief Returns the canonical persisted name of a document kind, or empty for None. */
    [[nodiscard]] std::string_view ToString(DocumentKind kind) noexcept;
    /**
     * @brief Parses a canonical persisted document-kind name.
     * @param value Canonical kind name.
     * @return Parsed kind or EditorSurfaceErrors::InvalidDocumentKind.
     */
    [[nodiscard]] Result<DocumentKind> ParseDocumentKind(std::string_view value);

    /** @brief Stable project-relative identity of one source document. */
    class SourceDocumentId final {
    public:
        SourceDocumentId() = default;

        /**
         * @brief Parses a canonical project-relative generic path.
         * @param value Project-relative UTF-8 path using `/` separators.
         * @return Validated source identity or EditorSurfaceErrors::InvalidSourceDocument.
         */
        [[nodiscard]] static Result<SourceDocumentId> Parse(std::string_view value);

        /** @brief Returns the canonical project-relative path. */
        [[nodiscard]] const std::string &Value() const noexcept;
        /** @brief Reports whether this value contains a parsed identity. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const SourceDocumentId &) const noexcept = default;

    private:
        explicit SourceDocumentId(std::string value) : value_(std::move(value)) {}

        std::string value_;
    };

    /** @brief Session-local identity of one open document instance; never persisted as source identity. */
    class DocumentInstanceId final {
    public:
        DocumentInstanceId() = default;

        /**
         * @brief Creates an owner-issued non-zero instance identity.
         * @param value Monotonic session-local value.
         * @return Validated instance identity or EditorSurfaceErrors::InvalidDocumentInstance.
         */
        [[nodiscard]] static Result<DocumentInstanceId> Create(std::uint64_t value);

        /** @brief Returns the owner-issued session-local value. */
        [[nodiscard]] constexpr std::uint64_t Value() const noexcept {
            return value_;
        }

        /** @brief Reports whether this value contains a usable identity. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value_ != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const DocumentInstanceId &) const noexcept = default;

    private:
        explicit constexpr DocumentInstanceId(std::uint64_t value) noexcept : value_(value) {}

        std::uint64_t value_{};
    };

    /** @brief Surface lifecycle capability bit. */
    enum class SurfaceCapability : std::uint8_t {
        Pinned = 1U << 0U,      /**< Host-owned singleton surface that remains available in its default slot. */
        Conditional = 1U << 1U, /**< Surface is available only while its owning runtime/editor state exists. */
        Closable = 1U << 2U,    /**< User may remove the surface instance from the workspace layout. */
        Restorable = 1U << 3U,  /**< Workspace persistence may restore the surface by stable type identity. */
    };

    /** @brief Explicit lifecycle capabilities for one surface descriptor. */
    struct SurfaceCapabilities final {
        std::byte bits{};

        [[nodiscard]] constexpr bool Has(const SurfaceCapability capability) const noexcept {
            return (bits & std::byte{static_cast<unsigned char>(capability)}) != std::byte{0};
        }

        [[nodiscard]] constexpr auto operator<=>(const SurfaceCapabilities &) const noexcept = default;
    };

    /** @brief Stable type and lifecycle contract for one editor surface. */
    struct SurfaceDescriptor final {
        SurfaceTypeId type;
        DocumentKind documentKind{DocumentKind::None};
        SurfaceCapabilities capabilities;

        /** @brief Returns whether the descriptor has a valid type and lifecycle contract. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Returns the pinned Viewport surface contract. */
        [[nodiscard]] static Result<SurfaceDescriptor> MakeViewport();
        /** @brief Returns the conditional Game surface contract. */
        [[nodiscard]] static Result<SurfaceDescriptor> MakeGame();

        [[nodiscard]] auto operator<=>(const SurfaceDescriptor &) const noexcept = default;
    };

    /** @brief Persistent identity used to find or restore a document instance. */
    struct DocumentOpenKey final {
        DocumentKind kind{DocumentKind::None};
        SourceDocumentId source;

        /** @brief Reports whether kind and source form a usable open identity. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const DocumentOpenKey &) const noexcept = default;
    };

    /** @brief Complete identity of one open document, including its session-local instance. */
    struct DocumentIdentity final {
        DocumentOpenKey key;
        DocumentInstanceId instance;

        /** @brief Reports whether the persistent key and session instance are valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const DocumentIdentity &) const noexcept = default;
    };

    /** @brief Outcome of opening a document key. */
    enum class DocumentOpenDisposition : std::uint8_t {
        Opened,
        FocusExisting,
    };

    /** @brief Identity plus routing outcome returned by a document open request. */
    struct DocumentOpenResult final {
        DocumentIdentity identity;
        DocumentOpenDisposition disposition{DocumentOpenDisposition::Opened};
    };

    /** @brief Persisted document routing key; intentionally excludes the session instance ID. */
    struct SerializedDocumentOpenKey final {
        std::string kind;
        std::string source;

        [[nodiscard]] auto operator<=>(const SerializedDocumentOpenKey &) const noexcept = default;
    };

    /**
     * @brief Serializes the persistent portion of a document identity for workspace restore.
     * @param key Persistent document kind and source identity.
     * @return Canonical serialized fields or a typed validation error.
     */
    [[nodiscard]] Result<SerializedDocumentOpenKey> SerializeDocumentOpenKey(const DocumentOpenKey &key);
    /**
     * @brief Restores a persistent document routing key without reusing a session instance ID.
     * @param serialized Persisted kind and project-relative source path.
     * @return Validated open key or EditorSurfaceErrors::SerializedKeyInvalid.
     */
    [[nodiscard]] Result<DocumentOpenKey> DeserializeDocumentOpenKey(const SerializedDocumentOpenKey &serialized);

    /** @brief Owns open document identities and enforces focus/reopen semantics. */
    class DocumentIdentityRegistry final {
    public:
        DocumentIdentityRegistry() = default;

        /**
         * @brief Starts allocation at an explicit session-local instance value.
         * @param nextInstance Next instance identity to issue; use the default constructor for the normal value of one.
         * @note The explicit seed keeps the monotonic allocator deterministic for restored hosts and boundary tests.
         */
        explicit DocumentIdentityRegistry(DocumentInstanceId nextInstance) noexcept : nextInstanceValue_(nextInstance.Value()) {}

        /**
         * @brief Opens one key or returns its existing instance for focus routing.
         * @param key Persistent document identity.
         * @return Opened or existing identity, or a typed validation/capacity error.
         */
        [[nodiscard]] Result<DocumentOpenResult> Open(const DocumentOpenKey &key);
        /**
         * @brief Closes one current instance.
         * @param instance Session-local instance to close.
         * @return Success or InstanceUnknown/InvalidDocumentInstance.
         */
        [[nodiscard]] Result<void> Close(DocumentInstanceId instance);

        /** @brief Finds the current instance for a persistent key, if open. */
        [[nodiscard]] std::optional<DocumentIdentity> Find(const DocumentOpenKey &key) const;
        /** @brief Finds the current identity for an instance, if open. */
        [[nodiscard]] std::optional<DocumentIdentity> Find(DocumentInstanceId instance) const;
        /** @brief Returns the number of currently open document instances. */
        [[nodiscard]] std::size_t Size() const noexcept;

    private:
        [[nodiscard]] auto FindInstanceIterator(DocumentInstanceId instance) noexcept -> std::vector<DocumentIdentity>::iterator;
        [[nodiscard]] auto FindInstanceIterator(DocumentInstanceId instance) const noexcept
            -> std::vector<DocumentIdentity>::const_iterator;

        std::uint64_t nextInstanceValue_{1};
        std::vector<DocumentIdentity> openDocuments_;
    };
}  // namespace Horo::Editor
