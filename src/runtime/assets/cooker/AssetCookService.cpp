/**
 * @copydoc AssetCookService.h
 */

#include "Horo/Assets/AssetCookService.h"

#include "../AssetErrors.h"
#include "AssetCookOperationScope.h"
#include "Horo/Assets/AssetCook.h"
#include "Horo/Assets/AssetCookCache.h"
#include "Horo/Assets/AssetCookOutput.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Sha256.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Assets {
    namespace {
        using Detail::CookOperationScope;

        /**
         * @brief Reads source bytes from a project-relative path under sourceRoot.
         */
        Result<std::vector<std::uint8_t>> ReadSourceBytes(const std::filesystem::path &sourceRoot, std::string_view relativePath,
                                                          std::size_t maxBytes) {
            auto fullPath = sourceRoot / relativePath;

            // Reject symlinks
            if (std::filesystem::is_symlink(fullPath))
                return Result<std::vector<std::uint8_t>>::Failure(Error{CookErrors::MalformedArtifact.code});

            std::error_code ec;
            if (!std::filesystem::exists(fullPath, ec) || ec)
                return Result<std::vector<std::uint8_t>>::Failure(Error{CookErrors::MalformedArtifact.code});

            const auto fileSize = std::filesystem::file_size(fullPath, ec);
            if (ec || fileSize > maxBytes)
                return Result<std::vector<std::uint8_t>>::Failure(Error{CookErrors::TooLarge.code});

            std::ifstream file(fullPath, std::ios::binary);
            if (!file)
                return Result<std::vector<std::uint8_t>>::Failure(Error{CookErrors::MalformedArtifact.code});

            std::vector<std::uint8_t> bytes(fileSize);
            file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(fileSize));
            if (!file || file.gcount() != static_cast<std::streamsize>(fileSize))
                return Result<std::vector<std::uint8_t>>::Failure(Error{CookErrors::MalformedArtifact.code});

            return Result<std::vector<std::uint8_t>>::Success(std::move(bytes));
        }

        struct CookSlot {
            AssetRecord record;
            std::vector<std::uint8_t> sourceBytes;
            Sha256Digest sourceDigest;
            AssetCookCacheKey cacheKey;
            bool cacheHit{false};
            std::vector<std::uint8_t> cacheArtifact;
            std::vector<std::uint8_t> cookedArtifact;
            std::optional<Error> cookError;
        };

        /** @brief Adds authoritative source navigation to a typed asset-scoped result. */
        void PublishAssetResult(const AssetCookRequest &request, const AssetRecord &record, const CookOperationScope &operation,
                                BuildOutputRecord result) {
            result.timestampUtc = std::chrono::system_clock::now();
            if (result.message.empty())
                result.message = record.sourcePath.String();
            result.source =
                DiagnosticSourceLocation{.absolutePath = (request.sourceRoot / record.sourcePath.String()).lexically_normal().string()};
            operation.Publish(std::move(result));
        }

        /** @brief Holds the common native writer through recovery, complete inventory replacement and success adoption. */
        Result<AssetCookGeneration> PublishOwnedGeneration(const AssetCookRequest &request,
                                                           const std::span<const AssetCookManifestEntry> entries,
                                                           const std::span<const std::vector<std::uint8_t>> payloads,
                                                           const CancellationToken &cancellation, CookOperationScope &operation,
                                                           std::string successMessage) {
            if (cancellation.IsCancellationRequested())
                return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::Cancelled));
            auto lock = request.publicationFiles->TryAcquireExclusive(request.cookedRoot / ".cook-writer.lock", "asset cook publication");
            if (lock.HasError())
                return Result<AssetCookGeneration>::Failure(lock.ErrorValue());
            const AssetCookPublicationPolicy policy{.files = request.publicationFiles.get(), .beforeCommit = [&cancellation] {
                return cancellation.IsCancellationRequested() ? Result<void>::Failure(MakeError(CookErrors::Cancelled))
                                                              : Result<void>::Success();
            }, .afterCommit = [&operation](const AssetCookGeneration &) noexcept {
                operation.RecordCommitted();
            }, .newOperationId = request.newPublicationOperationId, .writerLease = &lock.Value()};
            constexpr std::size_t maximumGenerationBytes = 1024U * 1024U * 1024U;
            const std::size_t maximumRecoveryBytes =
                request.limits.maximumArtifactBytes > maximumGenerationBytes / request.limits.maximumAssets
                    ? maximumGenerationBytes
                    : request.limits.maximumArtifactBytes * request.limits.maximumAssets;
            if (auto recovered = RecoverCookPublication(request.cookedRoot, request.target, maximumRecoveryBytes, request.limits, policy);
                recovered.HasError())
                return Result<AssetCookGeneration>::Failure(recovered.ErrorValue());
            if (cancellation.IsCancellationRequested())
                return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::Cancelled));
            auto published = PublishCookGeneration(request.cookedRoot, request.target, entries, payloads, request.limits, policy);
            if (published.HasError())
                return published;
            operation.Succeed(std::move(successMessage));
            return published;
        }

        /** @brief Publishes the canonical empty manifest through the same transaction as a nonempty full cook. */
        Result<AssetCookReport> HandleEmptyCookSnapshot(const AssetCookRequest &request, const CancellationToken &cancellation,
                                                        CookOperationScope &operation) {
            auto generation = PublishOwnedGeneration(request, {}, {}, cancellation, operation, "Cooked 0 assets");
            if (generation.HasError()) {
                operation.RecordError(generation.ErrorValue());
                return Result<AssetCookReport>::Failure(generation.ErrorValue());
            }
            return Result<AssetCookReport>::Success(AssetCookReport{.generation = std::move(generation).Value()});
        }

        Result<void> CookAndEncodeSlot(const CookerCatalogSnapshot &catalog, CookSlot &slot, const AssetCookTargetId &target,
                                       const CancellationToken &cancellation) {
            const auto *strategy = catalog.Find(slot.record.type, target);

            if (!strategy)
                return Result<void>::Failure(Error{CookErrors::CookerMissing.code});

            const CookSourceView sourceView{
                .id = slot.record.id,
                .type = slot.record.type,
                .target = target,
                .sourceDigest = slot.sourceDigest,
                .bytes = slot.sourceBytes,
                .sourceContext = slot.record.sourcePath.String(),
            };

            auto cookResult = strategy->Cook(sourceView, cancellation);
            if (cookResult.HasError())
                return Result<void>::Failure(cookResult.ErrorValue());

            auto sink = std::move(cookResult).Value();
            if (auto validated = strategy->ValidateCookedPayload(sourceView, sink.payload); validated.HasError())
                return Result<void>::Failure(validated.ErrorValue());
            const AssetCookArtifact artifact{
                .id = sourceView.id,
                .type = sourceView.type,
                .target = sourceView.target,
                .cacheKeyDigest = slot.cacheKey.digest,
                .sourceDigest = sourceView.sourceDigest,
                .payloadDigest = ComputeSha256(std::as_bytes(std::span{sink.payload})),
                .payload = std::move(sink.payload),
            };

            auto encodeResult = EncodeCookedArtifact(artifact);
            if (encodeResult.HasError())
                return Result<void>::Failure(encodeResult.ErrorValue());

            slot.cookedArtifact = std::move(encodeResult).Value();
            return Result<void>::Success();
        }

        /** @brief Retains source-scoped failures before the joined job boundary handles an unexpected exception. */
        Result<void> ExecuteCookWork(CookSlot &slot, const CookerCatalogSnapshot &catalog, const AssetCookTargetId &target,
                                     const CancellationToken &jobCancellation) {
            if (jobCancellation.IsCancellationRequested()) {
                slot.cookError = MakeError(CookErrors::Cancelled);
                return JobCancelled(*slot.cookError);
            }
            try {
                Result<void> cooked = CookAndEncodeSlot(catalog, slot, target, jobCancellation);
                if (cooked.HasError())
                    slot.cookError = cooked.ErrorValue();
                if (cooked.HasError() &&
                    (IsJobCancelled(cooked.ErrorValue()) ||
                     (cooked.ErrorValue().code.Value() == CookErrors::Cancelled.code.Value() && jobCancellation.IsCancellationRequested())))
                    return JobCancelled(cooked.ErrorValue());
                return cooked;
            } catch (const std::exception &exception) {
                slot.cookError = MakeError(CookErrors::CookerFailed, exception.what());
                throw;
            } catch (...) {
                slot.cookError = MakeError(CookErrors::CookerFailed);
                throw;
            }
        }

        /** @brief Borrows one slot/catalog until the caller joins all submitted work. */
        JobFunction MakeCookWork(CookSlot &slot, const CookerCatalogSnapshot &catalog, const AssetCookTargetId &target) {
            return [&slot, &catalog, &target](const CancellationToken &jobCancellation) {
                return ExecuteCookWork(slot, catalog, target, jobCancellation);
            };
        }

        /** @brief Projects owned domain findings and one asset failure without parsing human text. */
        void PublishAssetFailure(const AssetCookRequest &request, const AssetRecord &record, const CookOperationScope &operation,
                                 const std::string_view stage, const Error &error) {
            const bool cancelled = IsJobCancelled(error) || error.code.Value() == CookErrors::Cancelled.code.Value();
            for (const Diagnostic &finding : error.diagnostics) {
                operation.Publish(BuildOutputRecord{
                    .timestampUtc = std::chrono::system_clock::now(),
                    .severity = finding.severity,
                    .stage = std::string{stage},
                    .code = finding.code,
                    .message = finding.message,
                    .source = DiagnosticSourceLocation{.absolutePath =
                                                           (request.sourceRoot / record.sourcePath.String()).lexically_normal().string(),
                                                       .line = finding.location.line,
                                                       .column = finding.location.column},
                });
            }
            PublishAssetResult(request, record, operation,
                               {
                                   .severity = cancelled ? DiagnosticSeverity::Warning
                                                         : DiagnosticSeverityForError(error.severity).value_or(DiagnosticSeverity::Error),
                                   .result = cancelled ? BuildOutputResult::Cancelled : BuildOutputResult::Failed,
                                   .stage = std::string(stage),
                                   .code = DiagnosticCode{error.code.Value()},
                                   .message = error.message,
                               });
        }

        /** @brief Publishes failures for callbacks that ran and siblings cancelled before execution. */
        void PublishCookFailures(const AssetCookRequest &request, std::span<CookSlot> slots, const CookOperationScope &operation,
                                 const bool groupFailed) {
            for (CookSlot &slot : slots) {
                if (!slot.cacheHit && groupFailed && !slot.cookError.has_value() && slot.cookedArtifact.empty())
                    slot.cookError = MakeError(CookErrors::Cancelled);
                if (slot.cookError.has_value())
                    PublishAssetFailure(request, slot.record, operation, "cook", *slot.cookError);
            }
        }

        /** @brief Runs uncached cook slots as a fail-fast group while preserving cancellation causes. */
        Result<void> CookUncachedSlots(JobSystem &jobs, const CookerCatalogSnapshot &catalog, const AssetCookRequest &request,
                                       std::span<CookSlot> slots, const CancellationToken &cancellation,
                                       const CookOperationScope &operation) {
            TaskGroup group(jobs, TaskGroupFailurePolicy::FailFast, cancellation);
            std::optional<Error> admissionError;
            for (CookSlot &slot : slots) {
                if (slot.cacheHit)
                    continue;
                if (auto spawned = group.Spawn({}, MakeCookWork(slot, catalog, request.target)); spawned.HasError()) {
                    slot.cookError = IsJobCancelled(spawned.ErrorValue()) ? MakeError(CookErrors::Cancelled) : spawned.ErrorValue();
                    admissionError = spawned.ErrorValue();
                    group.RequestCancel();
                    break;
                }
            }
            Result<void> joined = group.Join();
            if (admissionError.has_value() && (!joined.HasError() || IsJobCancelled(joined.ErrorValue())))
                joined = Result<void>::Failure(*admissionError);
            PublishCookFailures(request, slots, operation, joined.HasError());
            return joined;
        }

        Result<AssetCookReport> PublishCookedSlots(const AssetCookRequest &request, const AssetCookCache &cache,

                                                   std::vector<CookSlot> &slots, const std::size_t cacheHits,
                                                   const CancellationToken &cancellation, CookOperationScope &operation) {
            if (request.buildOutputStore != nullptr) {
                for (const auto &slot : slots) {
                    if (!slot.cacheHit) {
                        PublishAssetResult(request, slot.record, operation,
                                           {.severity = DiagnosticSeverity::Note,
                                            .result = BuildOutputResult::Succeeded,
                                            .stage = "cook",
                                            .code = DiagnosticCode{"asset.cook.asset_cooked"}});
                    }
                }
            }

            operation.Update("publish", "Publishing cooked generation", 0.8F);
            std::vector<AssetCookManifestEntry> manifestEntries;
            std::vector<std::vector<std::uint8_t>> manifestPayloads;

            for (auto &slot : slots) {
                if (auto storeResult = cache.Store(slot.cacheKey, slot.cookedArtifact, cancellation); storeResult.HasError())
                    return Result<AssetCookReport>::Failure(storeResult.ErrorValue());

                auto artifactHash = ComputeSha256(std::as_bytes(std::span{slot.cookedArtifact}));
                manifestEntries.push_back(AssetCookManifestEntry{
                    .assetId = slot.record.id,
                    .assetType = slot.record.type,
                    .artifactFile = slot.record.id.ToString() + ".cooked",
                    .artifactHash = artifactHash,
                });
                manifestPayloads.push_back(std::move(slot.cookedArtifact));
            }

            if (cancellation.IsCancellationRequested())
                return Result<AssetCookReport>::Failure(Error{CookErrors::Cancelled.code});

            const std::size_t cookedCount = slots.size() - cacheHits;
            auto pubResult = PublishOwnedGeneration(request, manifestEntries, manifestPayloads, cancellation, operation,
                                                    std::format("{} cooked, {} cached", cookedCount, cacheHits));
            if (pubResult.HasError())
                return Result<AssetCookReport>::Failure(pubResult.ErrorValue());

            return Result<AssetCookReport>::Success(AssetCookReport{
                .generation = std::move(pubResult).Value(),
                .totalAssets = slots.size(),
                .cookedAssets = cookedCount,
                .cacheHits = cacheHits,
            });
        }

        Result<std::vector<CookSlot>> PrepareCookSlots(const AssetCookRequest &request, const CookerCatalogSnapshot &catalog,
                                                       std::span<const AssetRecord> records, CookOperationScope &operation) {
            std::vector<CookSlot> slots;
            slots.reserve(records.size());
            operation.Update("prepare", "Reading asset sources", 0.1F);

            for (const auto &record : records) {
                const auto *contribution = catalog.FindContribution(record.type, request.target);
                if (!contribution) {
                    PublishAssetResult(request, record, operation,
                                       {.severity = DiagnosticSeverity::Error,
                                        .result = BuildOutputResult::Failed,
                                        .stage = "prepare",
                                        .code = DiagnosticCode{CookErrors::CookerMissing.code.Value()}});
                    return Result<std::vector<CookSlot>>::Failure(Error{CookErrors::CookerMissing.code});
                }

                auto readResult = ReadSourceBytes(request.sourceRoot, record.sourcePath.String(), request.limits.maximumSourceBytes);
                if (readResult.HasError()) {
                    PublishAssetResult(request, record, operation,
                                       {.severity = DiagnosticSeverity::Error,
                                        .result = BuildOutputResult::Failed,
                                        .stage = "prepare",
                                        .code = DiagnosticCode{readResult.ErrorValue().code.Value()}});
                    return Result<std::vector<CookSlot>>::Failure(readResult.ErrorValue());
                }

                auto sourceBytes = std::move(readResult).Value();
                auto sourceDigest = ComputeSha256(std::as_bytes(std::span{sourceBytes}));
                const auto identity = contribution->strategy->CacheIdentity();
                auto cacheKey = BuildAssetCookCacheKey(AssetCookCacheKeyInputs{
                    .assetId = record.id,
                    .assetType = record.type,
                    .sourceDigest = sourceDigest,
                    .settingsDigest = identity.settingsDigest,
                    .settingsSchemaVersion = identity.settingsSchemaVersion,
                    .cookerContributionId = contribution->contributionId,
                    .cookerVersion = identity.version,
                    .target = request.target,
                    .artifactFormatVersion = AssetCookArtifact::CurrentFormatVersion,
                });

                slots.push_back(CookSlot{
                    .record = record,
                    .sourceBytes = std::move(sourceBytes),
                    .sourceDigest = sourceDigest,
                    .cacheKey = cacheKey,
                });
            }
            return Result<std::vector<CookSlot>>::Success(std::move(slots));
        }

        /** @brief Verifies exact generic and domain identity before an existing cache entry can be admitted. */
        Result<void> ValidateCachedSlot(const AssetCookRequest &request, const CookerCatalogSnapshot &catalog, const CookSlot &slot,
                                        const std::span<const std::uint8_t> bytes) {
            auto decoded = DecodeCookedArtifact(bytes, request.limits);
            if (decoded.HasError())
                return Result<void>::Failure(decoded.ErrorValue());
            const auto &artifact = decoded.Value();
            if (artifact.id != slot.record.id || artifact.type != slot.record.type || artifact.target != request.target ||
                artifact.sourceDigest != slot.sourceDigest || artifact.cacheKeyDigest != slot.cacheKey.digest)
                return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
            const auto *strategy = catalog.Find(slot.record.type, request.target);
            if (strategy == nullptr)
                return Result<void>::Failure(MakeError(CookErrors::CookerMissing));
            const CookSourceView sourceView{
                .id = slot.record.id,
                .type = slot.record.type,
                .target = request.target,
                .sourceDigest = slot.sourceDigest,
                .bytes = slot.sourceBytes,
                .sourceContext = slot.record.sourcePath.String(),
            };
            return strategy->ValidateCookedPayload(sourceView, artifact.payload);
        }

        Result<std::size_t> ResolveCacheHits(const AssetCookRequest &request, const CookerCatalogSnapshot &catalog,
                                             const AssetCookCache &cache, std::span<CookSlot> slots, const CancellationToken &cancellation,
                                             CookOperationScope &operation) {
            operation.Update("cache_check", "Checking artifact cache", 0.2F);
            std::size_t cacheHits = 0;
            for (auto &slot : slots) {
                const auto fail = [&](const Error &error) {
                    PublishAssetFailure(request, slot.record, operation, "cache_check", error);
                    return Result<std::size_t>::Failure(error);
                };
                auto cacheResult = cache.Load(slot.cacheKey, cancellation);
                if (cacheResult.HasError())
                    return fail(cacheResult.ErrorValue());

                if (auto cached = std::move(cacheResult).Value(); cached.has_value()) {
                    if (auto validated = ValidateCachedSlot(request, catalog, slot, *cached); validated.HasError())
                        return fail(validated.ErrorValue());
                    slot.cacheHit = true;
                    slot.cookedArtifact = std::move(*cached);
                    ++cacheHits;
                    if (request.buildOutputStore != nullptr) {
                        operation.Publish(BuildOutputRecord{
                            .timestampUtc = std::chrono::system_clock::now(),
                            .result = BuildOutputResult::Cached,
                            .stage = "cache_check",
                            .code = DiagnosticCode{"asset.cook.cache_hit"},
                            .message = slot.record.sourcePath.String(),
                            .source =
                                DiagnosticSourceLocation{
                                    .absolutePath = (request.sourceRoot / slot.record.sourcePath.String()).lexically_normal().string()},
                        });
                    }
                }
            }
            return Result<std::size_t>::Success(cacheHits);
        }

        /** @brief Allocates the optional build-output identity before admitting an operation row. */
        [[nodiscard]] Result<std::optional<BuildOutputSessionId>> BeginOutputSession(BuildOutputStore *output) {
            if (output == nullptr)
                return Result<std::optional<BuildOutputSessionId>>::Success(std::nullopt);
            std::optional<BuildOutputSessionId> sessionId = output->BeginSession();
            if (!sessionId.has_value())
                return Result<std::optional<BuildOutputSessionId>>::Failure(MakeError(CookErrors::OutputIdentityExhausted));
            return Result<std::optional<BuildOutputSessionId>>::Success(std::move(sessionId));
        }

        /** @brief Requires an explicit bounded publication composition before cooking can perform any writes. */
        [[nodiscard]] bool HasValidCookComposition(const AssetCookRequest &request, const bool hasCatalog) noexcept {
            return hasCatalog && request.publicationFiles && request.newPublicationOperationId && !request.cookedRoot.empty() &&
                   request.cookedRoot.is_absolute() && request.limits.maximumAssets > 0U && request.limits.maximumArtifactBytes > 0U;
        }

        /** @brief Admits the optional authoritative cook row only after its output identity was allocated. */
        [[nodiscard]] Result<std::optional<OperationId>> BeginCookOperation(const AssetCookRequest &request,
                                                                            const std::size_t recordCount) {
            if (request.operationStore == nullptr)
                return Result<std::optional<OperationId>>::Success(std::nullopt);
            auto operation = request.operationStore->Begin(OperationDescriptor{.kind = OperationKind::Cook,
                                                                               .title = "Cook assets",
                                                                               .phase = "prepare",
                                                                               .message = std::format("{} assets", recordCount),
                                                                               .progress = 0.0F,
                                                                               .cancellable = static_cast<bool>(request.requestCancel),
                                                                               .requestCancel = request.requestCancel});
            if (!operation.has_value())
                return Result<std::optional<OperationId>>::Failure(MakeError(CookErrors::OperationAdmissionFailed));
            return Result<std::optional<OperationId>>::Success(operation);
        }

        /** @brief Executes the admitted immutable registry closure and preserves typed stage outcomes until publication. */
        [[nodiscard]] Result<AssetCookReport> CookRegistryRecords(JobSystem &jobs, const CookerCatalogSnapshot &catalog,
                                                                  const AssetCookRequest &request,
                                                                  const std::span<const AssetRecord> records,
                                                                  const CancellationToken &cancellation, CookOperationScope &operation) {
            if (records.empty())
                return HandleEmptyCookSnapshot(request, cancellation, operation);
            if (records.size() > request.limits.maximumAssets) {
                operation.RecordOutcome(false);
                return Result<AssetCookReport>::Failure(Error{CookErrors::TooLarge.code});
            }
            AssetCookCache cache(request.cacheRoot, request.limits);
            auto slotsResult = PrepareCookSlots(request, catalog, records, operation);
            if (slotsResult.HasError()) {
                operation.RecordError(slotsResult.ErrorValue());
                return Result<AssetCookReport>::Failure(slotsResult.ErrorValue());
            }
            auto slots = std::move(slotsResult).Value();
            auto cacheHitsResult = ResolveCacheHits(request, catalog, cache, slots, cancellation, operation);
            if (cacheHitsResult.HasError()) {
                operation.RecordError(cacheHitsResult.ErrorValue());
                return Result<AssetCookReport>::Failure(cacheHitsResult.ErrorValue());
            }
            const std::size_t cacheHits = cacheHitsResult.Value();
            if (cacheHits < slots.size()) {
                operation.Update("cook", std::format("Cooking {} assets", slots.size() - cacheHits), 0.4F);
                if (const auto cooked = CookUncachedSlots(jobs, catalog, request, slots, cancellation, operation); cooked.HasError()) {
                    const bool cancelled = IsJobCancelled(cooked.ErrorValue());
                    Error error = cancelled ? WithCause(MakeError(CookErrors::Cancelled), cooked.ErrorValue()) : cooked.ErrorValue();
                    operation.RecordOutcome(cancelled);
                    return Result<AssetCookReport>::Failure(std::move(error));
                }
            }
            Result<AssetCookReport> published = PublishCookedSlots(request, cache, slots, cacheHits, cancellation, operation);
            if (published.HasError())
                operation.RecordError(published.ErrorValue());
            return published;
        }
    }  // namespace

    AssetCookService::AssetCookService(JobSystem &jobs, std::shared_ptr<const CookerCatalogSnapshot> catalog)
        : jobs_(jobs), catalog_(std::move(catalog)) {}

    Result<AssetCookReport> AssetCookService::Cook(const AssetCookRequest &request, const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return Result<AssetCookReport>::Failure(Error{CookErrors::Cancelled.code});

        if (!HasValidCookComposition(request, static_cast<bool>(catalog_)))
            return Result<AssetCookReport>::Failure(Error{CookErrors::MalformedArtifact.code});

        auto records = request.registry.Records();

        Result<std::optional<BuildOutputSessionId>> outputSession = BeginOutputSession(request.buildOutputStore);
        if (outputSession.HasError())
            return Result<AssetCookReport>::Failure(outputSession.ErrorValue());
        auto operationId = BeginCookOperation(request, records.size());
        if (operationId.HasError())
            return Result<AssetCookReport>::Failure(operationId.ErrorValue());
        CookOperationScope operation{request.operationStore, request.buildOutputStore, cancellation,
                                     outputSession.Value().value_or(BuildOutputSessionId{}), operationId.Value()};

        if (request.buildOutputStore != nullptr) {
            const auto now = std::chrono::system_clock::now();
            operation.Publish(BuildOutputRecord{
                .timestampUtc = now,
                .stage = "prepare",
                .code = DiagnosticCode{"asset.cook.started"},
                .message = std::format("Cooking {} assets", records.size()),
            });
        }

        return CookRegistryRecords(jobs_, *catalog_, request, records, cancellation, operation);
    }

}  // namespace Horo::Assets
