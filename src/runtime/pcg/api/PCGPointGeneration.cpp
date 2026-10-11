#include "Horo/PCG/PCGPointGeneration.h"

#include "Horo/PCG/PCGCookedPlan.h"
#include "Horo/PCG/PCGErrors.h"
#include "PCGPointGenerationInternal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <new>
#include <span>
#include <utility>

namespace Horo::PCG {
    namespace {
        /** @brief Checks typed source and finite schema-1 settings before encoding or accepting a payload. */
        [[nodiscard]] bool Valid(const PCGJitterGenerationSettings &settings) {
            return settings.grid.IsValid() && std::isfinite(settings.fraction) && settings.fraction >= 0.0 && settings.fraction <= 1.0;
        }

        /** @brief Checks typed source and finite schema-1 settings before encoding or accepting a payload. */
        [[nodiscard]] bool Valid(const PCGStratifiedGenerationSettings &settings) {
            return settings.volume.IsValid() && std::ranges::all_of(settings.strata, [](const std::uint32_t value) {
                return value != 0;
            });
        }

        /** @brief Checks typed source and finite schema-1 settings before encoding or accepting a payload. */
        [[nodiscard]] bool Valid(const PCGPoissonGenerationSettings &settings) {
            return settings.volume.IsValid() && std::isfinite(settings.minimumDistance) && settings.minimumDistance > 0.0;
        }

        /** @brief Reads one fixed-width network-order settings word after exact payload-size validation. */
        [[nodiscard]] std::uint64_t ReadWord(const std::span<const std::uint8_t> bytes) {
            std::uint64_t value{};
            for (const auto byte : bytes)
                value = (value << 8) | byte;
            return value;
        }

        /** @brief Rejects unsupported envelopes before decoding the exact nonzero spatial source identity. */
        [[nodiscard]] Result<SpatialElementId> DecodeSource(const std::span<const std::uint8_t> bytes, const std::size_t expectedSize) {
            if (bytes.size() != expectedSize)
                return Result<SpatialElementId>::Failure(MakeError(PCGErrors::CpuEvaluationInvalid));
            if (ReadWord(bytes.first(4)) != 1)
                return Result<SpatialElementId>::Failure(MakeError(PCGErrors::CpuEvaluationUnsupported));
            const auto id = SpatialElementId::Create(ReadWord(bytes.subspan(4, 8)));
            if (id.HasError())
                return Result<SpatialElementId>::Failure(MakeError(PCGErrors::CpuEvaluationInvalid));
            return id;
        }

        /** @brief Appends exactly the declared network-order word width to pre-sized settings. */
        void WriteWord(const std::span<std::uint8_t> bytes, const std::uint64_t value) {
            for (std::size_t index = 0; index < bytes.size(); ++index)
                bytes[index] = static_cast<std::uint8_t>(value >> ((bytes.size() - index - 1) * 8));
        }

        /** @brief Owns one finite settings payload and translates its allocation failure at the public boundary. */
        template <typename Write>
        [[nodiscard]] Result<std::vector<std::uint8_t>> Encode(const bool valid, const std::size_t count, Write write) {
            if (!valid)
                return Result<std::vector<std::uint8_t>>::Failure(MakeError(PCGErrors::CpuEvaluationInvalid));
            try {
                std::vector<std::uint8_t> bytes(count);
                write(std::span(bytes));
                return Result<std::vector<std::uint8_t>>::Success(std::move(bytes));
            } catch (const std::bad_alloc &) {
                return Result<std::vector<std::uint8_t>>::Failure(MakeError(PCGErrors::CpuEvaluationCapacityExceeded));
            }
        }

        /** @brief Writes the exact schema-1 envelope shared by the new generation settings. */
        void WriteSource(const std::span<std::uint8_t> bytes, const SpatialElementId source) {
            WriteWord(bytes.first(4), 1);
            WriteWord(bytes.subspan(4, 8), source.Value());
        }
    }  // namespace

    /** @copydoc EncodePCGPointGenerationSettings(const PCGGridGenerationSettings&) */
    Result<std::vector<std::uint8_t>> EncodePCGPointGenerationSettings(const PCGGridGenerationSettings &settings) {
        return Encode(settings.grid.IsValid(), 8, [&settings](const std::span<std::uint8_t> bytes) {
            WriteWord(bytes, settings.grid.Value());
        });
    }

    /** @copydoc EncodePCGPointGenerationSettings(const PCGJitterGenerationSettings&) */
    Result<std::vector<std::uint8_t>> EncodePCGPointGenerationSettings(const PCGJitterGenerationSettings &settings) {
        return Encode(Valid(settings), 20, [&settings](const std::span<std::uint8_t> bytes) {
            WriteSource(bytes, settings.grid);
            WriteWord(bytes.subspan(12, 8), std::bit_cast<std::uint64_t>(settings.fraction == 0.0 ? 0.0 : settings.fraction));
        });
    }

