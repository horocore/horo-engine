#pragma once

/**
 * @file RenderGraph.h
 * @brief Backend-neutral bounded render-graph authoring contracts.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/RenderBackend.h"
#include "Horo/Runtime/Render/RenderResource.h"
#include "Horo/Runtime/Render/RenderSubmission.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <variant>
#include <vector>

namespace Horo::Render {
    namespace Detail {
        class PreparedRenderGraphTemplate;
    }

    /** @brief Process-local owner identity assigned to one render-graph builder. */
    struct RenderGraphOwnerId {
        std::uint64_t value{0};

        /**
         * @brief Reports whether the identity was issued to a builder.
         * @return True for a non-zero issued identity.
         */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderGraphOwnerId &) const noexcept = default;
    };

    /** @brief Builder-scoped reference to the canonical render-pass identity. */
    struct RenderGraphPassRef {
        RenderGraphOwnerId owner;
        RenderPassId id;

        /**
         * @brief Reports whether both owner and canonical pass identities are valid.
         * @return True when both identity components are non-zero.
         */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return owner.IsValid() && id.IsValid();
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderGraphPassRef &) const noexcept = default;
    };

    /** @brief Builder-scoped identity of one logical graph resource. */
    struct RenderGraphResourceId {
        RenderGraphOwnerId owner;
        std::uint32_t value{0};

        /**
         * @brief Reports whether the owner and local identity are valid.
         * @return True when both identity components are non-zero.
         */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return owner.IsValid() && value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderGraphResourceId &) const noexcept = default;
    };

    /** @brief Coarse logical resource category used during initial graph authoring. */
    enum class RenderGraphResourceKind : std::uint8_t {
        Buffer,
        Texture,
    };

    /** @brief Lifetime and ownership class declared for one logical graph resource. */
    enum class RenderGraphResourceClass : std::uint8_t {
        External,
        Persistent,
        Transient,
        History,
    };

    /**
     * @brief Optional resident identity imported by one logical graph resource.
     *
     * A transient resource uses `std::monostate`. Every other resource class carries
     * the generation-safe Horo buffer or texture identity that the graph borrows.
     * The finalized graph does not own or extend the resident resource lifetime.
     */
    using RenderGraphResourceBinding = std::variant<std::monostate, RenderBufferHandle, RenderTextureHandle>;

    /** @brief Access direction declared by one pass-resource use. */
    enum class RenderGraphAccess : std::uint8_t {
        Read,
        Write,
        ReadWrite,
    };

    /** @brief Backend-neutral semantic role of one pass-resource use. */
    enum class RenderGraphUsageKind : std::uint8_t {
        Sampled,
        Storage,
        ColorAttachment,
        DepthStencilAttachment,
        CopySource,
        CopyDestination,
        VertexBuffer,  /**< Read-only vertex fetch in a graphics pass. */
        IndexBuffer,   /**< Read-only indexed-primitive fetch in a graphics pass. */
        UniformBuffer, /**< Read-only shader parameter access in a graphics pass. */
    };

    /** @brief Explicit reason for an authored pass dependency. */
    enum class RenderGraphDependencyKind : std::uint8_t {
        ExecutionOrder,
        ResourceHazard,
        ExternalSynchronization,
    };

    /** @brief Explicit policy controlling whether a pass may be removed when its outputs are unused. */
    enum class RenderGraphPassCullPolicy : std::uint8_t {
        ConservativeKeep,
        AllowCullIfOutputsUnused,
    };

    /** @brief Finite builder capacities admitted before any graph records are authored. */
    struct RenderGraphLimits {
        static constexpr std::size_t HardMaxPasses = 4'096;
        static constexpr std::size_t HardMaxResources = 8'192;
        static constexpr std::size_t HardMaxUsages = 32'768;
        static constexpr std::size_t HardMaxDependencies = 32'768;

        std::size_t maxPasses{256};
        std::size_t maxResources{512};
        std::size_t maxUsages{2'048};
        std::size_t maxDependencies{2'048};

        /**
         * @brief Reports whether every capacity is non-zero and within its hard bound.
         * @return True when every capacity is admitted by the engine hard limits.
         */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return maxPasses > 0 && maxPasses <= HardMaxPasses && maxResources > 0 && maxResources <= HardMaxResources && maxUsages > 0 &&
                   maxUsages <= HardMaxUsages && maxDependencies > 0 && maxDependencies <= HardMaxDependencies;
        }
    };

    /** @brief One pass authored with the canonical pass identity and an explicit queue role. */
    struct RenderGraphPass {
        RenderGraphPassRef reference;
        RenderPassKind kind{RenderPassKind::Graphics};
        RenderQueueRole queue{RenderQueueRole::Graphics};
        RenderGraphPassCullPolicy cullPolicy{RenderGraphPassCullPolicy::ConservativeKeep};
    };

    /** @brief One graph-local logical resource owned by the finalized graph value. */
    struct RenderGraphResource {
        RenderGraphResourceId id;
        RenderGraphResourceKind kind{RenderGraphResourceKind::Buffer};
        RenderGraphResourceClass resourceClass{RenderGraphResourceClass::Transient};
        RenderGraphResourceBinding binding;
    };

    /** @brief One graph resource explicitly retained as an observable graph output. */
    struct RenderGraphResourceExport {
        RenderGraphResourceId resource;
    };

    /** @brief One explicit semantic use of a graph resource by a pass. */
    struct RenderGraphResourceUsage {
        RenderGraphPassRef pass;
        RenderGraphResourceId resource;
        RenderGraphAccess access{RenderGraphAccess::Read};
        RenderGraphUsageKind kind{RenderGraphUsageKind::Sampled};
    };

    /** @brief One explicit directed dependency between two authored passes. */
    struct RenderGraphDependency {
        RenderGraphPassRef before;
        RenderGraphPassRef after;
        RenderGraphDependencyKind kind{RenderGraphDependencyKind::ExecutionOrder};
    };

    /** @brief Whether graph compilation retained or removed one authored pass. */
    enum class RenderGraphPassDispositionKind : std::uint8_t {
        Retained,
        Culled,
    };

    /** @brief Stable backend-neutral provenance for one pass scheduling decision. */
    enum class RenderGraphPassDispositionReason : std::uint8_t {
        ConservativePolicy,
        ExportedOutput,
        ImportedResourceMutation,
        ExternalSynchronization,
        RequiredDependency,
        RequiredResourceProducer,
        UnusedTransientOutputs,
        OnlyRequiredByCulledPasses,
    };

    /** @brief Typed scheduling decision retained for diagnostics and graph inspection. */
    struct RenderGraphPassDisposition {
        RenderGraphPassRef pass;
        RenderGraphPassDispositionKind disposition{RenderGraphPassDispositionKind::Retained};
        RenderGraphPassDispositionReason reason{RenderGraphPassDispositionReason::ConservativePolicy};
    };

    /**
     * @brief Owning deterministic pass schedule produced by graph validation and conservative culling.
     *
     * Pass references retain the source graph owner identity but do not borrow graph storage. The
     * schedule is backend-neutral and contains no frame, queue-instance, or native synchronization state.
     */
    class RenderGraphSchedule final {
    public:
        RenderGraphSchedule(const RenderGraphSchedule &) = delete;
        RenderGraphSchedule &operator=(const RenderGraphSchedule &) = delete;
        RenderGraphSchedule(RenderGraphSchedule &&other) noexcept;
        RenderGraphSchedule &operator=(RenderGraphSchedule &&other) noexcept;
        ~RenderGraphSchedule() = default;

        /**
         * @brief Returns the source graph owner identity.
         * @return Non-zero identity copied from the compiled graph.
         */
        [[nodiscard]] RenderGraphOwnerId Owner() const noexcept;

        /**
         * @brief Returns retained passes in deterministic dependency order.
         * @return Immutable view valid for the lifetime of this schedule.
         */
        [[nodiscard]] std::span<const RenderGraphPassRef> OrderedPasses() const noexcept;

        /**
         * @brief Returns typed retain/cull provenance in deterministic authoring order.
         * @return Immutable view valid for the lifetime of this schedule.
         */
        [[nodiscard]] std::span<const RenderGraphPassDisposition> PassDispositions() const noexcept;

    private:
        friend Result<RenderGraphSchedule> CompileRenderGraph(const class RenderGraph &graph);

        /** @brief Adopts validated schedule storage for one source graph owner. */
        RenderGraphSchedule(RenderGraphOwnerId owner, std::vector<RenderGraphPassRef> orderedPasses,
                            std::vector<RenderGraphPassDisposition> passDispositions) noexcept;

        RenderGraphOwnerId owner_;
        std::vector<RenderGraphPassRef> orderedPasses_;
        std::vector<RenderGraphPassDisposition> passDispositions_;
    };

    /** @brief Immutable owning graph value produced by successful authoring finalization. */
    class RenderGraph final {
    public:
        RenderGraph(const RenderGraph &) = delete;
        RenderGraph &operator=(const RenderGraph &) = delete;
        RenderGraph(RenderGraph &&other) noexcept;
        RenderGraph &operator=(RenderGraph &&other) noexcept;
        ~RenderGraph() = default;

        /**
         * @brief Returns the identity that owns every graph-local reference.
         * @return Non-zero process-local owner identity.
         */
        [[nodiscard]] RenderGraphOwnerId Owner() const noexcept;

        /**
         * @brief Returns the finite limits admitted for this graph.
         * @return Immutable admitted limit set owned by this graph.
         */
        [[nodiscard]] const RenderGraphLimits &Limits() const noexcept;

        /**
         * @brief Returns immutable passes in deterministic authoring order.
         * @return View valid for the lifetime of this graph value.
         */
        [[nodiscard]] std::span<const RenderGraphPass> Passes() const noexcept;

        /**
         * @brief Returns immutable resources in deterministic authoring order.
         * @return View valid for the lifetime of this graph value.
         */
        [[nodiscard]] std::span<const RenderGraphResource> Resources() const noexcept;

        /**
         * @brief Returns explicit resource exports in deterministic authoring order.
         * @return View valid for the lifetime of this graph value.
         */
        [[nodiscard]] std::span<const RenderGraphResourceExport> Exports() const noexcept;

        /**
         * @brief Returns immutable resource uses in deterministic authoring order.
         * @return View valid for the lifetime of this graph value.
         */
        [[nodiscard]] std::span<const RenderGraphResourceUsage> Usages() const noexcept;

        /**
         * @brief Returns immutable dependencies in deterministic authoring order.
         * @return View valid for the lifetime of this graph value.
         */
        [[nodiscard]] std::span<const RenderGraphDependency> Dependencies() const noexcept;

    private:
        friend class RenderGraphBuilder;

        RenderGraph(RenderGraphOwnerId owner, const RenderGraphLimits &limits, std::vector<RenderGraphPass> passes,
                    std::vector<RenderGraphResource> resources, std::vector<RenderGraphResourceExport> exports,
                    std::vector<RenderGraphResourceUsage> usages, std::vector<RenderGraphDependency> dependencies) noexcept;

        RenderGraphOwnerId owner_;
        RenderGraphLimits limits_;
        std::vector<RenderGraphPass> passes_;
        std::vector<RenderGraphResource> resources_;
        std::vector<RenderGraphResourceExport> exports_;
        std::vector<RenderGraphResourceUsage> usages_;
        std::vector<RenderGraphDependency> dependencies_;
    };

    /**
     * @brief Validates, deterministically orders, and conservatively culls one finalized graph.
     *
     * Compilation is synchronous, backend-neutral, and bounded by the graph's admitted limits. It
     * rejects dependency cycles and transient reads that have no preceding write. Dependency ties
     * are resolved by authoring order into the total schedule consumed by later hazard synthesis;
     * resource users do not require a redundant authored dependency path. Imported resources are
     * treated as initialized for this coarse validation stage; exact initial-state evidence remains
     * owned by later ADR-175 state/hazard synthesis. A pass is culled only when it explicitly opts
     * into culling and its transient outputs cannot contribute to an export, imported-resource
     * mutation, externally synchronized pass, or another retained pass. The immutable graph may be
     * compiled on any thread and remains unchanged.
     *
     * @param graph Finalized immutable graph to compile.
     * @return Owning schedule or a typed invalid-graph, cycle, read-before-write, or allocation failure.
     */
    [[nodiscard]] Result<RenderGraphSchedule> CompileRenderGraph(const RenderGraph &graph);

    /** @brief Observable lifecycle state of a bounded render-graph builder. */
    enum class RenderGraphBuilderState : std::uint8_t {
        Open,
        Finalized,
        Cancelled,
        Shutdown,
        MovedFrom,
    };

    /**
     * @brief Owner-thread-only bounded authoring surface for one backend-neutral render graph.
     *
     * The builder owns every admitted record and allocates its declared capacities during
     * creation. Authoring and finalization must occur on the creating thread. Cancellation
     * and shutdown never submit backend work; shutdown is idempotent and may run during
     * partial initialization or destruction.
     */
    class RenderGraphBuilder final {
    public:
        RenderGraphBuilder(const RenderGraphBuilder &) = delete;
        RenderGraphBuilder &operator=(const RenderGraphBuilder &) = delete;
        RenderGraphBuilder(RenderGraphBuilder &&other) noexcept;
        RenderGraphBuilder &operator=(RenderGraphBuilder &&) = delete;
        ~RenderGraphBuilder();

        /**
         * @brief Creates an open builder and reserves every declared finite capacity.
         * @param limits Requested capacities constrained by the engine hard bounds.
         * @return Open builder or a typed invalid-limit, identity, or allocation failure.
         */
        [[nodiscard]] static Result<RenderGraphBuilder> Create(const RenderGraphLimits &limits = {});

        /**
         * @brief Returns the process-local identity that scopes issued references.
         * @return Builder owner identity, or an invalid identity after move.
         */
        [[nodiscard]] RenderGraphOwnerId Owner() const noexcept;

        /**
         * @brief Returns the explicit lifecycle state.
         * @return Current builder state.
         */
        [[nodiscard]] RenderGraphBuilderState State() const noexcept;

        /**
         * @brief Adds one pass and assigns the next canonical render-pass identity.
         * @param kind Backend-neutral pass category.
         * @param queue Explicit queue role; incompatible or unknown roles fail without fallback.
         * @param cullPolicy Conservative by default; opt-in only when unused declared outputs prove the pass removable.
         * @return Builder-scoped reference or a typed affinity, lifecycle, capacity, or support failure.
         */
        [[nodiscard]] Result<RenderGraphPassRef> AddPass(
            RenderPassKind kind, RenderQueueRole queue, RenderGraphPassCullPolicy cullPolicy = RenderGraphPassCullPolicy::ConservativeKeep);

        /**
         * @brief Adds one graph-local transient resource with no resident binding.
         * @param kind Backend-neutral resource category.
         * @return Owner-scoped identity or a typed affinity, lifecycle, capacity, or support failure.
         */
        [[nodiscard]] Result<RenderGraphResourceId> AddTransientResource(RenderGraphResourceKind kind);

        /**
         * @brief Imports one generation-safe resident buffer into the graph.
         * @param handle Borrowed Horo buffer identity; its generation must outlive graph use and submission.
         * @param resourceClass External, persistent, or history lifetime classification.
         * @return Graph-local identity or a typed affinity, lifecycle, capacity, binding, or support failure.
         */
        [[nodiscard]] Result<RenderGraphResourceId> ImportBuffer(RenderBufferHandle handle, RenderGraphResourceClass resourceClass);

        /**
         * @brief Imports one generation-safe resident texture into the graph.
         * @param handle Borrowed Horo texture identity; its generation must outlive graph use and submission.
         * @param resourceClass External, persistent, or history lifetime classification.
         * @return Graph-local identity or a typed affinity, lifecycle, capacity, binding, or support failure.
         */
        [[nodiscard]] Result<RenderGraphResourceId> ImportTexture(RenderTextureHandle handle, RenderGraphResourceClass resourceClass);

        /**
         * @brief Marks one graph-local resource as an observable output.
         * @param resource Resource issued by this builder.
         * @return Success or a typed affinity, lifecycle, ownership, reference, or duplicate-export failure.
         */
        [[nodiscard]] Result<void> ExportResource(RenderGraphResourceId resource);

        /**
         * @brief Records one pass-resource use after validating ownership and semantic compatibility.
         * @param usage Complete use declaration referencing records from this builder.
         * @return Success or a typed affinity, ownership, reference, capacity, or support failure.
         */
        [[nodiscard]] Result<void> AddUsage(const RenderGraphResourceUsage &usage);

        /**
         * @brief Records one directed dependency between two distinct passes from this builder.
         * @param dependency Complete dependency declaration.
         * @return Success or a typed affinity, ownership, reference, capacity, or support failure.
         */
        [[nodiscard]] Result<void> AddDependency(const RenderGraphDependency &dependency);

        /**
         * @brief Finalizes the builder and transfers all owned records into an immutable graph.
         * @return Owning graph or a typed affinity, lifecycle, or empty-graph failure.
         */
        [[nodiscard]] Result<RenderGraph> Finalize();

        /**
         * @brief Cancels open authoring and releases all authored records on the owner thread.
         * @return Success, including repeated cancellation, or a typed affinity/lifecycle failure.
         */
        [[nodiscard]] Result<void> Cancel();

        /**
         * @brief Releases retained authoring storage without backend or GPU work.
         *
         * Repeated calls are safe. The caller must quiesce owner-thread authoring
         * before shutdown; teardown may then continue on another thread.
         */
        void Shutdown() noexcept;

    private:
        friend class RenderFrontend;
        friend class Detail::PreparedRenderGraphTemplate;
        RenderGraphBuilder(RenderGraphOwnerId owner, const RenderGraphLimits &limits) noexcept;

        [[nodiscard]] Result<void> Reserve();
        [[nodiscard]] Result<void> ValidateOpenOnOwnerThread() const;
        [[nodiscard]] Result<void> ValidateUsageReferences(const RenderGraphResourceUsage &usage) const;
        [[nodiscard]] Result<void> ValidateUsageSemantics(const RenderGraphResourceUsage &usage) const;
        [[nodiscard]] Result<void> ValidateDependencyReferences(const RenderGraphDependency &dependency) const;
        [[nodiscard]] Result<RenderGraphResourceId> AddImportedResource(RenderGraphResourceKind kind,
                                                                        RenderGraphResourceClass resourceClass,
                                                                        RenderGraphResourceBinding binding);
        [[nodiscard]] Result<RenderGraphResourceId> AddResourceDeclaration(RenderGraphResourceKind kind,
                                                                           RenderGraphResourceClass resourceClass,
                                                                           RenderGraphResourceBinding binding);
        void ReleaseStorage() noexcept;
        [[nodiscard]] const RenderGraphPass *FindPass(RenderGraphPassRef reference) const noexcept;
        [[nodiscard]] const RenderGraphResource *FindResource(RenderGraphResourceId id) const noexcept;

        RenderGraphOwnerId owner_;
        RenderGraphLimits limits_;
        std::thread::id ownerThread_;
        RenderGraphBuilderState state_{RenderGraphBuilderState::MovedFrom};
        std::vector<RenderGraphPass> passes_;
        std::vector<RenderGraphResource> resources_;
        std::vector<RenderGraphResourceExport> exports_;
        std::vector<std::uint8_t> exportedResources_;
        std::vector<RenderGraphResourceUsage> usages_;
        std::vector<RenderGraphDependency> dependencies_;
    };

}  // namespace Horo::Render
