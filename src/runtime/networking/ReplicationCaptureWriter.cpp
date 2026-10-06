#include "ReplicationStateCaptureInternal.h"

#include <algorithm>
#include <cmath>

namespace Horo::Network {
    namespace ReplicationCaptureErrors {
        const ErrorCodeDescriptor CallbackFault{ErrorDomainId{"network"}, ErrorCode{"network.capture.callback_fault"}, ErrorSeverity::Error,
                                                "Committed replication callback failed unexpectedly.",
                                                "Repair the declaring owner or codec before retrying capture."};
        const ErrorCodeDescriptor Invalid{ErrorDomainId{"network"}, ErrorCode{"network.capture.invalid"}, ErrorSeverity::Error,
                                          "Invalid committed replication capture.", "Validate declared field types and capture order."};
        const ErrorCodeDescriptor Capacity{ErrorDomainId{"network"}, ErrorCode{"network.capture.capacity"}, ErrorSeverity::Error,
                                           "Prepared replication capture capacity exhausted.",
                                           "Release pins or revise finite capture budgets."};
        const ErrorCodeDescriptor Stale{ErrorDomainId{"network"}, ErrorCode{"network.capture.stale"}, ErrorSeverity::Error,
                                        "Replication capture generation is stale.", "Compose a complete current owner generation."};
        const ErrorCodeDescriptor Uncommitted{ErrorDomainId{"network"}, ErrorCode{"network.capture.uncommitted"}, ErrorSeverity::Error,
                                              "Canonical owner state is not committed.", "Capture after the complete owner transaction."};
        const ErrorCodeDescriptor Closed{ErrorDomainId{"network"}, ErrorCode{"network.capture.closed"}, ErrorSeverity::Error,
                                         "Replication capture admission is closed.", "Prepare a new complete coordinator generation."};
    }  // namespace ReplicationCaptureErrors

    /** @copydoc ReplicationCapturedState::IsCurrent */
    bool ReplicationCapturedState::IsCurrent() const noexcept {
        return admission_ && admission_->load() && world_.IsCurrent();
    }

    /** @copydoc ReplicationCapturedState::Object */
    const NetworkObjectMappingEntry &ReplicationCapturedState::Object() const noexcept {
        return object_;
    }

    /** @copydoc ReplicationCapturedState::SimulationTick */
    std::uint64_t ReplicationCapturedState::SimulationTick() const noexcept {
        return simulationTick_;
    }

    /** @copydoc ReplicationCapturedState::SourceRevision */
    std::uint64_t ReplicationCapturedState::SourceRevision() const noexcept {
        return sourceRevision_;
    }

    /** @copydoc ReplicationCapturedState::CommitRevision */
    std::uint64_t ReplicationCapturedState::CommitRevision() const noexcept {
        return commitRevision_;
    }

    /** @copydoc ReplicationCapturedState::PublicationRevision */
    std::uint64_t ReplicationCapturedState::PublicationRevision() const noexcept {
        return publicationRevision_;
    }

    /** @copydoc ReplicationCapturedState::Descriptors */
    const ReplicationDescriptorSnapshotPtr &ReplicationCapturedState::Descriptors() const noexcept {
        return descriptors_;
    }

    /** @copydoc ReplicationCapturedState::Fields */
    std::span<const ReplicationCapturedField> ReplicationCapturedState::Fields() const noexcept {
        return fields_;
    }

    /** @copydoc ReplicationCaptureWriter::ReplicationCaptureWriter */
    ReplicationCaptureWriter::ReplicationCaptureWriter(ReplicationCapturedState &state,
                                                       const std::span<Detail::ReplicationPreparedCaptureField> fields) noexcept
        : state_(state), fields_(fields) {
        for (auto &field : fields_)
            field.written = false;
    }

