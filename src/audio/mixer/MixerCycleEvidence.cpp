#include "MixerPlanState.h"

#include <algorithm>
#include <format>
#include <map>

namespace Horo::Audio::MixerDetail {
    namespace {
        /** @brief Bounded control-only DFS state; all traversal choices use ascending stable identities. */
        struct Search final {
            std::vector<std::uint64_t> ids;
            std::vector<std::vector<std::size_t>> edges;
            std::vector<std::uint8_t> colors;
            std::vector<std::size_t> stack;
            std::vector<std::uint64_t> cycle;
        };

        /** @brief Capture a back edge and rotate its cycle to the smallest encoded bus identity. */
        void Capture(Search &s, const std::size_t destination) {
            const auto first = std::ranges::find(s.stack, destination);
            for (auto at = first; at != s.stack.end(); ++at)
                s.cycle.push_back(s.ids[*at]);
            const auto minimum = std::ranges::min_element(s.cycle);
            std::ranges::rotate(s.cycle, minimum);
        }

        /** @brief Find one deterministic cycle with recursion bounded by the compiled bus ceiling. */
        bool Visit(Search &s, const std::size_t index) {
            s.colors[index] = 1;
            s.stack.push_back(index);
            for (const std::size_t next : s.edges[index]) {
                if (s.colors[next] == 1) {
                    Capture(s, next);
                    return true;
                }
                if (s.colors[next] == 0 && Visit(s, next))
                    return true;
            }
            s.stack.pop_back();
            s.colors[index] = 2;
            return false;
        }

        /** @brief Resolve only structurally usable identity graphs; other schema failures retain their original cause. */
        bool Resolve(const MixerAssetSchema &asset, Search &s) {
            std::map<std::uint64_t, std::size_t> indices;
            for (const MixerBusDescriptor &bus : asset.buses)
                if (!bus.id.IsValid() || !indices.emplace(bus.id.Value(), 0).second)
                    return false;
            for (auto &[id, index] : indices) {
                index = s.ids.size();
                s.ids.push_back(id);
            }
            s.edges.resize(s.ids.size());
            s.colors.resize(s.ids.size());
            for (const MixerRouteDescriptor &route : asset.routes) {
                const auto source = indices.find(route.source.Value());
                const auto destination = indices.find(route.destination.Value());
                if (source == indices.end() || destination == indices.end())
                    return false;
                s.edges[source->second].push_back(destination->second);
            }
            for (auto &edges : s.edges)
                std::ranges::sort(edges);
            return true;
        }
    }  // namespace

    /** @copydoc CycleEvidence */
    std::string CycleEvidence(const MixerAssetSchema &asset) {
        if (asset.buses.size() > MaximumMixerAssetBuses || asset.routes.size() > MaximumMixerAssetRoutes)
            return {};
        Search search;
        if (!Resolve(asset, search))
            return {};
        for (std::size_t i = 0; i < search.ids.size(); ++i) {
            if (search.colors[i] == 0 && Visit(search, i)) {
                std::string result = "Cycle buses: ";
                for (const std::uint64_t id : search.cycle)
                    result += std::format("{} -> ", id);
                return std::format("{}{}", result, search.cycle.front());
            }
        }
        return {};
    }
}  // namespace Horo::Audio::MixerDetail
