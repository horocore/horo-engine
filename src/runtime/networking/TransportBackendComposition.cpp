#include "Horo/Network/TransportBackendComposition.h"

#include "Horo/Network/NetworkErrors.h"

#include <algorithm>
#include <utility>

namespace Horo::Network {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }
    }  // namespace

    /** @copydoc TransportBackendId::IsValid */
    bool TransportBackendId::IsValid() const noexcept {
        if (value.empty() || value.size() > 64)
            return false;
        return std::ranges::all_of(value, [](const unsigned char character) {
            return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '.' ||
                   character == '-' || character == '_';
        });
    }

    /** @copydoc TransportBackendComposition::~TransportBackendComposition */
    TransportBackendComposition::~TransportBackendComposition() noexcept {
        Shutdown();
    }

    /** @copydoc TransportBackendComposition::Register */
    Result<void> TransportBackendComposition::Register(TransportBackendDescriptor descriptor) {
        if (state_ == TransportBackendCompositionState::Closed)
            return Failure<void>(NetworkErrors::TransportBackendShuttingDown);
        if (state_ == TransportBackendCompositionState::Cancelling)
            return Failure<void>(NetworkErrors::TransportBackendCancelled);
        if (state_ != TransportBackendCompositionState::Configuring)
            return Failure<void>(NetworkErrors::TransportBackendConflict);
        if (!descriptor.id.IsValid() || !ValidateTransportCapabilities(descriptor.capabilities) || !descriptor.factory)
            return Failure<void>(NetworkErrors::TransportBackendInvalid);
        if (std::ranges::any_of(descriptors_, [&descriptor](const auto &entry) {
            return entry.id == descriptor.id;
        }))
            return Failure<void>(NetworkErrors::TransportBackendConflict);
        if (descriptors_.size() >= MaximumBackends)
            return Failure<void>(NetworkErrors::TransportBackendCapacityExceeded);
        descriptors_.push_back(std::move(descriptor));
        return Result<void>::Success();
    }

    /** @copydoc TransportBackendComposition::Seal */
    Result<void> TransportBackendComposition::Seal() noexcept {
        if (state_ == TransportBackendCompositionState::Closed)
            return Failure<void>(NetworkErrors::TransportBackendShuttingDown);
        if (state_ == TransportBackendCompositionState::Cancelling)
            return Failure<void>(NetworkErrors::TransportBackendCancelled);
        if (state_ == TransportBackendCompositionState::Configuring)
            state_ = TransportBackendCompositionState::Sealed;
        return Result<void>::Success();
    }

    /** @copydoc TransportBackendComposition::Select */
    Result<void> TransportBackendComposition::Select(const TransportBackendId &id) {
        if (state_ == TransportBackendCompositionState::Closed)
            return Failure<void>(NetworkErrors::TransportBackendShuttingDown);
        if (state_ == TransportBackendCompositionState::Cancelling)
            return Failure<void>(NetworkErrors::TransportBackendCancelled);
        if (!id.IsValid() || state_ == TransportBackendCompositionState::Configuring)
            return Failure<void>(NetworkErrors::TransportBackendInvalid);
        if (state_ == TransportBackendCompositionState::Active || state_ == TransportBackendCompositionState::Selected)
            return selected_ == id ? Result<void>::Success() : Failure<void>(NetworkErrors::TransportBackendConflict);
        const auto found = std::ranges::find_if(descriptors_, [&id](const auto &entry) {
            return entry.id == id;
        });
        if (found == descriptors_.end())
            return Failure<void>(NetworkErrors::TransportBackendUnavailable);
        if (!found->hostSupported)
            return Failure<void>(NetworkErrors::TransportBackendUnsupported);
        if (!found->configured)
            return Failure<void>(NetworkErrors::TransportBackendNotConfigured);
        selected_ = id;
        state_ = TransportBackendCompositionState::Selected;
        return Result<void>::Success();
    }

    /** @copydoc TransportBackendComposition::Activate */
    Result<void> TransportBackendComposition::Activate() {
        if (state_ == TransportBackendCompositionState::Closed)
            return Failure<void>(NetworkErrors::TransportBackendShuttingDown);
        if (state_ == TransportBackendCompositionState::Cancelling)
            return Failure<void>(NetworkErrors::TransportBackendCancelled);
        if (state_ == TransportBackendCompositionState::Active)
            return Result<void>::Success();
        if (state_ != TransportBackendCompositionState::Selected || !selected_.has_value())
            return Failure<void>(NetworkErrors::TransportBackendInvalid);
        const auto found = std::ranges::find_if(descriptors_, [this](const auto &entry) {
            return entry.id == *selected_;
        });
        if (found == descriptors_.end())
            return Failure<void>(NetworkErrors::TransportBackendUnavailable);
        Result<TransportBackendInstance> created = [&found]() {
            try {
                return found->factory();
            } catch (...) {
                return Failure<TransportBackendInstance>(NetworkErrors::TransportBackendFactoryFailed);
            }
        }();
        if (created.HasError())
            return Result<void>::Failure(created.ErrorValue());
        if (!created.Value())
            return Failure<void>(NetworkErrors::TransportBackendFactoryFailed);
        active_ = std::move(created).Value();
        state_ = TransportBackendCompositionState::Active;
        return Result<void>::Success();
    }

    /** @copydoc TransportBackendComposition::Status */
    Result<TransportBackendStatus> TransportBackendComposition::Status(const TransportBackendId &id) const {
        if (!id.IsValid())
            return Failure<TransportBackendStatus>(NetworkErrors::TransportBackendInvalid);
        const auto found = std::ranges::find_if(descriptors_, [&id](const auto &entry) {
            return entry.id == id;
        });
        if (found == descriptors_.end())
            return Result<TransportBackendStatus>::Success({});
        const bool selected = selected_ == id;
        return Result<TransportBackendStatus>::Success(
            {true, found->hostSupported, found->configured, selected, selected && state_ == TransportBackendCompositionState::Active});
    }

    /** @copydoc TransportBackendComposition::InstalledIds */
    std::vector<TransportBackendId> TransportBackendComposition::InstalledIds() const {
        std::vector<TransportBackendId> ids;
        ids.reserve(descriptors_.size());
        for (const auto &entry : descriptors_)
            ids.push_back(entry.id);
        return ids;
    }

    /** @copydoc TransportBackendComposition::BeginCancellation */
    void TransportBackendComposition::BeginCancellation() noexcept {
        if (state_ == TransportBackendCompositionState::Cancelling || state_ == TransportBackendCompositionState::Closed)
            return;
        state_ = TransportBackendCompositionState::Cancelling;
        if (active_)
            active_->RequestCancellation();
    }

    /** @copydoc TransportBackendComposition::Shutdown */
    void TransportBackendComposition::Shutdown() noexcept {
        if (state_ == TransportBackendCompositionState::Closed)
            return;
        BeginCancellation();
        if (active_) {
            active_->Shutdown();
            active_.reset();
        }
        selected_.reset();
        state_ = TransportBackendCompositionState::Closed;
    }
}  // namespace Horo::Network
