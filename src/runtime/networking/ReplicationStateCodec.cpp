#include "ReplicationStateCodecInternal.h"

#include <new>

namespace Horo::Network {
    using namespace StateCodecDetail;

    namespace ReplicationStateErrors {
        const ErrorCodeDescriptor Invalid{ErrorDomainId{"network"}, ErrorCode{"network.state.invalid"}, ErrorSeverity::Error,
                                          "Invalid replication state record.", "Validate canonical framing and projection."};
        const ErrorCodeDescriptor Stale{ErrorDomainId{"network"}, ErrorCode{"network.state.stale"}, ErrorSeverity::Error,
                                        "Replication state identity or baseline is stale.",
                                        "Request full state for the current admitted generation."};
        const ErrorCodeDescriptor Capacity{ErrorDomainId{"network"}, ErrorCode{"network.state.capacity"}, ErrorSeverity::Error,
                                           "Replication state limit exceeded.", "Use the admitted finite record and value budgets."};
        const ErrorCodeDescriptor Closed{ErrorDomainId{"network"}, ErrorCode{"network.state.closed"}, ErrorSeverity::Error,
                                         "Replication state codec is closed.", "Compose a fresh admitted session projection."};
        const ErrorCodeDescriptor CallbackFault{ErrorDomainId{"network"}, ErrorCode{"network.state.callback_fault"}, ErrorSeverity::Error,
                                                "Replication field codec failed unexpectedly.", "Repair the declaring codec."};
    }  // namespace ReplicationStateErrors

    namespace {
        /** @brief Validates finite supported work/storage ceilings before allocating projection storage. */
        bool ValidLimits(const ReplicationStateCodecLimits &limits) noexcept {
            return limits.maximumFields > 0 && limits.maximumFields <= 4096 && limits.maximumWireBytes >= HeaderBytes &&
                   limits.maximumWireBytes <= 16 * 1024 * 1024 && limits.maximumValueBytes > 0 &&
                   limits.maximumValueBytes <= 16 * 1024 * 1024;
        }

        /** @brief Evaluates exact custom evidence once during projection preparation. */
        Result<bool> Visible(const ReplicationFieldDescriptor &field, const ReplicationRoleBinding &recipient,
                             const ReplicationRecordKind record, const std::span<const ReplicationStateCustomVisibility> custom) {
            const auto found = std::ranges::find(custom, field.id, &ReplicationStateCustomVisibility::field);
            return EvaluateReplicationCondition(field, recipient, record,
                                                found == custom.end() ? std::nullopt : std::optional{found->evidence});
        }

        /** @brief Rejects duplicate or foreign custom evidence instead of silently ignoring it. */
        bool ValidCustom(const ReplicationSchemaDescriptor &schema, const std::span<const ReplicationStateCustomVisibility> custom) {
            if (custom.size() > schema.fields.size())
                return false;
            for (std::size_t index{}; index < custom.size(); ++index) {
                const auto found = std::ranges::find(schema.fields, custom[index].field, &ReplicationFieldDescriptor::id);
                if (found == schema.fields.end() || found->condition != ReplicationCondition::Custom ||
                    std::ranges::find(custom.first(index), custom[index].field, &ReplicationStateCustomVisibility::field) !=
                        custom.begin() + static_cast<std::ptrdiff_t>(index))
                    return false;
            }
            return true;
        }
    }  // namespace

    /** @copydoc ReplicationStateCodec::Create */
    Result<std::unique_ptr<ReplicationStateCodec>> ReplicationStateCodec::Create(
        std::shared_ptr<const ReplicationSerializerRegistry> serializers, const ReplicationRoleBinding &recipient,
        const ReplicationRecordKind record, const std::uint64_t descriptorGeneration,
        const std::span<const ReplicationStateCustomVisibility> custom, const ReplicationStateCodecLimits &limits) {
        if (!serializers || !recipient.IsValid() || descriptorGeneration == 0 || !ValidLimits(limits) ||
            (recipient.role != ReplicationExecutionRole::AutonomousClient && recipient.role != ReplicationExecutionRole::SimulatedClient))
            return Fail<std::unique_ptr<ReplicationStateCodec>>(ReplicationStateErrors::Invalid);
        const auto schema = serializers->Schemas()->Find(recipient.schema);
        if (schema.HasError())
            return Result<std::unique_ptr<ReplicationStateCodec>>::Failure(schema.ErrorValue());
        if (schema.Value()->version != recipient.schemaVersion || schema.Value()->fields.size() > limits.maximumFields ||
            !ValidCustom(*schema.Value(), custom))
            return Fail<std::unique_ptr<ReplicationStateCodec>>(ReplicationStateErrors::Invalid);
        try {
            std::vector<FieldId> projection;
            projection.reserve(schema.Value()->fields.size());
            std::size_t valueBytes{};
            std::vector<std::byte> identity;
            identity.reserve(schema.Value()->fields.size() * 4 + 1);
            Append(identity, static_cast<std::uint8_t>(record), 1);
            for (const auto &field : schema.Value()->fields) {
                const auto visible = Visible(field, recipient, record, custom);
                if (visible.HasError())
                    return Result<std::unique_ptr<ReplicationStateCodec>>::Failure(visible.ErrorValue());
                if (!visible.Value())
                    continue;
                const auto metadata = serializers->DescriptorFor(recipient.schema, field.id);
                if (metadata.HasError())
                    return Result<std::unique_ptr<ReplicationStateCodec>>::Failure(metadata.ErrorValue());
                // Text/byte containers retain one byte per element; scalar storage is bounded separately by maximumFields.
                if (metadata.Value()->valueKind == ReplicationValueKind::Utf8Text ||
                    metadata.Value()->valueKind == ReplicationValueKind::ByteSequence) {
                    const auto bound =
                        std::min(metadata.Value()->maximumElementCount, static_cast<std::size_t>(field.limits.maximumElementCount));
                    if (bound > limits.maximumValueBytes - valueBytes)
                        return Fail<std::unique_ptr<ReplicationStateCodec>>(ReplicationStateErrors::Capacity);
                    valueBytes += bound;
                }
                projection.push_back(field.id);
                Append(identity, field.id.Value(), 4);
            }
            const auto fingerprint = ComputeSha256(identity);
            return Result<std::unique_ptr<ReplicationStateCodec>>::Success(
                std::unique_ptr<ReplicationStateCodec>{new ReplicationStateCodec{std::move(serializers), recipient, descriptorGeneration,
                                                                                 limits, std::move(projection), fingerprint}});
        } catch (const std::bad_alloc &) {
            return Fail<std::unique_ptr<ReplicationStateCodec>>(ReplicationStateErrors::Capacity);
        }
    }

