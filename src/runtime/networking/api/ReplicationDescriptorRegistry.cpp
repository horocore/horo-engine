#include "Horo/Network/ReplicationDescriptorRegistry.h"

#include "Horo/Network/NetworkErrors.h"

#include <algorithm>
#include <format>
#include <limits>
#include <new>
#include <string_view>
#include <utility>

namespace Horo::Network {
    namespace {
        /** @brief Canonical big-endian byte builder for stable cross-platform schema fingerprints. */
        class FingerprintBytes final {
        public:
            void U8(const std::uint8_t value) {
                bytes_.push_back(static_cast<std::byte>(value));
            }

            void U16(const std::uint16_t value) {
                U8(static_cast<std::uint8_t>(value >> 8U));
                U8(static_cast<std::uint8_t>(value));
            }

            void U32(const std::uint32_t value) {
                for (int shift = 24; shift >= 0; shift -= 8)
                    U8(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
            }

            void U64(const std::uint64_t value) {
                for (int shift = 56; shift >= 0; shift -= 8)
                    U8(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
            }

            void Size(const std::size_t value) {
                U64(static_cast<std::uint64_t>(value));
            }

            void Text(const std::string_view value) {
                Size(value.size());
                for (const char character : value)
                    U8(static_cast<std::uint8_t>(character));
            }

            void Bytes(const std::span<const std::byte> value) {
                Size(value.size());
                bytes_.insert(bytes_.end(), value.begin(), value.end());
            }

            [[nodiscard]] std::span<const std::byte> View() const noexcept {
                return bytes_;
            }

        private:
            std::vector<std::byte> bytes_;
        };

        /** @brief Appends one schema version to the canonical fingerprint stream. */
        void Append(FingerprintBytes &bytes, const ReplicationSchemaVersion version) {
            bytes.U16(version.major);
            bytes.U16(version.minor);
        }

        /** @brief Computes a semantic digest independent of input order, names, paths, and storage layout. */
        [[nodiscard]] Sha256Digest Fingerprint(const std::span<const ReplicationSchemaDescriptor> schemas) {
            FingerprintBytes bytes;
            bytes.Text("horo.network.replication-schema-set.v1");
            bytes.Size(schemas.size());
            for (const ReplicationSchemaDescriptor &schema : schemas) {
                bytes.U64(schema.id.Value());
                Append(bytes, schema.version);
                Append(bytes, schema.compatibility.minimum);
                Append(bytes, schema.compatibility.maximum);
                bytes.Text(schema.owner.value);
                bytes.Size(schema.fields.size());
                for (const ReplicationFieldDescriptor &field : schema.fields) {
                    bytes.U32(field.id.Value());
                    bytes.U32(field.valueType.Value());
                    bytes.U32(field.codec.Value());
                    Append(bytes, field.introducedVersion);
                    bytes.U8(static_cast<std::uint8_t>(field.condition));
                    bytes.U8(static_cast<std::uint8_t>(field.requirement));
                    bytes.U8(static_cast<std::uint8_t>(field.writePolicy));
                    bytes.U32(field.limits.maximumEncodedBytes);
                    bytes.U32(field.limits.maximumElementCount);
                    bytes.U8(field.canonicalDefault.has_value() ? 1U : 0U);
                    if (field.canonicalDefault.has_value())
                        bytes.Bytes(field.canonicalDefault->canonicalBytes);
                    bytes.U8(field.customCondition.has_value() ? 1U : 0U);
                    if (field.customCondition.has_value())
                        bytes.U32(field.customCondition->Value());
                }
                bytes.Size(schema.tombstonedFields.size());
                for (const FieldId tombstone : schema.tombstonedFields)
                    bytes.U32(tombstone.Value());
            }
            return ComputeSha256(bytes.View());
        }

        /** @brief Finds a schema in identity-sorted storage. */
        [[nodiscard]] const ReplicationSchemaDescriptor *FindSchema(const std::span<const ReplicationSchemaDescriptor> schemas,
                                                                    const ReplicationSchemaId id) noexcept {
            const auto found = std::ranges::lower_bound(schemas, id, {}, &ReplicationSchemaDescriptor::id);
            return found != schemas.end() && found->id == id ? std::to_address(found) : nullptr;
        }

        /** @brief Finds a field in identity-sorted storage. */
        [[nodiscard]] const ReplicationFieldDescriptor *FindField(const ReplicationSchemaDescriptor &schema, const FieldId id) noexcept {
            const auto found = std::ranges::lower_bound(schema.fields, id, {}, &ReplicationFieldDescriptor::id);
            return found != schema.fields.end() && found->id == id ? std::to_address(found) : nullptr;
        }

        /** @brief Retains deterministic attributed diagnostics without permitting overflow to publish a generation. */
        struct ReplacementDiagnostics final {
            Error error{MakeError(NetworkErrors::ReplicationDescriptorIncompatible)};
            std::size_t maximum{};
            bool overflowed{};

            void Add(const ReplicationSchemaDescriptor &previous, const ReplicationSchemaDescriptor *candidate, const std::string_view code,
                     const std::string_view reason, const std::optional<FieldId> field = {}) {
                if (error.diagnostics.size() == maximum) {
                    overflowed = true;
                    return;
                }
                error.diagnostics.push_back(
                    {.code = DiagnosticCode{std::string{code}},
                     .severity = DiagnosticSeverity::Error,
                     .message =
                         std::format("Module '{}', schema {} (prior {}.{}, candidate {}): {}.", previous.owner.value, previous.id.Value(),
                                     previous.version.major, previous.version.minor,
                                     candidate ? std::format("{}.{}", candidate->version.major, candidate->version.minor) : "unavailable",
                                     reason),
                     .path = field ? std::format("schemas[{}].fields[{}]", previous.id.Value(), field->Value())
                                   : std::format("schemas[{}]", previous.id.Value())});
            }
        };

        /** @brief Collects all forbidden stable field changes and retirements using the unchanged registry invariants. */
        void CollectPriorFields(const ReplicationSchemaDescriptor &previous, const ReplicationSchemaDescriptor &candidate,
                                ReplacementDiagnostics &diagnostics) {
            for (const auto &field : previous.fields) {
                if (const auto *replacement = FindField(candidate, field.id); replacement != nullptr) {
                    if (*replacement != field)
                        diagnostics.Add(previous, &candidate, "replication.field_semantics_changed", "stable field semantics changed",
                                        field.id);
                    continue;
                }
                if (!std::ranges::binary_search(candidate.tombstonedFields, field.id))
                    diagnostics.Add(previous, &candidate, "replication.field_retirement_missing", "removed field has no tombstone",
                                    field.id);
                if (candidate.compatibility.Contains(previous.version) && field.requirement == ReplicationFieldRequirement::Required)
                    diagnostics.Add(previous, &candidate, "replication.required_field_removed",
                                    "compatible projection removes a required field", field.id);
            }
        }

        /** @brief Collects every incompatible addition without changing major-version replacement admission. */
        void CollectAdditions(const ReplicationSchemaDescriptor &previous, const ReplicationSchemaDescriptor &candidate,
                              ReplacementDiagnostics &diagnostics) {
            if (!candidate.compatibility.Contains(previous.version))
                return;
            for (const auto &field : candidate.fields) {
                if (FindField(previous, field.id) == nullptr &&
                    (field.introducedVersion <= previous.version || field.requirement != ReplicationFieldRequirement::Optional ||
                     !field.canonicalDefault))
                    diagnostics.Add(previous, &candidate, "replication.addition_incompatible",
                                    "compatible addition is not new and optional with a canonical default", field.id);
            }
        }

        /** @brief Fully validated canonical storage awaiting immutable snapshot construction. */
        struct CandidateData final {
            std::vector<ReplicationSchemaDescriptor> schemas;
            Sha256Digest fingerprint;
        };

        /** @brief Canonicalizes and validates each schema while enforcing aggregate default capacity. */
        [[nodiscard]] Result<void> ValidateCandidateSchemas(std::vector<ReplicationSchemaDescriptor> &schemas,
                                                            const ReplicationDescriptorLimits &limits) {
            std::size_t totalDefaultBytes{};
            for (ReplicationSchemaDescriptor &schema : schemas) {
                std::ranges::sort(schema.fields, {}, &ReplicationFieldDescriptor::id);
                std::ranges::sort(schema.tombstonedFields);
                if (const Result<void> valid = ValidateReplicationSchemaDescriptor(schema, limits); valid.HasError())
                    return valid;
                for (const ReplicationFieldDescriptor &field : schema.fields) {
                    if (!field.canonicalDefault.has_value())
                        continue;
                    const std::size_t byteCount = field.canonicalDefault->canonicalBytes.size();
                    if (byteCount > limits.maximumTotalDefaultBytes - totalDefaultBytes)
                        return Result<void>::Failure(MakeError(NetworkErrors::ReplicationCapacityExceeded));
                    totalDefaultBytes += byteCount;
                }
            }
            return Result<void>::Success();
        }

        /** @brief Validates stable identity and compatible-minor rules across one schema replacement. */
        void CollectReplacementSchema(const ReplicationSchemaDescriptor &previous, const ReplicationSchemaDescriptor &candidate,
                                      ReplacementDiagnostics &diagnostics) {
            if (candidate.owner != previous.owner)
                diagnostics.Add(previous, &candidate, "replication.owner_mismatch", "declaring owner changed");
            if (candidate.version < previous.version)
                diagnostics.Add(previous, &candidate, "replication.version_regression", "schema version regressed");
            if (candidate.version == previous.version && candidate != previous)
                diagnostics.Add(previous, &candidate, "replication.unversioned_change",
                                "published schema changed without a version increment");
            for (const auto field : previous.tombstonedFields)
                if (!std::ranges::binary_search(candidate.tombstonedFields, field))
                    diagnostics.Add(previous, &candidate, "replication.tombstone_missing", "retired identity was not retained", field);
            CollectPriorFields(previous, candidate, diagnostics);
            CollectAdditions(previous, candidate, diagnostics);
        }

        /** @brief Copies, canonicalizes, validates, and hashes one complete candidate. */
        [[nodiscard]] Result<CandidateData> BuildCandidateData(const std::span<const ReplicationSchemaDescriptor> descriptors,
                                                               const ReplicationDescriptorLimits &limits) {
            try {
                if (limits.maximumSchemas == 0 || descriptors.empty())
                    return Result<CandidateData>::Failure(MakeError(NetworkErrors::ReplicationDescriptorInvalid));
                if (descriptors.size() > limits.maximumSchemas)
                    return Result<CandidateData>::Failure(MakeError(NetworkErrors::ReplicationCapacityExceeded));

                std::vector<ReplicationSchemaDescriptor> schemas{descriptors.begin(), descriptors.end()};
                std::ranges::sort(schemas, {}, &ReplicationSchemaDescriptor::id);
                if (std::ranges::adjacent_find(schemas, {}, &ReplicationSchemaDescriptor::id) != schemas.end())
                    return Result<CandidateData>::Failure(MakeError(NetworkErrors::ReplicationDescriptorConflict));
                if (const Result<void> valid = ValidateCandidateSchemas(schemas, limits); valid.HasError())
                    return Result<CandidateData>::Failure(valid.ErrorValue());

                const Sha256Digest fingerprint = Fingerprint(schemas);
                return Result<CandidateData>::Success(CandidateData{std::move(schemas), fingerprint});
            } catch (const std::bad_alloc &) {
                return Result<CandidateData>::Failure(MakeError(NetworkErrors::ReplicationCapacityExceeded));
            }
        }
    }  // namespace

