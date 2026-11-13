#include "CanonicalPhysicsStateBuild.h"

#include "CanonicalPhysicsStateFpControl.h"
#include "HoroPhysicsStateBuildFacts.h"

#include <algorithm>

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Empty generated/host fields cannot accidentally enable production
         * capture. */
        bool Present(const Sha256Digest &digest) noexcept {
            return std::ranges::any_of(digest.bytes, [](auto value) {
                return value != 0;
            });
        }

        /** @brief Static Physics is part of the executing image, not the separately
         * linked archive on disk. */
        bool ValidImage(const PhysicsStateBuildBinding &binding) noexcept {
            if (!binding.codeLease || !Present(binding.executableArtifact) || !Present(binding.physicsArtifact))
                return false;
            return binding.lifetime == PhysicsStateCodeLifetime::ProcessStatic && binding.physicsArtifact == binding.executableArtifact;
        }

        /** @brief The complete exact tuple includes native source/patch/config facts
         * and actual host image artifact hashes. */
        Sha256Digest BuildDigest(const PhysicsStateBuildBinding &binding) noexcept {
            const std::array<std::span<const std::byte>, 5> parts{std::as_bytes(std::span{binding.executableArtifact.bytes}),
                                                                  std::as_bytes(std::span{binding.physicsArtifact.bytes}),
                                                                  std::as_bytes(std::span{StateBuildFacts::SolverInputs.bytes}),
                                                                  std::as_bytes(std::span{StateBuildFacts::PhysicsInputs.bytes}),
                                                                  std::as_bytes(std::span{StateBuildFacts::PlatformAbi.bytes})};
            return ComputeSha256Fragments(parts);
        }
    }  // namespace

    /** @copydoc PrepareCanonicalPhysicsStateBuild */
    Result<CanonicalPhysicsStateBuildIdentity> PrepareCanonicalPhysicsStateBuild(const PhysicsStateBuildBinding &binding) {
        static_assert(StateBuildFacts::ContractVersion == 1);
        if (const std::array facts{&StateBuildFacts::SolverInputs, &StateBuildFacts::PhysicsInputs, &StateBuildFacts::PlatformAbi};
            !std::ranges::all_of(facts,
                                 [](const auto *digest) {
            return Present(*digest);
        }) ||
            !ValidImage(binding) || binding.platformAbi != StateBuildFacts::PlatformAbi)
            return Result<CanonicalPhysicsStateBuildIdentity>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable,
                                                                                 "Physics state requires a genuine loaded-image lease and "
                                                                                 "exact generated build/platform facts."));
        auto floatingPoint = ReadCanonicalPhysicsStateFpControl();
        if (floatingPoint.HasError())
            return Result<CanonicalPhysicsStateBuildIdentity>::Failure(floatingPoint.ErrorValue());
        return Result<CanonicalPhysicsStateBuildIdentity>::Success(
            {BuildDigest(binding), StateBuildFacts::SolverInputs, StateBuildFacts::PlatformAbi, floatingPoint.Value()});
    }
}  // namespace Horo::Physics::Detail
