#include "Horo/PCG/PCGProvenance.h"
#include "PCGPointIdentityInternal.h"

#include <algorithm>
#include <array>
#include <new>
#include <ranges>

namespace Horo::PCG::detail {
    namespace {
        using Composite = std::array<std::uint64_t, 5>;

        /** @brief Preserves the owning typed error domain at the kernel boundary. */
        template <typename T> Result<T> Reject(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Reconstructs unchanged network-order key bytes, without introducing another hash codec. */
        Sha256Digest RootKey(const Composite &identity) noexcept {
            Sha256Digest key;
            for (std::size_t word = 0; word < 4; ++word)
                for (std::size_t byte = 0; byte < 8; ++byte)
                    key.bytes[word * 8 + byte] = static_cast<std::uint8_t>(identity[word] >> ((7 - byte) * 8));
            return key;
        }

        /** @brief Copies and verifies exact composite identity lengths and canonical seed consistency. */
        Result<void> CopyIdentities(const PCGPointReadView &input, std::span<Composite> destination,
                                    const CancellationToken &cancellation) {
            std::array<std::span<const std::uint64_t>, 5> columns;
            for (std::size_t word = 0; word < 4; ++word)
                columns[word] = input.FindColumn<PCGUnsignedIntegerColumn>(PCGSourceRootAttributes[word]);
            columns[4] = input.FindColumn<PCGUnsignedIntegerColumn>(PCGSourceSampleAttribute);
            if (std::ranges::any_of(columns, [&input](const auto column) {
                return column.size() != input.PointCount();
            }))
                return Reject<void>(PCGErrors::GenerationIdentityInvalid);
            for (std::size_t index = 0; index < input.PointCount(); ++index) {
                if (index % 256 == 0 && cancellation.IsCancellationRequested())
                    return Reject<void>(PCGErrors::CpuEvaluationClosed);
                for (std::size_t word = 0; word < columns.size(); ++word)
                    destination[index][word] = columns[word][index];
                const auto sample = SourceSampleId::Create(destination[index][4]);
                if (sample.HasError())
                    return Reject<void>(PCGErrors::GenerationIdentityInvalid);
                const auto seed = DerivePCGSampleSeed(RootKey(destination[index]), sample.Value());
                if (seed.HasError() || seed.Value() != input.Seeds()[index])
                    return Reject<void>(PCGErrors::GenerationIdentityInvalid);
            }
            return Result<void>::Success();
        }

        /** @brief Merges one pair of sorted runs with at most one five-word comparison per emitted record. */
        Result<void> MergeRun(const std::span<const Composite> source, const std::span<Composite> destination, const std::size_t begin,
                              const std::size_t middle, const std::size_t end, const CancellationToken &cancellation) {
            std::size_t left = begin;
            std::size_t right = middle;
            for (std::size_t index = begin; index < end; ++index) {
                if ((index - begin) % 256 == 0 && cancellation.IsCancellationRequested())
                    return Reject<void>(PCGErrors::CpuEvaluationClosed);
                if (right == end || (left < middle && source[left] <= source[right]))
                    destination[index] = source[left++];
                else
                    destination[index] = source[right++];
            }
            return Result<void>::Success();
        }

        /** @brief Fixed bottom-up merge passes bound comparisons by count*ceil(log2(count)); original point order is untouched. */
        Result<void> UniqueIdentities(std::vector<Composite> &records, std::vector<Composite> &temporary,
                                      const CancellationToken &cancellation) {
            const std::size_t count = records.size();
            for (std::size_t width = 1; width < count;) {
                for (std::size_t begin = 0; begin < count;) {
                    if (cancellation.IsCancellationRequested())
                        return Reject<void>(PCGErrors::CpuEvaluationClosed);
                    const std::size_t middle = begin + std::min(width, count - begin);
                    const std::size_t end = middle + std::min(width, count - middle);
                    if (const auto merged = MergeRun(records, temporary, begin, middle, end, cancellation); merged.HasError())
                        return merged;
                    begin = end;
                }
                records.swap(temporary);
                if (width > count / 2)
                    break;
                width *= 2;
            }
            for (std::size_t index = 1; index < count; ++index) {
                if (index % 256 == 0 && cancellation.IsCancellationRequested())
                    return Reject<void>(PCGErrors::CpuEvaluationClosed);
                if (records[index] == records[index - 1])
                    return Reject<void>(PCGErrors::GenerationIdentityInvalid);
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc HasPointIdentity */
    Result<bool> HasPointIdentity(const PCGPointSchema &schema) {
        std::size_t fields{};
        const auto valid = [&schema, &fields](const std::string_view name) {
            const auto *field = schema.Find(name);
            if (field == nullptr)
                return true;
            ++fields;
            return field->type == PCGAttributeType::UnsignedInteger;
        };
        if (!valid(PCGSourceSampleAttribute))
            return Reject<bool>(PCGErrors::GenerationIdentityInvalid);
        for (const auto name : PCGSourceRootAttributes)
            if (!valid(name))
                return Reject<bool>(PCGErrors::GenerationIdentityInvalid);
        if (fields != 0 && fields != 5)
            return Reject<bool>(PCGErrors::GenerationIdentityInvalid);
        return Result<bool>::Success(fields == 5);
    }

    /** @copydoc ChargePointIdentity */
    Result<PointGenerationCharge> ChargePointIdentity(const std::size_t maximumPoints) {
        const auto scratch = CheckedPCGMultiply(maximumPoints, sizeof(Composite) * 2);
        if (scratch.HasError())
            return Result<PointGenerationCharge>::Failure(scratch.ErrorValue());
        std::size_t passes{};
        for (std::size_t remaining = maximumPoints; remaining > 1; remaining = remaining / 2 + remaining % 2)
            ++passes;
        const auto comparisons = CheckedPCGMultiply(passes, maximumPoints);
        if (comparisons.HasError())
            return Result<PointGenerationCharge>::Failure(comparisons.ErrorValue());
        const auto words = CheckedPCGMultiply(comparisons.Value(), 5);
        if (words.HasError())
            return Result<PointGenerationCharge>::Failure(words.ErrorValue());
        const auto linear = CheckedPCGMultiply(maximumPoints, 2);
        if (linear.HasError())
            return Result<PointGenerationCharge>::Failure(linear.ErrorValue());
        const auto visits = CheckedPCGAdd(words.Value(), linear.Value());
        if (visits.HasError())
            return Result<PointGenerationCharge>::Failure(visits.ErrorValue());
        return Result<PointGenerationCharge>::Success({scratch.Value(), visits.Value(), 0});
    }

    /** @copydoc ValidatePointIdentities */
    Result<void> ValidatePointIdentities(const std::span<const PCGPointReadView> inputs, CancellationToken cancellation) {
        std::size_t count{};
        for (const auto &input : inputs) {
            const auto total = CheckedPCGAdd(count, input.PointCount());
            if (total.HasError())
                return Result<void>::Failure(total.ErrorValue());
            count = total.Value();
        }
        if (cancellation.IsCancellationRequested())
            return Reject<void>(PCGErrors::CpuEvaluationClosed);
        try {
            std::vector<Composite> records(count);
            std::vector<Composite> temporary(count);
            std::size_t offset{};
            for (const auto &input : inputs) {
                if (const auto copied = CopyIdentities(input, std::span(records).subspan(offset, input.PointCount()), cancellation);
                    copied.HasError())
                    return copied;
                offset += input.PointCount();
            }
            return UniqueIdentities(records, temporary, cancellation);
        } catch (const std::bad_alloc &) {
            return Reject<void>(PCGErrors::CpuEvaluationCapacityExceeded);
        }
    }
}  // namespace Horo::PCG::detail
