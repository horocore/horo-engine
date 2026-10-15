#include "Horo/Runtime/Save/SaveIdentity.h"

#include <algorithm>
#include <charconv>
#include <optional>

namespace Horo::Runtime {
    namespace {
        [[nodiscard]] bool IsCanonicalParticipantCharacter(const unsigned char character) noexcept {
            return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_';
        }

        /** @brief Reports whether text has the fixed canonical UUID layout. */
        [[nodiscard]] bool HasCanonicalUuidLayout(const std::string_view text) noexcept {
            return text.size() == 36 && text[8] == '-' && text[13] == '-' && text[18] == '-' && text[23] == '-';
        }

        /** @brief Parses exactly two lowercase hexadecimal characters into one byte. */
        [[nodiscard]] std::optional<std::uint8_t> ParseCanonicalByte(const std::string_view text) noexcept {
            if (text.size() != 2 || !std::ranges::all_of(text, [](const char character) {
                return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
            }))
                return std::nullopt;

            unsigned int parsedByte{};
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsedByte, 16);
            if (error != std::errc{} || end != text.data() + text.size())
                return std::nullopt;
            return static_cast<std::uint8_t>(parsedByte);
        }

        /** @brief Reports whether one participant-ID segment is canonical. */
        [[nodiscard]] bool IsCanonicalParticipantSegment(const std::string_view segment) noexcept {
            return !segment.empty() && segment.front() >= 'a' && segment.front() <= 'z' &&
                   std::ranges::all_of(segment, IsCanonicalParticipantCharacter);
        }

        template <typename Range> [[nodiscard]] std::uint64_t StableHash64(const Range &bytes) noexcept {
            std::uint64_t hash = 14695981039346656037ULL;
            for (const auto byte : bytes) {
                hash ^= static_cast<unsigned char>(byte);
                hash *= 1099511628211ULL;
            }
            return hash;
        }
    }  // namespace

    namespace SaveIdentityDetail {
        /** @copydoc ParseUuid */
        Result<Bytes> ParseUuid(const std::string_view text) {
            if (!HasCanonicalUuidLayout(text))
                return Result<Bytes>::Failure(MakeError(SaveErrors::IdentityMalformed));

            std::array<char, 32> compactText{};
            if (const auto compactEnd = std::ranges::copy_if(text, compactText.begin(),
                                                             [](const char character) {
                return character != '-';
            }).out;
                compactEnd != compactText.end())
                return Result<Bytes>::Failure(MakeError(SaveErrors::IdentityMalformed));

            Bytes bytes{};
            const std::string_view compactView{compactText.data(), compactText.size()};
            for (std::size_t byteIndex = 0; byteIndex < bytes.size(); ++byteIndex) {
                const auto parsedByte = ParseCanonicalByte(compactView.substr(byteIndex * 2, 2));
                if (!parsedByte.has_value())
                    return Result<Bytes>::Failure(MakeError(SaveErrors::IdentityMalformed));
                bytes[byteIndex] = *parsedByte;
            }
            return ValidateUuid(bytes);
        }

        /** @copydoc ValidateUuid */
        Result<Bytes> ValidateUuid(Bytes bytes) {
            if (std::ranges::none_of(bytes, [](const std::uint8_t value) {
                return value != 0;
            }))
                return Result<Bytes>::Failure(MakeError(SaveErrors::IdentityInvalid));
            return Result<Bytes>::Success(bytes);
        }

        /** @copydoc FormatUuid */
        std::string FormatUuid(const Bytes &bytes) {
            constexpr char kHex[] = "0123456789abcdef";
            std::array<char, 36> text{};
            std::size_t position{};
            for (std::size_t index = 0; index < bytes.size(); ++index) {
                if (index == 4 || index == 6 || index == 8 || index == 10)
                    text[position++] = '-';
                text[position++] = kHex[static_cast<std::size_t>(bytes[index]) >> 4U];
                text[position++] = kHex[static_cast<std::size_t>(bytes[index]) & 0x0fU];
            }
            // MSVC Debug's noexcept default string constructor allocates its iterator proxy.
            // Use the throwing constructor so allocation failure reaches the owner's recovery boundary.
            return std::string{text.data(), text.size()};
        }

        /** @copydoc HashUuid */
        std::uint64_t HashUuid(const Bytes &bytes) noexcept {
            return StableHash64(bytes);
        }
    }  // namespace SaveIdentityDetail

    /** @copydoc SaveParticipantId::Parse */
    Result<SaveParticipantId> SaveParticipantId::Parse(const std::string_view value) {
        if (value.empty() || value.size() > MaximumSaveParticipantIdBytes || value.find('.') == std::string_view::npos)
            return Result<SaveParticipantId>::Failure(MakeError(SaveErrors::ParticipantIdInvalid));

        std::size_t segmentBegin{};
        while (segmentBegin <= value.size()) {
            const std::size_t segmentEnd = value.find('.', segmentBegin);
            if (const std::string_view segment = value.substr(segmentBegin, segmentEnd - segmentBegin);
                !IsCanonicalParticipantSegment(segment))
                return Result<SaveParticipantId>::Failure(MakeError(SaveErrors::ParticipantIdInvalid));
            if (segmentEnd == std::string_view::npos)
                break;
            segmentBegin = segmentEnd + 1;
        }
        return Result<SaveParticipantId>::Success(SaveParticipantId{std::string{value}});
    }

    /** @copydoc SaveParticipantId::Value */
    const std::string &SaveParticipantId::Value() const noexcept {
        return value_;
    }

    /** @copydoc SaveParticipantId::IsValid */
    bool SaveParticipantId::IsValid() const noexcept {
        return !value_.empty();
    }

    /** @copydoc SaveParticipantIdHash::operator() */
    std::size_t SaveParticipantIdHash::operator()(const SaveParticipantId &value) const noexcept {
        return static_cast<std::size_t>(StableHash64(value.Value()));
    }
}  // namespace Horo::Runtime
