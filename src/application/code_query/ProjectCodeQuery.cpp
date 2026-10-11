#include "Horo/Foundation/Utf8.h"
#include "ProjectCodeQueryInternal.h"

#include <algorithm>
#include <new>

namespace Horo::Application {
    namespace {
        /** @brief Projects query budgets onto the narrower read-only filesystem contract. */
        Platform::ProjectReadLimits FileLimits(const CodeQueryLimits &limits) {
            return {limits.maximumFiles, limits.maximumFileBytes, limits.maximumObservationBytes, limits.maximumDuration};
        }

        /** @brief Validates portable typed input before any provider or disk read. */
        Result<void> Admit(const CodeQueryRequest &request, const CodeQueryLimits &limits) {
            using enum CodeQueryKind;
            if (static_cast<unsigned>(request.kind) > static_cast<unsigned>(TestStatus) || request.limit == 0 ||
                request.limit > (request.kind == Text ? limits.maximumTextPageBytes : limits.maximumPageItems) ||
                (request.expectedRevision && (request.expectedRevision->empty() || request.expectedRevision->size() > 256 ||
                                              !IsValidUtf8ScalarSequence(*request.expectedRevision))))
                return Result<void>::Failure(MakeError(CodeQueryErrors::Invalid));
            if (const bool allowRoot =
                    request.kind == Files || request.kind == Diagnostics || request.kind == BuildStatus || request.kind == TestStatus;
                !CodeQueryDetail::ValidPath(request.path, allowRoot))
                return Result<void>::Failure(MakeError(CodeQueryErrors::UnsafePath));
            if (request.kind == Search && (request.pattern.empty() || request.pattern.size() > limits.maximumPatternBytes ||
                                           !IsValidUtf8ScalarSequence(request.pattern) || request.pattern.find('\0') != std::string::npos))
                return Result<void>::Failure(MakeError(CodeQueryErrors::Invalid));
            if (request.kind != Search && !request.pattern.empty())
                return Result<void>::Failure(MakeError(CodeQueryErrors::Invalid));
            if ((request.kind == BuildStatus || request.kind == TestStatus) && !request.path.empty())
                return Result<void>::Failure(MakeError(CodeQueryErrors::Invalid));
            return Result<void>::Success();
        }

        /** @brief Admits an existing owner snapshot without changing provider error precedence or falling back to disk. */
        Result<CodeQueryTextSnapshot> ExistingText(CodeQueryTextSnapshot snapshot, const CodeQueryRequest &request,
                                                   const CodeQueryContext &context, const CodeQueryLimits &limits,
                                                   const CodeQueryDetail::Scope &scope) {
            if (snapshot.projectIdentity != context.projectIdentity || snapshot.projectGeneration != context.projectGeneration ||
                snapshot.path != request.path)
                return Result<CodeQueryTextSnapshot>::Failure(MakeError(CodeQueryErrors::Stale));
            if (!snapshot.text || snapshot.revision.empty() || snapshot.revision.size() > 256 ||
                !IsValidUtf8ScalarSequence(snapshot.revision))
                return Result<CodeQueryTextSnapshot>::Failure(MakeError(CodeQueryErrors::Invalid));
            if (snapshot.text->size() > limits.maximumFileBytes)
                return Result<CodeQueryTextSnapshot>::Failure(MakeError(CodeQueryErrors::Capacity));
            if (auto valid = CodeQueryDetail::ValidateText(*snapshot.text, scope); valid.HasError())
                return Result<CodeQueryTextSnapshot>::Failure(valid.ErrorValue());
            return Result<CodeQueryTextSnapshot>::Success(std::move(snapshot));
        }

