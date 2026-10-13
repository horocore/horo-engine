#include "Horo/PCG/PCGPointFiltering.h"

#include "Horo/PCG/PCGCookedPlan.h"
#include "Horo/PCG/PCGErrors.h"
#include "PCGPointFilteringInternal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <new>
#include <type_traits>

namespace Horo::PCG {
    namespace {
        /** @brief Preserves the evaluator invalid-settings error at the codec boundary. */
        template <class T> Result<T> Invalid() {
            return Result<T>::Failure(MakeError(PCGErrors::CpuEvaluationInvalid));
        }

        /** @brief Rejects inverted, nonfinite and out-of-domain authored intervals. */
        bool Range(double low, double high, double ceiling) {
            return std::isfinite(low) && std::isfinite(high) && low >= 0.0 && low <= high && high <= ceiling;
        }

        /** @brief Checks every supported vector component without coercing its schema. */
        template <class V> bool FiniteVector(const V &v) {
            bool valid = std::isfinite(v.x) && std::isfinite(v.y);
            if constexpr (requires { v.z; })
                valid = valid && std::isfinite(v.z);
            if constexpr (requires { v.w; })
                valid = valid && std::isfinite(v.w);
            return valid;
        }

        /** @brief Admits ordering only for exact numeric values and equality for finite Boolean/vector values. */
        template <class V> bool ValidAttributeValue(const V &value, PCGAttributeComparison comparison) {
            if constexpr (std::is_same_v<V, double>)
                return std::isfinite(value);
            else if constexpr (std::is_same_v<V, std::int64_t> || std::is_same_v<V, std::uint64_t>)
                return true;
            else {
                if (comparison != PCGAttributeComparison::Equal && comparison != PCGAttributeComparison::NotEqual)
                    return false;
                if constexpr (std::is_same_v<V, bool>)
                    return true;
                else
                    return FiniteVector(value);
            }
        }

        /** @brief Checks the attribute key and closed comparison vocabulary before visiting its exact value. */
        bool ValidAttribute(const PCGAttributeFilterSettings &settings) {
            if (settings.key.Value().empty() || settings.comparison > PCGAttributeComparison::GreaterEqual)
                return false;
            return std::visit([&]<class V>(const V &value) {
                return ValidAttributeValue(value, settings.comparison);
            }, settings.value);
        }

        /** @brief Validates the closed typed settings vocabulary before canonical encoding. */
        bool Valid(const PCGPointFilterSettings &settings) {
            return std::visit([]<class T>(const T &s) {
                if constexpr (std::is_same_v<T, PCGDensityRangeFilterSettings>)
                    return Range(s.minimum, s.maximum, 1.0);
                else if constexpr (std::is_same_v<T, PCGSlopeFilterSettings>)
                    return Range(s.minimumDegrees, s.maximumDegrees, 180.0);
                else if constexpr (std::is_same_v<T, PCGDistanceFilterSettings>)
                    return std::isfinite(s.minimumDistance) && s.minimumDistance >= 0.0;
                else if constexpr (std::is_same_v<T, PCGBoundsFilterSettings>)
                    return FiniteVector(s.bounds.minimum) && FiniteVector(s.bounds.maximum) && s.bounds.minimum.x <= s.bounds.maximum.x &&
                           s.bounds.minimum.y <= s.bounds.maximum.y && s.bounds.minimum.z <= s.bounds.maximum.z;
                else
                    return ValidAttribute(s);
            }, settings);
        }

        /** @brief Appends an exact network-order word to a bounded settings payload. */
        void Word(std::vector<std::uint8_t> &bytes, std::uint64_t value, std::size_t width = 8) {
            for (std::size_t i = width; i != 0; --i)
                bytes.push_back(static_cast<std::uint8_t>(value >> ((i - 1) * 8)));
        }

        /** @brief Canonicalizes Binary64 signed zero while preserving all other finite bits. */
        void Number(std::vector<std::uint8_t> &bytes, double value) {
            Word(bytes, std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value));
        }

        /** @brief Canonicalizes Float32 signed zero at its exact representation width. */
        void Component(std::vector<std::uint8_t> &bytes, float value) {
            Word(bytes, std::bit_cast<std::uint32_t>(value == 0.0F ? 0.0F : value), 4);
        }

        /** @brief Writes components in semantic axis order without structure padding. */
        template <class V> void Vector(std::vector<std::uint8_t> &bytes, const V &v) {
            Component(bytes, v.x);
            Component(bytes, v.y);
            if constexpr (requires { v.z; })
                Component(bytes, v.z);
            if constexpr (requires { v.w; })
                Component(bytes, v.w);
        }

