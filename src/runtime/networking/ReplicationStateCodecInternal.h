#pragma once

#include "Horo/Network/ReplicationStateCodec.h"

#include <algorithm>
#include <array>
#include <new>
#include <utility>

namespace Horo::Network::StateCodecDetail {
    inline constexpr std::size_t HeaderBytes = 172;
    inline constexpr std::size_t FieldHeaderBytes = 16;
    inline constexpr std::uint64_t Magic = 0x31535248;

    /** @brief Allocation-free outcome of a contained foreign-codec transaction. */
    enum class CodecFault {
        None,
        Capacity,
        Callback
    };

    /** @brief Contains all transaction exceptions without allocating diagnostic state.
     * @param operation Synchronous transaction retaining its result in caller-owned storage.
     * @return Fault category; the caller creates typed errors outside this noexcept boundary.
     */
    template <typename Operation> CodecFault ContainCodecFault(Operation operation) noexcept {
        using enum CodecFault;
        try {
            operation();
            return None;
        } catch (const std::bad_alloc &) {
            return Capacity;
        } catch (...) {
            return Callback;
        }
    }

    /** @brief Emits a fixed-width little-endian integer; callers have charged complete bounded storage. */
    inline void Append(std::vector<std::byte> &output, std::uint64_t value, const std::size_t width) {
        for (std::size_t index{}; index < width; ++index) {
            output.push_back(static_cast<std::byte>(value & 0xff));
            value >>= 8;
        }
    }

    /** @brief Bounds every wire read before advancing; framing does not allocate or invoke codecs. */
    struct Reader final {
        std::span<const std::byte> bytes;
        std::size_t offset{};

        [[nodiscard]] bool Read(const std::size_t width, std::uint64_t &value) noexcept {
            if (width > 8 || width > bytes.size() - offset)
                return false;
            value = 0;
            for (std::size_t index{}; index < width; ++index)
                value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[offset++])) << (index * 8);
            return true;
        }

        [[nodiscard]] bool Match(const std::uint64_t expected, const std::size_t width) noexcept {
            std::uint64_t actual{};
            return Read(width, actual) && actual == expected;
        }

        [[nodiscard]] bool Skip(const std::size_t width) noexcept {
            if (width > bytes.size() - offset)
                return false;
            offset += width;
            return true;
        }
    };

    /** @brief Rejects recursive codec entry and releases the guard on every return or exception. */
    struct OperationGuard final {
        bool &operating;

        explicit OperationGuard(bool &value) noexcept : operating(value) {
            value = true;
        }

        OperationGuard(const OperationGuard &) = delete;
        OperationGuard &operator=(const OperationGuard &) = delete;

        ~OperationGuard() {
            operating = false;
        }
    };

    /** @brief Returns a typed failure without copying private payload into diagnostic context. */
    template <typename T> Result<T> Fail(const ErrorCodeDescriptor &error) {
        return Result<T>::Failure(MakeError(error));
    }

    /** @brief Resolves one identity-sorted captured field without accessing the owner. */
    inline const ReplicationCapturedField *Find(const std::span<const ReplicationCapturedField> fields, const FieldId id) noexcept {
        const auto found = std::ranges::lower_bound(fields, id, {}, &ReplicationCapturedField::field);
        return found != fields.end() && found->field == id ? std::to_address(found) : nullptr;
    }

    /** @brief Defines the fixed identity wire order once for both emit and match operations. */
    inline std::array<std::pair<std::uint64_t, std::size_t>, 10> IdentityValues(const ReplicationRoleBinding &recipient,
                                                                                const std::uint64_t generation) noexcept {
        return {{{recipient.session.Value(), 8},
                 {generation, 8},
                 {recipient.revision.Value(), 8},
                 {recipient.localPeer->Value(), 8},
                 {recipient.object.Epoch().Value(), 8},
                 {recipient.object.Slot(), 8},
                 {recipient.object.Generation(), 4},
                 {recipient.schema.Value(), 8},
                 {recipient.schemaVersion.major, 2},
                 {recipient.schemaVersion.minor, 2}}};
    }

    /** @brief Emits exact negotiated identity independently of native representation. */
    inline void AppendIdentity(std::vector<std::byte> &output, const ReplicationRoleBinding &recipient, const std::uint64_t generation) {
        for (const auto &[value, width] : IdentityValues(recipient, generation))
            Append(output, value, width);
    }

    /** @brief Matches complete admitted identity before any field decoder runs. */
    inline bool MatchIdentity(Reader &reader, const ReplicationRoleBinding &recipient, const std::uint64_t generation) noexcept {
        return std::ranges::all_of(IdentityValues(recipient, generation), [&reader](const auto &item) {
            return reader.Match(item.first, item.second);
        });
    }

    /** @brief Emits a canonical fingerprint without native object-layout copies. */
    inline void AppendDigest(std::vector<std::byte> &output, const Sha256Digest &digest) {
        for (const auto value : digest.bytes)
            Append(output, value, 1);
    }

    /** @brief Matches the full schema/projection fingerprint before decode. */
    inline bool MatchDigest(Reader &reader, const Sha256Digest &digest) noexcept {
        return std::ranges::all_of(digest.bytes, [&reader](const auto value) {
            return reader.Match(value, 1);
        });
    }
}  // namespace Horo::Network::StateCodecDetail
