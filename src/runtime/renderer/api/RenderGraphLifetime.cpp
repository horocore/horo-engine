#include "Horo/Runtime/Render/RenderGraphLifetime.h"

#include "Horo/Runtime/Render/RenderGraphLifetimeErrors.h"

#include <algorithm>
#include <array>
#include <compare>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <queue>
#include <utility>

namespace Horo::Render {
    namespace {
        constexpr std::size_t NoUse = std::numeric_limits<std::size_t>::max();

        [[nodiscard]] constexpr std::size_t GraphIndex(const RenderGraphPassRef pass) noexcept {
            return pass.id.value - 1;
        }

        [[nodiscard]] constexpr std::size_t GraphIndex(const RenderGraphResourceId resource) noexcept {
            return resource.value - 1;
        }

        struct CompatibilityKey {
            std::array<std::uint64_t, 10> values{};

            [[nodiscard]] auto operator<=>(const CompatibilityKey &) const noexcept = default;
        };

        [[nodiscard]] CompatibilityKey MakeCompatibilityKey(const RenderGraphTransientDescriptor &descriptor) noexcept {
            CompatibilityKey key;
            if (const auto *buffer = std::get_if<RenderBufferDescriptor>(&descriptor)) {
                key.values = {0, buffer->byteSize, static_cast<std::uint8_t>(buffer->usage), static_cast<std::uint8_t>(buffer->access)};
                return key;
            }
            const auto &texture = std::get<RenderTextureDescriptor>(descriptor);
            key.values = {1,
                          static_cast<std::uint8_t>(texture.dimension),
                          texture.extent.width,
                          texture.extent.height,
                          static_cast<std::uint8_t>(texture.format),
                          texture.mipCount,
                          texture.layerCount,
                          texture.sampleCount,
                          static_cast<std::uint8_t>(texture.usage),
                          texture.depth};
            return key;
        }

        [[nodiscard]] bool DescriptorMatches(const RenderGraphResourceKind kind,
                                             const RenderGraphTransientDescriptor &descriptor) noexcept {
            if (const auto *buffer = std::get_if<RenderBufferDescriptor>(&descriptor)) {
                return kind == RenderGraphResourceKind::Buffer && buffer->IsValid();
            }
            const auto &texture = std::get<RenderTextureDescriptor>(descriptor);
            return kind == RenderGraphResourceKind::Texture && texture.IsValid();
        }

        [[nodiscard]] bool BufferDescriptorSupports(const RenderBufferDescriptor &descriptor, const RenderGraphUsageKind usage) noexcept {
            switch (usage) {
                case RenderGraphUsageKind::VertexBuffer:
                    return HasBufferUsage(descriptor.usage, RenderBufferUsage::Vertex);
                case RenderGraphUsageKind::IndexBuffer:
                    return HasBufferUsage(descriptor.usage, RenderBufferUsage::Index);
                case RenderGraphUsageKind::UniformBuffer:
                    return HasBufferUsage(descriptor.usage, RenderBufferUsage::Uniform);
                case RenderGraphUsageKind::Storage:
                    return HasBufferUsage(descriptor.usage, RenderBufferUsage::Storage);
                case RenderGraphUsageKind::CopySource:
                    return HasBufferUsage(descriptor.usage, RenderBufferUsage::CopySource);
                case RenderGraphUsageKind::CopyDestination:
                    return HasBufferUsage(descriptor.usage, RenderBufferUsage::CopyDestination);
                default:
                    return false;
            }
        }

        [[nodiscard]] bool IsDepthFormat(const RenderTextureFormat format) noexcept {
            using enum RenderTextureFormat;
            return format == Depth16Unorm || format == Depth24Stencil8 || format == Depth32Float || format == Depth32FloatStencil8;
        }

        [[nodiscard]] bool TextureDescriptorSupports(const RenderTextureDescriptor &descriptor, const RenderGraphUsageKind usage) noexcept {
            switch (usage) {
                case RenderGraphUsageKind::Sampled:
                    return HasTextureUsage(descriptor.usage, RenderTextureUsage::Sampled);
                case RenderGraphUsageKind::Storage:
                    return HasTextureUsage(descriptor.usage, RenderTextureUsage::Storage);
                case RenderGraphUsageKind::ColorAttachment:
                    return !IsDepthFormat(descriptor.format) && HasTextureUsage(descriptor.usage, RenderTextureUsage::RenderAttachment);
                case RenderGraphUsageKind::DepthStencilAttachment:
                    return IsDepthFormat(descriptor.format) && HasTextureUsage(descriptor.usage, RenderTextureUsage::RenderAttachment);
                case RenderGraphUsageKind::CopySource:
                    return HasTextureUsage(descriptor.usage, RenderTextureUsage::CopySource);
                case RenderGraphUsageKind::CopyDestination:
                    return HasTextureUsage(descriptor.usage, RenderTextureUsage::CopyDestination);
                default:
                    return false;
            }
        }

