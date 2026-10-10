#include "Horo/Application/CodeQueryStoreProviders.h"

#include "Horo/Foundation/Utf8.h"
#include "ProjectCodeQueryInternal.h"

#include <limits>

namespace Horo::Application {
    namespace {
        /** @brief Never discloses native absolute paths or source locations outside the authorized project. */
        std::string RelativeSource(const std::filesystem::path &root, const std::string &absolute) {
            if (absolute.size() > 4096 || !IsValidUtf8ScalarSequence(absolute))
                return {};
            try {
                const std::filesystem::path path{std::u8string{absolute.begin(), absolute.end()}};
                if (!path.is_absolute())
                    return {};
                const auto spelling = path.lexically_normal().lexically_relative(root).generic_u8string();
                std::string relative{spelling.begin(), spelling.end()};
                return CodeQueryDetail::ValidPath(relative) ? relative : std::string{};
            } catch (const std::filesystem::filesystem_error &) {
                return {};
            }
        }

        /** @brief Reserves bounded projected strings before constructing an owned result row. */
        bool ReserveRow(const std::size_t pathBytes, const std::size_t labelBytes, std::size_t &bytes) {
            constexpr std::size_t maximumBytes = 1U << 20U;
            if (labelBytes > 4096 || pathBytes > maximumBytes - bytes || labelBytes > maximumBytes - bytes - pathBytes)
                return false;
            bytes += pathBytes + labelBytes;
            return true;
        }

        /** @brief Typed operation state projection; no title/string heuristic identifies the responsibility. */
        std::string State(const OperationState state) {
            using enum OperationState;
            switch (state) {
                case Queued:
                    return "queued";
                case Running:
                    return "running";
                case Waiting:
                    return "waiting";
                case Cancelling:
                    return "cancelling";
                case Succeeded:
                    return "succeeded";
                case Failed:
                    return "failed";
                case Cancelled:
                    return "cancelled";
            }
            return {};
        }

        /** @brief Captures one current diagnostic store snapshot and projects contained source locations. */
        Result<CodeQueryObservation> CaptureDiagnostics(const IBuildOutputQuery &query, const std::filesystem::path &root,
                                                        const std::string &identity, const std::uint64_t generation,
                                                        const CodeQueryRequest &request, const CodeQueryContext &context) {
            if (auto stop = Platform::CheckProjectReadContext(context); stop.HasError())
                return Result<CodeQueryObservation>::Failure(stop.ErrorValue());
            // No query cache: the sentinel requests one owned current snapshot, including an empty revision-zero store.
            auto snapshot = query.SnapshotIfChanged(std::numeric_limits<std::uint64_t>::max());
            if (!snapshot)
                return Result<CodeQueryObservation>::Failure(MakeError(CodeQueryErrors::Unavailable));
            if (snapshot->records.size() > 4096)
                return Result<CodeQueryObservation>::Failure(MakeError(CodeQueryErrors::Capacity));
            CodeQueryObservation observation{identity,
                                             generation,
                                             "build-output:" + std::to_string(snapshot->revision),
                                             {},
                                             snapshot->droppedRecordCount};
            std::size_t bytes{};
            for (const auto &record : snapshot->records) {
                if (auto stop = Platform::CheckProjectReadContext(context); stop.HasError())
                    return Result<CodeQueryObservation>::Failure(stop.ErrorValue());
                const auto path = record.source ? RelativeSource(root, record.source->absolutePath) : std::string{};
                if (!request.path.empty() && request.path != path)
                    continue;
                if (record.code.Value().size() > 4094 || record.message.size() > 4094 - record.code.Value().size() ||
                    !ReserveRow(path.size(), record.code.Value().size() + 2 + record.message.size(), bytes))
                    return Result<CodeQueryObservation>::Failure(MakeError(CodeQueryErrors::Capacity));
                observation.records.emplace_back(CodeQueryRecordKind::Diagnostic, path, record.code.Value() + ": " + record.message,
                                                 path.empty() ? 0 : record.source->line, path.empty() ? 0 : record.source->column,
                                                 record.sequence, 0);
            }
            return Result<CodeQueryObservation>::Success(std::move(observation));
        }

        /** @brief Captures typed build operations without inferring responsibility from display titles. */
        Result<CodeQueryObservation> CaptureBuildStatus(const IOperationQuery &query, const std::string &identity,
                                                        const std::uint64_t generation, const CodeQueryContext &context) {
            if (auto stop = Platform::CheckProjectReadContext(context); stop.HasError())
                return Result<CodeQueryObservation>::Failure(stop.ErrorValue());
            auto snapshot = query.SnapshotIfChanged(std::numeric_limits<std::uint64_t>::max());
            if (!snapshot)
                return Result<CodeQueryObservation>::Failure(MakeError(CodeQueryErrors::Unavailable));
            if (snapshot->operations.size() > 4096)
                return Result<CodeQueryObservation>::Failure(MakeError(CodeQueryErrors::Capacity));
            CodeQueryObservation observation{identity,
                                             generation,
                                             "operations:" + std::to_string(snapshot->revision),
                                             {},
                                             snapshot->droppedTerminalCount};
            for (const auto &record : snapshot->operations) {
                if (auto stop = Platform::CheckProjectReadContext(context); stop.HasError())
                    return Result<CodeQueryObservation>::Failure(stop.ErrorValue());
                if (record.kind != OperationKind::Build)
                    continue;
                const auto state = State(record.state);
                if (state.empty())
                    return Result<CodeQueryObservation>::Failure(MakeError(CodeQueryErrors::Invalid));
                observation.records.emplace_back(CodeQueryRecordKind::Build, std::string{}, state, 0, 0, record.id, 0);
            }
            return Result<CodeQueryObservation>::Success(std::move(observation));
        }
    }  // namespace

    /** @copydoc MakeCodeQueryStoreProviders */
    CodeQueryProviders MakeCodeQueryStoreProviders(std::filesystem::path root, std::string identity, const std::uint64_t generation,
                                                   std::shared_ptr<const IBuildOutputQuery> diagnostics,
                                                   std::shared_ptr<const IOperationQuery> operations) {
        CodeQueryProviders providers;
        if (diagnostics) {
            providers.diagnostics = [root = std::move(root), identity, generation,
                                     query = std::move(diagnostics)](const CodeQueryRequest &request,
                                                                     const CodeQueryContext &context) -> Result<CodeQueryObservation> {
                return CaptureDiagnostics(*query, root, identity, generation, request, context);
            };
        }
        if (operations) {
            providers.buildStatus = [identity = std::move(identity), generation,
                                     query = std::move(operations)](const CodeQueryRequest &,
                                                                    const CodeQueryContext &context) -> Result<CodeQueryObservation> {
                return CaptureBuildStatus(*query, identity, generation, context);
            };
        }
        return providers;
    }
}  // namespace Horo::Application
