#include "Horo/PCG/PCGErrors.h"
#include "Horo/PCG/PCGPointGeneration.h"
#include "PCGPointTransformationInternal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <ranges>

namespace Horo::PCG {
    namespace {
        template <class T> Result<T> Invalid() {
            return Result<T>::Failure(MakeError(PCGErrors::CpuEvaluationInvalid));
        }

        /** @brief Reads one bounded network-order word after exact envelope validation. */
        std::uint64_t Word(std::span<const std::uint8_t> bytes) {
            std::uint64_t value{};
            for (auto byte : bytes)
                value = (value << 8) | byte;
            return value;
        }

        /** @brief Writes exactly the selected fixed network-order width. */
        void Word(std::span<std::uint8_t> bytes, std::uint64_t value) {
            for (std::size_t i = 0; i < bytes.size(); ++i)
                bytes[i] = static_cast<std::uint8_t>(value >> ((bytes.size() - i - 1) * 8));
        }

        /** @brief Canonical zero is positive; NaNs, infinities and negative zero are rejected when decoding. */
        bool Canonical(double value) {
            return std::isfinite(value) && !(value == 0 && std::signbit(value));
        }

        bool Frame(PCGPointCoordinateSpace space) {
            return space == PCGPointCoordinateSpace::Snapshot || space == PCGPointCoordinateSpace::PointLocal;
        }

        bool Valid(const PCGPointTransformSettings &s) {
            return Frame(s.space) && Math::IsFinite(s.delta.translation) && Math::IsFinite(s.delta.scale) &&
                   s.delta.rotation.TryNormalized().HasValue();
        }

        bool Valid(const PCGPointAlignmentSettings &s) {
            return std::isfinite(s.maximumVerticalDistance) && s.maximumVerticalDistance >= 0;
        }

        bool Valid(const PCGPointRandomOffsetSettings &s) {
            return Frame(s.space) && Math::IsFinite(s.halfExtent) && s.halfExtent.x >= 0 && s.halfExtent.y >= 0 && s.halfExtent.z >= 0;
        }

        /** @brief Noise targets are floating columns and cannot overwrite canonical sample identity. */
        bool Valid(const PCGPointNoiseSettings &s) {
            if (s.type < PCGAttributeType::Scalar || s.type > PCGAttributeType::Vector4 ||
                s.attribute.Value() == PCGSourceSampleAttribute ||
                std::ranges::find(PCGSourceRootAttributes, s.attribute.Value()) != PCGSourceRootAttributes.end())
                return false;
            return !s.attribute.Value().empty() && s.attribute.Value().size() <= MaximumAttributeKeyBytes && std::isfinite(s.frequency) &&
                   s.frequency > 0 && std::isfinite(s.amplitude) && s.amplitude >= 0 && s.amplitude <= std::numeric_limits<float>::max() &&
                   s.octaves >= 1 && s.octaves <= 16 && std::isfinite(s.persistence) && s.persistence >= 0 && s.persistence <= 1 &&
                   std::isfinite(s.lacunarity) && s.lacunarity >= 1 && s.lacunarity <= 8 &&
                   (s.normalization == PCGPointNoiseNormalization::Signed || s.normalization == PCGPointNoiseNormalization::UnitInterval);
        }

        /** @brief Allocates one bounded authored payload and translates allocation failure. */
        template <class Write> Result<std::vector<std::uint8_t>> Encode(bool valid, std::size_t size, Write write) {
            if (!valid)
                return Invalid<std::vector<std::uint8_t>>();
            try {
                std::vector<std::uint8_t> bytes(size);
                Word(std::span(bytes).first(4), 1);
                write(std::span(bytes));
                return Result<std::vector<std::uint8_t>>::Success(std::move(bytes));
            } catch (const std::bad_alloc &) {
                return Result<std::vector<std::uint8_t>>::Failure(MakeError(PCGErrors::CpuEvaluationCapacityExceeded));
            }
        }

        void Number(std::span<std::uint8_t> bytes, double value) {
            Word(bytes, std::bit_cast<std::uint64_t>(value == 0 ? 0.0 : value));
        }

        double Number(std::span<const std::uint8_t> bytes) {
            return std::bit_cast<double>(Word(bytes));
        }

        /** @brief Validates each float-valued field before narrowing from canonical binary64 storage. */
        bool FloatFields(std::span<const std::uint8_t> bytes, std::size_t count) {
            for (std::size_t i = 0; i < count; ++i) {
                const double value = Number(bytes.subspan(i * 8, 8));
                if (!Canonical(value) || std::abs(value) > std::numeric_limits<float>::max() ||
                    static_cast<double>(static_cast<float>(value)) != value)
                    return false;
            }
            return true;
        }

        Math::Vec3 Vector(std::span<const std::uint8_t> b) {
            return {static_cast<float>(Number(b.first(8))), static_cast<float>(Number(b.subspan(8, 8))),
                    static_cast<float>(Number(b.subspan(16, 8)))};
        }