    /** @copydoc ReplicationDescriptorSnapshot::Schemas */
    std::span<const ReplicationSchemaDescriptor> ReplicationDescriptorSnapshot::Schemas() const noexcept {
        return schemas_;
    }

    /** @copydoc ReplicationDescriptorSnapshot::Find */
    Result<const ReplicationSchemaDescriptor *> ReplicationDescriptorSnapshot::Find(const ReplicationSchemaId id) const {
        const ReplicationSchemaDescriptor *found = FindSchema(schemas_, id);
        if (found == nullptr)
            return Result<const ReplicationSchemaDescriptor *>::Failure(MakeError(NetworkErrors::ReplicationSchemaUnknown));
        return Result<const ReplicationSchemaDescriptor *>::Success(found);
    }

    /** @copydoc ReplicationDescriptorSnapshot::Fingerprint */
    const Sha256Digest &ReplicationDescriptorSnapshot::Fingerprint() const noexcept {
        return fingerprint_;
    }

    /** @brief Owns already canonicalized descriptors and their semantic fingerprint. */
    ReplicationDescriptorSnapshot::ReplicationDescriptorSnapshot(ConstructionKey, std::vector<ReplicationSchemaDescriptor> schemas,
                                                                 const Sha256Digest &fingerprint)
        : schemas_(std::move(schemas)), fingerprint_(fingerprint) {}

