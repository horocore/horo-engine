#include "Horo/Foundation/Sha256.h"

#include "../FoundationErrors.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace Horo {
    namespace {
        constexpr std::array<std::uint32_t, 64> RoundConstants{
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
            0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
            0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
            0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
            0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
        };

        /**
         * @brief Loads one SHA-256 message word from four bytes in network order.
         * @param bytes Pointer to at least four readable bytes.
         * @return Decoded 32-bit message word.
         */
        [[nodiscard]] constexpr std::uint32_t LoadBigEndian(const std::uint8_t *bytes) noexcept {
            return (static_cast<std::uint32_t>(bytes[0]) << 24U) | (static_cast<std::uint32_t>(bytes[1]) << 16U) |
                   (static_cast<std::uint32_t>(bytes[2]) << 8U) | static_cast<std::uint32_t>(bytes[3]);
        }

        /**
         * @brief Incorporates one complete SHA-256 message block into the chaining state.
         * @param block Pointer to exactly 64 readable message bytes.
         * @param state Chaining state updated in place.
         */
        void ProcessBlock(const std::uint8_t *block, std::array<std::uint32_t, 8> &state) noexcept {
            std::array<std::uint32_t, 64> words{};
            for (std::size_t index = 0; index < 16; ++index)
                words[index] = LoadBigEndian(block + index * 4);

            for (std::size_t index = 16; index < words.size(); ++index) {
                const std::uint32_t s0 = std::rotr(words[index - 15], 7) ^ std::rotr(words[index - 15], 18) ^ (words[index - 15] >> 3U);
                const std::uint32_t s1 = std::rotr(words[index - 2], 17) ^ std::rotr(words[index - 2], 19) ^ (words[index - 2] >> 10U);
                words[index] = words[index - 16] + s0 + words[index - 7] + s1;
            }

            std::uint32_t a = state[0];
            std::uint32_t b = state[1];
            std::uint32_t c = state[2];
            std::uint32_t d = state[3];
            std::uint32_t e = state[4];
            std::uint32_t f = state[5];
            std::uint32_t g = state[6];
            std::uint32_t h = state[7];

            for (std::size_t index = 0; index < words.size(); ++index) {
                const std::uint32_t sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
                const std::uint32_t choice = (e & f) ^ (~e & g);
                const std::uint32_t temporary1 = h + sum1 + choice + RoundConstants[index] + words[index];
                const std::uint32_t sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
                const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
                const std::uint32_t temporary2 = sum0 + majority;

                h = g;
                g = f;
                f = e;
                e = d + temporary1;
                d = c;
                c = b;
                b = a;
                a = temporary1 + temporary2;
            }

            state[0] += a;
            state[1] += b;
            state[2] += c;
            state[3] += d;
            state[4] += e;
            state[5] += f;
            state[6] += g;
            state[7] += h;
        }

        class Sha256Accumulator final {
        public:
            void Update(const std::span<const std::byte> input) noexcept {
                totalBytes_ += input.size();
                std::size_t offset = 0;
                while (offset < input.size()) {
                    const std::size_t copied = std::min(block_.size() - blockBytes_, input.size() - offset);
                    for (std::size_t index = 0; index < copied; ++index)
                        block_[blockBytes_ + index] = std::to_integer<std::uint8_t>(input[offset + index]);
                    blockBytes_ += copied;
                    offset += copied;
                    if (blockBytes_ == block_.size()) {
                        ProcessBlock(block_.data(), state_);
                        blockBytes_ = 0;
                    }
                }
            }

            [[nodiscard]] Sha256Digest Finalize() noexcept {
                block_[blockBytes_] = 0x80U;
                ++blockBytes_;
                if (blockBytes_ > 56) {
                    while (blockBytes_ < block_.size())
                        block_[blockBytes_++] = 0;
                    ProcessBlock(block_.data(), state_);
                    blockBytes_ = 0;
                }
                while (blockBytes_ < 56)
                    block_[blockBytes_++] = 0;
                const std::uint64_t bitLength = totalBytes_ * 8U;
                for (std::size_t index = 0; index < 8; ++index) {
                    const auto shift = static_cast<unsigned int>((7 - index) * 8);
                    block_[56 + index] = static_cast<std::uint8_t>(bitLength >> shift);
                }
                ProcessBlock(block_.data(), state_);

                Sha256Digest digest;
                for (std::size_t wordIndex = 0; wordIndex < state_.size(); ++wordIndex) {
                    for (std::size_t byteIndex = 0; byteIndex < 4; ++byteIndex) {
                        const auto shift = static_cast<unsigned int>((3 - byteIndex) * 8);
                        digest.bytes[wordIndex * 4 + byteIndex] = static_cast<std::uint8_t>(state_[wordIndex] >> shift);
                    }
                }
                return digest;
            }

        private:
            std::array<std::uint32_t, 8> state_{
                0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
            };
            std::array<std::uint8_t, 64> block_{};
            std::size_t blockBytes_{};
            std::uint64_t totalBytes_{};
        };

        /**
         * @brief Decodes one canonical lowercase hexadecimal digit.
         * @param character Character to decode.
         * @return Value in `[0, 15]`, or `-1` when @p character is noncanonical.
         */
        [[nodiscard]] constexpr int HexValue(const char character) noexcept {
            if (character >= '0' && character <= '9')
                return character - '0';
            if (character >= 'a' && character <= 'f')
                return character - 'a' + 10;
            return -1;
        }

        /**
         * @brief Creates the stable typed failure used for noncanonical SHA-256 text.
         * @return Failed digest result without embedding rejected input.
         */
        [[nodiscard]] Result<Sha256Digest> InvalidSha256Text() {
            return Result<Sha256Digest>::Failure(MakeError(HashingErrors::InvalidSha256Text, "SHA-256 text is not canonical."));
        }
    }  // namespace

    /** @copydoc ComputeSha256 */
    Sha256Digest ComputeSha256(const std::span<const std::byte> input) noexcept {
        const std::array fragments{input};
        return ComputeSha256Fragments(std::span<const std::span<const std::byte>>{fragments});
    }

    /** @copydoc ComputeSha256Fragments */
    Sha256Digest ComputeSha256Fragments(const std::span<const std::span<const std::byte>> inputs) noexcept {
        Sha256Accumulator accumulator;
        for (const auto input : inputs)
            accumulator.Update(input);
        return accumulator.Finalize();
    }

    /** @copydoc FormatSha256 */
    std::string FormatSha256(const Sha256Digest &digest) {
        constexpr std::string_view HexDigits = "0123456789abcdef";
        std::string text = "sha256:";
        text.reserve(71);
        for (const std::uint8_t byte : digest.bytes) {
            text.push_back(HexDigits[static_cast<std::size_t>(byte) >> 4U]);
            text.push_back(HexDigits[static_cast<std::size_t>(byte) & 0x0fU]);
        }
        return text;
    }

    /** @copydoc ParseSha256 */
    Result<Sha256Digest> ParseSha256(const std::string_view text) {
        constexpr std::string_view Prefix = "sha256:";
        if (constexpr std::size_t CanonicalLength = 71; text.size() != CanonicalLength || !text.starts_with(Prefix))
            return InvalidSha256Text();

        Sha256Digest digest;
        for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
            const int high = HexValue(text[Prefix.size() + index * 2]);
            const int low = HexValue(text[Prefix.size() + index * 2 + 1]);
            if (high < 0 || low < 0)
                return InvalidSha256Text();
            digest.bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
        }
        return Result<Sha256Digest>::Success(digest);
    }
}  // namespace Horo