        [[nodiscard]] bool DescriptorSupports(const RenderGraphTransientDescriptor &descriptor, const RenderGraphUsageKind usage) noexcept {
            if (const auto *buffer = std::get_if<RenderBufferDescriptor>(&descriptor)) {
                return BufferDescriptorSupports(*buffer, usage);
            }
            return TextureDescriptorSupports(std::get<RenderTextureDescriptor>(descriptor), usage);
        }

        [[nodiscard]] bool IsKnown(const RenderGraphResourceKind kind) noexcept {
            return kind == RenderGraphResourceKind::Buffer || kind == RenderGraphResourceKind::Texture;
        }

        [[nodiscard]] bool IsKnown(const RenderGraphResourceClass resourceClass) noexcept {
            using enum RenderGraphResourceClass;
            return resourceClass == External || resourceClass == Persistent || resourceClass == Transient || resourceClass == History;
        }

        struct LifetimeCompileRecords {
            std::vector<RenderGraphResourceLifetime> lifetimes;
            std::vector<RenderGraphTransientAllocationRequirement> allocations;
            std::vector<RenderGraphAliasOpportunity> aliases;
        };

        struct SlotGroupKey {
            RenderGraphCompatibilityClassId compatibilityClass;
            RenderQueueRole role{RenderQueueRole::Graphics};

            [[nodiscard]] auto operator<=>(const SlotGroupKey &) const noexcept = default;
        };

        struct SlotGroup {
            using ActiveSlot = std::pair<std::size_t, std::uint32_t>;

            std::priority_queue<ActiveSlot, std::vector<ActiveSlot>, std::greater<>> active;
            std::priority_queue<std::uint32_t, std::vector<std::uint32_t>, std::greater<>> available;
        };

        class LifetimeCompiler final {
        public:
            LifetimeCompiler(const RenderGraph &graph, const RenderGraphSchedule &schedule,
                             const std::span<const RenderGraphTransientRequirement> requirements, const RenderGraphLifetimeLimits &limits)
                : graph_(graph), schedule_(schedule), requirements_(requirements), limits_(limits),
                  passPositions_(graph.Passes().size(), NoUse), requirementByResource_(graph.Resources().size(), nullptr),
                  lifetimes_(graph.Resources().size()), aliasRoleByResource_(graph.Resources().size()),
                  mixedAliasRole_(graph.Resources().size()), compatibilityByResource_(graph.Resources().size()),
                  slotsByResource_(graph.Resources().size()) {}

            [[nodiscard]] Result<LifetimeCompileRecords> Run() {
                if (!limits_.IsValid()) {
                    return Result<LifetimeCompileRecords>::Failure(MakeError(RenderGraphLifetimeErrors::InvalidLimits));
                }
                if (const Result<void> graph = ValidateGraph(); graph.HasError()) {
                    return Result<LifetimeCompileRecords>::Failure(graph.ErrorValue());
                }
                if (const Result<void> schedule = ValidateSchedule(); schedule.HasError()) {
                    return Result<LifetimeCompileRecords>::Failure(schedule.ErrorValue());
                }
                if (const Result<void> requirements = ValidateRequirements(); requirements.HasError()) {
                    return Result<LifetimeCompileRecords>::Failure(requirements.ErrorValue());
                }
                CompileLifetimes();
                AssignCompatibilityClasses();
                if (const Result<void> slots = AssignAllocationSlots(); slots.HasError()) {
                    return Result<LifetimeCompileRecords>::Failure(slots.ErrorValue());
                }
                return Result<LifetimeCompileRecords>::Success(BuildRecords());
            }

