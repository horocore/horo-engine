#include "Horo/PlatformServices/PlatformProgressionIdempotency.h"

#include "Horo/Foundation/Sha256.h"

#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Horo::PlatformServices {
    namespace {
        constexpr std::string_view MutationDomain = "horo.platform.progression.mutation.v2";

        [[nodiscard]] constexpr bool IsValidConfiguration(const PlatformProgressionIdempotencyConfig &config) noexcept {
            return config.maximumInFlight != 0 && config.maximumInFlight <= PlatformProgressionMaximumInFlightMutations;
        }

        class CanonicalWriter final {
        public:
            void AddByte(const std::byte value) noexcept {
                if (size_ == bytes_.size()) {
                    overflowed_ = true;
                    return;
                }
                bytes_[size_++] = value;
            }

            void AddU64(const std::uint64_t value) noexcept {
                for (int shift = 56; shift >= 0; shift -= 8)
                    AddByte(static_cast<std::byte>((value >> shift) & 0xffU));
            }

            void AddBytes(const std::span<const std::byte> values) noexcept {
                for (const auto value : values)
                    AddByte(value);
            }

            [[nodiscard]] std::span<const std::byte> Bytes() const noexcept {
                return {bytes_.data(), size_};
            }

            [[nodiscard]] bool IsValid() const noexcept {
                return !overflowed_;
            }

        private:
            std::array<std::byte, 256> bytes_{};
            std::size_t size_{};
            bool overflowed_{};
        };

        [[nodiscard]] bool IsKnown(const PlatformServiceIdKind kind) noexcept {
            using enum PlatformServiceIdKind;
            switch (kind) {
                case Achievement:
                case Leaderboard:
                case Stat:
                case PresenceStatus:
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool IsKnown(const PlatformProgressionMutationKind kind) noexcept {
            using enum PlatformProgressionMutationKind;
            switch (kind) {
                case UnlockOnce:
                case SetProgressMaximum:
                case SetStatMaximum:
                case SetStatMinimum:
                case SetStatSnapshot:
                case AddStatOnce:
                case SubmitBestScore:
                case ReplaceScoreAtRevision:
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool IsKnown(const ProgressionAuthorityMode authority) noexcept {
            using enum ProgressionAuthorityMode;
            switch (authority) {
                case LocalProduct:
                case AuthorityServer:
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool IsNumeric(const PlatformProgressionValue &value) noexcept {
            return std::holds_alternative<std::int64_t>(value) || std::holds_alternative<std::uint64_t>(value);
        }

        [[nodiscard]] bool RequiresRevision(const PlatformProgressionMutationKind kind) noexcept {
            return kind == PlatformProgressionMutationKind::SetStatSnapshot ||
                   kind == PlatformProgressionMutationKind::ReplaceScoreAtRevision;
        }

        [[nodiscard]] bool DefinitionMatches(const PlatformServiceIdKind definitionKind,
                                             const PlatformProgressionMutationKind mutationKind) noexcept {
            using enum PlatformProgressionMutationKind;
            using enum PlatformServiceIdKind;
            switch (mutationKind) {
                case UnlockOnce:
                case SetProgressMaximum:
                    return definitionKind == Achievement;
                case SetStatMaximum:
                case SetStatMinimum:
                case SetStatSnapshot:
                case AddStatOnce:
                    return definitionKind == Stat;
                case SubmitBestScore:
                case ReplaceScoreAtRevision:
                    return definitionKind == Leaderboard;
            }
            return false;
        }

        struct SemanticFields final {
            const PlatformProgressionSessionScope &scope;
            PlatformServiceIdKind definitionKind;
            PlatformServiceStableIdValue definition;
            PlatformProgressionMutationKind mutationKind;
            const PlatformProgressionValue &value;
            const std::optional<std::uint64_t> &expectedRevision;
            ProgressionAuthorityMode authority;
            PlatformProgressionPolicyRevision policy;
        };

        [[nodiscard]] bool ValidateSemanticFields(const SemanticFields &fields) noexcept {
            if (!fields.scope.IsValid() || !IsKnown(fields.definitionKind) || !fields.definition.IsValid() ||
                !IsKnown(fields.mutationKind) || !DefinitionMatches(fields.definitionKind, fields.mutationKind) ||
                !IsKnown(fields.authority) || !fields.policy.IsValid())
                return false;

            if (fields.mutationKind == PlatformProgressionMutationKind::UnlockOnce) {
                if (!std::holds_alternative<std::monostate>(fields.value) || fields.expectedRevision.has_value())
                    return false;
            } else if (fields.mutationKind == PlatformProgressionMutationKind::SetProgressMaximum) {
                if (!std::holds_alternative<std::uint64_t>(fields.value))
                    return false;
            } else if (!IsNumeric(fields.value)) {
                return false;
            }

            if (RequiresRevision(fields.mutationKind))
                return fields.expectedRevision.has_value() && *fields.expectedRevision != 0;
            return !fields.expectedRevision.has_value();
        }

        [[nodiscard]] Result<void> ValidateCandidate(const PlatformProgressionMutationCandidate &candidate) {
            if (!candidate.occurrence.IsValid() || !ValidateSemanticFields(SemanticFields{.scope = candidate.scope,
                                                                                          .definitionKind = candidate.definitionKind,
                                                                                          .definition = candidate.definition,
                                                                                          .mutationKind = candidate.kind,
                                                                                          .value = candidate.value,
                                                                                          .expectedRevision = candidate.expectedRevision,
                                                                                          .authority = candidate.authority,
                                                                                          .policy = candidate.policy}))
                return Result<void>::Failure(MakeError(PlatformProgressionErrors::InvalidMutation));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateEnvelope(const PlatformProgressionMutationEnvelope &envelope) {
            if (!envelope.mutation.IsValid())
                return Result<void>::Failure(MakeError(PlatformProgressionErrors::InvalidMutation));
            if (!ValidateSemanticFields(SemanticFields{.scope = envelope.scope,
                                                       .definitionKind = envelope.definitionKind,
                                                       .definition = envelope.definition,
                                                       .mutationKind = envelope.kind,
                                                       .value = envelope.value,
                                                       .expectedRevision = envelope.expectedRevision,
                                                       .authority = envelope.authority,
                                                       .policy = envelope.policy}))
                return Result<void>::Failure(MakeError(PlatformProgressionErrors::InvalidMutation));
            return Result<void>::Success();
        }

        void AppendCandidate(CanonicalWriter &writer, const PlatformProgressionMutationCandidate &candidate) noexcept {
            writer.AddBytes(candidate.occurrence.bytes);
            writer.AddU64(candidate.scope.provider.value);
            writer.AddU64(candidate.scope.session.value);
            writer.AddU64(candidate.scope.accessPolicy.value);
            for (const auto byte : candidate.scope.subjectPartition.digest.bytes)
                writer.AddByte(static_cast<std::byte>(byte));
            writer.AddByte(static_cast<std::byte>(candidate.definitionKind));
            writer.AddU64(candidate.definition.value);
            writer.AddByte(static_cast<std::byte>(candidate.kind));
            writer.AddByte(static_cast<std::byte>(candidate.value.index()));
            std::visit([&writer](const auto &value) {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, std::int64_t>)
                    writer.AddU64(static_cast<std::uint64_t>(value));
                else if constexpr (std::is_same_v<Value, std::uint64_t>)
                    writer.AddU64(value);
            }, candidate.value);
            writer.AddByte(static_cast<std::byte>(candidate.expectedRevision.has_value()));
            if (candidate.expectedRevision.has_value())
                writer.AddU64(*candidate.expectedRevision);
            writer.AddByte(static_cast<std::byte>(candidate.authority));
            writer.AddU64(candidate.policy.value);
        }
    }  // namespace

    /** @copydoc PlatformProgressionMutationEnvelope::IsValid */
    bool PlatformProgressionMutationEnvelope::IsValid() const noexcept {
        return ValidateSemanticFields(SemanticFields{.scope = scope,
                                                     .definitionKind = definitionKind,
                                                     .definition = definition,
                                                     .mutationKind = kind,
                                                     .value = value,
                                                     .expectedRevision = expectedRevision,
                                                     .authority = authority,
                                                     .policy = policy}) &&
               mutation.IsValid();
    }

    /** @copydoc GeneratePlatformProgressionMutationId */
    Result<PlatformProgressionMutationId> GeneratePlatformProgressionMutationId(const PlatformProgressionMutationCandidate &candidate) {
        if (const auto valid = ValidateCandidate(candidate); valid.HasError())
            return Result<PlatformProgressionMutationId>::Failure(valid.ErrorValue());

        CanonicalWriter writer;
        writer.AddBytes(std::as_bytes(std::span{MutationDomain.data(), MutationDomain.size()}));
        writer.AddByte(std::byte{2});
        AppendCandidate(writer, candidate);
        if (!writer.IsValid())
            return Result<PlatformProgressionMutationId>::Failure(MakeError(PlatformProgressionErrors::InvalidMutation));
        const auto digest = ComputeSha256(writer.Bytes());

        PlatformProgressionMutationId mutation;
        for (std::size_t index = 0; index < mutation.bytes.size(); ++index)
            mutation.bytes[index] = static_cast<std::byte>(digest.bytes[index]);
        if (!mutation.IsValid())
            mutation.bytes.back() = std::byte{1};
        return Result<PlatformProgressionMutationId>::Success(mutation);
    }

    /** @copydoc BuildPlatformProgressionMutationEnvelope */
    Result<PlatformProgressionMutationEnvelope> BuildPlatformProgressionMutationEnvelope(
        const PlatformProgressionMutationCandidate &candidate) {
        const auto mutation = GeneratePlatformProgressionMutationId(candidate);
        if (mutation.HasError())
            return Result<PlatformProgressionMutationEnvelope>::Failure(mutation.ErrorValue());
        return Result<PlatformProgressionMutationEnvelope>::Success(
            PlatformProgressionMutationEnvelope{.mutation = mutation.Value(),
                                                .scope = candidate.scope,
                                                .definitionKind = candidate.definitionKind,
                                                .definition = candidate.definition,
                                                .kind = candidate.kind,
                                                .value = candidate.value,
                                                .expectedRevision = candidate.expectedRevision,
                                                .authority = candidate.authority,
                                                .policy = candidate.policy});
    }

    /** @copydoc PlatformProgressionIdempotencyStore::PlatformProgressionIdempotencyStore */
    PlatformProgressionIdempotencyStore::PlatformProgressionIdempotencyStore(const PlatformProgressionIdempotencyConfig config)
        : config_(config) {
        if (IsValidConfiguration(config_))
            records_.reserve(config_.maximumInFlight);
    }

    /** @copydoc PlatformProgressionIdempotencyStore::Admit */
    Result<PlatformProgressionAdmission> PlatformProgressionIdempotencyStore::Admit(const PlatformProgressionMutationEnvelope &envelope) {
        if (!IsValidConfiguration(config_))
            return Result<PlatformProgressionAdmission>::Failure(MakeError(PlatformProgressionErrors::InvalidConfiguration));
        if (const auto valid = ValidateEnvelope(envelope); valid.HasError())
            return Result<PlatformProgressionAdmission>::Failure(valid.ErrorValue());

        std::lock_guard lock(mutex_);
        if (closed_)
            return Result<PlatformProgressionAdmission>::Failure(MakeError(PlatformProgressionErrors::Closed));

        if (const auto existing = std::ranges::find_if(records_,
                                                       [&envelope](const Record &record) {
            return record.envelope.mutation == envelope.mutation;
        });
            existing != records_.end()) {
            if (existing->envelope != envelope)
                return Result<PlatformProgressionAdmission>::Failure(MakeError(PlatformProgressionErrors::IdempotencyConflict));
            return Result<PlatformProgressionAdmission>::Success(
                PlatformProgressionAdmission{.mutation = envelope.mutation,
                                             .disposition = PlatformProgressionAdmissionDisposition::JoinedExisting});
        }
        if (records_.size() >= config_.maximumInFlight)
            return Result<PlatformProgressionAdmission>::Failure(MakeError(PlatformProgressionErrors::CapacityExceeded));

        records_.push_back(Record{.envelope = envelope});
        return Result<PlatformProgressionAdmission>::Success(
            PlatformProgressionAdmission{.mutation = envelope.mutation, .disposition = PlatformProgressionAdmissionDisposition::Started});
    }

    /** @copydoc PlatformProgressionIdempotencyStore::Retire */
    Result<PlatformProgressionRetireDisposition> PlatformProgressionIdempotencyStore::Retire(
        const PlatformProgressionMutationEnvelope &envelope) {
        if (!IsValidConfiguration(config_))
            return Result<PlatformProgressionRetireDisposition>::Failure(MakeError(PlatformProgressionErrors::InvalidConfiguration));
        if (const auto valid = ValidateEnvelope(envelope); valid.HasError())
            return Result<PlatformProgressionRetireDisposition>::Failure(valid.ErrorValue());

        std::lock_guard lock(mutex_);
        const auto existing = std::ranges::find_if(records_, [&envelope](const Record &record) {
            return record.envelope.mutation == envelope.mutation;
        });
        if (existing == records_.end())
            return Result<PlatformProgressionRetireDisposition>::Success(PlatformProgressionRetireDisposition::Unchanged);
        if (existing->envelope != envelope)
            return Result<PlatformProgressionRetireDisposition>::Failure(MakeError(PlatformProgressionErrors::IdempotencyConflict));
        records_.erase(existing);
        return Result<PlatformProgressionRetireDisposition>::Success(PlatformProgressionRetireDisposition::Retired);
    }

    /** @copydoc PlatformProgressionIdempotencyStore::Find */
    std::optional<PlatformProgressionMutationEnvelope> PlatformProgressionIdempotencyStore::Find(
        const PlatformProgressionMutationId &mutation, const PlatformProgressionSessionScope &scope) const {
        if (!mutation.IsValid() || !scope.IsValid())
            return std::nullopt;
        std::lock_guard lock(mutex_);
        const auto existing = std::ranges::find_if(records_, [&mutation, &scope](const Record &record) {
            return record.envelope.mutation == mutation && record.envelope.scope == scope;
        });
        if (existing == records_.end())
            return std::nullopt;
        return existing->envelope;
    }

    /** @copydoc PlatformProgressionIdempotencyStore::Shutdown */
    void PlatformProgressionIdempotencyStore::Shutdown() noexcept {
        std::lock_guard lock(mutex_);
        closed_ = true;
        records_.clear();
    }

    /** @copydoc PlatformProgressionIdempotencyStore::InFlightCount */
    std::size_t PlatformProgressionIdempotencyStore::InFlightCount() const {
        std::lock_guard lock(mutex_);
        return records_.size();
    }

    /** @copydoc PlatformProgressionIdempotencyStore::IsClosed */
    bool PlatformProgressionIdempotencyStore::IsClosed() const {
        std::lock_guard lock(mutex_);
        return closed_;
    }
}  // namespace Horo::PlatformServices