        /** @brief Captures an existing document observation or bounded disk bytes; never opens a document session. */
        Result<CodeQueryTextSnapshot> Text(const std::shared_ptr<const Platform::IProjectReadFiles> &files, const CodeQueryRequest &request,
                                           const CodeQueryContext &context, const CodeQueryProviders &providers,
                                           const CodeQueryLimits &limits, const CodeQueryDetail::Scope &scope) {
            if (providers.existingText) {
                auto existing = providers.existingText(request.path, {context.projectIdentity, context.projectGeneration,
                                                                      context.cancellation, scope.deadline, context.authorityStopped});
                if (existing.HasError())
                    return Result<CodeQueryTextSnapshot>::Failure(existing.ErrorValue());
                if (existing.Value())
                    return ExistingText(*existing.Value(), request, context, limits, scope);
            }
            if (!files)
                return Result<CodeQueryTextSnapshot>::Failure(MakeError(CodeQueryErrors::Unavailable));
            auto snapshot = files->Text(request.path,
                                        {context.projectIdentity, context.projectGeneration, context.cancellation, scope.deadline,
                                         context.authorityStopped},
                                        FileLimits(limits));
            if (snapshot.HasError())
                return snapshot;
            if (!snapshot.Value().text || snapshot.Value().text->size() > limits.maximumFileBytes)
                return Result<CodeQueryTextSnapshot>::Failure(MakeError(CodeQueryErrors::Capacity));
            if (snapshot.Value().revision.empty() || snapshot.Value().revision.size() > 256 ||
                !IsValidUtf8ScalarSequence(snapshot.Value().revision))
                return Result<CodeQueryTextSnapshot>::Failure(MakeError(CodeQueryErrors::Invalid));
            if (snapshot.Value().path != request.path || snapshot.Value().projectIdentity != context.projectIdentity ||
                snapshot.Value().projectGeneration != context.projectGeneration)
                return Result<CodeQueryTextSnapshot>::Failure(MakeError(CodeQueryErrors::Stale));
            if (auto valid = CodeQueryDetail::ValidateText(*snapshot.Value().text, scope); valid.HasError())
                return Result<CodeQueryTextSnapshot>::Failure(valid.ErrorValue());
            return snapshot;
        }

        /** @brief Resolves the exact injected query capability; absence never becomes empty success. */
        const std::function<Result<CodeQueryObservation>(const CodeQueryRequest &, const CodeQueryContext &)> &Provider(
            const CodeQueryKind kind, const CodeQueryProviders &providers) {
            using enum CodeQueryKind;
            switch (kind) {
                case Symbols:
                    return providers.symbols;
                case Diagnostics:
                    return providers.diagnostics;
                case BuildStatus:
                    return providers.buildStatus;
                default:
                    return providers.testStatus;
            }
        }

        /** @brief Captures and projects a bounded native manifest without opening document sessions. */
        Result<CodeQueryPage> FilePage(const std::shared_ptr<const Platform::IProjectReadFiles> &files, const CodeQueryRequest &request,
                                       const CodeQueryContext &context, const CodeQueryLimits &limits,
                                       const CodeQueryDetail::Scope &scope) {
            if (!files)
                return Result<CodeQueryPage>::Failure(MakeError(CodeQueryErrors::Unavailable));
            auto captured = files->Files(request.path,
                                         {context.projectIdentity, context.projectGeneration, context.cancellation, scope.deadline,
                                          context.authorityStopped},
                                         FileLimits(limits));
            if (captured.HasError())
                return Result<CodeQueryPage>::Failure(captured.ErrorValue());
            auto manifest = std::move(captured).Value();
            if (manifest.entries.size() > limits.maximumFiles)
                return Result<CodeQueryPage>::Failure(MakeError(CodeQueryErrors::Capacity));
            CodeQueryObservation observation{std::move(manifest.projectIdentity),
                                             manifest.projectGeneration,
                                             std::move(manifest.revision),
                                             {},
                                             0};
            observation.records.reserve(manifest.entries.size());
            for (auto &entry : manifest.entries) {
                if (auto stop = scope.Check(); stop.HasError())
                    return Result<CodeQueryPage>::Failure(stop.ErrorValue());
                observation.records.push_back({CodeQueryRecordKind::File, std::move(entry.path), {}, 0, 0, 0, 0});
            }
            return CodeQueryDetail::ObservationPage(request, std::move(observation), context, limits, scope);
        }

        /** @brief Projects one owned source snapshot as text or bounded literal matches. */
        Result<CodeQueryPage> SourcePage(const std::shared_ptr<const Platform::IProjectReadFiles> &files, const CodeQueryRequest &request,
                                         const CodeQueryContext &context, const CodeQueryLimits &limits,
                                         const CodeQueryDetail::Scope &scope, const CodeQueryProviders &providers) {
            auto snapshot = Text(files, request, context, providers, limits, scope);
            if (snapshot.HasError())
                return Result<CodeQueryPage>::Failure(snapshot.ErrorValue());
            if (request.kind == CodeQueryKind::Text)
                return CodeQueryDetail::TextPage(request, snapshot.Value());
            auto found = CodeQueryDetail::Search(request, snapshot.Value(), limits, scope);
            if (found.HasError())
                return Result<CodeQueryPage>::Failure(found.ErrorValue());
            return CodeQueryDetail::ObservationPage(request, std::move(found).Value(), context, limits, scope);
        }