        void Vector(std::span<std::uint8_t> b, Math::Vec3 v) {
            Number(b.first(8), v.x);
            Number(b.subspan(8, 8), v.y);
            Number(b.subspan(16, 8), v.z);
        }

        /** @brief Decodes fixed component settings after exact length and canonical float validation. */
        Result<detail::PointTransformationSettings> Transform(std::span<const std::uint8_t> b) {
            if (b.size() != 85 || !FloatFields(b.subspan(5), 10))
                return Invalid<detail::PointTransformationSettings>();
            PCGPointTransformSettings s;
            s.space = static_cast<PCGPointCoordinateSpace>(b[4]);
            s.delta.translation = Vector(b.subspan(5, 24));
            s.delta.scale = Vector(b.subspan(61, 24));
            s.delta.rotation = {static_cast<float>(Number(b.subspan(29, 8))), static_cast<float>(Number(b.subspan(37, 8))),
                                static_cast<float>(Number(b.subspan(45, 8))), static_cast<float>(Number(b.subspan(53, 8)))};
            if (!Valid(s))
                return Invalid<detail::PointTransformationSettings>();
            return Result<detail::PointTransformationSettings>::Success(s);
        }

        /** @brief Decodes bounded noise output metadata before reading its variable canonical key. */
        Result<detail::PointTransformationSettings> Noise(std::span<const std::uint8_t> b) {
            if (b.size() < 51 || b[50] > MaximumAttributeKeyBytes || b.size() != 51 + static_cast<std::size_t>(b[50]))
                return Invalid<detail::PointTransformationSettings>();
            const auto key = PCGAttributeKey::Create(std::string_view(reinterpret_cast<const char *>(b.data() + 51), b[50]));
            if (key.HasError())
                return Result<detail::PointTransformationSettings>::Failure(key.ErrorValue());
            PCGPointNoiseSettings s{key.Value()};
            s.type = static_cast<PCGAttributeType>(b[4]);
            s.normalization = static_cast<PCGPointNoiseNormalization>(b[5]);
            s.octaves = static_cast<std::uint32_t>(Word(b.subspan(6, 4)));
            s.salt = Word(b.subspan(10, 8));
            s.frequency = Number(b.subspan(18, 8));
            s.amplitude = Number(b.subspan(26, 8));
            s.persistence = Number(b.subspan(34, 8));
            s.lacunarity = Number(b.subspan(42, 8));
            if (!Canonical(s.frequency) || !Canonical(s.amplitude) || !Canonical(s.persistence) || !Canonical(s.lacunarity))
                return Invalid<detail::PointTransformationSettings>();
            if (!Valid(s))
                return Invalid<detail::PointTransformationSettings>();
            return Result<detail::PointTransformationSettings>::Success(std::move(s));
        }
    }  // namespace

    /** @copydoc EncodePCGPointTransformationSettings(const PCGPointTransformSettings&) */
    Result<std::vector<std::uint8_t>> EncodePCGPointTransformationSettings(const PCGPointTransformSettings &s) {
        return Encode(Valid(s), 85, [&](std::span<std::uint8_t> b) {
            b[4] = static_cast<std::uint8_t>(s.space);
            Vector(b.subspan(5, 24), s.delta.translation);
            Number(b.subspan(29, 8), s.delta.rotation.x);
            Number(b.subspan(37, 8), s.delta.rotation.y);
            Number(b.subspan(45, 8), s.delta.rotation.z);
            Number(b.subspan(53, 8), s.delta.rotation.w);
            Vector(b.subspan(61, 24), s.delta.scale);
        });
    }

    /** @copydoc EncodePCGPointTransformationSettings(const PCGPointAlignmentSettings&) */
    Result<std::vector<std::uint8_t>> EncodePCGPointTransformationSettings(const PCGPointAlignmentSettings &s) {
        return Encode(Valid(s), 12, [&](std::span<std::uint8_t> b) {
            Number(b.subspan(4, 8), s.maximumVerticalDistance);
        });
    }

    /** @copydoc EncodePCGPointTransformationSettings(const PCGPointRandomOffsetSettings&) */
    Result<std::vector<std::uint8_t>> EncodePCGPointTransformationSettings(const PCGPointRandomOffsetSettings &s) {
        return Encode(Valid(s), 37, [&](std::span<std::uint8_t> b) {
            b[4] = static_cast<std::uint8_t>(s.space);
            Vector(b.subspan(5, 24), s.halfExtent);
            Word(b.subspan(29, 8), s.salt);
        });
    }

    /** @copydoc EncodePCGPointTransformationSettings(const PCGPointNoiseSettings&) */
    Result<std::vector<std::uint8_t>> EncodePCGPointTransformationSettings(const PCGPointNoiseSettings &s) {
        return Encode(Valid(s), 51 + s.attribute.Value().size(), [&](std::span<std::uint8_t> b) {
            b[4] = static_cast<std::uint8_t>(s.type);
            b[5] = static_cast<std::uint8_t>(s.normalization);
            Word(b.subspan(6, 4), s.octaves);
            Word(b.subspan(10, 8), s.salt);
            Number(b.subspan(18, 8), s.frequency);
            Number(b.subspan(26, 8), s.amplitude);
            Number(b.subspan(34, 8), s.persistence);
            Number(b.subspan(42, 8), s.lacunarity);
            b[50] = static_cast<std::uint8_t>(s.attribute.Value().size());
            std::ranges::copy(s.attribute.Value(), b.begin() + 51);
        });
    }

