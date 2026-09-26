#include "Horo/Runtime/Save/SaveCanonicalCodec.h"

#include "Horo/Runtime/Save/SaveErrors.h"
#include "SaveCanonicalCodecInternal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace Horo::Runtime {
    struct CanonicalReadState final {
        std::atomic<std::size_t> decodedBytes{};
        std::atomic<std::size_t> readWorkBytes{};
    };

    namespace {
        void AppendPath(std::string &source, const CanonicalPathNode *node) {
            if (!node)
                return;
            AppendPath(source, node->parent.get());
            source += std::format("/field:{}", node->field.Value());
        }

        const ErrorCodeDescriptor &DescriptorFor(const Error &error) noexcept {
            const std::array descriptors{&SaveErrors::CanonicalCodecInvalid,
                                         &SaveErrors::CanonicalCodecCorrupt,
                                         &SaveErrors::CanonicalCodecLimitExceeded,
                                         &SaveErrors::CanonicalCodecDuplicate,
                                         &SaveErrors::CanonicalCodecNonFinite,
                                         &SaveErrors::CanonicalCodecUtf8Invalid,
                                         &SaveErrors::CanonicalCodecConfigurationInvalid,
                                         &SaveErrors::CanonicalCodecAllocationFailed};
            for (const auto *descriptor : descriptors) {
                if (error.code.Value() == descriptor->code.Value())
                    return *descriptor;
            }
            return SaveErrors::CanonicalCodecInvalid;
        }

        [[nodiscard]] Error CanonicalErrorAt(const ErrorCodeDescriptor &descriptor, const CanonicalPathNode *path,
                                             const std::size_t offset) {
            Error error = MakeError(descriptor);
            std::string source{"canonical"};
            AppendPath(source, path);
            error.diagnostics.push_back(
                {DiagnosticCode{"save.canonical_codec.location"},
                 DiagnosticSeverity::Error,
                 std::string{descriptor.summary},
                 {std::move(source), 0,
                  static_cast<std::uint32_t>(std::min(offset, static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())))}});
            return error;
        }
    }  // namespace

    /** @copydoc CanonicalFieldId::Create */
    Result<CanonicalFieldId> CanonicalFieldId::Create(const ValueType value) {
        return value ? Result<CanonicalFieldId>::Success(CanonicalFieldId{value})
                     : Result<CanonicalFieldId>::Failure(MakeError(SaveErrors::CanonicalCodecInvalid));
    }

    /** @copydoc CanonicalValueWriter::CanonicalValueWriter */
    CanonicalValueWriter::CanonicalValueWriter(const CanonicalCodecLimits &limits) : limits_(limits) {}

    CanonicalValueWriter::CanonicalValueWriter(const CanonicalCodecLimits &limits, std::shared_ptr<const CanonicalPathNode> path)
        : limits_(limits), path_(std::move(path)) {}

    /** @copydoc CanonicalValueWriter::ForField */
    Result<CanonicalValueWriter> CanonicalValueWriter::ForField(const CanonicalFieldId field) const {
        if (!CanonicalCodecDetail::ValidLimits(limits_))
            return Result<CanonicalValueWriter>::Failure(ErrorAt(SaveErrors::CanonicalCodecConfigurationInvalid));
        const std::size_t depth = path_ ? path_->depth + 1 : 1;
        if (depth > limits_.maximumNestingDepth)
            return Result<CanonicalValueWriter>::Failure(ErrorAt(SaveErrors::CanonicalCodecLimitExceeded));
        Error allocationFailure = ErrorAt(SaveErrors::CanonicalCodecAllocationFailed);
        try {
            auto path = std::make_shared<CanonicalPathNode>(field, path_, depth);
            return Result<CanonicalValueWriter>::Success(CanonicalValueWriter{limits_, std::move(path)});
        } catch (const std::bad_alloc &) {
            return Result<CanonicalValueWriter>::Failure(std::move(allocationFailure));
        }
    }

    Error CanonicalValueWriter::ErrorAt(const ErrorCodeDescriptor &descriptor) const {
        return CanonicalErrorAt(descriptor, path_.get(), bytes_.size());
    }

    Result<void> CanonicalValueWriter::Fail(Error error) {
        if (failure_)
            return Result<void>::Failure(ErrorAt(*failure_));
        failure_ = &DescriptorFor(error);
        return Result<void>::Failure(std::move(error));
    }

    Result<void> CanonicalValueWriter::Append(const std::span<const std::byte> value) {
        if (failure_)
            return Result<void>::Failure(ErrorAt(*failure_));
        if (!CanonicalCodecDetail::ValidLimits(limits_))
            return Fail(ErrorAt(SaveErrors::CanonicalCodecConfigurationInvalid));
        if (value.size() > limits_.maximumBytes || bytes_.size() > limits_.maximumBytes - value.size())
            return Fail(ErrorAt(SaveErrors::CanonicalCodecLimitExceeded));
        Error allocationFailure = ErrorAt(SaveErrors::CanonicalCodecAllocationFailed);
        try {
            bytes_.insert(bytes_.end(), value.begin(), value.end());
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Fail(std::move(allocationFailure));
        }
    }

    Result<void> CanonicalValueWriter::AppendLengthDelimited(const std::span<const std::byte> value) {
        constexpr std::size_t LengthBytes = sizeof(std::uint32_t);
        if (!CanonicalCodecDetail::ValidLimits(limits_))
            return Fail(ErrorAt(SaveErrors::CanonicalCodecConfigurationInvalid));
        if (limits_.maximumBytes < LengthBytes || value.size() > std::numeric_limits<std::uint32_t>::max() ||
            value.size() > limits_.maximumBytes - LengthBytes || bytes_.size() > limits_.maximumBytes - LengthBytes - value.size())
            return Fail(ErrorAt(SaveErrors::CanonicalCodecLimitExceeded));
        auto length = WriteUInt32(static_cast<std::uint32_t>(value.size()));
        return length.HasError() ? length : Append(value);
    }

    Result<void> CanonicalValueWriter::AdmitComposite(const std::size_t childDepth) {
        if (failure_)
            return Result<void>::Failure(ErrorAt(*failure_));
        if (!CanonicalCodecDetail::ValidLimits(limits_))
            return Fail(ErrorAt(SaveErrors::CanonicalCodecConfigurationInvalid));
        if (childDepth >= limits_.maximumNestingDepth)
            return Fail(ErrorAt(SaveErrors::CanonicalCodecLimitExceeded));
        structuralDepth_ = std::max(structuralDepth_, childDepth + 1);
        return Result<void>::Success();
    }

    Result<void> CanonicalValueWriter::AdmitCollection(const std::size_t childDepth, const std::size_t count, const std::size_t maximum) {
        if (Result<void> admitted = AdmitComposite(childDepth); admitted.HasError())
            return admitted;
        return count <= maximum ? Result<void>::Success() : Fail(ErrorAt(SaveErrors::CanonicalCodecLimitExceeded));
    }

    /** @copydoc CanonicalValueWriter::Finalize */
    Result<CanonicalEncodedValue> CanonicalValueWriter::Finalize() && {
        if (failure_)
            return Result<CanonicalEncodedValue>::Failure(ErrorAt(*failure_));
        if (!CanonicalCodecDetail::ValidLimits(limits_))
            return Result<CanonicalEncodedValue>::Failure(ErrorAt(SaveErrors::CanonicalCodecConfigurationInvalid));
        return Result<CanonicalEncodedValue>::Success(CanonicalEncodedValue{std::move(bytes_), structuralDepth_});
    }

    CanonicalValueReader::CanonicalValueReader(std::span<const std::byte> bytes, const CanonicalCodecLimits &limits,
                                               std::shared_ptr<CanonicalReadState> state, const std::size_t depth,
                                               std::shared_ptr<const CanonicalPathNode> path)
        : bytes_(bytes), limits_(limits), state_(std::move(state)), depth_(depth), path_(std::move(path)) {}

    /** @copydoc CanonicalValueReader::Create */
    Result<CanonicalValueReader> CanonicalValueReader::Create(const std::span<const std::byte> bytes, const CanonicalCodecLimits &limits) {
        if (!CanonicalCodecDetail::ValidLimits(limits))
            return Result<CanonicalValueReader>::Failure(MakeError(SaveErrors::CanonicalCodecConfigurationInvalid));
        if (bytes.size() > limits.maximumBytes)
            return Result<CanonicalValueReader>::Failure(MakeError(SaveErrors::CanonicalCodecLimitExceeded));
        Error allocationFailure = MakeError(SaveErrors::CanonicalCodecAllocationFailed);
        try {
            return Result<CanonicalValueReader>::Success(
                CanonicalValueReader{bytes, limits, std::make_shared<CanonicalReadState>(), 0, {}});
        } catch (const std::bad_alloc &) {
            return Result<CanonicalValueReader>::Failure(std::move(allocationFailure));
        }
    }

    /** @copydoc CanonicalDecodedValue::OpenReader */
    Result<CanonicalValueReader> CanonicalDecodedValue::OpenReader() const {
        return Result<CanonicalValueReader>::Success(CanonicalValueReader{bytes_, limits_, state_, depth_, path_});
    }

    Error CanonicalValueReader::ErrorAt(const ErrorCodeDescriptor &descriptor) const {
        return CanonicalErrorAt(descriptor, path_.get(), offset_);
    }

    Result<void> CanonicalValueReader::Charge(const std::size_t bytes) const {
        std::size_t used = state_->decodedBytes.load(std::memory_order_relaxed);
        while (true) {
            if (used > limits_.maximumDecodedBytes || bytes > limits_.maximumDecodedBytes - used)
                return Result<void>::Failure(ErrorAt(SaveErrors::CanonicalCodecLimitExceeded));
            if (state_->decodedBytes.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed))
                return Result<void>::Success();
        }
    }

    Result<void> CanonicalValueReader::ChargeReadWork(const std::size_t bytes) const {
        std::size_t used = state_->readWorkBytes.load(std::memory_order_relaxed);
        while (true) {
            if (used > limits_.maximumReadWorkBytes || bytes > limits_.maximumReadWorkBytes - used) {
                Error error = ErrorAt(SaveErrors::CanonicalCodecLimitExceeded);
                error.diagnostics.front().code = DiagnosticCode{"save.canonical_codec.limit.read_work"};
                error.diagnostics.front().message = "Canonical read work budget exceeded.";
                return Result<void>::Failure(std::move(error));
            }
            if (state_->readWorkBytes.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed))
                return Result<void>::Success();
        }
    }

    Result<void> CanonicalValueReader::ChargeElements(const std::size_t count, const std::size_t elementSize) const {
        if (elementSize != 0 && count > std::numeric_limits<std::size_t>::max() / elementSize)
            return Result<void>::Failure(ErrorAt(SaveErrors::CanonicalCodecLimitExceeded));
        return Charge(count * elementSize);
    }

    Result<void> CanonicalValueReader::AdmitElements(const std::size_t count, const std::size_t elementSize,
                                                     const std::size_t minimumWireBytesPerElement) const {
        if (const std::size_t remainingBytes = offset_ <= bytes_.size() ? bytes_.size() - offset_ : 0;
            minimumWireBytesPerElement != 0 && count > remainingBytes / minimumWireBytesPerElement) {
            return Result<void>::Failure(ErrorAt(SaveErrors::CanonicalCodecCorrupt));
        }
        return ChargeElements(count, elementSize);
    }

    Result<void> CanonicalValueReader::AdmitComposite() const {
        return depth_ < limits_.maximumNestingDepth ? Result<void>::Success()
                                                    : Result<void>::Failure(ErrorAt(SaveErrors::CanonicalCodecLimitExceeded));
    }

    /** @copydoc CanonicalValueReader::ReadExactBytes */
    Result<std::span<const std::byte>> CanonicalValueReader::ReadExactBytes(const std::size_t count) {
        if (offset_ > bytes_.size() || count > bytes_.size() - offset_)
            return Result<std::span<const std::byte>>::Failure(ErrorAt(SaveErrors::CanonicalCodecCorrupt));
        if (auto charged = ChargeReadWork(count); charged.HasError())
            return Result<std::span<const std::byte>>::Failure(charged.ErrorValue());
        const auto value = bytes_.subspan(offset_, count);
        offset_ += count;
        return Result<std::span<const std::byte>>::Success(value);
    }

    Result<std::size_t> CanonicalValueReader::ReadLength(const std::size_t maximum) {
        auto length = ReadUInt32();
        if (length.HasError())
            return Result<std::size_t>::Failure(length.ErrorValue());
        return length.Value() <= maximum ? Result<std::size_t>::Success(length.Value())
                                         : Result<std::size_t>::Failure(ErrorAt(SaveErrors::CanonicalCodecLimitExceeded));
    }

    /** @copydoc CanonicalValueReader::RequireFinished */
    Result<void> CanonicalValueReader::RequireFinished() const {
        return offset_ == bytes_.size() ? Result<void>::Success() : Result<void>::Failure(ErrorAt(SaveErrors::CanonicalCodecCorrupt));
    }
}  // namespace Horo::Runtime
