#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsStateConstructionManifestOwner.h"

#include <algorithm>
#include <array>

namespace Horo::Physics::StateDetail {
    namespace {
        template <class T> bool Prefix(std::span<const T> input, const std::vector<T> &owned) noexcept {
            return input.empty() || (input.data() == owned.data() && input.size() <= owned.size());
        }
    }  // namespace

    /** @copydoc ConstructionManifestOwner::Seal */
    Result<void> ConstructionManifestOwner::Seal(const PhysicsStateConstructionManifest &manifest, PhysicsStateFpControl current) {
        if (sealed_)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (const std::array<bool, 10> owned{Prefix(manifest.shapes.nodes, nodes_), Prefix(manifest.shapes.edges, edges_),
                                             Prefix(manifest.shapes.roots, roots_), Prefix(manifest.bodies, bodies_),
                                             Prefix(manifest.colliders, colliders_), Prefix(manifest.materials, materials_),
                                             Prefix(manifest.joints, joints_), Prefix(manifest.fixtures, fixtures_),
                                             Prefix(manifest.queryChildren, children_), Prefix(manifest.owners, owners_)};
            !std::ranges::all_of(owned, [](bool value) {
            return value;
        }))
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        auto encoded = EncodeConstructionManifest(manifest, limits_, current, validation_, encoded_);
        if (encoded.HasError())
            return Result<void>::Failure(encoded.ErrorValue());
        view_ = manifest;
        usedBytes_ = encoded.Value();
        sealed_ = true;
        return Result<void>::Success();
    }

    /** @copydoc ConstructionManifestOwner::Decode */
    Result<void> ConstructionManifestOwner::Decode(std::span<const std::byte> input, PhysicsStateFpControl current) {
        if (sealed_)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (input.size() > encoded_.size())
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        auto decoded = DecodeConstructionManifest(input, limits_, current, validation_, Staging());
        if (decoded.HasError())
            return Result<void>::Failure(decoded.ErrorValue());
        // Canonical decode has validated all rows before this private owner publishes immutable values.
        std::copy(input.begin(), input.end(), encoded_.begin());
        view_ = decoded.Value();
        usedBytes_ = input.size();
        sealed_ = true;
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::StateDetail
