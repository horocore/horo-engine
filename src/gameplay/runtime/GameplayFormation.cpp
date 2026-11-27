#include "Horo/Gameplay/GameplayFormation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>

namespace Horo::Gameplay {
    namespace {
        using Navigation::NavigationErrors::AgentDescriptorInvalid;
        using Preparation = Result<GameplayFormationPlan>;

        /** @brief Accepts finite planar geometry before any coordinate arithmetic. */
        bool IsPlanar(const Math::Vec3 value) noexcept {
            return Math::IsFinite(value) && value.y == 0.0F;
        }

        /** @brief Validates the transform and optional leader authority before reading agent records. */
        bool ValidAnchor(const GameplayFormationRequest &request, const Navigation::NavigationWorldId world) noexcept {
            if (!IsPlanar(request.forward) || request.forward == Math::Vec3{} || !std::isfinite(request.spacingMeters) ||
                request.spacingMeters <= 0)
                return false;
            return !request.preferredLeader || (request.preferredLeader->IsValid() && request.preferredLeader->world == world);
        }

        /** @brief Rejects malformed shape data and offsets that reuse the leader or another slot. */
        bool ValidLayout(const GameplayFormationRequest &request) noexcept {
            if (request.shape != GameplayFormationShape::Custom)
                return request.shape < GameplayFormationShape::Custom && request.customSlots.empty();
            const auto slots = request.customSlots;
            for (std::size_t index = 0; index < slots.size(); ++index) {
                if (const auto offset = slots[index]; !IsPlanar(offset) || offset == Math::Vec3{} ||
                                                      std::ranges::find(slots.first(index), offset) != slots.first(index).end())
                    return false;
            }
            return true;
        }

        /** @brief Builds offsets in the leader's local right/forward coordinates. */
        Math::Vec3 LocalSlot(const GameplayFormationRequest &request, const std::size_t slot) noexcept {
            using enum GameplayFormationShape;
            if (slot == 0)
                return {};
            if (request.shape == Custom)
                return request.customSlots[slot - 1];
            if (request.shape == Column)
                return {0, 0, -static_cast<float>(slot) * request.spacingMeters};
            const auto row = static_cast<float>((slot + 1) / 2) * request.spacingMeters;
            const auto side = slot % 2 == 1 ? row : -row;
            return {side, 0, request.shape == Wedge ? -row : 0.0F};
        }

        /** @brief Converts a candidate into world coordinates without normalizing through overflowing floats. */
        std::optional<Math::Vec3> WorldSlot(const Math::Vec3 anchor, const double x, const double z, const Math::Vec3 local) noexcept {
            const auto worldX = anchor.x + z * local.x + x * local.z;
            const auto worldZ = anchor.z - x * local.x + z * local.z;
            if (constexpr auto limit = std::numeric_limits<float>::max(); !(std::abs(worldX) <= limit && std::abs(worldZ) <= limit))
                return std::nullopt;
            return Math::Vec3{static_cast<float>(worldX), anchor.y, static_cast<float>(worldZ)};
        }

        /** @brief Keeps survivor order stable when promoting the requested leader. */
        void PromoteLeader(const std::span<GameplayFormationMember> members, const Navigation::CrowdAgentHandle preferred) {
            const auto leader = std::ranges::find(members, preferred, &GameplayFormationMember::agent);
            if (leader != members.end())
                std::rotate(members.begin(), leader, std::next(leader));
        }