    /** @brief Admits one field exactly once before touching prepared value storage. */
    Result<std::size_t> ReplicationCaptureWriter::Admit(const FieldId field, const ReplicationValueKind kind, const std::size_t elements) {
        const auto found = std::ranges::lower_bound(fields_, field, {}, &Detail::ReplicationPreparedCaptureField::field);
        if (found == fields_.end() || found->field != field || found->written || found->kind != kind) {
            failed_ = true;
            return Result<std::size_t>::Failure(MakeError(ReplicationCaptureErrors::Invalid));
        }
        if (elements > found->maximumElements) {
            failed_ = true;
            return Result<std::size_t>::Failure(MakeError(ReplicationCaptureErrors::Capacity));
        }
        found->written = true;
        return Result<std::size_t>::Success(static_cast<std::size_t>(found - fields_.begin()));
    }

    /** @brief Rejects an incomplete candidate even if its owner ignored a writer error. */
    Result<void> ReplicationCaptureWriter::Complete() const {
        if (failed_ || !std::ranges::all_of(fields_, &Detail::ReplicationPreparedCaptureField::written))
            return Result<void>::Failure(MakeError(ReplicationCaptureErrors::Invalid));
        return Result<void>::Success();
    }

    /** @copydoc ReplicationCaptureWriter::Write */
    Result<void> ReplicationCaptureWriter::Write(const FieldId field, const bool value) {
        const auto index = Admit(field, ReplicationValueKind::Boolean, 1);
        if (index.HasError())
            return Result<void>::Failure(index.ErrorValue());
        state_.fields_[index.Value()].value = value;
        return Result<void>::Success();
    }

    /** @copydoc ReplicationCaptureWriter::Write */
    Result<void> ReplicationCaptureWriter::Write(const FieldId field, const std::int64_t value) {
        const auto index = Admit(field, ReplicationValueKind::SignedInteger, 1);
        if (index.HasError())
            return Result<void>::Failure(index.ErrorValue());
        state_.fields_[index.Value()].value = value;
        return Result<void>::Success();
    }

    /** @copydoc ReplicationCaptureWriter::Write */
    Result<void> ReplicationCaptureWriter::Write(const FieldId field, const std::uint64_t value) {
        const auto index = Admit(field, ReplicationValueKind::UnsignedInteger, 1);
        if (index.HasError())
            return Result<void>::Failure(index.ErrorValue());
        state_.fields_[index.Value()].value = value;
        return Result<void>::Success();
    }

    /** @copydoc ReplicationCaptureWriter::Write */
    Result<void> ReplicationCaptureWriter::Write(const FieldId field, const double value) {
        if (!std::isfinite(value)) {
            failed_ = true;
            return Result<void>::Failure(MakeError(ReplicationCaptureErrors::Invalid));
        }
        const auto index = Admit(field, ReplicationValueKind::FloatingPoint, 1);
        if (index.HasError())
            return Result<void>::Failure(index.ErrorValue());
        state_.fields_[index.Value()].value = value;
        return Result<void>::Success();
    }

    /** @copydoc ReplicationCaptureWriter::Write */
    Result<void> ReplicationCaptureWriter::Write(const FieldId field, const std::string_view value) {
        const auto index = Admit(field, ReplicationValueKind::Utf8Text, value.size());
        if (index.HasError())
            return Result<void>::Failure(index.ErrorValue());
        std::get<std::string>(state_.fields_[index.Value()].value).assign(value);
        return Result<void>::Success();
    }

    /** @copydoc ReplicationCaptureWriter::Write */
    Result<void> ReplicationCaptureWriter::Write(const FieldId field, const std::span<const std::byte> value) {
        const auto index = Admit(field, ReplicationValueKind::ByteSequence, value.size());
        if (index.HasError())
            return Result<void>::Failure(index.ErrorValue());
        auto &destination = std::get<std::vector<std::byte>>(state_.fields_[index.Value()].value);
        destination.assign(value.begin(), value.end());
        return Result<void>::Success();
    }
}  // namespace Horo::Network
