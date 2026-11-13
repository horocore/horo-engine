#include "PhysicsStateRestartInput.h"

#include "Horo/Physics/PhysicsErrors.h"

#include <limits>
#include <new>
#include <stdexcept>

namespace Horo::Physics::StateDetail {
    namespace {
        bool CountsFit(const SnapshotEnvelopeSections &sections, const RestartInputLimits &limits) noexcept {
            return sections.header.pairCount <= limits.maximumEventPairs && sections.header.eventCount <= limits.maximumEvents;
        }

        struct InputPlan final {
            SnapshotEnvelopeSections sections;
            PhysicsStateConstructionManifestLimits construction;
        };

        Result<InputPlan> PlanInput(std::span<const std::byte> bytes, const RestartInputLimits &limits, PhysicsStateFpControl actual) {
            using Outcome = Result<InputPlan>;
            if (limits.maximumRetainedBytes < sizeof(RestartInput) || bytes.size() > limits.maximumWireBytes)
                return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            auto sections = InspectSnapshotEnvelope(bytes);
            if (sections.HasError())
                return Outcome::Failure(sections.ErrorValue());
            if (!CountsFit(sections.Value(), limits))
                return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            if (sections.Value().header.compatibility.floatingPoint != actual)
                return Outcome::Failure(MakeError(PhysicsErrors::ProfileUnsupported));
            auto sized = SizeConstructionManifest(sections.Value().construction, limits.construction);
            if (sized.HasError())
                return Outcome::Failure(sized.ErrorValue());
            return Outcome::Success({sections.Value(), sized.Value()});
        }

        void AddBytes(std::size_t &total, std::size_t bytes) noexcept {
            total = bytes > std::numeric_limits<std::size_t>::max() - total ? std::numeric_limits<std::size_t>::max() : total + bytes;
        }

        template <class T> void AddVector(std::size_t &total, const std::vector<T> &values) noexcept {
            if (values.capacity() > std::numeric_limits<std::size_t>::max() / sizeof(T))
                total = std::numeric_limits<std::size_t>::max();
            else
                AddBytes(total, values.capacity() * sizeof(T));
        }
    }  // namespace

    /** @copydoc RestartInput::RetainedPayloadBytes */
    std::size_t RestartInput::RetainedPayloadBytes() const noexcept {
        auto total = sizeof(*this);
        if (solver_)
            AddBytes(total, solver_->RetainedPayloadBytes());
        if (construction_)
            AddBytes(total, construction_->RetainedPayloadBytes());
        AddVector(total, bytes_);
        AddVector(total, pairs_);
        AddVector(total, events_);
        AddVector(total, ordinals_);
        return total;
    }

    /** @copydoc RestartInput::Allocate */
    Result<void> RestartInput::Allocate(std::span<const std::byte> bytes, const RestartInputLimits &limits,
                                        const SnapshotEnvelopeSections &sections,
                                        const PhysicsStateConstructionManifestLimits &constructionLimits) {
        auto solver = PrepareSolverStorage(limits.solver);
        if (solver.HasError())
            return Result<void>::Failure(solver.ErrorValue());
        auto construction = ConstructionManifestOwner::Prepare(constructionLimits);
        if (construction.HasError())
            return Result<void>::Failure(construction.ErrorValue());
        solver_ = std::move(solver).Value();
        construction_ = std::move(construction).Value();
        bytes_.assign(bytes.begin(), bytes.end());
        pairs_.resize(sections.header.pairCount);
        events_.resize(sections.header.eventCount);
        ordinals_.resize(sections.header.eventCount);
        if (RetainedPayloadBytes() > limits.maximumRetainedBytes)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        return Result<void>::Success();
    }

    /** @copydoc RestartInput::Decode */
    Result<void> RestartInput::Decode(PhysicsStateFpControl actual) {
        auto parsed = DecodeSnapshotEnvelope(bytes_, *solver_, pairs_, events_, ordinals_);
        if (parsed.HasError())
            return Result<void>::Failure(parsed.ErrorValue());
        if (auto complete = construction_->Decode(parsed.Value().construction, actual); complete.HasError())
            return complete;
        envelope_ = parsed.Value();
        return Result<void>::Success();
    }

    /** @copydoc RestartInput::Prepare */
    Result<std::unique_ptr<RestartInput>> RestartInput::Prepare(std::span<const std::byte> bytes, const RestartInputLimits &limits,
                                                                PhysicsStateFpControl actual) {
        using Outcome = Result<std::unique_ptr<RestartInput>>;
        auto plan = PlanInput(bytes, limits, actual);
        if (plan.HasError())
            return Outcome::Failure(plan.ErrorValue());
        try {
            auto result = std::unique_ptr<RestartInput>{new RestartInput};
            if (auto allocated = result->Allocate(bytes, limits, plan.Value().sections, plan.Value().construction); allocated.HasError())
                return Outcome::Failure(allocated.ErrorValue());
            if (auto decoded = result->Decode(actual); decoded.HasError())
                return Outcome::Failure(decoded.ErrorValue());
            return Outcome::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        } catch (const std::length_error &) {
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        }
    }
}  // namespace Horo::Physics::StateDetail