    namespace detail {
        /** @copydoc IsPointTransformation */
        bool IsPointTransformation(PCGCpuNodeKind kind) noexcept {
            const auto k = static_cast<unsigned>(kind);
            return k >= 12 && k <= 16;
        }

        /** @copydoc DecodePointTransformation */
        Result<PointTransformationSettings> DecodePointTransformation(const PCGCookedNode &node) {
            const auto b = std::span(node.payload);
            if (b.size() < 4 || Word(b.first(4)) != 1)
                return Invalid<PointTransformationSettings>();
            try {
                if (node.type == PCGCpuNodeType(PCGCpuNodeKind::Transform).Value())
                    return Transform(b);
                if (node.type == PCGCpuNodeType(PCGCpuNodeKind::SurfaceAlignment).Value()) {
                    if (b.size() != 12)
                        return Invalid<PointTransformationSettings>();
                    const PCGPointAlignmentSettings s{Number(b.subspan(4, 8))};
                    if (!Valid(s) || !Canonical(s.maximumVerticalDistance))
                        return Invalid<PointTransformationSettings>();
                    return Result<PointTransformationSettings>::Success(s);
                }
                if (node.type == PCGCpuNodeType(PCGCpuNodeKind::RandomOffset).Value()) {
                    if (b.size() != 37 || !FloatFields(b.subspan(5, 24), 3))
                        return Invalid<PointTransformationSettings>();
                    const PCGPointRandomOffsetSettings s{Vector(b.subspan(5, 24)), static_cast<PCGPointCoordinateSpace>(b[4]),
                                                         Word(b.subspan(29, 8))};
                    if (!Valid(s))
                        return Invalid<PointTransformationSettings>();
                    return Result<PointTransformationSettings>::Success(s);
                }
                if (node.type == PCGCpuNodeType(PCGCpuNodeKind::PerlinNoise).Value() ||
                    node.type == PCGCpuNodeType(PCGCpuNodeKind::SimplexNoise).Value())
                    return Noise(b);
                return Result<PointTransformationSettings>::Failure(MakeError(PCGErrors::CpuEvaluationUnsupported));
            } catch (const std::bad_alloc &) {
                return Result<PointTransformationSettings>::Failure(MakeError(PCGErrors::CpuEvaluationCapacityExceeded));
            }
        }

        /** @copydoc ChargePointTransformation */
        Result<PCGNodeCost> ChargePointTransformation(const PointTransformationSettings &s, std::size_t points, std::size_t surfaces) {
            std::size_t multiplier = 4;
            if (const auto *noise = std::get_if<PCGPointNoiseSettings>(&s))
                multiplier += static_cast<std::size_t>(noise->octaves) * 4 * 8;
            const auto visits = CheckedPCGMultiply(points, multiplier);
            if (visits.HasError())
                return Result<PCGNodeCost>::Failure(visits.ErrorValue());
            const auto geometry = CheckedPCGMultiply(points, std::holds_alternative<PCGPointAlignmentSettings>(s) ? surfaces : 0);
            if (geometry.HasError())
                return Result<PCGNodeCost>::Failure(geometry.ErrorValue());
            const auto total = CheckedPCGAdd(visits.Value(), 1);
            if (total.HasError())
                return Result<PCGNodeCost>::Failure(total.ErrorValue());
            return Result<PCGNodeCost>::Success({total.Value(), geometry.Value(), 1024});
        }

        /** @copydoc ValidatePointTransformation */
        Result<void> ValidatePointTransformation(const PointTransformationSettings &s, const PCGPointOutputBound &input,
                                                 const PCGPointOutputBound &output) {
            if (input.schema->Tier() != output.schema->Tier() || input.maximumPoints > output.maximumPoints)
                return Invalid<void>();
            const auto *noise = std::get_if<PCGPointNoiseSettings>(&s);
            for (const auto &field : input.schema->Attributes()) {
                const auto *target = output.schema->Find(field.key.Value());
                if (target == nullptr || target->type != field.type)
                    return Invalid<void>();
            }
            if (noise == nullptr)
                return std::ranges::equal(input.schema->Attributes(), output.schema->Attributes()) ? Result<void>::Success()
                                                                                                   : Invalid<void>();
            const auto *target = output.schema->Find(noise->attribute.Value());
            if (target == nullptr || target->type != noise->type)
                return Invalid<void>();
            const bool existed = input.schema->Find(noise->attribute.Value()) != nullptr;
            if (output.schema->Attributes().size() != input.schema->Attributes().size() + (existed ? 0 : 1))
                return Invalid<void>();
            return Result<void>::Success();
        }
    }  // namespace detail
}  // namespace Horo::PCG
