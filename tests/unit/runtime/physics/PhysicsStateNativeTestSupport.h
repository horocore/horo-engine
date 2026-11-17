#pragma once

#include "AllocationProbe.h"
#include "CanonicalPhysicsRuntimeInternal.h"
#include "CanonicalPhysicsStateCapture.h"
#include "CanonicalPhysicsStateRestore.h"
#include "PhysicsStateSolverStorage.h"
#include "PhysicsTestUtils.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Physics::Detail::StateTest {
    template <class T> T Take(Result<T> result) {
        INFO((result.HasError() ? result.ErrorValue().message : std::string{"success"}));
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    struct RuntimeOwner final {
        CanonicalRuntimeHandle handle;

        ~RuntimeOwner() {
            DestroyCanonicalRuntime(handle);
        }
    };

    struct WorldOwner final {
        CanonicalWorldHandle handle;

        ~WorldOwner() {
            DestroyCanonicalWorld(handle);
        }
    };

    enum class Profile {
        AxisAligned,
        RotatedCompound
    };

    /** @brief Genuine native fixture; no fake solver/state or manufactured native
     * handle is admitted. */
    class Fixture final {
    public:
        explicit Fixture(Profile profile = Profile::AxisAligned);
        void Step(std::uint64_t firstTick, std::uint32_t count);
        PhysicsStateSolverView Capture(StateDetail::SolverStorage &storage);
        CanonicalPhysicsStateRestoreScratch Scratch() noexcept;

        CanonicalWorld &Native() const noexcept {
            return *static_cast<CanonicalWorld *>(world.handle.value);
        }

        const PhysicsWorldSettings settings;
        RuntimeOwner runtime;
        WorldOwner world;
        CanonicalPhysicsStateBindingStorage bindings;
        std::unique_ptr<StateDetail::SolverStorage> captured, recapture, latest;
        std::vector<std::byte> temporary, nativeBytes, expectedWire, actualWire;
        std::vector<std::uint32_t> active;
        CanonicalPhysicsStateCaptureMetrics metrics;

    private:
        void Build(Profile profile);
        void Prepare();
    };
}  // namespace Horo::Physics::Detail::StateTest
