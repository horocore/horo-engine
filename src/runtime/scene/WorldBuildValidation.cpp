#include "Horo/Runtime/Scene/WorldBuildValidation.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <tuple>

namespace Horo::Runtime {
    namespace {
        struct Validation final {
            const WorldStreaming::WorldPartitionDescriptor &partition;
            std::span<const WorldBuildCell> cells;
            std::span<const std::shared_ptr<const RuntimeSceneCellPayload>> payloads;
            const WorldBuildValidationLimits &limits;
            const CancellationToken &cancellation;
            WorldBuildValidationReport report;
            std::size_t copiedSourceBytes{};

            /** @brief Adds one owned finding or rejects diagnostic truncation. */
            Result<void> Add(std::string code, std::string message, const std::optional<DiagnosticSourceLocation> &source) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Cancelled));
                if (report.diagnostics.size() == limits.maximumDiagnostics ||
                    (source && source->absolutePath.size() > limits.maximumSourceBytes - copiedSourceBytes))
                    return Result<void>::Failure(MakeError(SceneCellPayloadErrors::CapacityExceeded));
                if (source)
                    copiedSourceBytes += source->absolutePath.size();
                report.diagnostics.push_back({.severity = DiagnosticSeverity::Error,
                                              .result = BuildOutputResult::Failed,
                                              .stage = "world.validation",
                                              .code = DiagnosticCode{std::move(code)},
                                              .message = std::move(message),
                                              .source = source});
                return Result<void>::Success();
            }

            /** @brief Finds an exact publication and object in admitted immutable output. */
            bool Contains(const WorldBuildEndpoint &endpoint) const {
                if (!endpoint.object.IsValid())
                    return false;
                if (const auto authored = std::ranges::find(cells, endpoint.cell, &WorldBuildCell::expected); authored == cells.end())
                    return false;
                return std::ranges::any_of(payloads, [&](const auto &payload) {
                    return payload && payload->Identity() == endpoint.cell &&
                           std::ranges::any_of(payload->Definition().Entities(), [&](const auto &entity) {
                        return entity.object == endpoint.object;
                    });
                });
            }

            /** @brief Accounts for one unique payload even when its publication is stale. */
            Result<void> CheckPayload(const RuntimeSceneCellPayload &payload, const WorldBuildCell &authored) {
                if (payload.Identity() != authored.expected) {
                    if (auto result = Add("world.build.stale_payload", "The cooked payload does not match the captured cell publication.",
                                          authored.source);
                        result.HasError())
                        return result;
                }
                const auto bytes = payload.RetainedBytes();
                if (bytes > std::numeric_limits<std::size_t>::max() - report.estimatedBytes)
                    return Result<void>::Failure(MakeError(SceneCellPayloadErrors::CapacityExceeded));
                report.estimatedBytes += bytes;
                if (bytes > limits.maximumCellBytes)
                    return Add("world.build.cell_budget", "The Scene payload logical byte estimate exceeds the cell budget.",
                               authored.source);
                return Result<void>::Success();
            }

            /** @brief Validates one topology cell without counting absent or ambiguous cooked output. */
            Result<void> CheckCell(const WorldStreaming::StreamingCellId &id) {
                const auto authored = std::ranges::find(cells, id, [](const auto &entry) {
                    return entry.expected.cell;
                });
                if (authored == cells.end())
                    return Add("world.build.missing_source", "A partition cell has no captured authored publication.", {});
                if (const auto count = std::ranges::count_if(payloads,
                                                             [&](const auto &payload) {
                    return payload && payload->Identity().cell == id;
                });
                    count != 1)
                    return Add(count == 0 ? "world.build.missing_payload" : "world.build.overlap",
                               count == 0 ? "The cell has no cooked Scene payload." : "Multiple Scene payloads claim the same cell.",
                               authored->source);
                const auto payload = std::ranges::find_if(payloads, [&](const auto &value) {
                    return value && value->Identity().cell == id;
                });
                return CheckPayload(**payload, *authored);
            }

            /** @brief Validates complete coverage in canonical topology order, then the aggregate budget. */
            Result<void> CheckCells() {
                for (const auto &cell : partition.Cells()) {
                    if (cancellation.IsCancellationRequested())
                        return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Cancelled));
                    if (auto result = CheckCell(cell.id); result.HasError())
                        return result;
                }
                if (report.estimatedBytes > limits.maximumWorldBytes)
                    return Add("world.build.world_budget", "The aggregate Scene payload logical byte estimate exceeds the world budget.",
                               {});
                return Result<void>::Success();
            }
        };

        /** @brief Validates navigable sources and aggregate copied path bytes before report allocation. */
        Result<void> AdmitLocation(const std::optional<DiagnosticSourceLocation> &source, std::size_t &bytes, std::size_t maximum) {
            if (!source)
                return Result<void>::Success();
            if (source->absolutePath.empty() || source->absolutePath.find('\0') != std::string::npos ||
                !std::filesystem::path(source->absolutePath).is_absolute())
                return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Invalid));
            if (source->absolutePath.size() > maximum - bytes)
                return Result<void>::Failure(MakeError(SceneCellPayloadErrors::CapacityExceeded));
            bytes += source->absolutePath.size();
            return Result<void>::Success();
        }

        /** @brief Rejects malformed or duplicate authored identities before location admission. */
        Result<void> AdmitCells(const Validation &validation, std::size_t &bytes) {
            for (const auto &cell : validation.cells) {
                if (validation.cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Cancelled));
                if (const auto &id = cell.expected; id.partition != validation.partition.Partition() || !id.scene.IsValid() ||
                                                    !id.revision.value ||
                                                    std::ranges::none_of(validation.partition.Cells(), [&](const auto &item) {
                    return item.id == id.cell;
                }) || std::ranges::count(validation.cells, id.cell, [](const auto &item) {
                    return item.expected.cell;
                }) != 1)
                    return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Invalid));
                if (const auto location = AdmitLocation(cell.source, bytes, validation.limits.maximumSourceBytes); location.HasError())
                    return location;
            }
            return Result<void>::Success();
        }

        /** @brief Admits reference navigation storage after authored source storage. */
        Result<void> AdmitReferences(const Validation &validation, std::span<const WorldBuildReference> references, std::size_t &bytes) {
            for (const auto &reference : references) {
                if (validation.cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Cancelled));
                if (const auto location = AdmitLocation(reference.location, bytes, validation.limits.maximumSourceBytes);
                    location.HasError())
                    return location;
            }
            return Result<void>::Success();
        }

        /** @brief Bounds object traversal and checks cooked topology membership, retaining null missing leases. */
        Result<void> AdmitPayloads(const Validation &validation) {
            std::size_t objects{};
            for (const auto &payload : validation.payloads) {
                if (validation.cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Cancelled));
                if (!payload)
                    continue;
                if (payload->Definition().Entities().size() > validation.limits.maximumObjects - objects)
                    return Result<void>::Failure(MakeError(SceneCellPayloadErrors::CapacityExceeded));
                objects += payload->Definition().Entities().size();
                if (payload->Identity().partition != validation.partition.Partition() ||
                    std::ranges::none_of(validation.partition.Cells(), [&](const auto &cell) {
                    return cell.id == payload->Identity().cell;
                }))
                    return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Invalid));
            }
            return Result<void>::Success();
        }

        /** @brief Checks structural capture validity before any owned diagnostics are allocated. */
        Result<void> Admit(const Validation &validation, std::span<const WorldBuildReference> references) {
            const auto &limits = validation.limits;
            if (!validation.report.revision || !limits.maximumCells || !limits.maximumDiagnostics || !limits.maximumSourceBytes ||
                !limits.maximumObjects || !limits.maximumCellBytes || !limits.maximumWorldBytes)
                return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Invalid));
            if (validation.partition.Cells().size() > limits.maximumCells || validation.cells.size() > limits.maximumCells ||
                validation.payloads.size() > limits.maximumCells || references.size() > limits.maximumReferences)
                return Result<void>::Failure(MakeError(SceneCellPayloadErrors::CapacityExceeded));
            std::size_t bytes{};
            if (auto result = AdmitCells(validation, bytes); result.HasError())
                return result;
            if (auto result = AdmitReferences(validation, references, bytes); result.HasError())
                return result;
            return AdmitPayloads(validation);
        }
    }  // namespace

    /** @copydoc ValidateWorldBuild */
    Result<WorldBuildValidationReport> ValidateWorldBuild(const WorldStreaming::WorldPartitionDescriptor &partition, std::uint64_t revision,
                                                          std::span<const WorldBuildCell> cells,
                                                          std::span<const std::shared_ptr<const RuntimeSceneCellPayload>> payloads,
                                                          std::span<const WorldBuildReference> references,
                                                          const WorldBuildValidationLimits &limits, const CancellationToken &cancellation) {
        Validation validation{partition, cells, payloads, limits, cancellation, {.revision = revision}};
        if (const auto admitted = Admit(validation, references); admitted.HasError())
            return Result<WorldBuildValidationReport>::Failure(admitted.ErrorValue());
        if (const auto checked = validation.CheckCells(); checked.HasError())
            return Result<WorldBuildValidationReport>::Failure(checked.ErrorValue());
        std::vector<const WorldBuildReference *> ordered;
        for (const auto &reference : references)
            ordered.push_back(&reference);
        std::ranges::sort(ordered, [](const auto *left, const auto *right) {
            return std::tie(left->source.cell, left->source.object, left->target.cell, left->target.object) <
                   std::tie(right->source.cell, right->source.object, right->target.cell, right->target.object);
        });
        for (const auto *captured : ordered) {
            const auto &reference = *captured;
            if (cancellation.IsCancellationRequested())
                return Result<WorldBuildValidationReport>::Failure(MakeError(SceneCellPayloadErrors::Cancelled));
            if (!validation.Contains(reference.source) || !validation.Contains(reference.target))
                if (const auto added =
                        validation.Add("world.build.invalid_reference", "A required reference has no exact cooked source or target object.",
                                       reference.location);
                    added.HasError())
                    return Result<WorldBuildValidationReport>::Failure(added.ErrorValue());
        }
        if (cancellation.IsCancellationRequested())
            return Result<WorldBuildValidationReport>::Failure(MakeError(SceneCellPayloadErrors::Cancelled));
        return Result<WorldBuildValidationReport>::Success(std::move(validation.report));
    }
}  // namespace Horo::Runtime
