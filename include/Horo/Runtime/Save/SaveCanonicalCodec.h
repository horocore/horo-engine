#pragma once

/**
 * @file SaveCanonicalCodec.h
 * @brief Bounded deterministic runtime-save value codecs.
 */
#include "Horo/Foundation/Result.h"
#include "Horo/Math/SceneMath.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::Runtime {
    class CanonicalValueReader;
    struct CanonicalReadState;
    struct CanonicalPathNode;

    /** @brief Stable nonzero numeric record-field identity. */
    class CanonicalFieldId final {
    public:
        using ValueType = std::uint32_t;
        /** @brief Creates an identity. @param value Nonzero schema value. @return Identity or typed error. */
        [[nodiscard]] static Result<CanonicalFieldId> Create(ValueType value);

        /** @brief Returns the stable numeric identity. @return Nonzero schema value. */
        [[nodiscard]] constexpr ValueType Value() const noexcept {
            return value_;
        }

        [[nodiscard]] constexpr auto operator<=>(const CanonicalFieldId &) const noexcept = default;

    private:
        explicit constexpr CanonicalFieldId(ValueType value) noexcept : value_(value) {}

        ValueType value_{};
    };

    /** @brief Trusted wire, decoded-memory, collection, field, and structural-depth limits. */
    struct CanonicalCodecLimits final {
        std::size_t maximumBytes{16 * 1024 * 1024};         /**< Maximum encoded root or child byte count. */
        std::size_t maximumDecodedBytes{32 * 1024 * 1024};  /**< Shared owned-memory admission budget. */
        std::size_t maximumStringBytes{1024 * 1024};        /**< Maximum encoded UTF-8 byte count. */
        std::size_t maximumCollectionElements{1024 * 1024}; /**< Maximum sequence, map, or set count. */
        std::size_t maximumFields{4096};                    /**< Maximum record field count. */
        std::size_t maximumNestingDepth{32};                /**< Maximum composite nodes along any path. */
        std::size_t maximumReadWorkBytes{64 * 1024 * 1024}; /**< Cumulative bytes inspected by a root and every reopened child. */
    };

    /** @brief Sealed canonical bytes with proven structural depth. */
    class CanonicalEncodedValue final {
    public:
        /** @brief Returns the immutable canonical representation. @return Borrowed bytes valid for this value's lifetime. */
        [[nodiscard]] std::span<const std::byte> Bytes() const noexcept {
            return bytes_;
        }

        /** @brief Returns the proven maximum composite depth. @return Zero for scalar-only bytes. */
        [[nodiscard]] std::size_t StructuralDepth() const noexcept {
            return structuralDepth_;
        }

    private:
        friend class CanonicalValueWriter;

        CanonicalEncodedValue(std::vector<std::byte> bytes, std::size_t depth) : bytes_(std::move(bytes)), structuralDepth_(depth) {}

        std::vector<std::byte> bytes_;
        std::size_t structuralDepth_{};
    };

    /** @brief Opaque borrowed child preserving root depth and shared decoded-memory budget. */
    class CanonicalDecodedValue final {
    public:
        /** @brief Opens a reader preserving depth, field path, and root memory budget. @return Child reader or allocation failure. */
        [[nodiscard]] Result<CanonicalValueReader> OpenReader() const;

    private:
        friend class CanonicalValueReader;

        CanonicalDecodedValue(std::span<const std::byte> bytes, const CanonicalCodecLimits &limits,
                              std::shared_ptr<CanonicalReadState> state, std::size_t depth, std::shared_ptr<const CanonicalPathNode> path)
            : bytes_(bytes), limits_(limits), state_(std::move(state)), depth_(depth), path_(std::move(path)) {}

        std::span<const std::byte> bytes_;
        CanonicalCodecLimits limits_;
        std::shared_ptr<CanonicalReadState> state_;
        std::size_t depth_{};
        std::shared_ptr<const CanonicalPathNode> path_;
    };

    /** @brief One sealed field supplied to deterministic record encoding. */
    struct CanonicalRecordField final {
        CanonicalFieldId id;         /**< Stable schema field identity. */
        CanonicalEncodedValue value; /**< Sealed canonical field value. */
    };

    /** @brief One sealed key/value pair supplied to deterministic map encoding. */
    struct CanonicalMapEntry final {
        CanonicalEncodedValue key;   /**< Sealed canonical key. */
        CanonicalEncodedValue value; /**< Sealed canonical mapped value. */
    };

    /** @brief One borrowed decoded record field. */
    struct CanonicalDecodedField final {
        CanonicalFieldId id;         /**< Stable decoded schema identity. */
        CanonicalDecodedValue value; /**< Borrowed opaque field value. */
    };

    /** @brief One borrowed decoded map entry. */
    struct CanonicalDecodedMapEntry final {
        CanonicalDecodedValue key;   /**< Borrowed opaque key. */
        CanonicalDecodedValue value; /**< Borrowed opaque mapped value. */
    };

    /** @brief Sticky-failure writer for explicitly approved canonical types. */
    class CanonicalValueWriter final {
    public:
        /** @brief Creates an empty sticky-failure writer. @param limits Trusted nonzero codec bounds. */
        explicit CanonicalValueWriter(const CanonicalCodecLimits &limits = {});
        /** @brief Creates a writer whose diagnostics append one field identity. @param field Stable field identity. @return Child writer
         * or typed configuration/allocation failure. */
        [[nodiscard]] Result<CanonicalValueWriter> ForField(CanonicalFieldId field) const;
        /** @brief Writes a canonical zero-or-one boolean. @param value Value to append. @return Success or typed failure. */
        [[nodiscard]] Result<void> WriteBool(bool value);
        /** @brief Writes an unsigned fixed-width scalar. @param value Value to append. @return Success or typed failure. */
        [[nodiscard]] Result<void> WriteUInt8(std::uint8_t value);
        /** @copydoc WriteUInt8 */
        [[nodiscard]] Result<void> WriteUInt16(std::uint16_t value);
        /** @copydoc WriteUInt8 */
        [[nodiscard]] Result<void> WriteUInt32(std::uint32_t value);
        /** @copydoc WriteUInt8 */
        [[nodiscard]] Result<void> WriteUInt64(std::uint64_t value);
        /** @brief Writes a two's-complement fixed-width scalar. @param value Value to append. @return Success or typed failure. */
        [[nodiscard]] Result<void> WriteInt8(std::int8_t value);
        /** @copydoc WriteInt8 */
        [[nodiscard]] Result<void> WriteInt16(std::int16_t value);
        /** @copydoc WriteInt8 */
        [[nodiscard]] Result<void> WriteInt32(std::int32_t value);
        /** @copydoc WriteInt8 */
        [[nodiscard]] Result<void> WriteInt64(std::int64_t value);
        /** @brief Writes finite binary32 and canonicalizes negative zero. @param value Value to append. @return Success or typed failure.
         */
        [[nodiscard]] Result<void> WriteFloat32(float value);
        /** @brief Writes finite binary64 and canonicalizes negative zero. @param value Value to append. @return Success or typed failure.
         */
        [[nodiscard]] Result<void> WriteFloat64(double value);
        /** @brief Writes validated UTF-8 without normalization. @param value Text to append. @return Success or typed failure. */
        [[nodiscard]] Result<void> WriteUtf8(std::string_view value);
        /** @brief Writes length-delimited opaque bytes. @param value Bytes to append. @return Success or typed failure. */
        [[nodiscard]] Result<void> WriteBytes(std::span<const std::byte> value);
        /** @brief Writes a finite vector component-wise. @param value Vector to append. @return Success or typed failure. */
        [[nodiscard]] Result<void> WriteVec2(Math::Vec2 value);
        /** @copydoc WriteVec2 */
        [[nodiscard]] Result<void> WriteVec3(Math::Vec3 value);
        /** @copydoc WriteVec2 */
        [[nodiscard]] Result<void> WriteVec4(Math::Vec4 value);
        /** @brief Writes a finite quaternion component-wise. @param value Quaternion to append. @return Success or typed failure. */
        [[nodiscard]] Result<void> WriteQuaternion(Math::Quaternion value);
        /** @brief Writes length-delimited values in semantic order. @param values Sealed values. @return Success or typed failure. */
        [[nodiscard]] Result<void> WriteSequence(std::span<const CanonicalEncodedValue> values);
        /** @brief Writes an optional sealed value. @param value Absent or present value. @return Success or typed failure. */
        [[nodiscard]] Result<void> WriteOptional(const std::optional<CanonicalEncodedValue> &value);
        /** @brief Writes a stable alternative and its value. @param index Zero-based alternative. @param alternativeCount Schema count.
         * @param value Sealed alternative value. @return Success or typed failure. */
        [[nodiscard]] Result<void> WriteVariant(std::uint32_t index, std::uint32_t alternativeCount, const CanonicalEncodedValue &value);
        /** @brief Sorts by encoded key and rejects duplicates. @param entries Sealed entries. @return Success or typed failure. */
        [[nodiscard]] Result<void> WriteMap(std::span<const CanonicalMapEntry> entries);
        /** @brief Sorts encoded values and rejects duplicates. @param elements Sealed elements. @return Success or typed failure. */
        [[nodiscard]] Result<void> WriteSet(std::span<const CanonicalEncodedValue> elements);
        /** @brief Sorts by field identity and rejects duplicates. @param fields Sealed fields. @return Success or typed failure. */
        [[nodiscard]] Result<void> WriteRecord(std::span<const CanonicalRecordField> fields);
        /** @brief Seals output; a failed writer exposes no bytes. @return Owned value or sticky error. */
        [[nodiscard]] Result<CanonicalEncodedValue> Finalize() &&;

    private:
        CanonicalValueWriter(const CanonicalCodecLimits &limits, std::shared_ptr<const CanonicalPathNode> path);
        [[nodiscard]] Result<void> Append(std::span<const std::byte> value);
        [[nodiscard]] Result<void> AppendLengthDelimited(std::span<const std::byte> value);
        [[nodiscard]] Result<void> CommitStaged(CanonicalValueWriter &&staging);
        [[nodiscard]] Result<void> AdmitComposite(std::size_t childDepth);
        [[nodiscard]] Result<void> AdmitCollection(std::size_t childDepth, std::size_t count, std::size_t maximum);
        [[nodiscard]] Result<void> Fail(Error error);
        [[nodiscard]] Error ErrorAt(const ErrorCodeDescriptor &descriptor) const;
        [[nodiscard]] Result<void> WriteFloatComponents(std::span<const float> components);
        template <typename Float, typename Unsigned> [[nodiscard]] Result<void> WriteFloating(Float value);
        template <typename WritePayload> [[nodiscard]] Result<void> WriteStaged(WritePayload writePayload);
        template <typename Value, typename Less, typename Equal, typename WriteValue>
        [[nodiscard]] Result<void> WriteOrderedCollection(std::span<const Value> values, Less less, Equal equal, WriteValue writeValue);
        template <typename Unsigned> [[nodiscard]] Result<void> WriteUnsigned(Unsigned value);
        template <typename Signed> [[nodiscard]] Result<void> WriteSigned(Signed value);
        CanonicalCodecLimits limits_;
        std::shared_ptr<const CanonicalPathNode> path_;
        std::vector<std::byte> bytes_;
        std::size_t structuralDepth_{};
        const ErrorCodeDescriptor *failure_{};
    };

    /** @brief Reader sharing memory admission and structural depth across child values. */
    class CanonicalValueReader final {
    public:
        /** @brief Creates a bounded root reader. @param bytes Complete borrowed value bytes. @param limits Trusted codec bounds.
         * @return Reader or configuration, wire-size, or allocation failure. */
        [[nodiscard]] static Result<CanonicalValueReader> Create(std::span<const std::byte> bytes, const CanonicalCodecLimits &limits = {});

        /** @brief Returns the next unread byte position. @return Offset relative to this value. */
        [[nodiscard]] std::size_t ByteOffset() const noexcept {
            return offset_;
        }

        /** @brief Reads a canonical boolean. @return Value or typed wire failure. */
        [[nodiscard]] Result<bool> ReadBool();
        /** @brief Reads an unsigned fixed-width scalar. @return Value or typed wire failure. */
        [[nodiscard]] Result<std::uint8_t> ReadUInt8();
        /** @copydoc ReadUInt8 */
        [[nodiscard]] Result<std::uint16_t> ReadUInt16();
        /** @copydoc ReadUInt8 */
        [[nodiscard]] Result<std::uint32_t> ReadUInt32();
        /** @copydoc ReadUInt8 */
        [[nodiscard]] Result<std::uint64_t> ReadUInt64();
        /** @brief Reads a two's-complement fixed-width scalar. @return Value or typed wire failure. */
        [[nodiscard]] Result<std::int8_t> ReadInt8();
        /** @copydoc ReadInt8 */
        [[nodiscard]] Result<std::int16_t> ReadInt16();
        /** @copydoc ReadInt8 */
        [[nodiscard]] Result<std::int32_t> ReadInt32();
        /** @copydoc ReadInt8 */
        [[nodiscard]] Result<std::int64_t> ReadInt64();
        /** @brief Reads finite canonical binary32. @return Value or typed wire failure. */
        [[nodiscard]] Result<float> ReadFloat32();
        /** @brief Reads finite canonical binary64. @return Value or typed wire failure. */
        [[nodiscard]] Result<double> ReadFloat64();
        /** @brief Reads owned validated UTF-8. @return Text or typed wire, quota, or allocation failure. */
        [[nodiscard]] Result<std::string> ReadUtf8();
        /** @brief Reads owned length-delimited bytes. @param maximumBytes Caller bound. @return Bytes or typed failure. */
        [[nodiscard]] Result<std::vector<std::byte>> ReadBytes(std::size_t maximumBytes);
        /** @brief Borrows an exact number of bytes without allocation. @param count Required count. @return Span or corruption failure. */
        [[nodiscard]] Result<std::span<const std::byte>> ReadExactBytes(std::size_t count);
        /** @brief Reads a finite vector component-wise. @return Value or typed wire failure. */
        [[nodiscard]] Result<Math::Vec2> ReadVec2();
        /** @copydoc ReadVec2 */
        [[nodiscard]] Result<Math::Vec3> ReadVec3();
        /** @copydoc ReadVec2 */
        [[nodiscard]] Result<Math::Vec4> ReadVec4();
        /** @brief Reads a finite quaternion component-wise. @return Value or typed wire failure. */
        [[nodiscard]] Result<Math::Quaternion> ReadQuaternion();
        /** @brief Reads borrowed sequence children. @return Children or typed failure. */
        [[nodiscard]] Result<std::vector<CanonicalDecodedValue>> ReadSequence();
        /** @brief Reads an absent or borrowed child. @return Optional child or typed failure. */
        [[nodiscard]] Result<std::optional<CanonicalDecodedValue>> ReadOptional();
        /** @brief Reads one bounded alternative. @param alternativeCount Schema count. @return Index and child or typed failure. */
        [[nodiscard]] Result<std::pair<std::uint32_t, CanonicalDecodedValue>> ReadVariant(std::uint32_t alternativeCount);
        /** @brief Reads strictly key-ordered borrowed entries. @return Entries or typed failure. */
        [[nodiscard]] Result<std::vector<CanonicalDecodedMapEntry>> ReadMap();
        /** @brief Reads strictly ordered borrowed values. @return Values or typed failure. */
        [[nodiscard]] Result<std::vector<CanonicalDecodedValue>> ReadSet();
        /** @brief Reads strictly identity-ordered borrowed fields. @return Fields or typed failure. */
        [[nodiscard]] Result<std::vector<CanonicalDecodedField>> ReadRecord();
        /** @brief Requires complete consumption of this value. @return Success or trailing-data corruption. */
        [[nodiscard]] Result<void> RequireFinished() const;

    private:
        friend class CanonicalDecodedValue;
        /** @brief Ordering policy shared by sequence and set decoding. */
        enum class ValueCollectionOrder : std::uint8_t {
            Preserve,
            RequireCanonical,
        };
        CanonicalValueReader(std::span<const std::byte> bytes, const CanonicalCodecLimits &limits,
                             std::shared_ptr<CanonicalReadState> state, std::size_t depth, std::shared_ptr<const CanonicalPathNode> path);
        [[nodiscard]] Result<std::size_t> ReadLength(std::size_t maximum);
        [[nodiscard]] Result<CanonicalDecodedValue> ReadChild(std::shared_ptr<const CanonicalPathNode> path);
        /** @brief Reads a bounded value collection with optional strict canonical ordering. */
        [[nodiscard]] Result<std::vector<CanonicalDecodedValue>> ReadValueCollection(ValueCollectionOrder order);
        [[nodiscard]] Result<void> AdmitComposite() const;
        [[nodiscard]] Result<void> Charge(std::size_t bytes) const;
        [[nodiscard]] Result<void> ChargeReadWork(std::size_t bytes) const;
        [[nodiscard]] Result<void> ChargeElements(std::size_t count, std::size_t elementSize) const;
        [[nodiscard]] Result<void> AdmitElements(std::size_t count, std::size_t elementSize, std::size_t minimumWireBytesPerElement) const;
        [[nodiscard]] Error ErrorAt(const ErrorCodeDescriptor &descriptor) const;
        [[nodiscard]] Result<void> ReadFloatComponents(std::span<float> components);
        template <typename Unsigned> [[nodiscard]] Result<Unsigned> ReadUnsigned();
        template <typename Signed> [[nodiscard]] Result<Signed> ReadSigned();
        std::span<const std::byte> bytes_;
        CanonicalCodecLimits limits_;
        std::shared_ptr<CanonicalReadState> state_;
        std::size_t depth_{};
        std::shared_ptr<const CanonicalPathNode> path_;
        std::size_t offset_{};
    };
}  // namespace Horo::Runtime
