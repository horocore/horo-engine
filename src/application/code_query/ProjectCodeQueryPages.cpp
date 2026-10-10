#include "Horo/Foundation/Sha256.h"
#include "Horo/Foundation/Utf8.h"
#include "ProjectCodeQueryInternal.h"

#include <algorithm>
#include <span>
#include <tuple>

namespace Horo::Application::CodeQueryDetail {
    namespace {
        /** @brief Identifies UTF-8 continuation bytes only after complete scalar validation. */
        bool Continuation(const char byte) {
            return (static_cast<unsigned char>(byte) & 0xc0U) == 0x80U;
        }

        /** @brief Fences a provider revision to this exact query, avoiding cross-query cursor reuse. */
        std::string QueryRevision(const CodeQueryRequest &request, const CodeQueryObservation &observation) {
            const std::string prefix = std::to_string(static_cast<unsigned>(request.kind)) + ":" + std::to_string(request.path.size()) +
                                       ":" + request.path + std::to_string(request.pattern.size()) + ":" + request.pattern +
                                       std::to_string(observation.projectIdentity.size()) + ":" + observation.projectIdentity + ":" +
                                       std::to_string(observation.projectGeneration) + ":" + std::to_string(observation.revision.size()) +
                                       ":" + observation.revision;
            return FormatSha256(ComputeSha256(std::as_bytes(std::span{prefix.data(), prefix.size()})));
        }

        /** @brief Closed projection vocabulary prevents a status provider from advertising query-unrelated rows. */
        CodeQueryRecordKind ExpectedKind(const CodeQueryKind kind) {
            using enum CodeQueryKind;
            switch (kind) {
                case Files:
                    return CodeQueryRecordKind::File;
                case Search:
                    return CodeQueryRecordKind::Match;
                case Symbols:
                    return CodeQueryRecordKind::Symbol;
                case Diagnostics:
                    return CodeQueryRecordKind::Diagnostic;
                case BuildStatus:
                    return CodeQueryRecordKind::Build;
                case TestStatus:
                    return CodeQueryRecordKind::Test;
                default:
                    return CodeQueryRecordKind::File;
            }
        }
    }  // namespace

    /** @copydoc TextPage */
    Result<CodeQueryPage> TextPage(const CodeQueryRequest &request, const CodeQueryTextSnapshot &snapshot) {
        const auto &text = *snapshot.text;
        const auto revision = QueryRevision(request, {snapshot.projectIdentity, snapshot.projectGeneration, snapshot.revision, {}, 0});
        if (auto fence = Fence(request, revision, text.size()); fence.HasError())
            return Result<CodeQueryPage>::Failure(fence.ErrorValue());
        if (request.offset < text.size() && Continuation(text[request.offset]))
            return Result<CodeQueryPage>::Failure(MakeError(CodeQueryErrors::Invalid));
        std::size_t end = request.offset + std::min(request.limit, text.size() - request.offset);
        while (end < text.size() && end > request.offset && Continuation(text[end]))
            --end;
        if (end == request.offset && request.offset < text.size())
            return Result<CodeQueryPage>::Failure(MakeError(CodeQueryErrors::Capacity));
        CodeQueryPage page{.revision = revision,
                           .projectGeneration = snapshot.projectGeneration,
                           .text = text.substr(request.offset, end - request.offset)};
        if (end < text.size())
            page.nextOffset = end;
        return Result<CodeQueryPage>::Success(std::move(page));
    }

