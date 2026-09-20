#pragma once

#include "Horo/Navigation/NavigationDataSerialization.h"
#include "Horo/Navigation/NavigationErrors.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>

namespace Horo::Navigation::SerializationInternal {
    inline constexpr std::array<std::uint8_t, 4> EnvelopeMagic{'H', 'N', 'A', 'V'};
    inline constexpr std::size_t EnvelopeHeaderBytes = 28;
    inline constexpr std::size_t AuthoredRecordHeaderBytes = 26;
    inline constexpr std::size_t GeneratedPayloadHeaderBytes = 18;
    inline constexpr std::size_t ChecksumBytes = 32;
    inline constexpr std::uint8_t RequiredRecordFlag = 0x01U;
    inline constexpr std::uint8_t OpaqueRecordFlag = 0x02U;
    inline constexpr std::uint8_t QuarantinedGeneratedFlag = 0x01U;
    inline constexpr std::uint8_t UnsupportedGeneratedVersionFlag = 0x02U;

    template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
        return Result<T>::Failure(MakeError(descriptor));
    }

    [[nodiscard]] constexpr bool IsSupportedEnvelopeVersion(const NavigationSourceSchemaVersion version) noexcept {
        return version.major == CurrentNavigationSourceSchemaVersion.major && version.minor <= CurrentNavigationSourceSchemaVersion.minor;
    }

    [[nodiscard]] constexpr bool IsValidRecordVersion(const NavigationSourceSchemaVersion version) noexcept {
        return version.major != 0;
    }

    [[nodiscard]] inline bool IsValidLimits(const NavigationSerializationLimits &limits) noexcept {
        return limits.maximumEnvelopeBytes >= EnvelopeHeaderBytes + ChecksumBytes &&
               limits.maximumEnvelopeBytes <= NavigationSerializationLimits::MaximumEnvelopeBytes &&
               limits.maximumAuthoredRecords <= NavigationSerializationLimits::MaximumAuthoredRecords &&
               limits.maximumGeneratedPayloads <= NavigationSerializationLimits::MaximumGeneratedPayloads &&
               limits.maximumRecordPayloadBytes <= NavigationSerializationLimits::MaximumRecordPayloadBytes &&
               limits.maximumGeneratedPayloadBytes <= NavigationSerializationLimits::MaximumGeneratedPayloadBytes &&
               limits.maximumTotalPayloadBytes <= NavigationSerializationLimits::MaximumTotalPayloadBytes;
    }

    [[nodiscard]] inline bool IsValidSupportTable(const std::span<const NavigationAuthoredRecordSupport> supports) noexcept {
        for (std::size_t index = 0; index < supports.size(); ++index) {
            const auto &support = supports[index];
            if (!support.type.IsValid() || !IsValidRecordVersion(support.minimum) || !IsValidRecordVersion(support.maximum) ||
                support.minimum > support.maximum)
                return false;
            for (std::size_t previous = 0; previous < index; ++previous) {
                if (supports[previous].type == support.type)
                    return false;
            }
        }
        return true;
    }

    [[nodiscard]] constexpr bool IsValidUnknownRecordPolicy(const NavigationUnknownRecordPolicy policy) noexcept {
        return static_cast<std::uint8_t>(policy) < static_cast<std::uint8_t>(NavigationUnknownRecordPolicy::Count);
    }

    [[nodiscard]] inline const NavigationAuthoredRecordSupport *FindSupport(const std::span<const NavigationAuthoredRecordSupport> supports,
                                                                            const NavigationAuthoredRecordTypeId type) noexcept {
        for (const auto &support : supports) {
            if (support.type == type)
                return &support;
        }
        return nullptr;
    }

    [[nodiscard]] inline bool IsSupported(const NavigationAuthoredRecordSupport *support,
                                          const NavigationSourceSchemaVersion version) noexcept {
        return support != nullptr && version >= support->minimum && version <= support->maximum;
    }

    [[nodiscard]] inline bool CheckedAdd(const std::size_t left, const std::size_t right, std::size_t &result) noexcept {
        if (right > std::numeric_limits<std::size_t>::max() - left)
            return false;
        result = left + right;
        return true;
    }

    [[nodiscard]] inline bool PayloadLess(const std::vector<std::byte> &left, const std::vector<std::byte> &right) noexcept {
        return std::ranges::lexicographical_compare(left, right, [](const std::byte lhs, const std::byte rhs) {
            return std::to_integer<std::uint8_t>(lhs) < std::to_integer<std::uint8_t>(rhs);
        });
    }

    [[nodiscard]] inline bool IsCanonicalGeneratedOrder(const NavigationGeneratedPayload &left,
                                                        const NavigationGeneratedPayload &right) noexcept {
        if (left.type != right.type)
            return left.type.Value() < right.type.Value();
        if (left.version != right.version)
            return left.version < right.version;
        return PayloadLess(left.payload, right.payload);
    }

    [[nodiscard]] inline bool QuarantineLess(const NavigationQuarantinedGeneratedPayload &left,
                                             const NavigationQuarantinedGeneratedPayload &right) noexcept {
        if (IsCanonicalGeneratedOrder(left.payload, right.payload))
            return true;
        if (IsCanonicalGeneratedOrder(right.payload, left.payload))
            return false;
        return left.reason < right.reason;
    }

    [[nodiscard]] constexpr std::uint8_t QuarantineFlag(const NavigationGeneratedPayloadQuarantineReason reason) noexcept {
        return reason == NavigationGeneratedPayloadQuarantineReason::UnknownType ? QuarantinedGeneratedFlag
                                                                                 : UnsupportedGeneratedVersionFlag;
    }

    [[nodiscard]] inline Result<void> ValidateContext(const NavigationSourceLoadContext &context) {
        if (!IsValidLimits(context.limits) || !IsValidSupportTable(context.supportedRecords) ||
            !IsValidSupportTable(context.supportedGeneratedPayloads) || !IsValidUnknownRecordPolicy(context.unknownPolicy))
            return Result<void>::Failure(MakeError(NavigationErrors::SourceEnvelopeInvalid));
        return Result<void>::Success();
    }
}  // namespace Horo::Navigation::SerializationInternal
