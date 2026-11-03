#include "PhysicsStateSolverCodecInternal.h"
#include "PhysicsStateValidationInternal.h"

namespace Horo::Physics::StateDetail {
    namespace {
        /** @brief Builds counts only after complete typed limits have admitted their
         * u32 representation. */
        SolverHeader Header(const PhysicsStateSolverView &state) noexcept {
            return {.previousDelta = state.previousDelta,
                    .gravity = state.gravity,
                    .counts = {static_cast<std::uint32_t>(state.bodies.size()), static_cast<std::uint32_t>(state.activeBodyOrder.size()),
                               static_cast<std::uint32_t>(state.constraints.size()), static_cast<std::uint32_t>(state.pairs.size()),
                               static_cast<std::uint32_t>(state.manifolds.size()), static_cast<std::uint32_t>(state.ccdManifolds.size())}};
        }

        /** @brief The writer must never overwrite a still-borrowed source record
         * halfway through serialization. */
        bool Aliases(const PhysicsStateSolverView &state, std::span<std::byte> bytes) noexcept {
            auto output = std::span{reinterpret_cast<std::uint8_t *>(bytes.data()), bytes.size()};
            return Overlaps(output, state.bodies) || Overlaps(output, state.activeBodyOrder) || Overlaps(output, state.constraints) ||
                   Overlaps(output, state.pairs) || Overlaps(output, state.manifolds) || Overlaps(output, state.ccdManifolds);
        }

        /** @brief Fixed record grammar contains every required scalar exactly once. */
        void WriteSolver(Wire &wire, const PhysicsStateSolverView &state) noexcept {
            WriteRows(wire, state.bodies, BodyFields);
            WriteRows(wire, state.activeBodyOrder, [](Wire &target, auto &id) {
                target.Integer(id);
            });
            WriteRows(wire, state.constraints, ConstraintFields);
            WriteRows(wire, state.pairs, PairFields);
            WriteRows(wire, state.manifolds, ManifoldFields);
            WriteRows(wire, state.ccdManifolds, ManifoldKeyFields);
        }
    }  // namespace

    /** @copydoc EncodeSolverState */
    Result<std::size_t> EncodeSolverState(const PhysicsStateSolverView &state, const PhysicsStateLimits &limits,
                                          std::span<std::uint8_t> scratch, std::span<std::byte> output) {
        if (auto valid = ValidatePhysicsStateSolver(state, limits, scratch); valid.HasError())
            return Result<std::size_t>::Failure(valid.ErrorValue());
        auto header = Header(state);
        std::size_t size{};
        if (!WireSize(header, output.size(), size))
            return Result<std::size_t>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        if (Aliases(state, output.first(size)))
            return Result<std::size_t>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "Physics state output aliases source records."));
        Wire wire{output.first(size)};
        HeaderFields(wire, header);
        WriteSolver(wire, state);
        return Result<std::size_t>::Success(wire.Size());
    }
}  // namespace Horo::Physics::StateDetail