    /** @copydoc BuildReplicationDescriptorSnapshot */
    Result<ReplicationDescriptorSnapshotPtr> BuildReplicationDescriptorSnapshot(
        const std::span<const ReplicationSchemaDescriptor> descriptors, const ReplicationDescriptorLimits &limits) {
        Result<CandidateData> candidate = BuildCandidateData(descriptors, limits);
        if (candidate.HasError())
            return Result<ReplicationDescriptorSnapshotPtr>::Failure(candidate.ErrorValue());
        try {
            CandidateData data = std::move(candidate).Value();
            return Result<ReplicationDescriptorSnapshotPtr>::Success(
                std::make_shared<ReplicationDescriptorSnapshot>(ReplicationDescriptorSnapshot::ConstructionKey{}, std::move(data.schemas),
                                                                data.fingerprint));
        } catch (const std::bad_alloc &) {
            return Result<ReplicationDescriptorSnapshotPtr>::Failure(MakeError(NetworkErrors::ReplicationCapacityExceeded));
        }
    }

    /** @copydoc BuildReplicationDescriptorReplacement */
    Result<ReplicationDescriptorSnapshotPtr> BuildReplicationDescriptorReplacement(
        const ReplicationDescriptorSnapshotPtr &previous, const std::span<const ReplicationSchemaDescriptor> descriptors,
        const ReplicationDescriptorLimits &limits) {
        return BuildReplicationDescriptorReplacement(previous, descriptors, limits, 1024);
    }