    /** @copydoc EncodePCGPointGenerationSettings(const PCGStratifiedGenerationSettings&) */
    Result<std::vector<std::uint8_t>> EncodePCGPointGenerationSettings(const PCGStratifiedGenerationSettings &settings) {
        return Encode(Valid(settings), 24, [&settings](const std::span<std::uint8_t> bytes) {
            WriteSource(bytes, settings.volume);
            for (std::size_t axis = 0; axis < settings.strata.size(); ++axis)
                WriteWord(bytes.subspan(12 + axis * 4, 4), settings.strata[axis]);
        });
    }

    /** @copydoc EncodePCGPointGenerationSettings(const PCGPoissonGenerationSettings&) */
    Result<std::vector<std::uint8_t>> EncodePCGPointGenerationSettings(const PCGPoissonGenerationSettings &settings) {
        return Encode(Valid(settings), 28, [&settings](const std::span<std::uint8_t> bytes) {
            WriteSource(bytes, settings.volume);
            WriteWord(bytes.subspan(12, 4), settings.count);
            WriteWord(bytes.subspan(16, 4), settings.attempts);
            WriteWord(bytes.subspan(20, 8), std::bit_cast<std::uint64_t>(settings.minimumDistance));
        });
    }

    namespace detail {
        /** @copydoc DecodePointGeneration */
        Result<PointGenerationSettings> DecodePointGeneration(const PCGCookedNode &node) {
            const auto type = [&](const PCGCpuNodeKind kind) {
                return PCGCpuNodeType(kind).Value() == node.type;
            };
            if (type(PCGCpuNodeKind::SnapshotGrid)) {
                if (node.payload.size() != 8)
                    return Result<PointGenerationSettings>::Failure(MakeError(PCGErrors::CpuEvaluationInvalid));
                const auto id = SpatialElementId::Create(ReadWord(node.payload));
                if (id.HasError())
                    return Result<PointGenerationSettings>::Failure(id.ErrorValue());
                return Result<PointGenerationSettings>::Success(PCGGridGenerationSettings{id.Value()});
            }
            const auto convert = []<class T>(Result<T> decoded) {
                if (decoded.HasError())
                    return Result<PointGenerationSettings>::Failure(decoded.ErrorValue());
                return Result<PointGenerationSettings>::Success(PointGenerationSettings{decoded.Value()});
            };
            if (type(PCGCpuNodeKind::GridJitter))
                return convert(DecodeJitterSettings(node.payload));
            if (type(PCGCpuNodeKind::Stratified))
                return convert(DecodeStratifiedSettings(node.payload));
            if (type(PCGCpuNodeKind::PoissonScatter))
                return convert(DecodePoissonSettings(node.payload));
            return Result<PointGenerationSettings>::Failure(MakeError(PCGErrors::CpuEvaluationUnsupported));
        }

        /** @copydoc DecodeJitterSettings */
        Result<PCGJitterGenerationSettings> DecodeJitterSettings(const std::span<const std::uint8_t> payload) {
            const auto source = DecodeSource(payload, 20);
            if (source.HasError())
                return Result<PCGJitterGenerationSettings>::Failure(source.ErrorValue());
            const PCGJitterGenerationSettings settings{source.Value(), std::bit_cast<double>(ReadWord(payload.subspan(12, 8)))};
            if (!Valid(settings) || (settings.fraction == 0.0 && std::signbit(settings.fraction)))
                return Result<PCGJitterGenerationSettings>::Failure(MakeError(PCGErrors::CpuEvaluationInvalid));
            return Result<PCGJitterGenerationSettings>::Success(settings);
        }

        /** @copydoc DecodeStratifiedSettings */
        Result<PCGStratifiedGenerationSettings> DecodeStratifiedSettings(const std::span<const std::uint8_t> payload) {
            const auto source = DecodeSource(payload, 24);
            if (source.HasError())
                return Result<PCGStratifiedGenerationSettings>::Failure(source.ErrorValue());
            PCGStratifiedGenerationSettings settings{source.Value()};
            for (std::size_t axis = 0; axis < settings.strata.size(); ++axis)
                settings.strata[axis] = static_cast<std::uint32_t>(ReadWord(payload.subspan(12 + axis * 4, 4)));
            if (!Valid(settings))
                return Result<PCGStratifiedGenerationSettings>::Failure(MakeError(PCGErrors::CpuEvaluationInvalid));
            return Result<PCGStratifiedGenerationSettings>::Success(settings);
        }

        /** @copydoc DecodePoissonSettings */
        Result<PCGPoissonGenerationSettings> DecodePoissonSettings(const std::span<const std::uint8_t> payload) {
            const auto source = DecodeSource(payload, 28);
            if (source.HasError())
                return Result<PCGPoissonGenerationSettings>::Failure(source.ErrorValue());
            const PCGPoissonGenerationSettings settings{source.Value(), static_cast<std::uint32_t>(ReadWord(payload.subspan(12, 4))),
                                                        static_cast<std::uint32_t>(ReadWord(payload.subspan(16, 4))),
                                                        std::bit_cast<double>(ReadWord(payload.subspan(20, 8)))};
            if (!Valid(settings))
                return Result<PCGPoissonGenerationSettings>::Failure(MakeError(PCGErrors::CpuEvaluationInvalid));
            return Result<PCGPoissonGenerationSettings>::Success(settings);
        }
    }  // namespace detail
}  // namespace Horo::PCG
