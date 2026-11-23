#include "Horo/Physics/PhysicsDeterminismPolicy.h"

#include "Horo/Physics/PhysicsErrors.h"

#include <tuple>

namespace Horo::Physics {
    namespace {
        /** @brief Writes one integer in fixed little-endian order. */
        template <typename Integer>
        void WriteLittleEndian(PhysicsDeterminismPolicyEncoding &output, std::size_t &offset, const Integer value) noexcept {
            for (std::size_t byte = 0; byte < sizeof(Integer); ++byte)
                output[offset++] = static_cast<std::uint8_t>(value >> (byte * 8U));
        }

        /** @brief Version-one SplitMix64 finalizer used only after complete domain encoding. */
        [[nodiscard]] std::uint64_t Mix64(std::uint64_t value) noexcept {
            value += 0x9e3779b97f4a7c15ULL;
            value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
            value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
            return value ^ (value >> 31U);
        }

        /** @brief Adds one typed field to a seed domain without standard-library hash behavior. */
        void MixField(std::uint64_t &state, const std::uint64_t tag, const std::uint64_t value) noexcept {
            state = Mix64(state ^ Mix64(tag) ^ Mix64(value));
        }
    }  // namespace

    /** @copydoc ValidatePhysicsCommandOrderKey */
    Result<void> ValidatePhysicsCommandOrderKey(const PhysicsCommandOrderKey &key) {
        if (key.protocolVersion != PhysicsCommandOrderingProtocolV1 || key.simulationTick == 0 || key.worldGeneration == 0 ||
            key.sceneGeneration == 0 || key.targetKind > PhysicsCommandTargetKind::Constraint || key.targetIdentity == 0 ||
            key.commandKind > PhysicsStructuralCommandKind::Destroy || !key.source.IsValid() || key.sourceSequence == 0)
            return Result<void>::Failure(MakeError(PhysicsErrors::CommandOrderInvalid));
        return Result<void>::Success();
    }

    /** @copydoc PhysicsCommandOrderLess */
    bool PhysicsCommandOrderLess(const PhysicsCommandOrderKey &left, const PhysicsCommandOrderKey &right) noexcept {
        return std::tie(left.protocolVersion, left.simulationTick, left.worldGeneration, left.sceneGeneration, left.targetKind,
                        left.targetIdentity, left.commandKind, left.source, left.sourceSequence) <
               std::tie(right.protocolVersion, right.simulationTick, right.worldGeneration, right.sceneGeneration, right.targetKind,
                        right.targetIdentity, right.commandKind, right.source, right.sourceSequence);
    }

    /** @copydoc ValidatePhysicsSeedPolicy */
    Result<void> ValidatePhysicsSeedPolicy(const PhysicsSeedPolicy &policy) {
        if (policy.contractVersion != PhysicsSeedPolicyContractV1 || policy.revision == 0 ||
            policy.algorithm != PhysicsSeedAlgorithm::SplitMix64V1 || policy.algorithmVersion != PhysicsSeedAlgorithmSplitMix64VersionV1 ||
            policy.worldGeneration == 0)
            return Result<void>::Failure(MakeError(PhysicsErrors::SeedPolicyInvalid));
        return Result<void>::Success();
    }

    /** @copydoc DerivePhysicsSeed */
    Result<std::uint64_t> DerivePhysicsSeed(const PhysicsSeedPolicy &policy, const PhysicsSeedConsumption &consumption) {
        if (const Result<void> valid = ValidatePhysicsSeedPolicy(policy); valid.HasError())
            return Result<std::uint64_t>::Failure(valid.ErrorValue());
        if (!consumption.stream.IsValid() || consumption.ownerIdentity == 0 || consumption.sequence == 0)
            return Result<std::uint64_t>::Failure(MakeError(PhysicsErrors::SeedPolicyInvalid));

        std::uint64_t state = 0x48524f5048595331ULL;  // "HROPHYS1", an algorithm-version domain, not ambient entropy.
        MixField(state, 1, policy.contractVersion);
        MixField(state, 2, policy.revision);
        MixField(state, 3, static_cast<std::uint32_t>(policy.algorithm));
        MixField(state, 4, policy.algorithmVersion);
        MixField(state, 5, policy.rootSeed);
        MixField(state, 6, policy.sessionSeed);
        MixField(state, 7, policy.worldGeneration);
        MixField(state, 8, consumption.stream.Value());
        MixField(state, 9, consumption.ownerIdentity);
        MixField(state, 10, consumption.sequence);
        return Result<std::uint64_t>::Success(state);
    }

    /** @copydoc EncodePhysicsDeterminismPolicy */
    Result<PhysicsDeterminismPolicyEncoding> EncodePhysicsDeterminismPolicy(const PhysicsSeedPolicy &policy) {
        if (const Result<void> valid = ValidatePhysicsSeedPolicy(policy); valid.HasError())
            return Result<PhysicsDeterminismPolicyEncoding>::Failure(valid.ErrorValue());
        PhysicsDeterminismPolicyEncoding output{};
        std::size_t offset{};
        WriteLittleEndian(output, offset, PhysicsDeterminismEncodingV1);
        WriteLittleEndian(output, offset, PhysicsCommandOrderingProtocolV1);
        WriteLittleEndian(output, offset, policy.contractVersion);
        WriteLittleEndian(output, offset, static_cast<std::uint32_t>(policy.algorithm));
        WriteLittleEndian(output, offset, policy.algorithmVersion);
        WriteLittleEndian(output, offset, policy.revision);
        WriteLittleEndian(output, offset, policy.rootSeed);
        WriteLittleEndian(output, offset, policy.sessionSeed);
        WriteLittleEndian(output, offset, policy.worldGeneration);
        return Result<PhysicsDeterminismPolicyEncoding>::Success(output);
    }
}  // namespace Horo::Physics