        /** @brief Encodes exact attribute words without coercion or changing the canonical value tag. */
        template <class V> void AppendAttributeValue(std::vector<std::uint8_t> &bytes, const V &value) {
            if constexpr (std::is_same_v<V, bool>)
                Word(bytes, value ? 1 : 0, 1);
            else if constexpr (std::is_same_v<V, std::int64_t>)
                Word(bytes, std::bit_cast<std::uint64_t>(value));
            else if constexpr (std::is_same_v<V, std::uint64_t>)
                Word(bytes, value);
            else if constexpr (std::is_same_v<V, double>)
                Number(bytes, value);
            else
                Vector(bytes, value);
        }

        /** @brief Writes the bounded key, comparison and exact value in version-one field order. */
        void AppendAttribute(std::vector<std::uint8_t> &bytes, const PCGAttributeFilterSettings &settings) {
            Word(bytes, settings.key.Value().size(), 1);
            for (const char ch : settings.key.Value())
                bytes.push_back(std::bit_cast<std::uint8_t>(ch));
            Word(bytes, static_cast<std::uint8_t>(settings.comparison), 1);
            Word(bytes, settings.value.index(), 1);
            std::visit([&]<class V>(const V &value) {
                AppendAttributeValue(bytes, value);
            }, settings.value);
        }

        /** @brief Writes each admitted filter's canonical fields into the caller-owned payload. */
        template <class T> void AppendSettings(std::vector<std::uint8_t> &bytes, const T &settings) {
            if constexpr (std::is_same_v<T, PCGDensityRangeFilterSettings>) {
                Number(bytes, settings.minimum);
                Number(bytes, settings.maximum);
            } else if constexpr (std::is_same_v<T, PCGSlopeFilterSettings>) {
                Number(bytes, settings.minimumDegrees);
                Number(bytes, settings.maximumDegrees);
            } else if constexpr (std::is_same_v<T, PCGDistanceFilterSettings>)
                Number(bytes, settings.minimumDistance);
            else if constexpr (std::is_same_v<T, PCGBoundsFilterSettings>) {
                Vector(bytes, settings.bounds.minimum);
                Vector(bytes, settings.bounds.maximum);
            } else
                AppendAttribute(bytes, settings);
        }

        /** @brief Reads only checked fixed-width fields; truncated envelopes remain invalid. */
        struct Reader final {
            std::span<const std::uint8_t> bytes;
            std::size_t offset{};
            bool valid{true};

            std::uint64_t Word(std::size_t width = 8) {
                if (width > bytes.size() - offset) {
                    valid = false;
                    return 0;
                }
                std::uint64_t result{};
                for (std::size_t i = 0; i < width; ++i)
                    result = (result << 8) | bytes[offset++];
                return result;
            }

            double Number() {
                return std::bit_cast<double>(Word());
            }

            float Component() {
                return std::bit_cast<float>(static_cast<std::uint32_t>(Word(4)));
            }

            Math::Vec3 Vec3() {
                const float x = Component();
                const float y = Component();
                const float z = Component();
                return {x, y, z};
            }
        };

        /** @brief Decodes the exact admitted value tag without numeric or vector coercion. */
        Result<PCGAttributePredicateValue> AttributeValue(Reader &reader, std::uint64_t type) {
            PCGAttributePredicateValue value;
            switch (type) {
                case 0: {
                    const auto boolean = reader.Word(1);
                    if (boolean > 1)
                        return Invalid<PCGAttributePredicateValue>();
                    value = boolean != 0;
                    break;
                }
                case 1:
                    value = std::bit_cast<std::int64_t>(reader.Word());
                    break;
                case 2:
                    value = reader.Word();
                    break;
                case 3:
                    value = reader.Number();
                    break;
                case 4: {
                    const float x = reader.Component();
                    const float y = reader.Component();
                    value = Math::Vec2{x, y};
                    break;
                }
                case 5:
                    value = reader.Vec3();
                    break;
                case 6: {
                    const auto v = reader.Vec3();
                    const float w = reader.Component();
                    value = Math::Vec4{v.x, v.y, v.z, w};
                    break;
                }
                default:
                    return Invalid<PCGAttributePredicateValue>();
            }
            return Result<PCGAttributePredicateValue>::Success(value);
        }

