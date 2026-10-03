#include "Horo/WorldStreaming/StreamingCellCandidate.h"

#include "Horo/WorldStreaming/WorldStreamingErrors.h"
#include "StreamingCellArtifactInternal.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

namespace Horo::WorldStreaming {
    namespace {
        constexpr std::uint64_t PayloadHeaderBytes = 40;
        constexpr std::uint64_t PayloadAlignment = 8;

        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] constexpr bool IsKnown(const StreamingCellCompression value) noexcept {
            return value < StreamingCellCompression::Count;
        }

        [[nodiscard]] constexpr bool IsKnown(const StreamingCellPayloadRequirement value) noexcept {
            return value < StreamingCellPayloadRequirement::Count;
        }

        [[nodiscard]] constexpr bool IsKnown(const StreamingCellCandidateLifecycle value) noexcept {
            return value < StreamingCellCandidateLifecycle::Count;
        }

        [[nodiscard]] constexpr std::uint16_t ProviderValue(const StreamingCellProvider provider) noexcept {
            return static_cast<std::underlying_type_t<StreamingCellProvider>>(provider);
        }

        [[nodiscard]] constexpr bool IsKnownProvider(const StreamingCellProvider provider) noexcept {
            using enum StreamingCellProvider;
            const auto value = ProviderValue(provider);
            return (value >= ProviderValue(CoreEcs) && value <= ProviderValue(Destruction)) || value >= ProviderValue(FirstCustom);
        }

        [[nodiscard]] bool CheckedAdd(const std::uint64_t left, const std::uint64_t right, std::uint64_t &sum) noexcept {
            if (right > std::numeric_limits<std::uint64_t>::max() - left)
                return false;
            sum = left + right;
            return true;
        }

        [[nodiscard]] bool AlignPayload(const std::uint64_t value, std::uint64_t &aligned) noexcept {
            std::uint64_t padded{};
            if (!CheckedAdd(value, PayloadAlignment - 1U, padded))
                return false;
            aligned = padded & ~(PayloadAlignment - 1U);
            return true;
        }

        template <typename Entry, typename Projection>
        [[nodiscard]] std::optional<std::size_t> FindCell(const std::span<const Entry> entries, const StreamingCellId &cell,
                                                          Projection identity) {
            const auto found = std::ranges::lower_bound(entries, cell, StreamingCellCanonicalLess{}, identity);
            if (found == entries.end() || std::invoke(identity, *found) != cell)
                return std::nullopt;
            return static_cast<std::size_t>(std::distance(entries.begin(), found));
        }

