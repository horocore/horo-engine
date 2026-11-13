#pragma once
#include "Horo/Foundation/Sha256.h"
#include "Horo/Physics/PhysicsStateConstruction.h"

#include <atomic>
#include <memory>
#include <thread>

namespace Horo::Physics::StateDetail {
    class ConstructionRecipePool;
    class ConstructionRecipeExport;

    /** @brief Immutable recipe lifetime; foreign copies/drops change only its atomic reader count. */
    class ConstructionRecipeLease final {
    public:
        ConstructionRecipeLease() noexcept = default;
        ConstructionRecipeLease(const ConstructionRecipeLease &) noexcept;
        ConstructionRecipeLease &operator=(const ConstructionRecipeLease &) noexcept;
        ConstructionRecipeLease(ConstructionRecipeLease &&) noexcept;
        ConstructionRecipeLease &operator=(ConstructionRecipeLease &&) noexcept;
        ~ConstructionRecipeLease();

        [[nodiscard]] bool IsValid() const noexcept {
            return owner_ != nullptr;
        }

        void Reset() noexcept;

    private:
        friend class ConstructionRecipePool;
        friend class ConstructionRecipeExport;
        ConstructionRecipeLease(std::shared_ptr<ConstructionRecipePool> owner, std::uint32_t slot, std::uint64_t serial) noexcept;
        std::shared_ptr<ConstructionRecipePool> owner_;
        std::uint32_t slot_{};
        std::uint64_t serial_{};
    };

    struct ConstructionRecipeChild final {
        ConstructionRecipeLease recipe;
        PhysicsPose localPose;
        std::uint64_t userData{};
    };

    /** @brief Exact actual-resource root binding; lease retains the complete transitive recipe DAG. */
    struct ConstructionRecipeRootBinding final {
        PhysicsStateObjectId id{};
        ConstructionRecipeLease recipe;
    };

    /** @brief Caller-prepared iterative DFS frame; recursion and hidden heap growth are forbidden. */
    struct ConstructionRecipeExportFrame final {
        std::uint32_t slot{};
        std::uint32_t nextEdge{};
    };

    struct ConstructionRecipeExportStorage final {
        std::span<PhysicsStateShapeRecipe> nodes;
        std::span<PhysicsStateShapeRecipeEdge> edges;
        std::span<PhysicsStateShapeRecipeRoot> roots;
        std::span<std::uint32_t> nodeMap;
        std::span<ConstructionRecipeExportFrame> frames;
    };

    /** @brief Finite cold storage owned by the SAME Physics runtime state inventory; not a global registry.
     * No native objects are destroyed here. Collect is owner-only; foreign lease release is atomic only.
     */
    class ConstructionRecipePool final : public std::enable_shared_from_this<ConstructionRecipePool> {
    public:
        [[nodiscard]] static Result<std::shared_ptr<ConstructionRecipePool>> Prepare(
            PhysicsStateConstructionLimits limits, PhysicsStateFpControl actualProfile,
            PhysicsStateConstructionTracking tracking = PhysicsStateConstructionTracking::Required);
        [[nodiscard]] Result<ConstructionRecipeLease> AddAnalytic(const PhysicsShapeDescriptor &, PhysicsStateFpControl actual);
        [[nodiscard]] Result<ConstructionRecipeLease> AddCompound(std::span<const ConstructionRecipeChild>, PhysicsStateFpControl actual);
        [[nodiscard]] Result<PhysicsStateShapeRecipe> Read(const ConstructionRecipeLease &) const;
        [[nodiscard]] Result<Sha256Digest> Digest(const ConstructionRecipeLease &) const;
        [[nodiscard]] Result<std::size_t> CopyChildren(const ConstructionRecipeLease &, std::span<ConstructionRecipeChild>);
        [[nodiscard]] Result<std::size_t> Collect(std::size_t maximumRecords);
        [[nodiscard]] Result<void> CanReserve(std::size_t nodes, std::size_t edges) const;
        /** @brief Exports actual retained construction in stable root/ordered-child postorder, independent of pool slots/serials.
         * Scratch/output is caller-owned private staging; failure issues no immutable view and never modifies the pool.
         * No allocations, native calls, or resource permission are implied. All supplied spans must be disjoint.
         */
        [[nodiscard]] Result<PhysicsStateShapeRecipes> Export(std::span<const ConstructionRecipeRootBinding>,
                                                              ConstructionRecipeExportStorage) const;
        [[nodiscard]] std::size_t RetainedPayloadBytes() const noexcept;

        [[nodiscard]] PhysicsStateFpControl Profile() const noexcept {
            return profile_;
        }

        [[nodiscard]] const PhysicsStateConstructionLimits &Limits() const noexcept {
            return limits_;
        }

        [[nodiscard]] PhysicsStateConstructionTracking Tracking() const noexcept {
            return tracking_;
        }

    private:
        friend class ConstructionRecipeLease;
        friend class ConstructionRecipeExport;
        static constexpr std::uint32_t None = ~std::uint32_t{};

        struct Node final {
            std::atomic<std::uint64_t> readers{};
            PhysicsStateShapeRecipe recipe;
            Sha256Digest digest;
            std::uint64_t serial{};
            std::uint32_t firstEdge{None};
            std::uint32_t depth{};
            bool occupied{};
        };

        struct Edge final {
            std::uint32_t child{};
            std::uint32_t next{None};
            std::uint64_t serial{};
            std::uint64_t userData{};
            PhysicsPose localPose;
        };

        ConstructionRecipePool(PhysicsStateConstructionLimits, PhysicsStateFpControl, PhysicsStateConstructionTracking);
        [[nodiscard]] static bool ConfigurationValid(const PhysicsStateConstructionLimits &, PhysicsStateFpControl,
                                                     PhysicsStateConstructionTracking) noexcept;
        [[nodiscard]] bool Owner() const noexcept;
        [[nodiscard]] bool Exact(const ConstructionRecipeLease &) const noexcept;
        [[nodiscard]] Result<std::uint32_t> ReserveNode(PhysicsStateFpControl);
        [[nodiscard]] Result<std::uint32_t> AdmitChildren(std::span<const ConstructionRecipeChild>) const;
        [[nodiscard]] ConstructionRecipeLease Seal(std::uint32_t slot) noexcept;
        void AddReader(std::uint32_t slot, std::uint64_t serial) noexcept;
        void DropReader(std::uint32_t slot, std::uint64_t serial) noexcept;
        void ReleaseNode(Node &) noexcept;
        PhysicsStateConstructionLimits limits_;
        PhysicsStateFpControl profile_;
        const PhysicsStateConstructionTracking tracking_;
        std::thread::id owner_;
        std::unique_ptr<Node[]> nodes_;
        std::unique_ptr<Edge[]> edges_;
        std::uint64_t nextSerial_{1};
        std::uint32_t freeEdge_{None};
        std::uint32_t freeEdges_{};
        std::size_t cursor_{};
    };
}  // namespace Horo::Physics::StateDetail
