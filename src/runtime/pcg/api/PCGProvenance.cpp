#include "Horo/PCG/PCGProvenance.h"

#include "Horo/PCG/PCGErrors.h"

#include <algorithm>
#include <tuple>
#include <utility>

namespace Horo::PCG {
    struct PCGProvenance::State final {
        State(PCGProvenanceCandidate candidate, const Sha256Digest digest) : data(std::move(candidate)), key(digest) {}

        PCGProvenanceCandidate data;
        Sha256Digest key;
    };

    namespace {
        constexpr std::size_t MaximumInputs = 64;
        constexpr std::size_t MaximumProviders = 64;
        constexpr std::size_t MaximumOutputs = 16'384;

        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Tests whether a digest contains usable canonical content evidence. */
        [[nodiscard]] bool HasContent(const Sha256Digest &digest) noexcept {
            return std::ranges::any_of(digest.bytes, [](const std::uint8_t byte) {
                return byte != 0;
            });
        }

        /** @brief Appends one fixed-width unsigned value in network byte order. */
        void Append64(std::vector<std::uint8_t> &bytes, std::uint64_t value) {
            for (int shift = 56; shift >= 0; shift -= 8)
                bytes.push_back(static_cast<std::uint8_t>(value >> shift));
        }

        /** @brief Appends a fixed-width canonical digest. */
        void AppendDigest(std::vector<std::uint8_t> &bytes, const Sha256Digest &digest) {
            bytes.insert(bytes.end(), digest.bytes.begin(), digest.bytes.end());
        }

        /** @brief Hashes a canonical byte buffer without hashing its storage address. */
        [[nodiscard]] Sha256Digest Hash(const std::vector<std::uint8_t> &bytes) noexcept {
            return ComputeSha256(std::as_bytes(std::span<const std::uint8_t>(bytes)));
        }

        /** @brief Encodes the entire validated provenance tuple with a versioned domain separator. */
        [[nodiscard]] Sha256Digest HashProvenance(const PCGProvenanceCandidate &candidate) {
            std::vector<std::uint8_t> bytes;
            bytes.reserve(256 + candidate.inputs.size() * 50 + candidate.providers.size() * 82);
            bytes.insert(bytes.end(), {'H', 'P', 'C', 'G', 'P', 'R', 'O', 'V', 1});
            Append64(bytes, candidate.graph.graph.Value());
            Append64(bytes, candidate.graph.revision.Value());
            AppendDigest(bytes, candidate.graphContent);
            Append64(bytes, candidate.graphSeed);
            Append64(bytes, candidate.world.Value());
            for (const std::int64_t coordinate : candidate.cell)
                Append64(bytes, static_cast<std::uint64_t>(coordinate));
            Append64(bytes, candidate.node.Value());
            bytes.push_back(static_cast<std::uint8_t>(candidate.determinism));
            bytes.push_back(static_cast<std::uint8_t>(candidate.numeric));
            Append64(bytes, candidate.numericPolicyVersion);
            AppendDigest(bytes, candidate.profile);
            Append64(bytes, candidate.inputs.size());
            for (const auto &input : candidate.inputs) {
                Append64(bytes, input.id.Value());
                Append64(bytes, input.revision);
                AppendDigest(bytes, input.content);
                bytes.push_back(static_cast<std::uint8_t>(input.determinism));
            }
            Append64(bytes, candidate.providers.size());
            for (const auto &provider : candidate.providers) {
                Append64(bytes, provider.provider.Value());
                Append64(bytes, provider.source.Value());
                Append64(bytes, provider.snapshot.Value());
                Append64(bytes, provider.revision.Value());
                Append64(bytes, provider.originEpoch);
                AppendDigest(bytes, provider.content);
                bytes.push_back(static_cast<std::uint8_t>(provider.determinism));
            }
            return Hash(bytes);
        }

