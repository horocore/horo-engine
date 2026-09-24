#include "Horo/PlatformServices/PlatformProgressionIdempotency.h"

#include "Horo/Foundation/Sha256.h"

#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include <utility>

namespace Horo::PlatformServices {
    namespace {
        constexpr std::string_view MutationDomain = "horo.platform.progression.mutation.v1";

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
            switch (kind) {
                case PlatformServiceIdKind::Achievement:
                case PlatformServiceIdKind::Leaderboard:
                case PlatformServiceIdKind::Stat:
                case PlatformServiceIdKind::PresenceStatus:
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool IsKnown(const PlatformProgressionMutationKind kind) noexcept {
            switch (kind) {
                case PlatformProgressionMutationKind::UnlockOnce:
                case PlatformProgressionMutationKind::SetProgressMaximum:
                case PlatformProgressionMutationKind::SetStatMaximum:
                case PlatformProgressionMutationKind::SetStatMinimum:
                case PlatformProgressionMutationKind::SetStatSnapshot:
                case PlatformProgressionMutationKind::AddStatOnce:
                case PlatformProgressionMutationKind::SubmitBestScore:
                case PlatformProgressionMutationKind::ReplaceScoreAtRevision:
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool IsKnown(const ProgressionAuthorityMode authority) noexcept {
            switch (authority) {
                case ProgressionAuthorityMode::LocalProduct:
                case ProgressionAuthorityMode::AuthorityServer:
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
            switch (mutationKind) {
                case PlatformProgressionMutationKind::UnlockOnce:
                case PlatformProgressionMutationKind::SetProgressMaximum:
                    return definitionKind == PlatformServiceIdKind::Achievement;
                case PlatformProgressionMutationKind::SetStatMaximum:
                case PlatformProgressionMutationKind::SetStatMinimum:
                case PlatformProgressionMutationKind::SetStatSnapshot:
                case PlatformProgressionMutationKind::AddStatOnce:
                    return definitionKind == PlatformServiceIdKind::Stat;
                case PlatformProgressionMutationKind::SubmitBestScore:
                case PlatformProgressionMutationKind::ReplaceScoreAtRevision:
                    return definitionKind == PlatformServiceIdKind::Leaderboard;
            }
            return false;
        }

        [[nodiscard]] bool ValidateSemanticFields(const PlatformProgressionSessionScope &scope, const PlatformServiceIdKind definitionKind,
                                                  const PlatformServiceStableIdValue definition,
                                                  const PlatformProgressionMutationKind mutationKind, const PlatformProgressionValue &value,
                                                  const std::optional<std::uint64_t> expectedRevision,
                                                  const ProgressionAuthorityMode authority,
                                                  const PlatformProgressionPolicyRevision policy) noexcept {
            if (!scope.IsValid() || !IsKnown(definitionKind) || !definition.IsValid() || !IsKnown(mutationKind) ||
                !DefinitionMatches(definitionKind, mutationKind) || !IsKnown(authority) || !policy.IsValid())
                return false;

            if (mutationKind == PlatformProgressionMutationKind::UnlockOnce) {
                if (!std::holds_alternative<std::monostate>(value) || expectedRevision.has_value())
                    return false;
            } else if (!IsNumeric(value)) {
                return false;
            }

            if (RequiresRevision(mutationKind))
                return expectedRevision.has_value() && *expectedRevision != 0;
            return !expectedRevision.has_value();
        }

        [[nodiscard]] Result<void> ValidateCandidate(const PlatformProgressionMutationCandidate &candidate) {
            if (!candidate.occurrence.IsValid() ||
                !ValidateSemanticFields(candidate.scope, candidate.definitionKind, candidate.definition, candidate.kind, candidate.value,
                                        candidate.expectedRevision, candidate.authority, candidate.policy))
                return Result<void>::Failure(MakeError(PlatformProgressionErrors::InvalidMutation));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateEnvelope(const PlatformProgressionMutationEnvelope &envelope) {
            if (!envelope.mutation.IsValid() ||
                !ValidateSemanticFields(envelope.scope, envelope.definitionKind, envelope.definition, envelope.kind, envelope.value,
                                        envelope.expectedRevision, envelope.authority, envelope.policy))
                return Result<void>::Failure(MakeError(PlatformProgressionErrors::InvalidMutation));
            return Result<void>::Success();
        }

        void AppendCandidate(CanonicalWriter &writer, const PlatformProgressionMutationCandidate &candidate) noexcept {
            writer.AddBytes(candidate.occurrence.bytes);
            writer.AddU64(candidate.scope.provider.value);
            writer.AddU64(candidate.scope.session.value);
            writer.AddU64(candidate.scope.accessPolicy.value);
            writer.AddByte(static_cast<std::byte>(candidate.definitionKind));
            writer.AddU64(candidate.definition.value);
            writer.AddByte(static_cast<std::byte>(candidate.kind));
            writer.AddByte(static_cast<std::byte>(candidate.value.index()));
            if (const auto *signedValue = std::get_if<std::int64_t>(&candidate.value))
                writer.AddU64(static_cast<std::uint64_t>(*signedValue));
            else if (const auto *unsignedValue = std::get_if<std::uint64_t>(&candidate.value))
                writer.AddU64(*unsignedValue);
            writer.AddByte(static_cast<std::byte>(candidate.expectedRevision.has_value()));
            if (candidate.expectedRevision)
                writer.AddU64(*candidate.expectedRevision);
            writer.AddByte(static_cast<std::byte>(candidate.authority));
            writer.AddU64(candidate.policy.value);
        }
    }  // namespace

    /** @copydoc PlatformProgressionMutationEnvelope::IsValid */
    bool PlatformProgressionMutationEnvelope::IsValid() const noexcept {
        return ValidateSemanticFields(scope, definitionKind, definition, kind, value, expectedRevision, authority, policy) &&
               mutation.IsValid();
    }

    /** @copydoc GeneratePlatformProgressionMutationId */
    Result<PlatformProgressionMutationId> GeneratePlatformProgressionMutationId(const PlatformProgressionMutationCandidate &candidate) {
        const auto valid = ValidateCandidate(candidate);
        if (valid.HasError())
            return Result<PlatformProgressionMutationId>::Failure(valid.ErrorValue());

        CanonicalWriter writer;
        writer.AddBytes(std::as_bytes(std::span<const char>{MutationDomain.data(), MutationDomain.size()}));
        writer.AddByte(std::byte{1});
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
        records_.reserve(config_.maximumInFlight);
    }

    /** @copydoc PlatformProgressionIdempotencyStore::Admit */
    Result<PlatformProgressionAdmission> PlatformProgressionIdempotencyStore::Admit(const PlatformProgressionMutationEnvelope &envelope) {
        if (config_.maximumInFlight == 0)
            return Result<PlatformProgressionAdmission>::Failure(MakeError(PlatformProgressionErrors::InvalidConfiguration));
        const auto valid = ValidateEnvelope(envelope);
        if (valid.HasError())
            return Result<PlatformProgressionAdmission>::Failure(valid.ErrorValue());

        std::lock_guard lock(mutex_);
        if (closed_)
            return Result<PlatformProgressionAdmission>::Failure(MakeError(PlatformProgressionErrors::Closed));

        const auto existing = std::ranges::find_if(records_, [&envelope](const Record &record) {
            return record.envelope.mutation == envelope.mutation;
        });
        if (existing != records_.end()) {
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
        if (config_.maximumInFlight == 0)
            return Result<PlatformProgressionRetireDisposition>::Failure(MakeError(PlatformProgressionErrors::InvalidConfiguration));
        const auto valid = ValidateEnvelope(envelope);
        if (valid.HasError())
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
