#include "AssetCookPreparation.h"

#include "../AssetErrors.h"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace Horo::Assets::Detail {
    namespace {
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

        /** @brief Requires exact identity, location, metadata path and byte ceiling for a source admitted by the immutable pin. */
        bool MatchesPinnedSource(const AssetCookRequest &request, const AssetRecord &record, const AssetCookPinnedSource *pinned) {
            return pinned != nullptr && pinned->record.type == record.type &&
                   pinned->record.sourcePath.String() == record.sourcePath.String() &&
                   pinned->record.metadataPath.String() == record.metadataPath.String() &&
                   request.pinnedInputs->SourceRoot() == request.sourceRoot && pinned->bytes.size() <= request.limits.maximumSourceBytes;
        }

        /** @brief Produces owned source bytes from a verified pin, or retains the existing unpinned filesystem read contract. */
        Result<std::vector<std::uint8_t>> ReadCookInput(const AssetCookRequest &request, const AssetRecord &record,
                                                        const AssetCookPinnedSource *pinned) {
            if (request.pinnedInputs && !MatchesPinnedSource(request, record, pinned))
                return Result<std::vector<std::uint8_t>>::Failure(MakeError(CookErrors::MalformedArtifact));
            return pinned ? Result<std::vector<std::uint8_t>>::Success(pinned->bytes)
                          : ReadSourceBytes(request.sourceRoot, record.sourcePath.String(), request.limits.maximumSourceBytes);
        }

        /** @brief Binds the selected strategy to exact source/metadata and, when pinned, the full candidate closure/resource envelopes. */
        Result<AssetCookCacheKey> PrepareCacheKey(const AssetCookRequest &request, const AssetRecord &record,
                                                  const CookerContribution &contribution, const Sha256Digest sourceDigest,
                                                  const AssetCookPinnedSource *pinned,
                                                  const std::span<const AssetCookDependencyIdentity> dependencies) {
            const auto identity = contribution.strategy->CacheIdentity();
            const AssetCookCacheKeyInputs inputs{
                .assetId = record.id,
                .assetType = record.type,
                .sourceDigest = sourceDigest,
                .metadataDigest = pinned ? pinned->metadataDigest : Sha256Digest{},
                .metadataSchemaVersion = pinned ? 1U : 0U,
                .settingsDigest = identity.settingsDigest,
                .settingsSchemaVersion = identity.settingsSchemaVersion,
                .cookerContributionId = contribution.contributionId,
                .cookerVersion = identity.version,
                .target = request.target,
                .artifactFormatVersion = AssetCookArtifact::CurrentFormatVersion,
            };
            if (request.pinnedInputs)
                return BuildAssetCookCacheKeyV2(inputs, dependencies, request.pinnedInputs->ClosureDigest(), request.limits.maximumAssets);
            return Result<AssetCookCacheKey>::Success(BuildAssetCookCacheKey(inputs));
        }

        /** @brief Preserves the existing preparation diagnostic stage and typed failure while returning no partial slot set. */
        Result<std::vector<CookSlot>> PreparationFailure(const AssetCookRequest &request, const AssetRecord &record,
                                                         const CookOperationScope &operation, const Error &error) {
            PublishAssetResult(request, record, operation,
                               {.severity = DiagnosticSeverity::Error,
                                .result = BuildOutputResult::Failed,
                                .stage = "prepare",
                                .code = DiagnosticCode{error.code.Value()}});
            return Result<std::vector<CookSlot>>::Failure(error);
        }
    }  // namespace

    /** @copydoc PublishAssetResult */
    void PublishAssetResult(const AssetCookRequest &request, const AssetRecord &record, const CookOperationScope &operation,
                            BuildOutputRecord result) {
        result.timestampUtc = std::chrono::system_clock::now();
        if (result.message.empty())
            result.message = record.sourcePath.String();
        result.source =
            DiagnosticSourceLocation{.absolutePath = (request.sourceRoot / record.sourcePath.String()).lexically_normal().string()};
        operation.Publish(std::move(result));
    }

    /** @copydoc PrepareCookSlots */
    Result<std::vector<CookSlot>> PrepareCookSlots(const AssetCookRequest &request, const CookerCatalogSnapshot &catalog,
                                                   const std::span<const AssetRecord> records, CookOperationScope &operation,
                                                   const std::span<const AssetCookDependencyIdentity> dependencies) {
        std::vector<CookSlot> slots;
        slots.reserve(records.size());
        operation.Update("prepare", "Reading asset sources", 0.1F);
        for (const auto &record : records) {
            const auto *contribution = catalog.FindContribution(record.type, request.target);
            if (!contribution)
                return PreparationFailure(request, record, operation, Error{CookErrors::CookerMissing.code});
            const auto *pinned = request.pinnedInputs ? request.pinnedInputs->Find(record.id) : nullptr;
            auto source = ReadCookInput(request, record, pinned);
            if (source.HasError()) {
                // An invalid pin is admission failure, not a native read diagnostic.
                if (request.pinnedInputs && !MatchesPinnedSource(request, record, pinned))
                    return Result<std::vector<CookSlot>>::Failure(source.ErrorValue());
                return PreparationFailure(request, record, operation, source.ErrorValue());
            }
            auto bytes = std::move(source).Value();
            const auto digest = ComputeSha256(std::as_bytes(std::span{bytes}));
            auto key = PrepareCacheKey(request, record, *contribution, digest, pinned, dependencies);
            if (key.HasError())
                return Result<std::vector<CookSlot>>::Failure(key.ErrorValue());
            slots.push_back({.record = record, .sourceBytes = std::move(bytes), .sourceDigest = digest, .cacheKey = key.Value()});
        }
        return Result<std::vector<CookSlot>>::Success(std::move(slots));
    }
}  // namespace Horo::Assets::Detail
