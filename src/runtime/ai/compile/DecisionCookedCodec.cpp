#include "DecisionBinary.h"
#include "DecisionCompileDetail.h"
#include "DecisionCompileLimits.h"
#include "Horo/AI/DecisionPlanCodec.h"

#include <algorithm>
#include <new>

namespace Horo::AI {
    namespace {
        constexpr std::uint64_t CookedMagic = 0x314b4f4f43494148ULL;

        template <typename T> Result<T> Invalid() {
            return Result<T>::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));
        }

        void WriteStorage(Detail::DecisionWriter &writer, const DecisionStorageRequirements &storage) {
            writer.Unsigned(storage.compiledBytes);
            writer.Unsigned(storage.coreInstanceBytes);
            writer.Unsigned(storage.scratchBytes);
            writer.Unsigned(storage.nativeDeclaredBytes);
            writer.Unsigned(storage.enforcedVmBytes);
            writer.Unsigned(storage.unknownNativeProviders);
            writer.Unsigned(storage.nativeDeclaredProviders);
            writer.Unsigned(storage.occurrences);
            writer.Unsigned(storage.maximumCallDepth);
            writer.Unsigned(storage.maximumExecutionDepth);
            writer.Unsigned(storage.taskSlots);
        }

        Result<std::span<const std::uint8_t>> AdmitEnvelope(std::span<const std::uint8_t> bytes, std::size_t maximum) {
            if (maximum == 0 || maximum > 16U * 1024U * 1024U || bytes.size() > maximum)
                return Invalid<std::span<const std::uint8_t>>();
            Detail::DecisionReader reader(bytes);
            if (reader.Unsigned() != CookedMagic || reader.Unsigned(4) != DecisionPlanCodec::CookedVersion)
                return Invalid<std::span<const std::uint8_t>>();
            const auto expected = reader.Bytes(Sha256Digest{}.bytes.size());
            const auto payload = reader.Bytes(reader.Count(maximum));
            if (!reader.Complete())
                return Invalid<std::span<const std::uint8_t>>();
            const auto actual = ComputeSha256(std::as_bytes(payload));
            if (!std::ranges::equal(expected, actual.bytes))
                return Invalid<std::span<const std::uint8_t>>();
            return Result<std::span<const std::uint8_t>>::Success(payload);
        }

        Result<std::span<const std::uint8_t>> ReadPayload(std::span<const std::uint8_t> payload,
                                                          const DecisionCompilationEnvironment &environment, DecisionCodecLimits limits) {
            Detail::DecisionReader reader(payload);
            if (reader.Unsigned(4) != environment.runtimeLayoutVersion)
                return Invalid<std::span<const std::uint8_t>>();
            const auto identity = reader.Bytes(Sha256Digest{}.bytes.size());
            auto current = Detail::DecisionEnvironmentIdentity(environment);
            if (current.HasError())
                return Result<std::span<const std::uint8_t>>::Failure(current.ErrorValue());
            if (!std::ranges::equal(identity, current.Value().bytes))
                return Invalid<std::span<const std::uint8_t>>();
            const auto source = reader.Bytes(reader.Count(limits.maximumBytes));
            // The complete layout declaration is verified by canonical re-encoding the newly admitted plan.
            for (std::size_t field = 0; field < 11; ++field)
                static_cast<void>(reader.Unsigned());
            if (!reader.Complete())
                return Invalid<std::span<const std::uint8_t>>();
            return Result<std::span<const std::uint8_t>>::Success(source);
        }
    }  // namespace

    Result<std::vector<std::uint8_t>> DecisionPlanCodec::EncodeCooked(const CookedDecisionPlan &plan, DecisionCodecLimits limits) {
        using Output = Result<std::vector<std::uint8_t>>;
        if (limits.maximumBytes == 0 || limits.maximumBytes > 16U * 1024U * 1024U || !Detail::ValidDecisionLimits(limits.compilation))
            return Invalid<std::vector<std::uint8_t>>();
        if (!plan.IsCurrent())
            return Output::Failure(MakeError(AIErrors::RuntimeUnavailable));
        try {
            Detail::DecisionWriter payload(limits.maximumBytes);
            payload.Unsigned(plan.runtimeLayoutVersion_, 4);
            payload.Bytes(plan.environmentIdentity_.bytes);
            payload.Count(plan.sourceBytes_.size(), limits.maximumBytes);
            payload.Bytes(plan.sourceBytes_);
            WriteStorage(payload, plan.storage_);
            if (!payload.Valid())
                return Invalid<std::vector<std::uint8_t>>();
            Detail::DecisionWriter envelope(limits.maximumBytes);
            envelope.Unsigned(CookedMagic);
            envelope.Unsigned(CookedVersion, 4);
            envelope.Bytes(ComputeSha256(std::as_bytes(payload.Data())).bytes);
            envelope.Count(payload.Data().size(), limits.maximumBytes);
            envelope.Bytes(payload.Data());
            if (!envelope.Valid())
                return Invalid<std::vector<std::uint8_t>>();
            return Output::Success(std::move(envelope).Take());
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(AIErrors::DecisionAssetStorageUnavailable));
        }
    }

    Result<DecisionGraphCompilation> DecisionPlanCodec::DecodeCooked(std::span<const std::uint8_t> bytes,
                                                                     const DecisionCompilationEnvironment &environment,
                                                                     DecisionCodecLimits limits) {
        auto payload = AdmitEnvelope(bytes, limits.maximumBytes);
        if (payload.HasError())
            return Result<DecisionGraphCompilation>::Failure(payload.ErrorValue());
        auto sourceBytes = ReadPayload(payload.Value(), environment, limits);
        if (sourceBytes.HasError())
            return Result<DecisionGraphCompilation>::Failure(sourceBytes.ErrorValue());
        DecisionGraphAssetId root;
        auto sources = DecodeSource(sourceBytes.Value(), root, limits);
        if (sources.HasError())
            return Result<DecisionGraphCompilation>::Failure(sources.ErrorValue());
        auto candidate = DecisionGraphCompiler::Compile(root, sources.Value(), environment, limits.compilation);
        if (candidate.HasError() || !candidate.Value().plan)
            return candidate;
        if (candidate.Value().plan->Digest() != ComputeSha256(std::as_bytes(bytes)))
            return Invalid<DecisionGraphCompilation>();
        return candidate;
    }
}  // namespace Horo::AI
