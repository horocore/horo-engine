#pragma once

/**
 * @file SourceFileOpenService.h
 * @brief Project-contained source classification and typed open routing.
 */

#include "Horo/Editor/EditorSurfaceIdentity.h"
#include "Horo/Foundation/Result.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Horo::Editor {
    namespace SourceOpenErrors {
        extern const ErrorCodeDescriptor InvalidRequest;    /**< Request has no usable source path. */
        extern const ErrorCodeDescriptor Missing;           /**< Canonical source file does not exist. */
        extern const ErrorCodeDescriptor Unsafe;            /**< Path or symlink escapes the project boundary. */
        extern const ErrorCodeDescriptor Unsupported;       /**< File type cannot use the selected route. */
        extern const ErrorCodeDescriptor EditorUnavailable; /**< No permitted editor route is available. */
    }  // namespace SourceOpenErrors

    /** @brief Semantic class assigned to a project file before source opening. */
    enum class SourceFileKind : std::uint8_t {
        NativeSource,
        HoroScript,
        ProjectText,
        Unsupported,
    };

    /** @brief Caller that requested one source-open operation. */
    enum class SourceOpenOrigin : std::uint8_t {
        AssetActivation,
        DiagnosticNavigation,
        Command,
    };

    /** @brief Whether an open request may use the explicit external-editor fallback. */
    enum class SourceOpenMode : std::uint8_t {
        EmbeddedOnly,
        AllowExternalFallback,
    };

    /** @brief Presentation route selected after path and policy validation. */
    enum class SourceOpenRoute : std::uint8_t {
        EmbeddedWorkspace,
        ExternalEditorFallback,
    };

    /**
     * @brief Extension and capability policy for project source opening.
     * @details Extension comparisons are case-insensitive. Entries may include or omit
     *          the leading dot; the service normalizes them before classification.
     */
    struct SourceFilePolicy final {
        std::vector<std::string> nativeSourceExtensions; /**< Native source suffixes. */
        std::vector<std::string> horoScriptExtensions;   /**< Horo Script suffixes. */
        std::vector<std::string> projectTextExtensions;  /**< Project text suffixes. */
        std::vector<std::string> projectTextFileNames;   /**< Extensionless/project-name text files. */
        bool allowSymlinkedFiles{true};                  /**< Permit symlinks whose resolved target stays inside the project. */
        bool embeddedEditorAvailable{true};              /**< Embedded workspace route is installed. */
        bool externalEditorAvailable{true};              /**< External fallback route is installed. */

        /**
         * @brief Returns the default M2 source/text policy.
         * @return A value containing native source, Horo Script, and project-text extensions.
         */
        [[nodiscard]] static SourceFilePolicy Default();
    };

    /** @brief Policy classification for one file name or path. */
    struct SourceFileClassification final {
        SourceFileKind kind{SourceFileKind::Unsupported};
        std::string extension;

        /** @brief Reports whether the policy recognizes this file as editable source/text. */
        [[nodiscard]] bool IsSupported() const noexcept {
            return kind != SourceFileKind::Unsupported;
        }
    };

    /** @brief One typed source-open request shared by asset, diagnostic, and command callers. */
    struct SourceOpenRequest final {
        std::filesystem::path path;                                 /**< Absolute or project-relative candidate path. */
        SourceOpenOrigin origin{SourceOpenOrigin::Command};         /**< Request source for host telemetry/routing. */
        SourceOpenMode mode{SourceOpenMode::AllowExternalFallback}; /**< Whether fallback may be selected. */
        std::uint32_t line{};                                       /**< One-based diagnostic line, or zero when not addressed. */
        std::uint32_t column{};                                     /**< One-based diagnostic column, or zero when not addressed. */
    };

    /** @brief Canonical project location returned for a validated source file. */
    struct SourceOpenLocation final {
        std::filesystem::path absolutePath; /**< Canonical absolute path after symlink resolution. */
        SourceDocumentId document;          /**< Canonical project-relative source identity. */
    };

    /**
     * @brief Result of one source-open operation.
     * @details Embedded routes carry the existing-or-new document identity. Fallback routes
     *          carry only the canonical safe location so the host can explicitly reveal or
     *          launch an external editor without receiving an unvalidated path.
     */
    struct SourceOpenResult final {
        SourceOpenOrigin origin{SourceOpenOrigin::Command};        /**< Origin copied from the request. */
        SourceOpenRoute route{SourceOpenRoute::EmbeddedWorkspace}; /**< Selected host presentation route. */
        SourceFileClassification classification;                   /**< Extension policy result. */
        SourceOpenLocation location;                               /**< Safe canonical location. */
        std::optional<DocumentOpenResult> document;                /**< Embedded identity result; empty for fallback. */
        std::uint32_t line{};                                      /**< Line copied from the request. */
        std::uint32_t column{};                                    /**< Column copied from the request. */
    };

    /**
     * @brief Classifies and routes project source files through one containment boundary.
     * @details The service resolves relative and absolute inputs to a canonical project
     *          location, permits symlinks only when their resolved target remains inside the
     *          project, and reuses one document identity registry for equivalent paths.
     */
    class SourceFileOpenService final {
    public:
        /**
         * @brief Creates a source-open service for one project root.
         * @param projectRoot Project root containing the source files.
         * @param policy Extension, symlink, and editor-capability policy.
         */
        explicit SourceFileOpenService(std::filesystem::path projectRoot, SourceFilePolicy policy = SourceFilePolicy::Default());

        /** @brief Returns the normalized project root captured by this service. */
        [[nodiscard]] const std::filesystem::path &ProjectRoot() const noexcept {
            return projectRoot_;
        }

        /** @brief Returns the immutable policy used for future requests. */
        [[nodiscard]] const SourceFilePolicy &Policy() const noexcept {
            return policy_;
        }

        /**
         * @brief Classifies a path using the configured extension policy.
         * @param path Path whose file name and extension should be inspected.
         * @return Classification; this method does not access the filesystem.
         */
        [[nodiscard]] SourceFileClassification Classify(const std::filesystem::path &path) const;

        /**
         * @brief Validates, canonicalizes, and routes one source-open request.
         * @param request Asset, diagnostic, or command request with an absolute or project-relative path.
         * @return A safe embedded/fallback route or SourceOpenErrors for invalid, missing,
         *         unsafe, unsupported, or unavailable requests.
         */
        [[nodiscard]] Result<SourceOpenResult> Open(const SourceOpenRequest &request);

    private:
        [[nodiscard]] std::filesystem::path NormalizeInputPath(const std::filesystem::path &path) const;
        [[nodiscard]] Result<SourceOpenLocation> ResolveLocation(const std::filesystem::path &path) const;

        bool projectRootValid_{false};
        std::filesystem::path projectRoot_;
        SourceFilePolicy policy_;
        DocumentIdentityRegistry documentRegistry_;
    };
}  // namespace Horo::Editor
