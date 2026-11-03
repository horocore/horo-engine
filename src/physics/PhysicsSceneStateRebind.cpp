#include "PhysicsSceneStateRebind.h"

#include <algorithm>
#include <limits>

namespace Horo::Physics::Detail {
    namespace {
        template <class T, class Handle, class Member>
        Result<Handle> Lookup(std::span<const T> rows, PhysicsStateObjectId id, Member member) {
            const auto found = std::ranges::lower_bound(rows, id, {}, &T::id);
            if (!id || found == rows.end() || found->id != id)
                return Result<Handle>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            return Result<Handle>::Success((*found).*member);
        }

        struct Mapper final {
            const PhysicsSceneStateProjection &old;
            const CanonicalRestartBindings &next;

            Result<BodyHandle> Body(BodyHandle value) const {
                return Lookup<PhysicsStateBodyBinding, BodyHandle>(next.bodies, old.BodyId(value), &PhysicsStateBodyBinding::body);
            }

            Result<ShapeHandle> Shape(ShapeHandle value) const {
                return Lookup<PhysicsStateShapeBinding, ShapeHandle>(next.shapes, old.ShapeId(value), &PhysicsStateShapeBinding::shape);
            }

            Result<ConstraintHandle> Constraint(ConstraintHandle value) const {
                return Lookup<PhysicsStateConstraintBinding, ConstraintHandle>(next.constraints, old.ConstraintId(value),
                                                                               &PhysicsStateConstraintBinding::constraint);
            }
        };

        template <class Rows> Result<void> Bodies(Rows &rows, const Mapper &mapper) {
            for (auto &row : rows) {
                auto next = mapper.Body(row.handle);
                if (next.HasError())
                    return Result<void>::Failure(next.ErrorValue());
                row.handle = next.Value();
            }
            return Result<void>::Success();
        }

        template <class Rows> Result<void> Shapes(Rows &rows, const Mapper &mapper) {
            for (auto &row : rows) {
                auto shape = mapper.Shape(row.handle);
                if (shape.HasError())
                    return Result<void>::Failure(shape.ErrorValue());
                auto body = mapper.Body(row.body);
                if (body.HasError())
                    return Result<void>::Failure(body.ErrorValue());
                row.handle = shape.Value();
                row.body = body.Value();
            }
            return Result<void>::Success();
        }

        template <class Rows> Result<void> Constraints(Rows &rows, const Mapper &mapper) {
            for (auto &row : rows) {
                auto next = mapper.Constraint(row.handle);
                if (next.HasError())
                    return Result<void>::Failure(next.ErrorValue());
                row.handle = next.Value();
            }
            return Result<void>::Success();
        }

        Result<void> SceneTables(PhysicsSceneStateRebind &tables, const Mapper &mapper) {
            if (auto bodies = Bodies(tables.bodies, mapper); bodies.HasError())
                return bodies;
            if (auto shapes = Shapes(tables.shapes, mapper); shapes.HasError())
                return shapes;
            return Constraints(tables.constraints, mapper);
        }

        template <class T> void Charge(std::size_t &bytes, const std::vector<T> &rows) noexcept {
            const auto maximum = std::numeric_limits<std::size_t>::max();
            if (rows.capacity() > (maximum - bytes) / sizeof(T))
                bytes = maximum;
            else
                bytes += rows.capacity() * sizeof(T);
        }
    }  // namespace

    /** @copydoc PhysicsSceneStateRebind::Rebind */
    Result<void> PhysicsSceneStateRebind::Rebind(const PhysicsSceneStateProjection &old, const CanonicalRestartBindings &next) {
        Mapper mapper{old, next};
        if (auto actual = SceneTables(*this, mapper); actual.HasError())
            return actual;
        if (auto bodies = Bodies(authoredBodies, mapper); bodies.HasError())
            return bodies;
        if (auto shapes = Shapes(authoredShapes, mapper); shapes.HasError())
            return shapes;
        if (auto constraints = Constraints(authoredConstraints, mapper); constraints.HasError())
            return constraints;
        if (auto bodies = Bodies(legacyBodies, mapper); bodies.HasError())
            return bodies;
        if (auto shapes = Shapes(legacyShapes, mapper); shapes.HasError())
            return shapes;
        return Constraints(legacyConstraints, mapper);
    }

    /** @copydoc PhysicsSceneStateRebind::RetainedPayloadBytes */
    std::size_t PhysicsSceneStateRebind::RetainedPayloadBytes() const noexcept {
        auto bytes = sizeof(*this);
        Charge(bytes, bodies);
        Charge(bytes, shapes);
        Charge(bytes, constraints);
        Charge(bytes, authoredBodies);
        Charge(bytes, authoredShapes);
        Charge(bytes, authoredConstraints);
        Charge(bytes, legacyBodies);
        Charge(bytes, legacyShapes);
        Charge(bytes, legacyConstraints);
        return bytes;
    }
}  // namespace Horo::Physics::Detail
