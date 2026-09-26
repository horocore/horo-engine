/**
 * @copydoc AssetCookService.h
 */

#include "Horo/Assets/AssetCookService.h"

#include "../AssetErrors.h"
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

        class CookOperationScope {
        public:
            CookOperationScope(OperationStore *store, BuildOutputStore *output, const CancellationToken &cancellation,
                               const BuildOutputSessionId sessionId, const std::optional<OperationId> id)
                : store_(store), output_(output), cancellation_(&cancellation), sessionId_(sessionId), id_(id) {}

            ~CookOperationScope() {
                if (completed_)
                    return;
                const bool cancelled = cancelledResult_.value_or(cancellation_->IsCancellationRequested());
                Publish(BuildOutputRecord{.timestampUtc = std::chrono::system_clock::now(),
                                          .severity = cancelled ? DiagnosticSeverity::Warning : DiagnosticSeverity::Error,
                                          .result = cancelled ? BuildOutputResult::Cancelled : BuildOutputResult::Failed,
                                          .stage = "cook",
                                          .code = DiagnosticCode{cancelled ? "asset.cook.cancelled" : "asset.cook.failed"},
                                          .message = cancelled ? "Cook cancelled" : "Cook failed"});
                if (store_ != nullptr && id_.has_value())
                    static_cast<void>(
                        store_->Update(*id_, OperationUpdate{.state = cancelled ? OperationState::Cancelled : OperationState::Failed,
                                                             .phase = "cook",
                                                             .message = cancelled ? "Cook cancelled" : "Cook failed"}));
            }

            CookOperationScope(const CookOperationScope &) = delete;
            CookOperationScope &operator=(const CookOperationScope &) = delete;

            void RecordOutcome(const bool cancelled) {
                cancelledResult_ = cancelled;
            }

            void RecordError(const Error &error) {
                const std::string_view domain = error.domain.Value();
                cancelledResult_ = error.code.Value() == CookErrors::Cancelled.code.Value() &&
                                   (domain.empty() || domain == CookErrors::Cancelled.domain.Value());
            }

            void Update(std::string phase, std::string message, const float progress) {
                if (store_ != nullptr && id_.has_value())
                    static_cast<void>(store_->Update(*id_, OperationUpdate{.state = OperationState::Running,
                                                                           .phase = std::move(phase),
                                                                           .message = std::move(message),
                                                                           .progress = progress}));
            }

            void Succeed(std::string message) {
                const std::string outputMessage = message;
                Publish(BuildOutputRecord{.timestampUtc = std::chrono::system_clock::now(),
                                          .result = BuildOutputResult::Succeeded,
                                          .stage = "complete",
                                          .code = DiagnosticCode{"asset.cook.succeeded"},
                                          .message = outputMessage});
                if (store_ != nullptr && id_.has_value())
                    static_cast<void>(store_->Update(*id_, OperationUpdate{.state = OperationState::Succeeded,
                                                                           .phase = "complete",
                                                                           .message = std::move(message),
                                                                           .progress = 1.0F}));
                completed_ = true;
            }

            void Publish(BuildOutputRecord record) const {
                if (output_ == nullptr)
                    return;
                record.sessionId = sessionId_;
                record.operationId = id_;
                output_->Append(std::move(record));
            }

        private:
            OperationStore *store_{};
            BuildOutputStore *output_{};
            const CancellationToken *cancellation_{};
            BuildOutputSessionId sessionId_;
            std::optional<OperationId> id_;
            bool completed_{false};
            std::optional<bool> cancelledResult_;
        };

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

        /** @brief Publishes an asset-scoped result with a navigable source path. */
        void PublishAssetResult(const AssetCookRequest &request, const AssetRecord &record, CookOperationScope &operation,
                                const std::string_view stage, const DiagnosticCode code, const DiagnosticSeverity severity,
                                const BuildOutputResult result) {
            operation.Publish(BuildOutputRecord{
                .timestampUtc = std::chrono::system_clock::now(),
                .severity = severity,
                .result = result,
                .stage = std::string(stage),
                .code = code,
                .message = record.sourcePath.String(),
                .source =
                    DiagnosticSourceLocation{.absolutePath = (request.sourceRoot / record.sourcePath.String()).lexically_normal().string()},
            });
        }

        Result<AssetCookReport> HandleEmptyCookSnapshot(const AssetCookRequest &request, CookOperationScope &operation) {
            const std::string currentStr = std::format(
                R"({{"schemaVersion":1,"target":"{}","manifestDigest":"0000000000000000000000000000000000000000000000000000000000000000","generationPath":"generations/empty","artifactCount":"0"}})",
                request.target.Value());
            const auto currentBytes =
                std::vector<std::uint8_t>(reinterpret_cast<const std::uint8_t *>(currentStr.data()),
                                          reinterpret_cast<const std::uint8_t *>(currentStr.data()) + currentStr.size());

            auto currentPath = request.cookedRoot / "current.json";
            auto tempPath = currentPath;
            tempPath += ".tmp";
            {
                std::ofstream temp(tempPath, std::ios::binary | std::ios::trunc);
                temp.write(reinterpret_cast<const char *>(currentBytes.data()), static_cast<std::streamsize>(currentBytes.size()));
            }
            std::filesystem::rename(tempPath, currentPath);

            operation.Succeed("Cooked 0 assets");
            return Result<AssetCookReport>::Success(AssetCookReport{
                .generation =
                    AssetCookGeneration{
                        .target = request.target,
                        .generationRoot = request.cookedRoot / "generations" / "empty",
                        .artifactCount = 0,
                    },
                .totalAssets = 0,
                .cookedAssets = 0,
                .cacheHits = 0,
            });
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
            };

            auto cookResult = strategy->Cook(sourceView, cancellation);
            if (cookResult.HasError())
                return Result<void>::Failure(cookResult.ErrorValue());

            auto sink = std::move(cookResult).Value();
            const AssetCookArtifact artifact{
                .id = sourceView.id,
                .type = sourceView.type,
                .target = sourceView.target,
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

        /** @brief Runs uncached cook slots as a fail-fast group while preserving cancellation causes. */
        Result<void> CookUncachedSlots(JobSystem &jobs, const CookerCatalogSnapshot &catalog, const AssetCookRequest &request,
                                       std::span<CookSlot> slots, const CancellationToken &cancellation, CookOperationScope &operation) {
            TaskGroup group(jobs, TaskGroupFailurePolicy::FailFast, cancellation);
            for (CookSlot &slot : slots) {
                if (slot.cacheHit)
                    continue;
                const JobFunction work = [&slot, &catalog, &request](const CancellationToken &jobCancellation) {
                    if (jobCancellation.IsCancellationRequested())
                        return JobCancelled(MakeError(CookErrors::Cancelled));
                    Result<void> cooked = CookAndEncodeSlot(catalog, slot, request.target, jobCancellation);
                    if (cooked.HasError())
                        slot.cookError = cooked.ErrorValue();
                    if (cooked.HasError() && cooked.ErrorValue().code.Value() == CookErrors::Cancelled.code.Value() &&
                        jobCancellation.IsCancellationRequested())
                        return JobCancelled(cooked.ErrorValue());
                    return cooked;
                };
                if (auto spawned = group.Spawn({}, work); spawned.HasError())
                    return Result<void>::Failure(spawned.ErrorValue());
            }
            Result<void> joined = group.Join();
            for (const CookSlot &slot : slots) {
                if (!slot.cookError.has_value())
                    continue;
                const bool cancelled = slot.cookError->code.Value() == CookErrors::Cancelled.code.Value();
                PublishAssetResult(request, slot.record, operation, "cook", DiagnosticCode{slot.cookError->code.Value()},
                                   cancelled ? DiagnosticSeverity::Warning : DiagnosticSeverity::Error,
                                   cancelled ? BuildOutputResult::Cancelled : BuildOutputResult::Failed);
            }
            return joined;
        }

        Result<AssetCookReport> PublishCookedSlots(const AssetCookRequest &request, const AssetCookCache &cache,

                                                   std::vector<CookSlot> &slots, const std::size_t cacheHits,
                                                   const CancellationToken &cancellation, CookOperationScope &operation) {
            if (request.buildOutputStore != nullptr) {
                for (const auto &slot : slots) {
                    if (!slot.cacheHit) {
                        PublishAssetResult(request, slot.record, operation, "cook", DiagnosticCode{"asset.cook.asset_cooked"},
                                           DiagnosticSeverity::Note, BuildOutputResult::Succeeded);
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

            auto pubResult = PublishCookGeneration(request.cookedRoot, request.target, manifestEntries, manifestPayloads, request.limits);
            if (pubResult.HasError())
                return Result<AssetCookReport>::Failure(pubResult.ErrorValue());

            const std::size_t cookedCount = slots.size() - cacheHits;
            operation.Succeed(std::format("{} cooked, {} cached", cookedCount, cacheHits));

            return Result<AssetCookReport>::Success(AssetCookReport{
                .generation = pubResult.Value(),
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
                if (const auto *strategy = catalog.Find(record.type, request.target); !strategy) {
                    PublishAssetResult(request, record, operation, "prepare", DiagnosticCode{CookErrors::CookerMissing.code.Value()},
                                       DiagnosticSeverity::Error, BuildOutputResult::Failed);
                    return Result<std::vector<CookSlot>>::Failure(Error{CookErrors::CookerMissing.code});
                }

                auto readResult = ReadSourceBytes(request.sourceRoot, record.sourcePath.String(), request.limits.maximumSourceBytes);
                if (readResult.HasError()) {
                    PublishAssetResult(request, record, operation, "prepare", DiagnosticCode{readResult.ErrorValue().code.Value()},
                                       DiagnosticSeverity::Error, BuildOutputResult::Failed);
                    return Result<std::vector<CookSlot>>::Failure(readResult.ErrorValue());
                }

                auto sourceBytes = std::move(readResult).Value();
                auto sourceDigest = ComputeSha256(std::as_bytes(std::span{sourceBytes}));
                auto cacheKey = BuildAssetCookCacheKey(AssetCookCacheKeyInputs{
                    .assetId = record.id,
                    .assetType = record.type,
                    .sourceDigest = sourceDigest,
                    .cookerContributionId = record.type.Value(),
                    .cookerVersion = "1.0.0",
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

        Result<std::size_t> ResolveCacheHits(const AssetCookRequest &request, const AssetCookCache &cache, std::span<CookSlot> slots,
                                             const CancellationToken &cancellation, CookOperationScope &operation) {
            operation.Update("cache_check", "Checking artifact cache", 0.2F);
            std::size_t cacheHits = 0;
            for (auto &slot : slots) {
                auto cacheResult = cache.Load(slot.cacheKey, cancellation);
                if (cacheResult.HasError())
                    return Result<std::size_t>::Failure(cacheResult.ErrorValue());

                if (auto cached = std::move(cacheResult).Value(); cached.has_value()) {
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
    }  // namespace

    AssetCookService::AssetCookService(JobSystem &jobs, std::shared_ptr<const CookerCatalogSnapshot> catalog)
        : jobs_(jobs), catalog_(std::move(catalog)) {}

    Result<AssetCookReport> AssetCookService::Cook(const AssetCookRequest &request, const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return Result<AssetCookReport>::Failure(Error{CookErrors::Cancelled.code});

        if (!catalog_)
            return Result<AssetCookReport>::Failure(Error{CookErrors::MalformedArtifact.code});

        auto records = request.registry.Records();

        Result<std::optional<BuildOutputSessionId>> outputSession = BeginOutputSession(request.buildOutputStore);
        if (outputSession.HasError())
            return Result<AssetCookReport>::Failure(outputSession.ErrorValue());
        std::optional<OperationId> operationId;
        if (request.operationStore != nullptr) {
            operationId = request.operationStore->Begin(OperationDescriptor{.kind = OperationKind::Cook,
                                                                            .title = "Cook assets",
                                                                            .phase = "prepare",
                                                                            .message = std::format("{} assets", records.size()),
                                                                            .progress = 0.0F,
                                                                            .cancellable = static_cast<bool>(request.requestCancel),
                                                                            .requestCancel = request.requestCancel});
            if (!operationId.has_value())
                return Result<AssetCookReport>::Failure(MakeError(CookErrors::OperationAdmissionFailed));
        }
        CookOperationScope operation{request.operationStore, request.buildOutputStore, cancellation,
                                     outputSession.Value().value_or(BuildOutputSessionId{}), operationId};

        if (request.buildOutputStore != nullptr) {
            const auto now = std::chrono::system_clock::now();
            operation.Publish(BuildOutputRecord{
                .timestampUtc = now,
                .stage = "prepare",
                .code = DiagnosticCode{"asset.cook.started"},
                .message = std::format("Cooking {} assets", records.size()),
            });
        }

        if (records.empty())
            return HandleEmptyCookSnapshot(request, operation);

        if (records.size() > request.limits.maximumAssets) {
            operation.RecordOutcome(false);
            return Result<AssetCookReport>::Failure(Error{CookErrors::TooLarge.code});
        }

        AssetCookCache cache(request.cacheRoot, request.limits);
        auto slotsResult = PrepareCookSlots(request, *catalog_, records, operation);
        if (slotsResult.HasError()) {
            operation.RecordError(slotsResult.ErrorValue());
            return Result<AssetCookReport>::Failure(slotsResult.ErrorValue());
        }
        auto slots = std::move(slotsResult).Value();

        auto cacheHitsResult = ResolveCacheHits(request, cache, slots, cancellation, operation);
        if (cacheHitsResult.HasError()) {
            operation.RecordError(cacheHitsResult.ErrorValue());
            return Result<AssetCookReport>::Failure(cacheHitsResult.ErrorValue());
        }
        const std::size_t cacheHits = cacheHitsResult.Value();

        if (cacheHits < slots.size()) {
            operation.Update("cook", std::format("Cooking {} assets", slots.size() - cacheHits), 0.4F);
            if (const auto cooked = CookUncachedSlots(jobs_, *catalog_, request, slots, cancellation, operation); cooked.HasError()) {
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

}  // namespace Horo::Assets