        /** @brief Validates the closed class-to-numeric mapping and certified profile evidence. */
        [[nodiscard]] bool ValidTier(const PCGProvenanceCandidate &candidate) noexcept {
            switch (candidate.determinism) {
                case PCGDeterminismClass::PortableDeterministic:
                    return candidate.numeric == PCGNumericSupport::PortableInteger && !HasContent(candidate.profile);
                case PCGDeterminismClass::ProfileDeterministic:
                    return candidate.numeric == PCGNumericSupport::CertifiedProfileFloat && HasContent(candidate.profile);
                case PCGDeterminismClass::BestEffortPreview:
                    return candidate.numeric == PCGNumericSupport::PreviewOnly && !HasContent(candidate.profile);
                case PCGDeterminismClass::Count:
                    return false;
            }
            return false;
        }

        /** @brief Returns whether the captured tier promises reproducible output. */
        [[nodiscard]] bool IsDeterministic(const PCGProvenanceCandidate &candidate) noexcept {
            return candidate.determinism == PCGDeterminismClass::PortableDeterministic ||
                   candidate.determinism == PCGDeterminismClass::ProfileDeterministic;
        }
    }  // namespace

    /** @copydoc PCGProvenance::Data */
    const PCGProvenanceCandidate &PCGProvenance::Data() const noexcept {
        return state_->data;
    }

    /** @copydoc PCGProvenance::Key */
    Sha256Digest PCGProvenance::Key() const noexcept {
        return state_->key;
    }

    /** @copydoc PCGProvenance::Seed */
    Result<std::uint64_t> PCGProvenance::Seed(const SourceSampleId sample) const {
        if (!sample.IsValid())
            return Failure<std::uint64_t>(PCGErrors::ProvenanceInvalid);
        if (!IsDeterministic(Data()))
            return Failure<std::uint64_t>(PCGErrors::ProvenanceTierUnsupported);
        std::array<std::uint8_t, 49> bytes{'H', 'P', 'C', 'G', 'S', 'E', 'E', 'D', 1};
        const Sha256Digest key = Key();
        std::ranges::copy(key.bytes, bytes.begin() + 9);
        const auto sampleBytes = SerializeStableIdentity(sample);
        std::ranges::copy(sampleBytes, bytes.begin() + 41);
        const Sha256Digest digest = ComputeSha256(std::as_bytes(std::span<const std::uint8_t>(bytes)));
        std::uint64_t seed{};
        for (std::size_t index = 0; index < sizeof(seed); ++index)
            seed = (seed << 8U) | digest.bytes[index];
        return Result<std::uint64_t>::Success(seed);
    }

    /** @copydoc CapturePCGProvenance */
    Result<PCGProvenance> CapturePCGProvenance(PCGProvenanceCandidate candidate, const PCGProvenanceAdmission admission) {
        if (admission != PCGProvenanceAdmission::Accepting)
            return Failure<PCGProvenance>(PCGErrors::ProvenanceLifecycleUnavailable);
        if (candidate.inputs.size() > MaximumInputs || candidate.providers.size() > MaximumProviders)
            return Failure<PCGProvenance>(PCGErrors::ProvenanceCapacityExceeded);
        if (!candidate.graph.IsValid() || !candidate.world.IsValid() || !candidate.node.IsValid() || candidate.numericPolicyVersion == 0 ||
            !HasContent(candidate.graphContent))
            return Failure<PCGProvenance>(PCGErrors::ProvenanceInvalid);
        if (!ValidTier(candidate))
            return Failure<PCGProvenance>(PCGErrors::ProvenanceTierUnsupported);

        std::ranges::sort(candidate.inputs, {}, &PCGInputStamp::id);
        for (std::size_t index = 0; index < candidate.inputs.size(); ++index) {
            const auto &input = candidate.inputs[index];
            if (!input.id.IsValid() || input.revision == 0 || !HasContent(input.content) || input.determinism == PCGInputDeterminism::Count)
                return Failure<PCGProvenance>(PCGErrors::ProvenanceInvalid);
            if (index != 0 && candidate.inputs[index - 1].id == input.id)
                return Failure<PCGProvenance>(PCGErrors::ProvenanceDuplicate);
            if (IsDeterministic(candidate) && input.determinism == PCGInputDeterminism::NonDeterministic)
                return Failure<PCGProvenance>(PCGErrors::ProvenanceTierUnsupported);
        }

        std::ranges::sort(candidate.providers, [](const PCGProviderStamp &left, const PCGProviderStamp &right) {
            return std::pair{left.provider, left.source} < std::pair{right.provider, right.source};
        });
        for (std::size_t index = 0; index < candidate.providers.size(); ++index) {
            const auto &provider = candidate.providers[index];
            if (!provider.provider.IsValid() || !provider.source.IsValid() || !provider.snapshot.IsValid() ||
                !provider.revision.IsValid() || provider.originEpoch == 0 || !HasContent(provider.content) ||
                provider.determinism == PCGInputDeterminism::Count)
                return Failure<PCGProvenance>(PCGErrors::ProvenanceInvalid);
            if (index != 0 && candidate.providers[index - 1].provider == provider.provider &&
                candidate.providers[index - 1].source == provider.source)
                return Failure<PCGProvenance>(PCGErrors::ProvenanceDuplicate);
            if (IsDeterministic(candidate) && provider.determinism == PCGInputDeterminism::NonDeterministic)
                return Failure<PCGProvenance>(PCGErrors::ProvenanceTierUnsupported);
        }

        const Sha256Digest key = HashProvenance(candidate);
        return Result<PCGProvenance>::Success(
            PCGProvenance{PCGProvenance::ConstructionKey{}, std::make_shared<const PCGProvenance::State>(std::move(candidate), key)});
    }

