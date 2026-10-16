#include "Horo/Runtime/Scene/SceneCellPayloadCodec.h"

#include "SceneCellPayloadReader.h"
#include "SceneCellPayloadWriter.h"

#include <array>
#include <ranges>

namespace Horo::Runtime {
    namespace {
        // Version two retains the shared canonical field order, eight-byte little-endian
        // scalar/count representation and explicit variant/optional tags. No native ABI bytes.
        constexpr std::array Header{std::byte{'H'}, std::byte{'O'}, std::byte{'C'},
                                    std::byte{'O'}, std::byte{'R'}, std::byte{'E'},
                                    std::byte{'C'}, std::byte{'S'}, std::byte{SceneCellPayloadCodecVersion},
                                    std::byte{0},   std::byte{0},   std::byte{0},
                                    std::byte{0},   std::byte{0},   std::byte{0},
                                    std::byte{0}};

        /** @brief Requires explicit positive independent budgets before any wire ownership is allocated. */
        bool ValidLimits(const SceneCellPayloadCodecLimits &limits) noexcept {
            return limits.maximumEncodedBytes >= Header.size() && limits.maximumDecodeAllocationBytes && limits.semantic.maximumEntities &&
                   limits.semantic.maximumDependencies && limits.semantic.maximumRetainedBytes;
        }

        /** @brief Emits the same immutable fields in both measurement and output passes. */
        void WritePayload(CellCookDetail::PayloadWriter &writer, const RuntimeSceneCellPayload &payload) {
            writer.Bytes(Header);
            writer.Fields(payload.Identity(), payload.Definition().Entities(), payload.Definition().AssetDependencies(),
                          payload.Definition().TriggerVolumeEvidence());
        }

        /** @brief Validates the exact supported header before constructing typed component storage. */
        Result<void> ValidateHeader(const std::span<const std::byte> bytes) {
            if (bytes.size() < Header.size() || !std::ranges::equal(bytes.first(8), std::span{Header}.first(8)))
                return Result<void>::Failure(
                    MakeError(SceneCellPayloadErrors::Invalid, "CoreEcs provider header is truncated or invalid."));
            if (bytes[8] != std::byte{SceneCellPayloadCodecVersion} || bytes[9] != std::byte{0} || bytes[10] != std::byte{0} ||
                bytes[11] != std::byte{0})
                return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Unsupported, "CoreEcs provider version is unsupported."));
            if (!std::ranges::all_of(bytes.subspan(12, 4), [](const auto byte) {
                return byte == std::byte{0};
            }))
                return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Invalid, "CoreEcs provider reserved fields must be zero."));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc EncodeSceneCellPayload */
    Result<std::vector<std::byte>> EncodeSceneCellPayload(const RuntimeSceneCellPayload &payload, const SceneCellPayloadCodecLimits &limits,
                                                          const CancellationToken &cancellation) {
        if (!ValidLimits(limits))
            return Result<std::vector<std::byte>>::Failure(MakeError(SceneCellPayloadErrors::Invalid));
        if (payload.RetainedBytes() > limits.semantic.maximumRetainedBytes ||
            payload.Definition().Entities().size() > limits.semantic.maximumEntities ||
            payload.Definition().AssetDependencies().size() > limits.semantic.maximumDependencies)
            return Result<std::vector<std::byte>>::Failure(MakeError(SceneCellPayloadErrors::CapacityExceeded));
        CellCookDetail::PayloadWriter measure{cancellation, limits.maximumEncodedBytes};
        WritePayload(measure, payload);
        const auto measured = measure.Finish();
        if (measured.HasError())
            return Result<std::vector<std::byte>>::Failure(measured.ErrorValue());
        std::vector<std::byte> bytes(measured.Value());
        CellCookDetail::PayloadWriter writer{cancellation, limits.maximumEncodedBytes, &bytes};
        WritePayload(writer, payload);
        const auto written = writer.Finish();
        if (written.HasError())
            return Result<std::vector<std::byte>>::Failure(written.ErrorValue());
        if (written.Value() != bytes.size())
            return Result<std::vector<std::byte>>::Failure(MakeError(SceneCellPayloadErrors::Invalid));
        return Result<std::vector<std::byte>>::Success(std::move(bytes));
    }

    /** @copydoc DecodeSceneCellPayload */
    Result<RuntimeSceneCellPayload> DecodeSceneCellPayload(const WorldStreaming::WorldPartitionDescriptor &partition,
                                                           const std::span<const std::byte> bytes, const SceneCellPayloadIdentity &expected,
                                                           const std::span<const SceneCellComponentSchema> componentSchemas,
                                                           const std::span<const SceneCellBehaviorSchema> behaviorSchemas,
                                                           const SceneCellPayloadCodecLimits &limits,
                                                           const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return Result<RuntimeSceneCellPayload>::Failure(MakeError(SceneCellPayloadErrors::Cancelled));
        if (!ValidLimits(limits))
            return Result<RuntimeSceneCellPayload>::Failure(MakeError(SceneCellPayloadErrors::Invalid));
        if (bytes.size() > limits.maximumEncodedBytes)
            return Result<RuntimeSceneCellPayload>::Failure(MakeError(SceneCellPayloadErrors::CapacityExceeded));
        if (const auto header = ValidateHeader(bytes); header.HasError())
            return Result<RuntimeSceneCellPayload>::Failure(header.ErrorValue());
        CellCookDetail::PayloadReader reader{bytes.subspan(Header.size()), limits.maximumDecodeAllocationBytes, cancellation};
        SceneCellPayloadIdentity identity;
        std::vector<RuntimeEntityDefinition> entities;
        std::vector<SceneAssetDependency> dependencies;
        std::vector<TriggerVolumeMigrationEvidence> migration;
        reader.Fields(identity);
        reader.Sequence(entities, limits.semantic.maximumEntities);
        reader.Sequence(dependencies, limits.semantic.maximumDependencies);
        reader.Sequence(migration, entities.size());
        if (const auto read = reader.Finish(); read.HasError())
            return Result<RuntimeSceneCellPayload>::Failure(read.ErrorValue());
        return CookRuntimeSceneCellPayload(partition, {identity, entities, dependencies, componentSchemas, behaviorSchemas, migration},
                                           expected, limits.semantic, cancellation);
    }
}  // namespace Horo::Runtime