        private:
            [[nodiscard]] Result<void> ValidateGraph() const {
                if (!graph_.Owner().IsValid() || !graph_.Limits().IsValid() || graph_.Resources().size() > graph_.Limits().maxResources ||
                    graph_.Passes().size() > graph_.Limits().maxPasses || graph_.Usages().size() > graph_.Limits().maxUsages) {
                    return Result<void>::Failure(MakeError(RenderGraphLifetimeErrors::InvalidGraph));
                }
                for (std::size_t index = 0; index < graph_.Resources().size(); ++index) {
                    const RenderGraphResource &resource = graph_.Resources()[index];
                    if (resource.id != RenderGraphResourceId{graph_.Owner(), static_cast<std::uint32_t>(index + 1)} ||
                        !IsKnown(resource.kind) || !IsKnown(resource.resourceClass)) {
                        return Result<void>::Failure(MakeError(RenderGraphLifetimeErrors::InvalidGraph));
                    }
                }
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> ValidateSchedule() {
                if (schedule_.Owner() != graph_.Owner()) {
                    return Result<void>::Failure(MakeError(RenderGraphLifetimeErrors::OwnerMismatch));
                }
                if (!schedule_.Owner().IsValid() || schedule_.PassDispositions().size() != graph_.Passes().size()) {
                    return Result<void>::Failure(MakeError(RenderGraphLifetimeErrors::InvalidSchedule));
                }
                if (const Result<void> passes = ValidateOrderedPasses(); passes.HasError()) {
                    return passes;
                }
                return ValidatePassDispositions();
            }

            [[nodiscard]] Result<void> ValidateOrderedPasses() {
                std::vector<std::uint8_t> scheduled(graph_.Passes().size(), 0);
                for (std::size_t position = 0; position < schedule_.OrderedPasses().size(); ++position) {
                    const RenderGraphPassRef pass = schedule_.OrderedPasses()[position];
                    if (!IsPassValid(pass) || scheduled[GraphIndex(pass)] != 0) {
                        return Result<void>::Failure(MakeError(RenderGraphLifetimeErrors::InvalidSchedule));
                    }
                    scheduled[GraphIndex(pass)] = 1;
                    passPositions_[GraphIndex(pass)] = position;
                }
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> ValidatePassDispositions() const {
                std::vector<std::uint8_t> dispositions(graph_.Passes().size(), 0);
                for (const RenderGraphPassDisposition &entry : schedule_.PassDispositions()) {
                    if (!IsPassValid(entry.pass) || dispositions[GraphIndex(entry.pass)] != 0) {
                        return Result<void>::Failure(MakeError(RenderGraphLifetimeErrors::InvalidSchedule));
                    }
                    const bool retained = entry.disposition == RenderGraphPassDispositionKind::Retained;
                    if (const bool culled = entry.disposition == RenderGraphPassDispositionKind::Culled;
                        (!retained && !culled) || retained != (passPositions_[GraphIndex(entry.pass)] != NoUse)) {
                        return Result<void>::Failure(MakeError(RenderGraphLifetimeErrors::InvalidSchedule));
                    }
                    dispositions[GraphIndex(entry.pass)] = 1;
                }
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> ValidateRequirements() {
                if (requirements_.size() > graph_.Resources().size()) {
                    return Result<void>::Failure(MakeError(RenderGraphLifetimeErrors::UnexpectedRequirement));
                }
                if (const Result<void> indexed = IndexRequirements(); indexed.HasError()) {
                    return indexed;
                }
                if (const Result<void> complete = ValidateRequirementCompleteness(); complete.HasError()) {
                    return complete;
                }
                return ValidateRequirementUsages();
            }

            [[nodiscard]] Result<void> IndexRequirements() {
                for (const RenderGraphTransientRequirement &requirement : requirements_) {
                    if (requirement.resource.owner != graph_.Owner()) {
                        return Result<void>::Failure(MakeError(RenderGraphLifetimeErrors::OwnerMismatch));
                    }
                    if (!IsResourceValid(requirement.resource)) {
                        return Result<void>::Failure(MakeError(RenderGraphLifetimeErrors::UnexpectedRequirement));
                    }
                    const std::size_t resourceIndex = GraphIndex(requirement.resource);
                    const RenderGraphResource &resource = graph_.Resources()[resourceIndex];
                    if (resource.resourceClass != RenderGraphResourceClass::Transient) {
                        return Result<void>::Failure(MakeError(RenderGraphLifetimeErrors::UnexpectedRequirement));
                    }
                    if (requirementByResource_[resourceIndex] != nullptr) {
                        return Result<void>::Failure(MakeError(RenderGraphLifetimeErrors::DuplicateRequirement));
                    }
                    if (!DescriptorMatches(resource.kind, requirement.descriptor)) {
                        return Result<void>::Failure(MakeError(RenderGraphLifetimeErrors::DescriptorInvalid));
                    }
                    requirementByResource_[resourceIndex] = &requirement;
                }
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> ValidateRequirementCompleteness() const {
                for (std::size_t index = 0; index < graph_.Resources().size(); ++index) {
                    if (graph_.Resources()[index].resourceClass == RenderGraphResourceClass::Transient &&
                        requirementByResource_[index] == nullptr) {
                        return Result<void>::Failure(MakeError(RenderGraphLifetimeErrors::MissingRequirement));
                    }
                }
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> ValidateRequirementUsages() const {
                for (const RenderGraphResourceUsage &usage : graph_.Usages()) {
                    if (IsResourceValid(usage.resource)) {
                        const std::size_t resource = GraphIndex(usage.resource);
                        if (requirementByResource_[resource] != nullptr &&
                            !DescriptorSupports(requirementByResource_[resource]->descriptor, usage.kind)) {
                            return Result<void>::Failure(MakeError(RenderGraphLifetimeErrors::DescriptorInvalid));
                        }
                    }
                }
                return Result<void>::Success();
            }

            void CompileLifetimes() noexcept {
                for (std::size_t index = 0; index < graph_.Resources().size(); ++index) {
                    lifetimes_[index].resource = graph_.Resources()[index].id;
                }
                for (const RenderGraphResourceUsage &usage : graph_.Usages()) {
                    if (!IsPassValid(usage.pass) || !IsResourceValid(usage.resource)) {
                        continue;
                    }
                    const std::size_t position = passPositions_[GraphIndex(usage.pass)];
                    if (position == NoUse) {
                        continue;
                    }
                    const std::size_t resource = GraphIndex(usage.resource);
                    RecordAliasRole(resource, graph_.Passes()[GraphIndex(usage.pass)].queue);
                    RenderGraphResourceLifetime &lifetime = lifetimes_[resource];
                    if (lifetime.disposition == RenderGraphLifetimeDisposition::Unused) {
                        lifetime.firstPass = usage.pass;
                        lifetime.lastPass = usage.pass;
                        lifetime.firstUseIndex = position;
                        lifetime.lastUseIndex = position;
                        lifetime.disposition = RenderGraphLifetimeDisposition::Used;
                    } else {
                        if (position < lifetime.firstUseIndex) {
                            lifetime.firstUseIndex = position;
                            lifetime.firstPass = usage.pass;
                        }
                        if (position > lifetime.lastUseIndex) {
                            lifetime.lastUseIndex = position;
                            lifetime.lastPass = usage.pass;
                        }
                    }
                }
            }

            void RecordAliasRole(const std::size_t resource, const RenderQueueRole role) noexcept {
                if (!aliasRoleByResource_[resource].has_value()) {
                    aliasRoleByResource_[resource] = role;
                } else if (aliasRoleByResource_[resource] != role) {
                    mixedAliasRole_[resource] = 1;
                }
            }

            void AssignCompatibilityClasses() {
                std::map<CompatibilityKey, RenderGraphCompatibilityClassId> classes;
                for (std::size_t index = 0; index < graph_.Resources().size(); ++index) {
                    if (!IsUsedTransient(index)) {
                        continue;
                    }
                    const RenderGraphTransientDescriptor &descriptor = requirementByResource_[index]->descriptor;
                    const CompatibilityKey key = MakeCompatibilityKey(descriptor);
                    auto found = classes.find(key);
                    if (found == classes.end()) {
                        const RenderGraphCompatibilityClassId id{static_cast<std::uint32_t>(classes.size() + 1)};
                        found = classes.try_emplace(key, id).first;
                    }
                    compatibilityByResource_[index] = found->second;
                }
            }

            [[nodiscard]] Result<void> AssignAllocationSlots() {
                const std::vector<std::size_t> resources = UsedTransientResources();
                std::map<SlotGroupKey, SlotGroup> groups;
                std::vector previousResourceBySlot(graph_.Resources().size() + 1, NoUse);
                std::uint32_t nextSlot = 1;
                for (const std::size_t resource : resources) {
                    if (const Result<void> assigned = AssignResourceSlot(resource, groups, previousResourceBySlot, nextSlot);
                        assigned.HasError()) {
                        return assigned;
                    }
                }
                return Result<void>::Success();
            }

            [[nodiscard]] std::vector<std::size_t> UsedTransientResources() const {
                std::vector<std::size_t> resources;
                resources.reserve(graph_.Resources().size());
                for (std::size_t index = 0; index < graph_.Resources().size(); ++index) {
                    if (IsUsedTransient(index)) {
                        resources.push_back(index);
                    }
                }
                std::ranges::sort(resources, [this](const std::size_t left, const std::size_t right) {
                    if (lifetimes_[left].firstUseIndex != lifetimes_[right].firstUseIndex) {
                        return lifetimes_[left].firstUseIndex < lifetimes_[right].firstUseIndex;
                    }
                    return left < right;
                });
                return resources;
            }

            [[nodiscard]] Result<void> AssignResourceSlot(const std::size_t resource, std::map<SlotGroupKey, SlotGroup> &groups,
                                                          std::vector<std::size_t> &previousResourceBySlot, std::uint32_t &nextSlot) {
                if (!CanAlias(resource)) {
                    slotsByResource_[resource] = {nextSlot++};
                    return Result<void>::Success();
                }
                SlotGroup &group = groups[{compatibilityByResource_[resource], *aliasRoleByResource_[resource]}];
                ReleaseCompletedSlots(resource, group);
                const std::uint32_t slot = AcquireSlot(resource, group, previousResourceBySlot, nextSlot);
                if (slot == 0) {
                    return Result<void>::Failure(MakeError(RenderGraphLifetimeErrors::CapacityExceeded));
                }
                slotsByResource_[resource] = {slot};
                previousResourceBySlot[slot] = resource;
                group.active.emplace(lifetimes_[resource].lastUseIndex, slot);
                return Result<void>::Success();
            }

            void ReleaseCompletedSlots(const std::size_t resource, SlotGroup &group) const {
                while (!group.active.empty() && group.active.top().first < lifetimes_[resource].firstUseIndex) {
                    group.available.push(group.active.top().second);
                    group.active.pop();
                }
            }

            [[nodiscard]] std::uint32_t AcquireSlot(const std::size_t resource, SlotGroup &group,
                                                    const std::vector<std::size_t> &previousResourceBySlot, std::uint32_t &nextSlot) {
                if (group.available.empty()) {
                    return nextSlot++;
                }
                if (aliases_.size() == limits_.maxAliasOpportunities) {
                    return 0;
                }
                const std::uint32_t slot = group.available.top();
                group.available.pop();
                const std::size_t previous = previousResourceBySlot[slot];
                aliases_.emplace_back(graph_.Resources()[previous].id, graph_.Resources()[resource].id, compatibilityByResource_[resource]);
                return slot;
            }

            [[nodiscard]] LifetimeCompileRecords BuildRecords() const {
                LifetimeCompileRecords records;
                records.lifetimes = lifetimes_;
                records.allocations.reserve(graph_.Resources().size());
                for (std::size_t index = 0; index < graph_.Resources().size(); ++index) {
                    if (!IsUsedTransient(index)) {
                        continue;
                    }
                    records.allocations.emplace_back(graph_.Resources()[index].id, compatibilityByResource_[index], slotsByResource_[index],
                                                     requirementByResource_[index]->descriptor);
                }
                records.aliases = aliases_;
                return records;
            }

            [[nodiscard]] bool IsPassValid(const RenderGraphPassRef pass) const noexcept {
                return pass.owner == graph_.Owner() && pass.id.value > 0 && pass.id.value <= graph_.Passes().size() &&
                       graph_.Passes()[GraphIndex(pass)].reference == pass;
            }

            [[nodiscard]] bool IsResourceValid(const RenderGraphResourceId resource) const noexcept {
                return resource.owner == graph_.Owner() && resource.value > 0 && resource.value <= graph_.Resources().size() &&
                       graph_.Resources()[GraphIndex(resource)].id == resource;
            }

            [[nodiscard]] bool IsUsedTransient(const std::size_t resource) const noexcept {
                return graph_.Resources()[resource].resourceClass == RenderGraphResourceClass::Transient &&
                       lifetimes_[resource].disposition == RenderGraphLifetimeDisposition::Used;
            }

            [[nodiscard]] bool CanAlias(const std::size_t resource) const noexcept {
                return IsUsedTransient(resource) && aliasRoleByResource_[resource].has_value() && mixedAliasRole_[resource] == 0;
            }

            const RenderGraph &graph_;
            const RenderGraphSchedule &schedule_;
            std::span<const RenderGraphTransientRequirement> requirements_;
            const RenderGraphLifetimeLimits &limits_;
            std::vector<std::size_t> passPositions_;
            std::vector<const RenderGraphTransientRequirement *> requirementByResource_;
            std::vector<RenderGraphResourceLifetime> lifetimes_;
            std::vector<std::optional<RenderQueueRole>> aliasRoleByResource_;
            std::vector<std::uint8_t> mixedAliasRole_;
            std::vector<RenderGraphCompatibilityClassId> compatibilityByResource_;
            std::vector<RenderGraphTransientAllocationSlot> slotsByResource_;
            std::vector<RenderGraphAliasOpportunity> aliases_;
        };
    }  // namespace

    /** @copydoc RenderGraphLifetimePlan::RenderGraphLifetimePlan */
    RenderGraphLifetimePlan::RenderGraphLifetimePlan(RenderGraphOwnerId owner, std::vector<RenderGraphResourceLifetime> lifetimes,
                                                     std::vector<RenderGraphTransientAllocationRequirement> allocations,
                                                     std::vector<RenderGraphAliasOpportunity> aliases) noexcept
        : owner_(owner), lifetimes_(std::move(lifetimes)), allocations_(std::move(allocations)), aliases_(std::move(aliases)) {}

    /** @copydoc RenderGraphLifetimePlan::RenderGraphLifetimePlan(RenderGraphLifetimePlan &&) */
    RenderGraphLifetimePlan::RenderGraphLifetimePlan(RenderGraphLifetimePlan &&other) noexcept
        : owner_(std::exchange(other.owner_, {})), lifetimes_(std::exchange(other.lifetimes_, {})),
          allocations_(std::exchange(other.allocations_, {})), aliases_(std::exchange(other.aliases_, {})) {}

    /** @copydoc RenderGraphLifetimePlan::operator= */
    RenderGraphLifetimePlan &RenderGraphLifetimePlan::operator=(RenderGraphLifetimePlan &&other) noexcept {
        if (this != &other) {
            owner_ = std::exchange(other.owner_, {});
            lifetimes_ = std::exchange(other.lifetimes_, {});
            allocations_ = std::exchange(other.allocations_, {});
            aliases_ = std::exchange(other.aliases_, {});
        }
        return *this;
    }

    /** @copydoc RenderGraphLifetimePlan::Owner */
    RenderGraphOwnerId RenderGraphLifetimePlan::Owner() const noexcept {
        return owner_;
    }

    /** @copydoc RenderGraphLifetimePlan::Lifetimes */
    std::span<const RenderGraphResourceLifetime> RenderGraphLifetimePlan::Lifetimes() const noexcept {
        return lifetimes_;
    }

    /** @copydoc RenderGraphLifetimePlan::AllocationRequirements */
    std::span<const RenderGraphTransientAllocationRequirement> RenderGraphLifetimePlan::AllocationRequirements() const noexcept {
        return allocations_;
    }

    /** @copydoc RenderGraphLifetimePlan::AliasOpportunities */
    std::span<const RenderGraphAliasOpportunity> RenderGraphLifetimePlan::AliasOpportunities() const noexcept {
        return aliases_;
    }

    /** @copydoc CompileRenderGraphLifetimePlan */
    Result<RenderGraphLifetimePlan> CompileRenderGraphLifetimePlan(
        const RenderGraph &graph, const RenderGraphSchedule &schedule,
        const std::span<const RenderGraphTransientRequirement> transientRequirements, const RenderGraphLifetimeLimits &limits) {
        try {
            auto compiled = LifetimeCompiler{graph, schedule, transientRequirements, limits}.Run();
            if (compiled.HasError()) {
                return Result<RenderGraphLifetimePlan>::Failure(compiled.ErrorValue());
            }
            LifetimeCompileRecords records = std::move(compiled).Value();
            return Result<RenderGraphLifetimePlan>::Success(RenderGraphLifetimePlan{graph.Owner(), std::move(records.lifetimes),
                                                                                    std::move(records.allocations),
                                                                                    std::move(records.aliases)});
        } catch (const std::bad_alloc &) {
            return Result<RenderGraphLifetimePlan>::Failure(MakeError(RenderGraphLifetimeErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render