    /** @copydoc ValidatePCGProvenanceReuse */
    Result<void> ValidatePCGProvenanceReuse(const PCGProvenance &captured, const PCGProvenance &current) {
        if (!IsDeterministic(captured.Data()) || !IsDeterministic(current.Data()))
            return Failure<void>(PCGErrors::ProvenanceTierUnsupported);
        if (captured.Key() != current.Key())
            return Failure<void>(PCGErrors::ProvenanceStale);
        return Result<void>::Success();
    }

    /** @copydoc HashPCGOutputs */
    Result<Sha256Digest> HashPCGOutputs(const PCGProvenance &provenance, const std::span<const PCGOutputStamp> outputs) {
        if (!IsDeterministic(provenance.Data()))
            return Failure<Sha256Digest>(PCGErrors::ProvenanceTierUnsupported);
        if (outputs.size() > MaximumOutputs)
            return Failure<Sha256Digest>(PCGErrors::ProvenanceCapacityExceeded);
        std::vector<PCGOutputStamp> ordered(outputs.begin(), outputs.end());
        std::ranges::sort(ordered, [](const PCGOutputStamp &left, const PCGOutputStamp &right) {
            return std::tuple{left.id.node, left.id.pin, left.id.sample, left.id.ordinal} <
                   std::tuple{right.id.node, right.id.pin, right.id.sample, right.id.ordinal};
        });
        for (std::size_t index = 0; index < ordered.size(); ++index) {
            const auto &output = ordered[index];
            if (!output.id.IsValid() || output.id.execution.generation != provenance.Data().graph ||
                output.id.node != provenance.Data().node || !HasContent(output.content))
                return Failure<Sha256Digest>(PCGErrors::ProvenanceInvalid);
            if (index != 0 && ordered[index - 1].id.execution != output.id.execution)
                return Failure<Sha256Digest>(PCGErrors::ProvenanceInvalid);
            if (index != 0 && ordered[index - 1].id.node == output.id.node && ordered[index - 1].id.pin == output.id.pin &&
                ordered[index - 1].id.sample == output.id.sample && ordered[index - 1].id.ordinal == output.id.ordinal)
                return Failure<Sha256Digest>(PCGErrors::ProvenanceDuplicate);
        }
        std::vector<std::uint8_t> bytes;
        bytes.reserve(64 + ordered.size() * 60);
        bytes.insert(bytes.end(), {'H', 'P', 'C', 'G', 'O', 'U', 'T', 1});
        AppendDigest(bytes, provenance.Key());
        Append64(bytes, ordered.size());
        for (const auto &output : ordered) {
            // Execution values identify attempts, not semantic output bytes.
            Append64(bytes, output.id.node.Value());
            Append64(bytes, output.id.pin.Value());
            Append64(bytes, output.id.sample.Value());
            Append64(bytes, output.id.ordinal);
            AppendDigest(bytes, output.content);
        }
        return Result<Sha256Digest>::Success(Hash(bytes));
    }
}  // namespace Horo::PCG