        [[nodiscard]] Result<void> ValidateContext(const CookedWorldIndexManifest &manifest, const StreamingCellCandidateContext &context) {
            if (!IsKnown(context.lifecycle) || !context.operation.IsValid() || context.maximumPayloads == 0 ||
                context.maximumDependencies == 0 || context.maximumCompressedBytes == 0 || context.maximumUncompressedBytes == 0)
                return Failure<void>(WorldStreamingErrors::CellCandidateInvalid);
            if (context.lifecycle != StreamingCellCandidateLifecycle::Active)
                return Failure<void>(WorldStreamingErrors::CellCandidateLifecycleUnavailable);
            if (context.operationKind != StreamingCellOperationKind::Load ||
                context.operationState != StreamingCellOperationState::Preparing)
                return Failure<void>(WorldStreamingErrors::CellCandidateUnsupported);
            if (context.operation.fence.partition != manifest.Descriptor().Partition())
                return Failure<void>(WorldStreamingErrors::CellCandidateStale);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateHeaderIdentity(const CookedWorldCellManifestEntry &record,
                                                          const StreamingCellCandidateContext &context,
                                                          const StreamingCellHeaderView &header) {
            if (header.majorVersion != StreamingCellHeaderView::CurrentMajorVersion)
                return Failure<void>(WorldStreamingErrors::CellCandidateUnsupported);
            if (!header.cell.IsValid() || !IsKnown(header.compression) || header.compressedSize == 0 || header.uncompressedSize == 0 ||
                header.payloads.empty())
                return Failure<void>(WorldStreamingErrors::CellCandidateInvalid);
            if (header.cell != context.operation.fence.cell || header.cell != record.cell ||
                header.compressedSize != record.compressedSize || header.uncompressedSize != record.uncompressedSize ||
                header.payloadCrc32 != record.payloadCrc32 || header.artifactHash != record.artifactHash)
                return Failure<void>(WorldStreamingErrors::CellCandidateStale);
            if (header.payloads.size() > context.maximumPayloads || header.compressedSize > context.maximumCompressedBytes ||
                header.uncompressedSize > context.maximumUncompressedBytes)
                return Failure<void>(WorldStreamingErrors::CellCandidateCapacityExceeded);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidatePayload(const StreamingCellPayloadHeader &payload, const StreamingCellHeaderView &header) {
            if (!IsKnown(payload.requirement) || payload.version == 0 || payload.compressedSize == 0 || payload.uncompressedSize == 0 ||
                (header.compression == StreamingCellCompression::None && payload.compressedSize != payload.uncompressedSize))
                return Failure<void>(WorldStreamingErrors::CellCandidateInvalid);
            if (header.minorVersion > StreamingCellHeaderView::CurrentMinorVersion &&
                ProviderValue(payload.provider) > ProviderValue(StreamingCellProvider::Destruction) &&
                payload.requirement == StreamingCellPayloadRequirement::Required)
                return Failure<void>(WorldStreamingErrors::CellCandidateUnsupported);
            if (!IsKnownProvider(payload.provider) &&
                !(header.minorVersion > StreamingCellHeaderView::CurrentMinorVersion &&
                  payload.requirement == StreamingCellPayloadRequirement::Optional &&
                  ProviderValue(payload.provider) > ProviderValue(StreamingCellProvider::Destruction)))
                return Failure<void>(WorldStreamingErrors::CellCandidateInvalid);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidatePayloadTable(const StreamingCellHeaderView &header) {
            if (header.payloads.front().provider != StreamingCellProvider::CoreEcs ||
                header.payloads.front().requirement != StreamingCellPayloadRequirement::Required)
                return Failure<void>(WorldStreamingErrors::CellCandidateInvalid);

            std::uint64_t tableBytes{};
            if (header.payloads.size() > std::numeric_limits<std::uint64_t>::max() / PayloadHeaderBytes)
                return Failure<void>(WorldStreamingErrors::CellCandidateCapacityExceeded);
            tableBytes = header.payloads.size() * PayloadHeaderBytes;
            std::uint64_t firstPayload{};
            if (!CheckedAdd(StreamingCellHeaderView::FixedHeaderBytes, tableBytes, firstPayload))
                return Failure<void>(WorldStreamingErrors::CellCandidateCapacityExceeded);
            std::uint64_t expectedOffset{};
            if (!AlignPayload(firstPayload, expectedOffset))
                return Failure<void>(WorldStreamingErrors::CellCandidateCapacityExceeded);
            std::uint64_t decodedBytes{};
            std::uint16_t previousProvider{};
            for (const auto &payload : header.payloads) {
                if (const auto valid = ValidatePayload(payload, header); valid.HasError())
                    return valid;
                const auto provider = ProviderValue(payload.provider);
                if (provider <= previousProvider || payload.offset != expectedOffset ||
                    !CheckedAdd(decodedBytes, payload.uncompressedSize, decodedBytes))
                    return Failure<void>(WorldStreamingErrors::CellCandidateInvalid);
                std::uint64_t payloadEnd{};
                if (!CheckedAdd(payload.offset, payload.compressedSize, payloadEnd))
                    return Failure<void>(WorldStreamingErrors::CellCandidateCapacityExceeded);
                if (!AlignPayload(payloadEnd, expectedOffset))
                    return Failure<void>(WorldStreamingErrors::CellCandidateCapacityExceeded);
                previousProvider = provider;
            }

            std::uint64_t artifactEnd{};
            if (!CheckedAdd(StreamingCellHeaderView::FixedHeaderBytes, header.compressedSize, artifactEnd))
                return Failure<void>(WorldStreamingErrors::CellCandidateCapacityExceeded);
            const auto &lastPayload = header.payloads.back();
            if (std::uint64_t lastPayloadEnd{}; !CheckedAdd(lastPayload.offset, lastPayload.compressedSize, lastPayloadEnd) ||
                                                lastPayloadEnd != artifactEnd || decodedBytes != header.uncompressedSize)
                return Failure<void>(WorldStreamingErrors::CellCandidateInvalid);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc StreamingCellCandidate::StreamingCellCandidate */
    StreamingCellCandidate::StreamingCellCandidate(StreamingCellOperationHandle operation, Assets::AssetId chunkAsset,
                                                   StreamingCellCandidateManifestRecord manifestEntry,
                                                   const StreamingCellCompression compression,
                                                   std::vector<StreamingCellPayloadHeader> payloads,
                                                   std::vector<StreamingCellId> hardDependencies) noexcept
        : operation_(std::move(operation)), chunkAsset_(std::move(chunkAsset)), manifestEntry_(std::move(manifestEntry)),
          compression_(compression), payloads_(std::move(payloads)), hardDependencies_(std::move(hardDependencies)) {}

    /** @copydoc StreamingCellCandidate::Operation */
    const StreamingCellOperationHandle &StreamingCellCandidate::Operation() const noexcept {
        return operation_;
    }

    /** @copydoc StreamingCellCandidate::ChunkAsset */
    const Assets::AssetId &StreamingCellCandidate::ChunkAsset() const noexcept {
        return chunkAsset_;
    }

    /** @copydoc StreamingCellCandidate::ManifestEntry */
    const StreamingCellCandidateManifestRecord &StreamingCellCandidate::ManifestEntry() const noexcept {
        return manifestEntry_;
    }

    /** @copydoc StreamingCellCandidate::Compression */
    StreamingCellCompression StreamingCellCandidate::Compression() const noexcept {
        return compression_;
    }

    /** @copydoc StreamingCellCandidate::Payloads */
    std::span<const StreamingCellPayloadHeader> StreamingCellCandidate::Payloads() const noexcept {
        return payloads_;
    }

    /** @copydoc StreamingCellCandidate::HardDependencies */
    std::span<const StreamingCellId> StreamingCellCandidate::HardDependencies() const noexcept {
        return hardDependencies_;
    }

    /** @copydoc PrepareStreamingCellCandidate */
    Result<StreamingCellCandidate> PrepareStreamingCellCandidate(const CookedWorldIndexManifest &manifest,
                                                                 const StreamingCellCandidateContext &context,
                                                                 const StreamingCellHeaderView &header) {
        if (const auto validContext = ValidateContext(manifest, context); validContext.HasError())
            return Result<StreamingCellCandidate>::Failure(validContext.ErrorValue());
        const auto manifestCells = manifest.Cells();
        const auto cellIndex = FindCell(manifestCells, context.operation.fence.cell, &CookedWorldCellManifestEntry::cell);
        if (!cellIndex.has_value())
            return Failure<StreamingCellCandidate>(WorldStreamingErrors::CellCandidateUnavailable);
        const auto index = *cellIndex;
        const auto &manifestRecord = manifestCells[index];
        if (const auto validHeader = ValidateHeaderIdentity(manifestRecord, context, header); validHeader.HasError())
            return Result<StreamingCellCandidate>::Failure(validHeader.ErrorValue());
        if (const auto validPayloads = ValidatePayloadTable(header); validPayloads.HasError())
            return Result<StreamingCellCandidate>::Failure(validPayloads.ErrorValue());

        const auto descriptorCells = manifest.Descriptor().Cells();
        const auto descriptorIndex = FindCell(descriptorCells, context.operation.fence.cell, &WorldPartitionCellDescriptor::id);
        if (!descriptorIndex.has_value())
            return Failure<StreamingCellCandidate>(WorldStreamingErrors::CellCandidateStale);
        const auto manifestDependencies = manifest.HardDependencies(index);
        if (manifestDependencies.size() > context.maximumDependencies)
            return Failure<StreamingCellCandidate>(WorldStreamingErrors::CellCandidateCapacityExceeded);
        std::vector<StreamingCellPayloadHeader> payloads{header.payloads.begin(), header.payloads.end()};
        std::vector<StreamingCellId> hardDependencies{manifestDependencies.begin(), manifestDependencies.end()};
        const StreamingCellCandidateManifestRecord candidateRecord{manifestRecord.cell, manifestRecord.uncompressedSize,
                                                                   manifestRecord.compressedSize, manifestRecord.payloadCrc32,
                                                                   manifestRecord.artifactHash};
        return Result<StreamingCellCandidate>::Success(
            StreamingCellCandidate{context.operation, descriptorCells[*descriptorIndex].package.chunkAsset, candidateRecord,
                                   header.compression, std::move(payloads), std::move(hardDependencies)});
    }

    /** @copydoc ParseStreamingCellArtifact */
    Result<StreamingCellCandidate> ParseStreamingCellArtifact(const CookedWorldIndexManifest &manifest,
                                                              const StreamingCellCandidateContext &context,
                                                              const std::span<const std::byte> artifact,
                                                              const CancellationToken &cancellation) {
        if (const auto valid = ValidateContext(manifest, context); valid.HasError())
            return Result<StreamingCellCandidate>::Failure(valid.ErrorValue());
        const auto index = FindCell(manifest.Cells(), context.operation.fence.cell, &CookedWorldCellManifestEntry::cell);
        if (!index.has_value())
            return Failure<StreamingCellCandidate>(WorldStreamingErrors::CellCandidateUnavailable);
        auto parsed = Detail::ParseCellArtifactBytes(artifact, context, manifest.Cells()[*index].artifactHash, cancellation);
        if (parsed.HasError())
            return Result<StreamingCellCandidate>::Failure(parsed.ErrorValue());
        auto owned = std::move(parsed).Value();
        owned.header.payloads = owned.payloads;
        if (cancellation.IsCancellationRequested())
            return Failure<StreamingCellCandidate>(WorldStreamingErrors::CellCandidateLifecycleUnavailable);
        return PrepareStreamingCellCandidate(manifest, context, owned.header);
    }
}  // namespace Horo::WorldStreaming
