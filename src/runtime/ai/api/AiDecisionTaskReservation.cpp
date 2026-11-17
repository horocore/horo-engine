#include "Horo/AI/AiDecisionTaskReservation.h"

#include <utility>

namespace Horo::AI {
    /** @copydoc AiDecisionTaskReservation::AiDecisionTaskReservation(BlackboardInstanceBinding,std::shared_ptr<const
     * BlackboardSchema>,std::uint32_t,std::uint32_t,CancellationToken) */
    AiDecisionTaskReservation::AiDecisionTaskReservation(BlackboardInstanceBinding binding, std::shared_ptr<const BlackboardSchema> schema,
                                                         const std::uint32_t first, const std::uint32_t count, CancellationToken parent)
        : binding_(binding), schema_(std::move(schema)), first_(first), count_(count), cancellation_(parent),
          owner_(std::this_thread::get_id()) {}

    /** @copydoc AiDecisionTaskReservation::AiDecisionTaskReservation(AiDecisionTaskReservation&&) */
    AiDecisionTaskReservation::AiDecisionTaskReservation(AiDecisionTaskReservation &&other) noexcept
        : binding_(std::exchange(other.binding_, {})), schema_(std::move(other.schema_)), first_(std::exchange(other.first_, 0)),
          count_(std::exchange(other.count_, 0)), cancellation_(std::move(other.cancellation_)), owner_(other.owner_) {}

    /** @copydoc AiDecisionTaskReservation::operator= */
    AiDecisionTaskReservation &AiDecisionTaskReservation::operator=(AiDecisionTaskReservation &&other) noexcept {
        if (this != &other) {
            if (schema_)
                cancellation_.RequestCancellation();
            binding_ = std::exchange(other.binding_, {});
            schema_ = std::move(other.schema_);
            first_ = std::exchange(other.first_, 0);
            count_ = std::exchange(other.count_, 0);
            cancellation_ = std::move(other.cancellation_);
            owner_ = other.owner_;
        }
        return *this;
    }

    /** @copydoc AiDecisionTaskReservation::~AiDecisionTaskReservation */
    AiDecisionTaskReservation::~AiDecisionTaskReservation() {
        if (schema_)
            cancellation_.RequestCancellation();
    }

    /** @copydoc AiDecisionTaskReservation::CanAdmit */
    bool AiDecisionTaskReservation::CanAdmit() const noexcept {
        return schema_ && owner_ == std::this_thread::get_id() && !cancellation_.Token().IsCancellationRequested();
    }

    /** @copydoc AiDecisionTaskReservation::Binding */
    const BlackboardInstanceBinding &AiDecisionTaskReservation::Binding() const noexcept {
        return binding_;
    }

    /** @copydoc AiDecisionTaskReservation::Schema */
    std::shared_ptr<const BlackboardSchema> AiDecisionTaskReservation::Schema() const noexcept {
        return schema_;
    }

    /** @copydoc AiDecisionTaskReservation::FirstSlot */
    std::uint32_t AiDecisionTaskReservation::FirstSlot() const noexcept {
        return first_;
    }

    /** @copydoc AiDecisionTaskReservation::Capacity */
    std::uint32_t AiDecisionTaskReservation::Capacity() const noexcept {
        return count_;
    }

    /** @copydoc AiDecisionTaskReservation::Cancellation */
    CancellationToken AiDecisionTaskReservation::Cancellation() const noexcept {
        return schema_ ? cancellation_.Token() : CancellationToken{};
    }
}  // namespace Horo::AI