        /** @brief Decodes a bounded canonical key and exact value tag; unknown fields fail. */
        Result<PCGAttributeFilterSettings> Attribute(Reader &reader) {
            const auto length = reader.Word(1);
            if (length == 0 || length > 96 || length > reader.bytes.size() - reader.offset)
                return Invalid<PCGAttributeFilterSettings>();
            std::string text;
            text.reserve(length);
            for (std::size_t i = 0; i < length; ++i)
                text.push_back(std::bit_cast<char>(reader.bytes[reader.offset++]));
            const auto key = PCGAttributeKey::Create(text);
            if (key.HasError())
                return Result<PCGAttributeFilterSettings>::Failure(key.ErrorValue());
            const auto op = reader.Word(1);
            const auto type = reader.Word(1);
            if (op > static_cast<unsigned>(PCGAttributeComparison::GreaterEqual) || type > 6)
                return Invalid<PCGAttributeFilterSettings>();
            const auto value = AttributeValue(reader, type);
            if (value.HasError())
                return Result<PCGAttributeFilterSettings>::Failure(value.ErrorValue());
            return Result<PCGAttributeFilterSettings>::Success({key.Value(), static_cast<PCGAttributeComparison>(op), value.Value()});
        }
    }  // namespace

    /** @copydoc EncodePCGPointFilterSettings */
    Result<std::vector<std::uint8_t>> EncodePCGPointFilterSettings(const PCGPointFilterSettings &settings) {
        if (!Valid(settings))
            return Invalid<std::vector<std::uint8_t>>();
        try {
            std::vector<std::uint8_t> bytes;
            bytes.reserve(128);
            Word(bytes, 1, 4);
            std::visit([&]<class T>(const T &s) {
                AppendSettings(bytes, s);
            }, settings);
            return Result<std::vector<std::uint8_t>>::Success(std::move(bytes));
        } catch (const std::bad_alloc &) {
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(PCGErrors::CpuEvaluationCapacityExceeded));
        }
    }

    namespace detail {
        /** @copydoc IsPointFilter */
        bool IsPointFilter(PCGCpuNodeKind kind) noexcept {
            return kind >= PCGCpuNodeKind::DensityRangeFilter && kind <= PCGCpuNodeKind::AttributeFilter;
        }

        /** @copydoc DecodePointFilter */
        Result<PCGPointFilterSettings> DecodePointFilter(const PCGCookedNode &node) {
            try {
                Reader reader{node.payload};
                if (reader.Word(4) != 1)
                    return Invalid<PCGPointFilterSettings>();
                PCGPointFilterSettings settings;
                if (const auto type = [&](PCGCpuNodeKind kind) {
                    return node.type == PCGCpuNodeType(kind).Value();
                }; type(PCGCpuNodeKind::DensityRangeFilter)) {
                    const double low = reader.Number();
                    const double high = reader.Number();
                    settings = PCGDensityRangeFilterSettings{low, high};
                } else if (type(PCGCpuNodeKind::BoundsFilter)) {
                    const auto low = reader.Vec3();
                    const auto high = reader.Vec3();
                    settings = PCGBoundsFilterSettings{{low, high}};
                } else if (type(PCGCpuNodeKind::SlopeFilter)) {
                    const double low = reader.Number();
                    const double high = reader.Number();
                    settings = PCGSlopeFilterSettings{low, high};
                } else if (type(PCGCpuNodeKind::DistanceFilter))
                    settings = PCGDistanceFilterSettings{reader.Number()};
                else if (type(PCGCpuNodeKind::AttributeFilter)) {
                    const auto decoded = Attribute(reader);
                    if (decoded.HasError())
                        return Result<PCGPointFilterSettings>::Failure(decoded.ErrorValue());
                    settings = decoded.Value();
                } else
                    return Invalid<PCGPointFilterSettings>();
                if (!reader.valid || reader.offset != reader.bytes.size())
                    return Invalid<PCGPointFilterSettings>();
                const auto canonical = EncodePCGPointFilterSettings(settings);
                if (canonical.HasError())
                    return Result<PCGPointFilterSettings>::Failure(canonical.ErrorValue());
                if (!std::ranges::equal(canonical.Value(), node.payload))
                    return Invalid<PCGPointFilterSettings>();
                return Result<PCGPointFilterSettings>::Success(std::move(settings));
            } catch (const std::bad_alloc &) {
                return Result<PCGPointFilterSettings>::Failure(MakeError(PCGErrors::CpuEvaluationCapacityExceeded));
            }
        }
    }  // namespace detail
}  // namespace Horo::PCG