        /** @brief Validates identities and returns only live grounded members in stable handle order. */
        Result<std::span<GameplayFormationMember>> CollectMembers(const Navigation::NavigationAgentSnapshot &agents,
                                                                  const std::span<const GameplayFormationMember> input,
                                                                  const std::span<GameplayFormationMember> buffer) {
            using Members = Result<std::span<GameplayFormationMember>>;
            auto sorted = buffer.first(input.size());
            std::ranges::copy(input, sorted.begin());
            std::ranges::sort(sorted, {}, &GameplayFormationMember::agent);
            if (std::ranges::adjacent_find(sorted, {}, &GameplayFormationMember::agent) != sorted.end())
                return Members::Failure(MakeError(AgentDescriptorInvalid));
            std::size_t count = 0;
            for (const auto &member : sorted) {
                if (!member.agent.IsValid() || member.agent.world != agents.Binding().world || !Math::IsFinite(member.position))
                    return Members::Failure(MakeError(AgentDescriptorInvalid));
                const auto record = agents.Find(member.agent);
                if (record.HasValue() && record.Value().enabled &&
                    record.Value().movementCapability == Navigation::NavigationAgentMovementCapability::Grounded)
                    buffer[count++] = member;
            }
            return Members::Success(buffer.first(count));
        }

        /** @brief Releases unfillable members explicitly. No proposal owns or updates Character motion. */
        Preparation AssignSlots(const GameplayFormationRequest &request, GameplayFormationPlan plan,
                                const std::span<const GameplayFormationMember> members) {
            const auto available = request.shape == GameplayFormationShape::Custom ? request.customSlots.size() + 1 : members.size();
            const auto count = std::min(available, members.size());
            plan.slots.reserve(count);
            plan.unassigned.reserve(members.size() - count);
            const auto length = std::hypot(static_cast<double>(request.forward.x), static_cast<double>(request.forward.z));
            const auto forwardX = request.forward.x / length;
            const auto forwardZ = request.forward.z / length;
            for (std::size_t index = 0; index < count; ++index) {
                const auto target = WorldSlot(members.front().position, forwardX, forwardZ, LocalSlot(request, index));
                if (!target || std::ranges::any_of(plan.slots, [&](const auto &slot) {
                    return slot.unprojectedTarget == *target;
                }))
                    return Preparation::Failure(MakeError(AgentDescriptorInvalid));
                plan.slots.emplace_back(static_cast<std::uint32_t>(index), members[index].agent, members[index].position, *target);
            }
            for (const auto &member : members.subspan(count))
                plan.unassigned.push_back(member.agent);
            plan.availability = plan.unassigned.empty() ? GameplayFormationAvailability::Complete : GameplayFormationAvailability::Degraded;
            return Preparation::Success(std::move(plan));
        }
    }  // namespace

    /** @copydoc PrepareGameplayFormation */
    Preparation PrepareGameplayFormation(const GameplayFormationRequest &request, const Navigation::NavigationAgentSnapshot &agents,
                                         const std::span<const GameplayFormationMember> members) {
        if (members.size() > GameplayFormationRequest::MaximumMembers ||
            request.customSlots.size() >= GameplayFormationRequest::MaximumMembers)
            return Preparation::Failure(MakeError(Navigation::NavigationErrors::CapacityExceeded));
        if (!agents.IsValid() || !request.group.IsValid() || request.revision == 0 || !ValidAnchor(request, agents.Binding().world) ||
            !ValidLayout(request))
            return Preparation::Failure(MakeError(AgentDescriptorInvalid));
        try {
            std::array<GameplayFormationMember, GameplayFormationRequest::MaximumMembers> buffer;
            const auto collected = CollectMembers(agents, members, buffer);
            if (collected.HasError())
                return Preparation::Failure(collected.ErrorValue());
            const auto live = collected.Value();
            GameplayFormationPlan plan{.group = request.group, .revision = request.revision, .binding = agents.Binding()};
            if (live.empty())
                return Preparation::Success(std::move(plan));
            if (request.preferredLeader)
                PromoteLeader(live, *request.preferredLeader);
            return AssignSlots(request, std::move(plan), live);
        } catch (const std::bad_alloc &) {
            return Preparation::Failure(MakeError(Navigation::NavigationErrors::CapacityExceeded));
        }
    }
}  // namespace Horo::Gameplay
