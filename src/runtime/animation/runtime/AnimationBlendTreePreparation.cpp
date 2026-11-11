#include "AnimationGraphInstanceInternal.h"

#include <algorithm>

namespace Horo::Animation::Detail {
    namespace {
        /** @brief Resolves explicit names in one instance namespace; unnamed groups remain occurrence-local. */
        std::uint32_t Group(GraphBlendWorkspace &workspace, const GraphBlendTreeNode &tree) {
            if (tree.group.IsValid())
                for (std::uint32_t i = 0; i < workspace.groups.size(); ++i)
                    if (workspace.groups[i].id == tree.group)
                        return i;
            workspace.groups.push_back({tree.group, tree.sync, {}});
            return static_cast<std::uint32_t>(workspace.groups.size() - 1);
        }
    }  // namespace

    /** @copydoc PrepareGraphBlendTrees */
    GraphInstanceFailure PrepareGraphBlendTrees(GraphInstanceState &state) {
        auto &workspace = state.blends;
        const auto &memory = state.program->Memory();
        workspace.trees.resize(memory.blendTrees);
        workspace.members.resize(memory.blendMembers);
        workspace.groups.reserve(memory.blendTrees);
        for (auto &publication : workspace.published)
            publication.resize(memory.blendMembers);
        const auto occurrences = state.program->Occurrences();
        for (std::uint32_t i = 0; i < occurrences.size(); ++i) {
            const auto &occurrence = occurrences[i];
            if (!occurrence.blendTree.has_value())
                continue;
            const auto &tree = std::get<GraphBlendTreeNode>(
                state.program->Definitions()[occurrence.definitionIndex].instructions[occurrence.instructionIndex].payload);
            const auto group = Group(workspace, tree);
            workspace.trees[*occurrence.blendTree] = {i, group};
            for (std::uint32_t sample = 0; sample < tree.samples.size(); ++sample) {
                const auto member = occurrence.blendSamples[sample].member;
                workspace.members[member].tree = *occurrence.blendTree;
                workspace.members[member].sample = sample;
                workspace.groups[group].members.push_back(member);
            }
        }
        return GraphInstanceFailure::None;
    }
}  // namespace Horo::Animation::Detail