        /** @brief Invokes only the declared read-only provider and validates its owned observation. */
        Result<CodeQueryPage> ProviderPage(const CodeQueryRequest &request, const CodeQueryContext &context, const CodeQueryLimits &limits,
                                           const CodeQueryDetail::Scope &scope, const CodeQueryProviders &providers) {
            const auto &provider = Provider(request.kind, providers);
            if (!provider)
                return Result<CodeQueryPage>::Failure(MakeError(CodeQueryErrors::Unavailable));
            auto observation = provider(request, {context.projectIdentity, context.projectGeneration, context.cancellation, scope.deadline,
                                                  context.authorityStopped});
            if (observation.HasError())
                return Result<CodeQueryPage>::Failure(observation.ErrorValue());
            return CodeQueryDetail::ObservationPage(request, std::move(observation).Value(), context, limits, scope);
        }

        /** @brief Dispatches admitted requests by domain responsibility within one query scope. */
        Result<CodeQueryPage> CapturePage(const std::shared_ptr<const Platform::IProjectReadFiles> &files, const CodeQueryRequest &request,
                                          const CodeQueryContext &context, const CodeQueryLimits &limits,
                                          const CodeQueryDetail::Scope &scope, const CodeQueryProviders &providers) {
            using enum CodeQueryKind;
            if (request.kind == Files)
                return FilePage(files, request, context, limits, scope);
            if (request.kind == Text || request.kind == Search)
                return SourcePage(files, request, context, limits, scope, providers);
            return ProviderPage(request, context, limits, scope, providers);
        }
    }  // namespace

    /** @copydoc ProjectCodeQuery::ProjectCodeQuery */
    ProjectCodeQuery::ProjectCodeQuery(std::shared_ptr<const Platform::IProjectReadFiles> files, std::string identity,
                                       const std::uint64_t generation, CodeQueryProviders providers, const CodeQueryLimits &limits)
        : files_(std::move(files)), identity_(std::move(identity)), generation_(generation), providers_(std::move(providers)),
          limits_(limits) {}

    /** @copydoc ProjectCodeQuery::Create */
    Result<std::shared_ptr<ProjectCodeQuery>> ProjectCodeQuery::Create(std::shared_ptr<const Platform::IProjectReadFiles> files,
                                                                       std::string identity, const std::uint64_t generation,
                                                                       CodeQueryProviders providers, const CodeQueryLimits &limits) {
        if (identity.empty() || identity.size() > 256 || !IsValidUtf8ScalarSequence(identity) || generation == 0 ||
            !CodeQueryDetail::ValidLimits(limits))
            return Result<std::shared_ptr<ProjectCodeQuery>>::Failure(MakeError(CodeQueryErrors::Invalid));
        try {
            return Result<std::shared_ptr<ProjectCodeQuery>>::Success(std::make_shared<ProjectCodeQuery>(
                ProjectCodeQuery{std::move(files), std::move(identity), generation, std::move(providers), limits}));
        } catch (const std::bad_alloc &) {
            return Result<std::shared_ptr<ProjectCodeQuery>>::Failure(MakeError(CodeQueryErrors::Capacity));
        }
    }

    /** @copydoc ProjectCodeQuery::Query */
    Result<CodeQueryPage> ProjectCodeQuery::Query(const CodeQueryRequest &request, const CodeQueryContext &context) const {
        if (context.projectIdentity != identity_ || context.projectGeneration != generation_)
            return Result<CodeQueryPage>::Failure(MakeError(CodeQueryErrors::Stale));
        const CodeQueryDetail::Scope scope{context, std::min(context.deadline, std::chrono::steady_clock::now() + limits_.maximumDuration)};
        if (auto stop = scope.Check(); stop.HasError())
            return Result<CodeQueryPage>::Failure(stop.ErrorValue());
        if (auto admitted = Admit(request, limits_); admitted.HasError())
            return Result<CodeQueryPage>::Failure(admitted.ErrorValue());
        try {
            auto result = CapturePage(files_, request, context, limits_, scope, providers_);
            if (auto stopped = scope.Check(); stopped.HasError())
                return Result<CodeQueryPage>::Failure(stopped.ErrorValue());
            return result;
        } catch (const std::bad_alloc &) {
            return Result<CodeQueryPage>::Failure(MakeError(CodeQueryErrors::Capacity));
        }
    }
}  // namespace Horo::Application
