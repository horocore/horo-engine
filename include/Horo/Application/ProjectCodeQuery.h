#pragma once

/** @file ProjectCodeQuery.h
 * @brief Bounded, read-only project code queries over explicit host-owned capabilities.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Platform/ProjectReadFiles.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Application {
    namespace CodeQueryErrors {
        extern const ErrorCodeDescriptor Invalid;
        extern const ErrorCodeDescriptor UnsafePath;
        extern const ErrorCodeDescriptor ReadFailed;
        extern const ErrorCodeDescriptor Binary;
        extern const ErrorCodeDescriptor Encoding;
        extern const ErrorCodeDescriptor Capacity;
        extern const ErrorCodeDescriptor Stale;
        extern const ErrorCodeDescriptor Unavailable;
    }  // namespace CodeQueryErrors

    /** @brief Closed read-only responsibilities; there is no document-open or execution operation. */
    enum class CodeQueryKind : std::uint8_t {
        Files,
        Text,
        Search,
        Symbols,
        Diagnostics,
        BuildStatus,
        TestStatus
    };
    /** @brief Typed observation classification, independent of protocol spelling. */
    enum class CodeQueryRecordKind : std::uint8_t {
        File,
        Match,
        Symbol,
        Diagnostic,
        Build,
        Test
    };

    /** @brief Finite application budgets; hosts may reduce but never exceed these defaults. */
    struct CodeQueryLimits final {
        std::size_t maximumFiles{4096};
        std::size_t maximumFileBytes{1U << 20U};
        std::size_t maximumObservationBytes{1U << 20U};
        std::size_t maximumPatternBytes{128};
        std::size_t maximumPageItems{128};
        std::size_t maximumTextPageBytes{4096};
        std::size_t maximumSearchSteps{4U << 20U};
        std::chrono::milliseconds maximumDuration{1000};
    };

    /** @brief Project fence and cooperative stop view borrowed only during one synchronous query. */
    using CodeQueryContext = Platform::ProjectReadContext;

    /** @brief Portable request; offsets index bytes for Text and records for all other operations. */
    struct CodeQueryRequest final {
        CodeQueryKind kind{CodeQueryKind::Files};
        std::string path;    /**< Canonical UTF-8 project-relative spelling; Files permits the empty project root. */
        std::string pattern; /**< Search is bounded, case-sensitive literal UTF-8; no regular-expression engine is invoked. */
        std::size_t offset{};
        std::size_t limit{64};
        std::optional<std::string> expectedRevision; /**< Mandatory for continuation pages; exact authority/content revision. */
    };

    /** @brief Safe owned query row; positions are one-based where present, otherwise zero. */
    struct CodeQueryRecord final {
        CodeQueryRecordKind kind{CodeQueryRecordKind::File};
        std::string path;
        std::string label;
        std::uint32_t line{};
        std::uint32_t column{};
        std::uint64_t byteOffset{};
        std::uint64_t byteCount{};
    };

    /** @brief Owned result with explicit continuation and exact revision fence. */
    struct CodeQueryPage final {
        std::string revision;
        std::uint64_t projectGeneration{};
        std::vector<CodeQueryRecord> records;
        std::string text;
        std::optional<std::size_t> nextOffset;
        std::uint64_t droppedRecords{}; /**< Producer retention loss, distinct from query pagination. */
    };

    /** @brief One authoritative text observation, never an editable document session. */
    using CodeQueryTextSnapshot = Platform::ProjectReadTextSnapshot;

    /** @brief Complete bounded producer projection; empty success still requires a valid revision. */
    struct CodeQueryObservation final {
        std::string projectIdentity;
        std::uint64_t projectGeneration{};
        std::string revision;
        std::vector<CodeQueryRecord> records;
        std::uint64_t droppedRecords{};
    };

    /** @brief Injected existing-snapshot/language/diagnostic/status reads; no authority to open, build or mutate.
     * Callbacks execute on the host-declared owner context, must honor stop/deadline and finite limits,
     * and retain their producer owners. A missing callback returns typed Unavailable. Existing text
     * callback returns no value only when no open document snapshot exists, allowing a bounded disk read.
     * Foreign errors are preserved. The service never discovers or invokes an editor/language singleton.
     */
    struct CodeQueryProviders final {
        std::function<Result<std::optional<CodeQueryTextSnapshot>>(std::string_view, const CodeQueryContext &)> existingText;
        std::function<Result<CodeQueryObservation>(const CodeQueryRequest &, const CodeQueryContext &)> symbols;
        std::function<Result<CodeQueryObservation>(const CodeQueryRequest &, const CodeQueryContext &)> diagnostics;
        std::function<Result<CodeQueryObservation>(const CodeQueryRequest &, const CodeQueryContext &)> buildStatus;
        std::function<Result<CodeQueryObservation>(const CodeQueryRequest &, const CodeQueryContext &)> testStatus;
    };

    /** @brief Explicit root-bound application capability, owning no cache, jobs, document or process.
     * Purely synchronous queries run on the composition root's declared owner thread; filesystem work
     * belongs on its Background scheduler. Const calls may run concurrently only when injected providers
     * support that use. Closing MCP admission/draining callbacks precedes destruction of provider owners.
     * No persistent state changes on query success/failure; dropping the capability needs no shutdown job.
     */
    class ProjectCodeQuery final {
    public:
        /** @brief Admits explicit project read capabilities and finite query policy.
         * @param files Owned root-bound native/restricted file capability, or absence for existing-snapshot-only hosts.
         * @param projectIdentity Exact host identity, not a client-supplied root path.
         * @param projectGeneration Nonzero project-session revision.
         * @param providers Explicit read-only capability leases.
         * @param limits Finite limits no larger than defaults.
         * @return Owned capability or typed configuration/path/allocation failure.
         */
        [[nodiscard]] static Result<std::shared_ptr<ProjectCodeQuery>> Create(std::shared_ptr<const Platform::IProjectReadFiles> files,
                                                                              std::string projectIdentity, std::uint64_t projectGeneration,
                                                                              CodeQueryProviders providers = {},
                                                                              const CodeQueryLimits &limits = {});
        /** @brief Reads one bounded page, preserving project/provider/content revision fences.
         * @param request Typed query with optional exact continuation revision.
         * @param context Current project, cancellation, deadline and live authority observer.
         * @return Owned bounded page or explicit path/text/service/stale/stop/capacity error.
         * @note Native file capabilities must retain root identity through no-follow handle traversal.
         * The application does no filesystem I/O itself. No query publishes a cache or document.
         */
        [[nodiscard]] Result<CodeQueryPage> Query(const CodeQueryRequest &request, const CodeQueryContext &context) const;

    private:
        /** @brief Retains only explicitly supplied read capabilities and owned finite configuration. */
        ProjectCodeQuery(std::shared_ptr<const Platform::IProjectReadFiles> files, std::string identity, std::uint64_t generation,
                         CodeQueryProviders providers, const CodeQueryLimits &limits);
        std::shared_ptr<const Platform::IProjectReadFiles> files_;
        std::string identity_;
        std::uint64_t generation_;
        CodeQueryProviders providers_;
        CodeQueryLimits limits_;
    };
}  // namespace Horo::Application
