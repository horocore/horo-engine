#include "Horo/Assets/AssetId.h"

#include "../AssetErrors.h"

#include <algorithm>
#include <string>

namespace Horo::Assets {
    namespace {
        [[nodiscard]] int HexValue(const char value) noexcept {
            if (value >= '0' && value <= '9')
                return value - '0';
            if (value >= 'a' && value <= 'f')
                return value - 'a' + 10;
            return -1;
        }
    }  // namespace

    /** @copydoc AssetId::Parse */
    Result<AssetId> AssetId::Parse(const std::string_view value) {
        if (value.size() != 36 || value[8] != '-' || value[13] != '-' || value[18] != '-' || value[23] != '-')
            return Result<AssetId>::Failure(MakeError(AssetErrors::IdentityInvalid));
        std::array<std::uint8_t, 16> bytes{};
        std::size_t byteIndex{};
        for (std::size_t index = 0; index < value.size();) {
            if (value[index] == '-') {
                ++index;
                continue;
            }
            const int high = HexValue(value[index++]);
            const int low = index < value.size() ? HexValue(value[index++]) : -1;
            if (high < 0 || low < 0 || byteIndex >= bytes.size())
                return Result<AssetId>::Failure(MakeError(AssetErrors::IdentityInvalid));
            bytes[byteIndex++] = static_cast<std::uint8_t>((high << 4) | low);
        }
        const AssetId parsed = FromBytes(bytes);
        if (byteIndex != bytes.size() || !parsed.IsValid())
            return Result<AssetId>::Failure(MakeError(AssetErrors::IdentityInvalid));
        return Result<AssetId>::Success(parsed);
    }

    /** @copydoc AssetId::IsValid */
    bool AssetId::IsValid() const noexcept {
        return std::ranges::any_of(bytes_, [](const std::uint8_t value) {
            return value != 0;
        });
    }

    /** @copydoc AssetId::ToString */
    std::string AssetId::ToString() const {
        constexpr char kHex[] = "0123456789abcdef";
        std::string result;
        result.reserve(36);
        for (std::size_t index = 0; index < bytes_.size(); ++index) {
            if (index == 4 || index == 6 || index == 8 || index == 10)
                result.push_back('-');
            result.push_back(kHex[static_cast<std::size_t>(bytes_[index]) >> 4U]);
            result.push_back(kHex[static_cast<std::size_t>(bytes_[index]) & 0x0fU]);
        }
        return result;
    }

    /** @copydoc AssetId::Bytes */
    const std::array<std::uint8_t, 16> &AssetId::Bytes() const noexcept {
        return bytes_;
    }

    /** @copydoc AssetIdHash::operator() */
    std::size_t AssetIdHash::operator()(const AssetId &value) const noexcept {
        std::size_t hash = sizeof(std::size_t) == 8 ? 1469598103934665603ULL : 2166136261U;
        for (const std::uint8_t byte : value.Bytes()) {
            hash ^= byte;
            hash *= sizeof(std::size_t) == 8 ? 1099511628211ULL : 16777619U;
        }
        return hash;
    }

    /** @copydoc AssetTypeId::AssetTypeId */
    AssetTypeId::AssetTypeId(AssetTypeId &&other) : AssetTypeId() {
        value_.swap(other.value_);
    }

    /** @copydoc AssetTypeId::Parse */
    Result<AssetTypeId> AssetTypeId::Parse(const std::string_view value) {
        if (value.empty() || value.size() > 96 || value.front() == '.' || value.back() == '.')
            return Result<AssetTypeId>::Failure(MakeError(AssetErrors::TypeInvalid));
        bool segmentStart = true;
        bool hasDot = false;
        for (const unsigned char character : value) {
            if (character == '.') {
                if (segmentStart)
                    return Result<AssetTypeId>::Failure(MakeError(AssetErrors::TypeInvalid));
                segmentStart = true;
                hasDot = true;
                continue;
            }
            const bool lower = character >= 'a' && character <= 'z';
            if (const bool digit = character >= '0' && character <= '9'; (!lower && !digit && character != '_') || (segmentStart && !lower))
                return Result<AssetTypeId>::Failure(MakeError(AssetErrors::TypeInvalid));
            segmentStart = false;
        }

        if (segmentStart || !hasDot)
            return Result<AssetTypeId>::Failure(MakeError(AssetErrors::TypeInvalid));
        return Result<AssetTypeId>::Success(AssetTypeId{value});
    }

    /** @copydoc AssetTypeId::Value */
    const std::string &AssetTypeId::Value() const noexcept {
        return value_;
    }
}  // namespace Horo::Assets
