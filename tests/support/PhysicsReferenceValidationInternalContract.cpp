#include "PhysicsReferenceValidation.h"

#include <type_traits>

namespace {
    using namespace Horo;
    using namespace Runtime::PhysicsValidationDetail;
    static_assert(!std::is_convertible_v<BodyReferenceDomain, bool>);
    static_assert(
        std::is_same_v<decltype(ValidateCollider(std::declval<const Runtime::ColliderComponent &>(), BodyReferenceDomain::PrefabLocal)),
                       Result<void>>);
    static_assert(std::is_same_v<decltype(ValidateConstraint(std::declval<const Runtime::PhysicsConstraintComponent &>(),
                                                             BodyReferenceDomain::Scene)),
                                 Result<void>>);
}  // namespace