    /** @copydoc Search */
    Result<CodeQueryObservation> Search(const CodeQueryRequest &request, const CodeQueryTextSnapshot &snapshot,
                                        const CodeQueryLimits &limits, const Scope &scope) {
        CodeQueryObservation result{snapshot.projectIdentity, snapshot.projectGeneration, snapshot.revision};
        const auto &text = *snapshot.text;
        std::size_t work{};
        std::uint32_t line{1};
        std::uint32_t column{1};
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (auto stop = scope.Check(); stop.HasError())
                return Result<CodeQueryObservation>::Failure(stop.ErrorValue());
            std::size_t matched{};
            while (matched < request.pattern.size() && matched < text.size() - i) {
                if (++work > limits.maximumSearchSteps)
                    return Result<CodeQueryObservation>::Failure(MakeError(CodeQueryErrors::Capacity));
                if (text[i + matched] != request.pattern[matched])
                    break;
                ++matched;
            }
            if (matched == request.pattern.size()) {
                if (result.records.size() == limits.maximumFiles)
                    return Result<CodeQueryObservation>::Failure(MakeError(CodeQueryErrors::Capacity));
                result.records.emplace_back(CodeQueryRecordKind::Match, snapshot.path, request.pattern, line, column, i, matched);
            }
            if (text[i] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        return Result<CodeQueryObservation>::Success(std::move(result));
    }

    /** @copydoc ObservationPage */
    Result<CodeQueryPage> ObservationPage(const CodeQueryRequest &request, CodeQueryObservation observation,
                                          const CodeQueryContext &context, const CodeQueryLimits &limits, const Scope &scope) {
        if (observation.projectIdentity != context.projectIdentity || observation.projectGeneration != context.projectGeneration)
            return Result<CodeQueryPage>::Failure(MakeError(CodeQueryErrors::Stale));
        if (observation.revision.empty() || observation.revision.size() > 256 || !IsValidUtf8ScalarSequence(observation.revision))
            return Result<CodeQueryPage>::Failure(MakeError(CodeQueryErrors::Invalid));
        if (observation.records.size() > limits.maximumFiles)
            return Result<CodeQueryPage>::Failure(MakeError(CodeQueryErrors::Capacity));
        std::size_t bytes{};
        for (const auto &row : observation.records) {
            if (auto stop = scope.Check(); stop.HasError())
                return Result<CodeQueryPage>::Failure(stop.ErrorValue());
            if (row.kind != ExpectedKind(request.kind) || (!row.path.empty() && !ValidPath(row.path)) || row.label.size() > 4096 ||
                !IsValidUtf8ScalarSequence(row.label))
                return Result<CodeQueryPage>::Failure(MakeError(CodeQueryErrors::Invalid));
            if ((request.kind == CodeQueryKind::Files || request.kind == CodeQueryKind::Symbols || request.kind == CodeQueryKind::Search) &&
                row.path.empty())
                return Result<CodeQueryPage>::Failure(MakeError(CodeQueryErrors::Invalid));
            if (!request.path.empty() && (request.kind == CodeQueryKind::Symbols || request.kind == CodeQueryKind::Diagnostics) &&
                row.path != request.path)
                return Result<CodeQueryPage>::Failure(MakeError(CodeQueryErrors::Invalid));
            if (request.kind == CodeQueryKind::Files && !request.path.empty() && !row.path.starts_with(request.path + '/'))
                return Result<CodeQueryPage>::Failure(MakeError(CodeQueryErrors::Invalid));
            if (row.path.size() + row.label.size() > limits.maximumObservationBytes - bytes)
                return Result<CodeQueryPage>::Failure(MakeError(CodeQueryErrors::Capacity));
            bytes += row.path.size() + row.label.size();
        }
        std::ranges::sort(observation.records, [](const auto &a, const auto &b) {
            return std::tie(a.path, a.line, a.column, a.byteOffset, a.label) < std::tie(b.path, b.line, b.column, b.byteOffset, b.label);
        });
        const auto revision = QueryRevision(request, observation);
        if (auto fence = Fence(request, revision, observation.records.size()); fence.HasError())
            return Result<CodeQueryPage>::Failure(fence.ErrorValue());
        CodeQueryPage page{.revision = revision,
                           .projectGeneration = observation.projectGeneration,
                           .droppedRecords = observation.droppedRecords};
        const auto end = request.offset + std::min(request.limit, observation.records.size() - request.offset);
        page.records.reserve(end - request.offset);
        for (auto i = request.offset; i < end; ++i)
            page.records.push_back(std::move(observation.records[i]));
        if (end < observation.records.size())
            page.nextOffset = end;
        return Result<CodeQueryPage>::Success(std::move(page));
    }
}  // namespace Horo::Application::CodeQueryDetail
