#include "Horo/Physics/PhysicsErrors.h"
#include "ScenePhysicsHost.h"

namespace Horo::Application::Internal {
    /** @copydoc ScenePhysicsHost::PublishedStateTarget */
    Result<Physics::Detail::PhysicsSceneStateTarget> ScenePhysicsHost::PublishedStateTarget() {
        using Outcome = Result<Physics::Detail::PhysicsSceneStateTarget>;
        if (std::this_thread::get_id() != owner_)
            return Outcome::Failure(MakeError(Physics::PhysicsErrors::ThreadAffinityViolation));
        if (!published_)
            return Outcome::Failure(MakeError(Physics::PhysicsErrors::CapabilityUnavailable));
        return Physics::Detail::PhysicsSceneHostAccess::StateTarget(*published_, true);
    }

    /** @copydoc ScenePhysicsHost::PendingStateTarget */
    Result<Physics::Detail::PhysicsSceneStateTarget> ScenePhysicsHost::PendingStateTarget(Runtime::SceneRuntimeId expected) {
        using Outcome = Result<Physics::Detail::PhysicsSceneStateTarget>;
        if (std::this_thread::get_id() != owner_)
            return Outcome::Failure(MakeError(Physics::PhysicsErrors::ThreadAffinityViolation));
        if (!pending_)
            return Outcome::Failure(MakeError(Physics::PhysicsErrors::CapabilityUnavailable));
        auto target = Physics::Detail::PhysicsSceneHostAccess::StateTarget(*pending_, false);
        if (target.HasError())
            return target;
        if (target.Value().Scene() != expected)
            return Outcome::Failure(MakeError(Physics::PhysicsErrors::CapabilityStale));
        return target;
    }
}  // namespace Horo::Application::Internal