    /** @copydoc ReplicationStateCodec::ReplicationStateCodec */
    ReplicationStateCodec::ReplicationStateCodec(std::shared_ptr<const ReplicationSerializerRegistry> serializers,
                                                 ReplicationRoleBinding recipient, const std::uint64_t generation,
                                                 const ReplicationStateCodecLimits limits, std::vector<FieldId> projection,
                                                 const Sha256Digest fingerprint)
        : serializers_(std::move(serializers)), recipient_(std::move(recipient)), generation_(generation), limits_(limits),
          admission_(std::make_shared<std::atomic_bool>(true)), owner_(std::this_thread::get_id()), projection_(std::move(projection)),
          fingerprint_(fingerprint) {
        wireCapacity_ = HeaderBytes;
        const auto schema = serializers_->Schemas()->Find(recipient_.schema).Value();
        for (const auto field : projection_) {
            const auto descriptor = std::ranges::find(schema->fields, field, &ReplicationFieldDescriptor::id);
            const auto remaining = limits_.maximumWireBytes - wireCapacity_;
            // Preparation caps reservation by the actual projection rather than allocating the whole host record allowance.
            wireCapacity_ += std::min(remaining, FieldHeaderBytes + static_cast<std::size_t>(descriptor->limits.maximumEncodedBytes));
        }
    }

    /** @copydoc ReplicationStateCodec::~ReplicationStateCodec */
    ReplicationStateCodec::~ReplicationStateCodec() {
        admission_->store(false);
    }

    /** @copydoc ReplicationStateCodec::Shutdown */
    void ReplicationStateCodec::Shutdown() noexcept {
        if (owner_ == std::this_thread::get_id())
            admission_->store(false);
    }

    /** @brief Checks owner affinity, permanent session closure and caller cancellation. */
    Result<void> ReplicationStateCodec::Admit(const CancellationToken &cancellation) const {
        if (owner_ != std::this_thread::get_id())
            return Fail<void>(ReplicationStateErrors::Invalid);
        if (!admission_->load())
            return Fail<void>(ReplicationStateErrors::Closed);
        if (cancellation.IsCancellationRequested())
            return Fail<void>(NetworkErrors::ReplicationWorldCancelled);
        return Result<void>::Success();
    }

    /** @brief Fences captured source authority and exact composed immutable schema/session/object occurrence. */
    bool ReplicationStateCodec::CurrentSource(const ReplicationCapturedStatePin &source) const noexcept {
        return source && source->IsCurrent() && source->Descriptors() == serializers_->Schemas() &&
               source->World().Descriptor().session == recipient_.session &&
               source->World().Descriptor().role == ReplicationExecutionRole::AuthorityServer &&
               source->Object().object == recipient_.object && source->Object().provenance.schema == recipient_.schema &&
               source->Object().provenance.schemaVersion == recipient_.schemaVersion;
    }

    /** @brief Only acknowledged same-generation source pins may serve as a delta root. */
    bool ReplicationStateCodec::UsableBaseline(const ReplicationCapturedStatePin &source,
                                               const ReplicationAcknowledgedBaseline &baseline) const noexcept {
        return CurrentSource(baseline.state) && baseline.roleRevision == recipient_.revision &&
               baseline.descriptorGeneration == generation_ && baseline.projectionFingerprint == fingerprint_ &&
               baseline.publicationRevision == baseline.state->PublicationRevision() && baseline.state->Object() == source->Object() &&
               baseline.state->World().MappingRevision() == source->World().MappingRevision() &&
               baseline.state->SimulationTick() <= source->SimulationTick() &&
               baseline.publicationRevision <= source->PublicationRevision();
    }
}  // namespace Horo::Network
