#include "PhysicsStateSolverCodecInternal.h"
#include "PhysicsStateValidationInternal.h"

namespace Horo::Physics::StateDetail {
    namespace {
        /** @brief Every decoded count must fit existing cold storage before a single
         * row is touched. */
        bool Fits(const SolverHeader &header, const SolverStorage &storage) noexcept {
            const std::array capacities{storage.bodies.size(), storage.activeBodyOrder.size(), storage.constraints.size(),
                                        storage.pairs.size(),  storage.manifolds.size(),       storage.ccdManifolds.size()};
            for (std::size_t index = 0; index < capacities.size(); ++index)
                if (header.counts[index] > capacities[index])
                    return false;
            return true;
        }

        /** @brief Prevents a caller from presenting the staging records themselves as
         * immutable encoded input. */
        template <class T> bool Aliases(std::span<const std::byte> input, const std::vector<T> &values) noexcept {
            if (input.empty() || values.empty())
                return false;
            const auto begin = reinterpret_cast<std::uintptr_t>(input.data());
            const auto row = reinterpret_cast<std::uintptr_t>(values.data());
            return begin <= row ? row - begin < input.size() : begin - row < values.size() * sizeof(T);
        }

        /** @brief Every staging payload is checked before decoding can overwrite it. */
        bool OverlapsStorage(std::span<const std::byte> input, const SolverStorage &storage) noexcept {
            return Aliases(input, storage.bodies) || Aliases(input, storage.activeBodyOrder) || Aliases(input, storage.constraints) ||
                   Aliases(input, storage.pairs) || Aliases(input, storage.manifolds) || Aliases(input, storage.ccdManifolds) ||
                   Aliases(input, storage.activeBodyScratch);
        }

        /** @brief Decodes only independently admitted existing prefixes; never grows a
         * container. */
        PhysicsStateSolverView ReadSolver(Wire &wire, const SolverHeader &header, SolverStorage &storage) noexcept {
            auto bodies = std::span{storage.bodies}.first(header.counts[0]);
            auto active = std::span{storage.activeBodyOrder}.first(header.counts[1]);
            auto constraints = std::span{storage.constraints}.first(header.counts[2]);
            auto pairs = std::span{storage.pairs}.first(header.counts[3]);
            auto manifolds = std::span{storage.manifolds}.first(header.counts[4]);
            auto ccd = std::span{storage.ccdManifolds}.first(header.counts[5]);
            ReadRows(wire, bodies, BodyFields);
            ReadRows(wire, active, [](Wire &source, auto &id) {
                source.Integer(id);
            });
            ReadRows(wire, constraints, ConstraintFields);
            ReadRows(wire, pairs, PairFields);
            ReadRows(wire, manifolds, ManifoldFields);
            ReadRows(wire, ccd, ManifoldKeyFields);
            return {header.previousDelta, header.gravity, bodies, active, constraints, pairs, manifolds, ccd};
        }
    }  // namespace

    /** @copydoc DecodeSolverState */
    Result<PhysicsStateSolverView> DecodeSolverState(std::span<const std::byte> input, SolverStorage &staging) {
        Wire wire{input};
        SolverHeader header;
        HeaderFields(wire, header);
        std::size_t expected{};
        if (wire.Failed() || header.tag != SolverWireTag || header.version != PhysicsStateSolverSchemaVersion ||
            !WireSize(header, staging.limits.maximumBytes, expected) || expected != input.size())
            return Result<PhysicsStateSolverView>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "Physics solver state wire is invalid."));
        if (!Fits(header, staging))
            return Result<PhysicsStateSolverView>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        if (OverlapsStorage(input, staging))
            return Result<PhysicsStateSolverView>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "Physics state staging aliases its input."));
        staging.used = {};
        const auto state = ReadSolver(wire, header, staging);
        if (!wire.Complete())
            return Result<PhysicsStateSolverView>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "Physics solver state contains malformed scalars."));
        if (auto valid = ValidatePhysicsStateSolver(state, staging.limits, staging.activeBodyScratch); valid.HasError())
            return Result<PhysicsStateSolverView>::Failure(valid.ErrorValue());
        staging.used = state;
        return Result<PhysicsStateSolverView>::Success(state);
    }
}  // namespace Horo::Physics::StateDetail
