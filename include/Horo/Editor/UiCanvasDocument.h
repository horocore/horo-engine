#pragma once

/**
 * @file UiCanvasDocument.h
 * @brief Persistent editor-owned UI Canvas document and workspace lifecycle contracts.
 */

#include "Horo/Editor/EditorSurfaceIdentity.h"
#include "Horo/Editor/ProjectMutation.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Runtime/Ui/UiDocument.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Horo::Editor {
    inline constexpr std::string_view UiCanvasDocumentExtension = ".uicanvas";
    inline constexpr std::uint32_t UiCanvasDocumentFormatVersion = 1;
    inline constexpr std::size_t MaximumUiCanvasDocumentBytes = 64ULL * 1024ULL * 1024ULL;

    namespace UiCanvasDocumentErrors {
        extern const ErrorCodeDescriptor InvalidPath;
        extern const ErrorCodeDescriptor Missing;
        extern const ErrorCodeDescriptor ReadFailed;
        extern const ErrorCodeDescriptor Malformed;
        extern const ErrorCodeDescriptor UnsupportedVersion;
        extern const ErrorCodeDescriptor InvalidDocument;
        extern const ErrorCodeDescriptor WriteFailed;
        extern const ErrorCodeDescriptor Conflict;
        extern const ErrorCodeDescriptor DirtyDocument;
        extern const ErrorCodeDescriptor Closed;
        extern const ErrorCodeDescriptor StateExhausted;
    }  // namespace UiCanvasDocumentErrors

    /** @brief Canonical byte identity of one `.uicanvas` source file. */
    struct UiCanvasFileFingerprint final {
        bool exists{false};
        std::uintmax_t byteSize{};
        std::string checksum;

        [[nodiscard]] bool operator==(const UiCanvasFileFingerprint &) const noexcept = default;
    };

    /** @brief Editor-owned identity of one committed in-memory UI Canvas state. */
    struct UiCanvasDocumentStateId final {
        std::uint64_t value{};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const UiCanvasDocumentStateId &) const noexcept = default;
    };

    /** @brief Immutable document snapshot passed to persistence and derived-data boundaries. */
    struct UiCanvasDocumentSnapshot final {
        Runtime::Ui::UiDocument document;
        UiCanvasDocumentStateId state;
        UiCanvasFileFingerprint fingerprint; /**< Canonical fingerprint of the bytes used to load the document. */
    };

    /** @brief Result category of a conflict-aware UI Canvas save attempt. */
    enum class UiCanvasDocumentSaveStatus : std::uint8_t {
        Saved,
        Conflict,
    };

    /** @brief Durable identity returned after a UI Canvas save attempt. */
    struct UiCanvasDocumentSaveResult final {
        UiCanvasDocumentSaveStatus status{UiCanvasDocumentSaveStatus::Conflict};
        UiCanvasFileFingerprint fingerprint;
    };

    /** @brief Policy used when a dirty UI Canvas is reloaded from disk. */
    enum class UiCanvasReloadPolicy : std::uint8_t {
        RequireClean,
        DiscardChanges,
    };

    /** @brief Policy used when a UI Canvas session is closed. */
    enum class UiCanvasClosePolicy : std::uint8_t {
        RequireClean,
        DiscardChanges,
    };

    /**
     * @brief Reads the bounded durable identity of one UI Canvas source file.
     * @param absolutePath Absolute `.uicanvas` path.
     * @return Existing fingerprint, a non-existing fingerprint, or a typed read/path error.
     */
    [[nodiscard]] Result<UiCanvasFileFingerprint> InspectUiCanvasDocumentFingerprint(const std::filesystem::path &absolutePath);

    /**
     * @brief Loads and validates one authored Runtime UI document from a `.uicanvas` file.
     * @param absolutePath Absolute `.uicanvas` path.
     * @return Immutable runtime UI document, initial editor state identity, and source fingerprint.
     */
    [[nodiscard]] Result<UiCanvasDocumentSnapshot> LoadUiCanvasDocument(const std::filesystem::path &absolutePath);

    /**
     * @brief Durably saves one immutable UI Canvas snapshot with conflict detection.
     * @param absoluteProjectRoot Absolute project root owning the document.
     * @param absolutePath Absolute project-contained `.uicanvas` destination.
     * @param snapshot Immutable authored document snapshot to serialize.
     * @param expectedFingerprint Last canonical fingerprint accepted by this session.
     * @param overwriteConflict True only after an explicit user decision to replace changed bytes.
     * @param mutations Shared project mutation coordinator.
     * @param files Durable filesystem implementation.
     * @return Saved fingerprint, Conflict without mutation, or a typed persistence error.
     */
    [[nodiscard]] Result<UiCanvasDocumentSaveResult> SaveUiCanvasDocument(
        const std::filesystem::path &absoluteProjectRoot, const std::filesystem::path &absolutePath,
        const UiCanvasDocumentSnapshot &snapshot, const UiCanvasFileFingerprint &expectedFingerprint, bool overwriteConflict,
        const ProjectMutationCoordinator &mutations, DurableFileSystem &files);

    /**
     * @brief Owns one open UI Canvas authoring session without owning renderer/runtime instances.
     * @details The session stores only an immutable Runtime UI authored snapshot, source identity,
     *          dirty/saved state and durable fingerprint. Workspace placement and session identity
     *          remain owned by WorkspacePanelHost/DocumentIdentityRegistry.
     */
    class UiCanvasDocument final {
    public:
        /**
         * @brief Opens one typed UI Canvas document session.
         * @param identity Existing workspace identity whose key must have kind UiCanvas.
         * @param absolutePath Absolute `.uicanvas` source path.
         * @return Loaded session or a typed load/identity error.
         */
        [[nodiscard]] static Result<UiCanvasDocument> Open(DocumentIdentity identity, const std::filesystem::path &absolutePath);

        UiCanvasDocument(UiCanvasDocument &&) noexcept = default;
        UiCanvasDocument &operator=(UiCanvasDocument &&) noexcept = default;
        UiCanvasDocument(const UiCanvasDocument &) = delete;
        UiCanvasDocument &operator=(const UiCanvasDocument &) = delete;
        ~UiCanvasDocument() = default;

        /** @brief Returns the session identity; the instance is never persisted. */
        [[nodiscard]] const DocumentIdentity &Identity() const noexcept {
            return identity_;
        }

        /** @brief Returns the canonical source path owned by this session. */
        [[nodiscard]] const std::filesystem::path &Path() const noexcept {
            return path_;
        }

        /** @brief Returns the immutable current Runtime UI authored snapshot. */
        [[nodiscard]] const Runtime::Ui::UiDocument &Document() const noexcept;

        /** @brief Returns the current editor state identity. */
        [[nodiscard]] UiCanvasDocumentStateId CurrentState() const noexcept {
            return currentState_;
        }

        /** @brief Returns the state identity last accepted by the canonical file. */
        [[nodiscard]] UiCanvasDocumentStateId SavedState() const noexcept {
            return savedState_;
        }

        /** @brief Reports whether current authored state differs from the last saved state. */
        [[nodiscard]] bool IsDirty() const noexcept {
            return currentState_ != savedState_;
        }

        /** @brief Reports whether the session has been explicitly closed. */
        [[nodiscard]] bool IsClosed() const noexcept {
            return closed_;
        }

        /**
         * @brief Replaces the immutable authored state after an editor command commits.
         * @param document Complete validated Runtime UI authored document.
         * @return Success or a stale/invalid/closed-state error; failure leaves the session unchanged.
         */
        [[nodiscard]] Result<void> Replace(Runtime::Ui::UiDocument document);

        /**
         * @brief Saves the current state through the project mutation and durable filesystem boundaries.
         * @param absoluteProjectRoot Absolute project root owning the source.
         * @param mutations Shared project mutation coordinator.
         * @param files Durable filesystem implementation.
         * @param overwriteConflict Explicitly replace externally changed bytes when true.
         * @return Save outcome or a typed persistence error.
         */
        [[nodiscard]] Result<UiCanvasDocumentSaveResult> Save(const std::filesystem::path &absoluteProjectRoot,
                                                              const ProjectMutationCoordinator &mutations, DurableFileSystem &files,
                                                              bool overwriteConflict = false);

        /**
         * @brief Reloads canonical bytes into the session.
         * @param policy Whether dirty authored state must be preserved or discarded.
         * @return Success or a typed dirty/load/closed error; failure leaves the current state unchanged.
         */
        [[nodiscard]] Result<void> Reload(UiCanvasReloadPolicy policy = UiCanvasReloadPolicy::RequireClean);

        /**
         * @brief Validates the close decision for this session and marks it closed.
         * @param policy Whether dirty authored state may be discarded.
         * @return Success or UiCanvasDocumentErrors::DirtyDocument/Closed.
         */
        [[nodiscard]] Result<void> Close(UiCanvasClosePolicy policy = UiCanvasClosePolicy::RequireClean);

    private:
        UiCanvasDocument(DocumentIdentity identity, std::filesystem::path path, Runtime::Ui::UiDocument document,
                         UiCanvasFileFingerprint fingerprint) noexcept;

        /**
         * @brief Allocates the next editor state identity without mutating the session.
         * @return The next state identity, or StateExhausted when no identity remains.
         */
        [[nodiscard]] Result<UiCanvasDocumentStateId> AdvanceState() const;

        DocumentIdentity identity_;
        std::filesystem::path path_;
        std::optional<Runtime::Ui::UiDocument> document_;
        UiCanvasFileFingerprint fingerprint_;
        UiCanvasDocumentStateId currentState_{1};
        UiCanvasDocumentStateId savedState_{1};
        bool closed_{false};
    };
}  // namespace Horo::Editor
