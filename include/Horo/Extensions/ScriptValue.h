#pragma once

/**
 * @file ScriptValue.h
 * @brief Bounded, owned language-neutral values and canonical script-boundary marshalling.
 */

#include "Horo/Extensions/ScriptExportDescriptor.h"
#include "Horo/Foundation/ErrorCode.h"
#include "Horo/Foundation/Result.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Horo::Extensions {
    /** @brief Host-issued script context identity used by opaque handles. */
    struct ScriptContextId final {
        std::uint64_t value{}; /**< Non-zero host-issued identity; zero is invalid. */

        /** @brief Reports whether this identity is usable. @return False only for zero. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        constexpr auto operator<=>(const ScriptContextId &) const noexcept = default;
    };

    /**
     * @brief Generation-checked opaque resource identity exposed to a script runtime.
     *
     * The fields are identifiers only. They never encode a native pointer, object address,
     * allocator token, function pointer, or VM object. The issuing invocation context owns
     * the corresponding resource and revokes it before the provider generation is released.
     */
    struct ScriptHandle final {
        ScriptContextId context;            /**< Context that may present this handle. */
        std::uint64_t providerGeneration{}; /**< Provider generation that issued the handle. */
        std::uint64_t value{};              /**< Host-issued opaque slot identity. */
        std::uint64_t generation{};         /**< Non-wrapping slot generation. */
        std::string type;                   /**< Stable bounded resource type identity. */

        /** @brief Reports whether all structural handle fields are present. @return True for a non-zero opaque identity. */
        [[nodiscard]] bool IsValid() const noexcept;

        bool operator==(const ScriptHandle &) const noexcept = default;
    };

    /** @brief Stable named enum value carried by the language-neutral value algebra. */
    struct ScriptEnumValue final {
        std::string type;     /**< Stable descriptor enum identity. */
        std::int64_t value{}; /**< Canonical enum numeric value. */

        bool operator==(const ScriptEnumValue &) const noexcept = default;
    };

    struct ScriptArray;
    struct ScriptMap;
    struct ScriptStruct;

    /**
     * @brief Closed owned value algebra crossing a script binding boundary.
     *
     * Every non-scalar payload is copied into host-owned storage. The type has no native
     * pointer, reference, exception, VM object, allocator handle, platform handle, or
     * executable callback alternative. Recursive values are held through immutable shared
     * storage so copying a value cannot create a borrowed child lifetime.
     */
    class ScriptValue final {
    public:
        /** @brief Closed logical value kind; no runtime-specific values are admitted. */
        enum class Kind : std::uint8_t {
            Null,
            Boolean,
            SignedInteger,
            UnsignedInteger,
            Number,
            String,
            Bytes,
            Handle,
            Enum,
            Array,
            Map,
            Struct,
            Count,
        };

        using Bytes = std::vector<std::byte>;
        using ArrayElements = std::vector<ScriptValue>;
        using MapEntry = std::pair<ScriptValue, ScriptValue>;
        using MapEntries = std::vector<MapEntry>;
        using StructField = std::pair<std::string, ScriptValue>;
        using StructFields = std::vector<StructField>;

        /** @brief Constructs the explicit nullable value. */
        ScriptValue() noexcept;

        /** @brief Constructs an owned boolean value. @param value Boolean payload. */
        explicit ScriptValue(bool value) noexcept;
        /** @brief Constructs an owned signed integer value. @param value Integer payload. */
        explicit ScriptValue(std::int64_t value) noexcept;
        /** @brief Constructs an owned unsigned integer value. @param value Integer payload. */
        explicit ScriptValue(std::uint64_t value) noexcept;
        /** @brief Constructs an owned finite number value. @param value Number payload. */
        explicit ScriptValue(double value) noexcept;
        /** @brief Constructs an owned string value by copying the input. @param value String payload. */
        explicit ScriptValue(std::string value);
        /** @brief Constructs an owned byte value by copying the input. @param value Byte payload. */
        explicit ScriptValue(Bytes value);
        /** @brief Constructs an owned opaque handle value. @param value Generation-safe handle. */
        explicit ScriptValue(ScriptHandle value);
        /** @brief Constructs an owned enum value. @param value Typed enum payload. */
        explicit ScriptValue(ScriptEnumValue value);

        /** @brief Creates the explicit nullable value. @return Null script value. */
        [[nodiscard]] static ScriptValue Null() noexcept;
        /** @brief Creates an owned string by copying a view. @param value String payload. @return Owned string value. */
        [[nodiscard]] static ScriptValue String(std::string_view value);
        /** @brief Creates an owned byte value. @param value Bytes to copy. @return Owned bytes value. */
        [[nodiscard]] static ScriptValue BytesValue(std::span<const std::byte> value);
        /** @brief Creates an immutable array by taking a copied vector. @param values Array elements. @return Owned array value. */
        [[nodiscard]] static ScriptValue Array(ArrayElements values);
        /** @brief Creates an immutable map by taking copied key/value entries. @param values Map entries. @return Owned map value. */
        [[nodiscard]] static ScriptValue Map(MapEntries values);
        /** @brief Creates an immutable typed struct by taking copied named fields. @param type Stable type identity. @param fields Struct
         * fields. @return Owned struct value. */
        [[nodiscard]] static ScriptValue Struct(std::string type, StructFields fields);

        /** @brief Returns the closed logical value kind. @return Exact kind. */
        [[nodiscard]] Kind GetKind() const noexcept;
        /** @brief Returns the boolean payload when this is Boolean. @return Borrowed payload or nullptr. */
        [[nodiscard]] const bool *AsBoolean() const noexcept;
        /** @brief Returns the signed payload when this is SignedInteger. @return Borrowed payload or nullptr. */
        [[nodiscard]] const std::int64_t *AsSignedInteger() const noexcept;
        /** @brief Returns the unsigned payload when this is UnsignedInteger. @return Borrowed payload or nullptr. */
        [[nodiscard]] const std::uint64_t *AsUnsignedInteger() const noexcept;
        /** @brief Returns the numeric payload when this is Number. @return Borrowed payload or nullptr. */
        [[nodiscard]] const double *AsNumber() const noexcept;
        /** @brief Returns the owned string payload when this is String. @return Borrowed view or empty for another kind. */
        [[nodiscard]] std::string_view AsString() const noexcept;
        /** @brief Returns the owned bytes when this is Bytes. @return Borrowed span or empty for another kind. */
        [[nodiscard]] std::span<const std::byte> AsBytes() const noexcept;
        /** @brief Returns the opaque payload when this is Handle. @return Borrowed payload or nullptr. */
        [[nodiscard]] const ScriptHandle *AsHandle() const noexcept;
        /** @brief Returns the enum payload when this is Enum. @return Borrowed payload or nullptr. */
        [[nodiscard]] const ScriptEnumValue *AsEnum() const noexcept;
        /** @brief Returns array elements when this is Array. @return Borrowed elements or empty for another kind. */
        [[nodiscard]] std::span<const ScriptValue> AsArray() const noexcept;
        /** @brief Returns map entries when this is Map. @return Borrowed entries or empty for another kind. */
        [[nodiscard]] std::span<const MapEntry> AsMap() const noexcept;
        /** @brief Returns the struct type identity when this is Struct. @return Borrowed identity or empty. */
        [[nodiscard]] std::string_view StructType() const noexcept;
        /** @brief Returns struct fields when this is Struct. @return Borrowed fields or empty for another kind. */
        [[nodiscard]] std::span<const StructField> AsStruct() const noexcept;

        /** @brief Reports whether this value is the nullable alternative. @return True for Null. */
        [[nodiscard]] bool IsNull() const noexcept;

        bool operator==(const ScriptValue &other) const noexcept;

    private:
        using ArrayStorage = std::shared_ptr<const ScriptArray>;
        using MapStorage = std::shared_ptr<const ScriptMap>;
        using StructStorage = std::shared_ptr<const ScriptStruct>;
        using Storage = std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string, Bytes, ScriptHandle,
                                     ScriptEnumValue, ArrayStorage, MapStorage, StructStorage>;

        explicit ScriptValue(Storage value) noexcept;

        Storage value_;
    };

    /** @brief Immutable array storage used by ScriptValue. */
    struct ScriptArray final {
        ScriptValue::ArrayElements values;
        bool operator==(const ScriptArray &) const noexcept = default;
    };

    /** @brief Immutable map storage used by ScriptValue. Map keys are values, not strings-only objects. */
    struct ScriptMap final {
        ScriptValue::MapEntries values;
        bool operator==(const ScriptMap &) const noexcept = default;
    };

    /** @brief Immutable named-field storage used by ScriptValue. */
    struct ScriptStruct final {
        std::string type;
        ScriptValue::StructFields fields;
        bool operator==(const ScriptStruct &) const noexcept = default;
    };

    /** @brief One bounded structured error detail carried without a native exception or cause pointer. */
    struct ScriptErrorDetail final {
        std::string id;    /**< Stable bounded detail identity. */
        ScriptValue value; /**< Owned detail value. */

        bool operator==(const ScriptErrorDetail &) const noexcept = default;
    };

    /** @brief Runtime-neutral structured error preserving stable identity and bounded evidence. */
    struct ScriptError final {
        std::string domain;                     /**< Stable error domain identity. */
        std::string code;                       /**< Stable machine-readable error code. */
        std::string message;                    /**< Optional bounded non-secret diagnostic message. */
        bool retryable{};                       /**< Whether retry may be meaningful without changing the request. */
        bool cancelled{};                       /**< Whether cancellation, timeout, or revocation caused the error. */
        std::vector<ScriptErrorDetail> details; /**< Bounded typed evidence; never a native exception or cause chain. */

        bool operator==(const ScriptError &) const noexcept = default;
    };

    /** @brief One successful or failed logical function result. Empty values represent a void success. */
    struct ScriptCallResult final {
        std::vector<ScriptValue> values;  /**< Ordered result values declared by the function. */
        std::optional<ScriptError> error; /**< Present only for failure. */

        /** @brief Creates a successful result. @param values Ordered result values. @return Success result. */
        [[nodiscard]] static ScriptCallResult Success(std::vector<ScriptValue> values = {});
        /** @brief Creates a failed result. @param error Structured stable error. @return Failure result. */
        [[nodiscard]] static ScriptCallResult Failure(ScriptError error);
        /** @brief Reports whether this is successful. @return True when no error is present. */
        [[nodiscard]] bool IsSuccess() const noexcept;
        /** @brief Reports whether this is failed. @return True when an error is present. */
        [[nodiscard]] bool IsFailure() const noexcept;

        bool operator==(const ScriptCallResult &) const noexcept = default;
    };

    /** @brief Finite limits applied to validation and every canonical codec operation. */
    struct ScriptValueLimits final {
        std::size_t maximumDepth{32};                        /**< Maximum recursive value depth. */
        std::size_t maximumElements{4096};                   /**< Maximum aggregate array/map/struct elements. */
        std::size_t maximumArrayElements{4096};              /**< Maximum elements in one array. */
        std::size_t maximumMapEntries{4096};                 /**< Maximum entries in one map. */
        std::size_t maximumStructFields{256};                /**< Maximum fields in one struct. */
        std::size_t maximumStringBytes{64U * 1024U};         /**< Maximum bytes in one string or identity. */
        std::size_t maximumBytes{4U * 1024U * 1024U};        /**< Maximum aggregate raw byte/string payload. */
        std::size_t maximumEncodedBytes{4U * 1024U * 1024U}; /**< Maximum canonical encoded payload. */
        std::size_t maximumWorkUnits{1U << 20};              /**< Maximum codec/validation work units. */
        std::size_t maximumTypeIdentityBytes{256};           /**< Maximum enum, struct, handle, and error identity bytes. */
        std::size_t maximumErrorDetails{32};                 /**< Maximum details in one structured error. */
    };

    /**
     * @brief Validates an owned value against finite structural limits.
     * @param value Candidate owned value.
     * @param limits Host-owned depth, size, collection, and work limits.
     * @return Success or a typed malformed/capacity error.
     */
    [[nodiscard]] Result<void> ValidateScriptValue(const ScriptValue &value, const ScriptValueLimits &limits = {});

    /**
     * @brief Validates one structured error and its bounded typed evidence.
     * @param error Candidate structured error.
     * @param limits Host-owned value and evidence limits.
     * @return Success or a typed malformed/capacity error.
     */
    [[nodiscard]] Result<void> ValidateScriptError(const ScriptError &error, const ScriptValueLimits &limits = {});

    /**
     * @brief Validates a logical result against finite bounds.
     * @param result Candidate successful or failed result.
     * @param limits Host-owned value and evidence limits.
     * @return Success or a typed malformed/capacity error.
     */
    [[nodiscard]] Result<void> ValidateScriptCallResult(const ScriptCallResult &result, const ScriptValueLimits &limits = {});

    /**
     * @brief Validates one value against a descriptor type reference without runtime lookup.
     * @param value Candidate value.
     * @param type Expected descriptor type.
     * @param descriptor Validated language-neutral API descriptor owning named types.
     * @param limits Host-owned value bounds.
     * @return Success or a typed malformed/type-mismatch/capacity error.
     */
    [[nodiscard]] Result<void> ValidateScriptValueForType(const ScriptValue &value, const ScriptExportTypeReference &type,
                                                          const ScriptExportDescriptor &descriptor, const ScriptValueLimits &limits = {});

    /**
     * @brief Validates ordered call arguments against a descriptor function declaration.
     * @param function Function declaration owning parameter order and optionality.
     * @param arguments Copied call arguments.
     * @param descriptor API descriptor owning referenced types.
     * @param limits Host-owned value bounds.
     * @return Success or a typed malformed/type-mismatch/capacity error.
     */
    [[nodiscard]] Result<void> ValidateScriptArguments(const ScriptExportFunctionDescriptor &function,
                                                       std::span<const ScriptValue> arguments, const ScriptExportDescriptor &descriptor,
                                                       const ScriptValueLimits &limits = {});

    /**
     * @brief Validates ordered function results against a descriptor result declaration.
     * @param function Function declaration owning result order.
     * @param result Candidate logical result.
     * @param descriptor API descriptor owning referenced types.
     * @param limits Host-owned value bounds.
     * @return Success or a typed malformed/type-mismatch/capacity error.
     */
    [[nodiscard]] Result<void> ValidateScriptResult(const ScriptExportFunctionDescriptor &function, const ScriptCallResult &result,
                                                    const ScriptExportDescriptor &descriptor, const ScriptValueLimits &limits = {});

    /**
     * @brief Encodes one value with a deterministic, owned, little-endian canonical wire format.
     * @param value Valid or invalid candidate value; validation runs before encoding.
     * @param limits Host-owned codec bounds.
     * @return Canonical bytes or a typed malformed/capacity failure.
     */
    [[nodiscard]] Result<std::vector<std::byte>> EncodeScriptValue(const ScriptValue &value, const ScriptValueLimits &limits = {});

    /**
     * @brief Decodes one complete canonical value without borrowing the input buffer.
     * @param bytes Complete canonical value bytes.
     * @param limits Host-owned codec bounds.
     * @return Owned value or a typed malformed/capacity failure.
     */
    [[nodiscard]] Result<ScriptValue> DecodeScriptValue(std::span<const std::byte> bytes, const ScriptValueLimits &limits = {});

    /** @brief Encodes a successful or failed logical result using the same bounded value codec. */
    [[nodiscard]] Result<std::vector<std::byte>> EncodeScriptCallResult(const ScriptCallResult &result,
                                                                        const ScriptValueLimits &limits = {});

    /** @brief Decodes a complete logical result without borrowing the input buffer. */
    [[nodiscard]] Result<ScriptCallResult> DecodeScriptCallResult(std::span<const std::byte> bytes, const ScriptValueLimits &limits = {});

    /**
     * @brief Converts a Foundation error into a bounded runtime-neutral error.
     * @param error Foundation error whose stable outer identity is preserved.
     * @param retryable Explicit retry policy owned by the calling boundary.
     * @param limits Host-owned evidence bounds.
     * @return Owned structured error or a typed capacity failure.
     */
    [[nodiscard]] Result<ScriptError> MakeScriptError(const Error &error, bool retryable = false, const ScriptValueLimits &limits = {});
}  // namespace Horo::Extensions