    /** @copydoc BuildReplicationDescriptorReplacement */
    Result<ReplicationDescriptorSnapshotPtr> BuildReplicationDescriptorReplacement(
        const ReplicationDescriptorSnapshotPtr &previous, const std::span<const ReplicationSchemaDescriptor> descriptors,
        const ReplicationDescriptorLimits &limits, const std::size_t maximumDiagnostics) {
        if (previous == nullptr || maximumDiagnostics == 0)
            return Result<ReplicationDescriptorSnapshotPtr>::Failure(MakeError(NetworkErrors::ReplicationDescriptorInvalid));
        Result<ReplicationDescriptorSnapshotPtr> candidate = BuildReplicationDescriptorSnapshot(descriptors, limits);
        if (candidate.HasError())
            return candidate;

        try {
            ReplacementDiagnostics diagnostics;
            diagnostics.maximum = maximumDiagnostics;
            for (const ReplicationSchemaDescriptor &previousSchema : previous->Schemas()) {
                const auto candidateSchema = candidate.Value()->Find(previousSchema.id);
                if (candidateSchema.HasError())
                    diagnostics.Add(previousSchema, nullptr, "replication.schema_missing", "required schema is missing");
                else
                    CollectReplacementSchema(previousSchema, *candidateSchema.Value(), diagnostics);
            }
            if (diagnostics.overflowed)
                diagnostics.error.diagnostics.push_back(
                    {.code = DiagnosticCode{"replication.diagnostic_overflow"},
                     .severity = DiagnosticSeverity::Error,
                     .message = "Replacement diagnostics exceeded the explicit limit; publication is denied."});
            if (!diagnostics.error.diagnostics.empty())
                return Result<ReplicationDescriptorSnapshotPtr>::Failure(std::move(diagnostics.error));
        } catch (const std::bad_alloc &) {
            return Result<ReplicationDescriptorSnapshotPtr>::Failure(MakeError(NetworkErrors::ReplicationCapacityExceeded));
        }
        return candidate;
    }
}  // namespace Horo::Network
