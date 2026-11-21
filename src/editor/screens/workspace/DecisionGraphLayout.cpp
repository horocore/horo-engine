#include "DecisionGraphLayout.h"

#include "DecisionGraphCanvasModel.h"
#include "Horo/Application/DecisionAssetCookHost.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <nlohmann/json.hpp>
#include <set>

namespace Horo::Editor {
    namespace {
        constexpr std::string_view LayoutKind = "horo.decision.graph.layout";

        /** @brief Rejects non-finite, repeated or out-of-budget placements before publication. */
        bool ValidPlacements(const DecisionGraphLayout &layout) {
            if (layout.size() > MaximumDecisionLayoutNodes)
                return false;
            std::set<AI::DecisionNodeId> seen;
            return std::ranges::all_of(layout, [&](const auto &position) {
                return position.node.IsValid() && std::isfinite(position.x) && std::isfinite(position.y) &&
                       std::abs(position.x) <= MaximumDecisionLayoutCoordinate && std::abs(position.y) <= MaximumDecisionLayoutCoordinate &&
                       seen.insert(position.node).second;
            });
        }

        /** @brief Parses the closed metadata schema without accepting duplicate keys or nested objects. */
        Result<DecisionGraphLayout> DecodeLayout(std::span<const std::byte> bytes, Assets::AssetId asset, AI::DecisionGraphAssetId graph) {
            using Layout = Result<DecisionGraphLayout>;
            using Json = nlohmann::json;
            const auto fail = [] {
                return Layout::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            };
            if (bytes.empty() || bytes.size() > MaximumDecisionLayoutBytes)
                return fail();
            bool invalid{};
            std::set<std::string> keys;
            const auto accept = [&](int depth, Json::parse_event_t event, Json &value) {
                if (depth > 3 || (event == Json::parse_event_t::object_start && depth > 0))
                    invalid = true;
                if (event == Json::parse_event_t::key && !keys.insert(value.get<std::string>()).second)
                    invalid = true;
                return !invalid;
            };
            const auto *text = reinterpret_cast<const char *>(bytes.data());
            const auto parsed = Json::parse(text, text + bytes.size(), accept, false);
            if (invalid || !parsed.is_object() || parsed.size() != 5 || parsed.value("kind", Json{}) != Json(std::string{LayoutKind}) ||
                parsed.value("asset", Json{}) != asset.ToString())
                return fail();
            const auto version = parsed.value("version", Json{});
            const auto identity = parsed.value("graph", Json{});
            if (!version.is_number_unsigned() || version != 1 || !identity.is_number_unsigned() || identity != graph.Value())
                return fail();
            const auto entries = parsed.find("nodes");
            if (entries == parsed.end() || !entries->is_array() || entries->size() > MaximumDecisionLayoutNodes)
                return fail();
            DecisionGraphLayout result;
            result.reserve(entries->size());
            for (const auto &item : *entries) {
                if (!item.is_array() || item.size() != 3 || !item[0].is_number_unsigned() || !item[1].is_number() || !item[2].is_number())
                    return fail();
                const auto id = AI::DecisionNodeId::Create(item[0].get<std::uint64_t>());
                if (id.HasError())
                    return fail();
                result.push_back({id.Value(), item[1].get<float>(), item[2].get<float>()});
            }
            if (!ValidPlacements(result))
                return fail();
            std::ranges::sort(result, {}, &DecisionNodePlacement::node);
            return Layout::Success(std::move(result));
        }
    }  // namespace

    /** @copydoc DecodeDecisionLayout */
    Result<DecisionGraphLayout> DecodeDecisionLayout(std::span<const std::byte> bytes, Assets::AssetId asset,
                                                     AI::DecisionGraphAssetId graph) {
        return DecodeLayout(bytes, asset, graph);
    }

    /** @copydoc MergeDecisionLayouts */
    Result<DecisionGraphLayout> MergeDecisionLayouts(const DecisionGraphLayout &base, const DecisionGraphLayout &draft,
                                                     const DecisionGraphLayout &disk) {
        using Layout = Result<DecisionGraphLayout>;
        std::map<AI::DecisionNodeId, std::array<std::optional<DecisionNodePlacement>, 3>> nodes;
        const std::array layouts{&base, &draft, &disk};
        for (std::size_t index = 0; index < layouts.size(); ++index)
            for (const auto &placement : *layouts[index])
                nodes[placement.node][index] = placement;
        DecisionGraphLayout merged;
        for (const auto &entry : nodes) {
            const auto &[original, local, external] = entry.second;
            if (local != original && external != original && local != external)
                return Layout::Failure(
                    MakeError(Assets::AssetSourceErrors::Conflict, "Both versions changed the same Decision placement."));
            const auto &selected = local == original ? external : local;
            if (selected)
                merged.push_back(*selected);
        }
        if (!ValidPlacements(merged))
            return Layout::Failure(MakeError(Assets::AssetSourceErrors::Limit));
        return Layout::Success(std::move(merged));
    }

    /** @copydoc DecisionLayoutPath */
    std::filesystem::path DecisionLayoutPath(const std::filesystem::path &source) {
        auto result = source;
        result += ".meta";
        return result;
    }

    /** @copydoc CaptureDecisionLayout */
    Result<DecisionGraphLayoutCapture> CaptureDecisionLayout(const std::filesystem::path &root, const std::filesystem::path &source,
                                                             Assets::AssetId asset, AI::DecisionGraphAssetId graph) {
        using Capture = Result<DecisionGraphLayoutCapture>;
        const auto file = Assets::CaptureAssetSource(root, DecisionLayoutPath(source), MaximumDecisionLayoutBytes);
        if (file.HasError())
            return Capture::Failure(file.ErrorValue());
        if (!file.Value().identity.exists)
            return Capture::Success({file.Value().identity, {}});
        auto layout = DecodeLayout(file.Value().bytes, asset, graph);
        if (layout.HasError())
            return Capture::Failure(layout.ErrorValue());
        return Capture::Success({file.Value().identity, std::move(layout).Value()});
    }

    /** @copydoc EncodeDecisionLayout */
    Result<std::vector<std::byte>> EncodeDecisionLayout(const DecisionGraphLayout &layout, Assets::AssetId asset,
                                                        AI::DecisionGraphAssetId graph) {
        using Encoded = Result<std::vector<std::byte>>;
        if (!asset.IsValid() || !graph.IsValid() || !ValidPlacements(layout))
            return Encoded::Failure(MakeError(Application::DecisionCookErrors::Invalid));
        auto sorted = layout;
        std::ranges::sort(sorted, {}, &DecisionNodePlacement::node);
        auto nodes = nlohmann::json::array();
        for (const auto &position : sorted)
            nodes.push_back(nlohmann::json::array({position.node.Value(), position.x, position.y}));
        const nlohmann::json file{{"kind", LayoutKind},
                                  {"version", 1},
                                  {"asset", asset.ToString()},
                                  {"graph", graph.Value()},
                                  {"nodes", std::move(nodes)}};
        const auto text = file.dump() + '\n';
        if (text.size() > MaximumDecisionLayoutBytes)
            return Encoded::Failure(MakeError(Assets::AssetSourceErrors::Limit));
        const auto bytes = std::as_bytes(std::span{text});
        return Encoded::Success({bytes.begin(), bytes.end()});
    }

    /** @copydoc AutomaticDecisionLayout */
    DecisionGraphLayout AutomaticDecisionLayout(const GraphViewSnapshot &graph) {
        constexpr std::size_t PinsPerRow = 9;
        const auto missing = graph.nodes.size();
        std::map<AI::DecisionNodeId, std::size_t> indices;
        std::vector<std::vector<std::size_t>> connections(graph.nodes.size());
        std::vector<std::size_t> depth(graph.nodes.size(), missing);
        std::vector<std::size_t> pending;
        pending.reserve(graph.nodes.size());
        for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
            const auto &node = graph.nodes[index];
            indices.emplace(node.id, index);
            if (node.initial) {
                depth[index] = 0;
                pending.push_back(index);
            }
        }
        for (const auto &link : graph.links)
            connections[indices.at(graph.pins[link.from].node)].push_back(indices.at(graph.pins[link.to].node));
        for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
            const auto origin = pending[cursor];
            for (const auto target : connections[origin]) {
                if (depth[target] == missing) {
                    depth[target] = depth[origin] + 1;
                    pending.push_back(target);
                }
            }
        }

        const auto lastReachable = pending.empty() ? 0 : depth[pending.back()];
        std::vector<float> rows(lastReachable + 2, 32.0F);
        DecisionGraphLayout result;
        result.reserve(graph.nodes.size());
        for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
            const auto &node = graph.nodes[index];
            const auto column = depth[index] == missing ? lastReachable + 1 : depth[index];
            const auto pinRows = (node.pinCount + PinsPerRow - 1) / PinsPerRow;
            result.push_back({node.id, 32.0F + static_cast<float>(column) * 236.0F, rows[column]});
            rows[column] += 116.0F + 24.0F * static_cast<float>(pinRows);
        }
        std::ranges::sort(result, {}, &DecisionNodePlacement::node);
        return result;
    }

    /** @copydoc ApplyDecisionLayout */
    Result<DecisionGraphLayout> ApplyDecisionLayout(const GraphViewSnapshot &graph, const DecisionGraphLayout &current,
                                                    const DecisionGraphLayoutRequest &request) {
        using Layout = Result<DecisionGraphLayout>;
        const auto fail = [] {
            return Layout::Failure(MakeError(Application::DecisionCookErrors::Invalid));
        };
        if (graph.instance != request.fence.instance || graph.revision != request.fence.revision)
            return Layout::Failure(MakeError(Application::DecisionCookErrors::Stale));
        if (graph.nodes.size() > MaximumDecisionLayoutNodes)
            return fail();
        if (request.operation == DecisionLayoutOperation::AutoLayout) {
            if (!request.placements.empty())
                return fail();
            auto automatic = AutomaticDecisionLayout(graph);
            return ValidPlacements(automatic) ? Layout::Success(std::move(automatic)) : fail();
        }
        if (request.operation != DecisionLayoutOperation::Move || !ValidPlacements(request.placements))
            return fail();
        auto candidate = PruneDecisionLayout(current, graph);
        const auto nodes = [&] {
            std::set<AI::DecisionNodeId> result;
            for (const auto &node : graph.nodes)
                result.insert(node.id);
            return result;
        }();
        for (const auto &position : request.placements) {
            if (!nodes.contains(position.node))
                return fail();
            const auto found = std::ranges::lower_bound(candidate, position.node, {}, &DecisionNodePlacement::node);
            if (found == candidate.end() || found->node != position.node)
                candidate.insert(found, position);
            else
                *found = position;
        }
        return ValidPlacements(candidate) ? Layout::Success(std::move(candidate)) : fail();
    }

    /** @copydoc PruneDecisionLayout */
    DecisionGraphLayout PruneDecisionLayout(const DecisionGraphLayout &layout, const GraphViewSnapshot &graph) {
        std::set<AI::DecisionNodeId> nodes;
        for (const auto &node : graph.nodes)
            nodes.insert(node.id);
        auto retained = layout;
        std::erase_if(retained, [&](const auto &position) {
            return !nodes.contains(position.node);
        });
        return retained;
    }
}  // namespace Horo::Editor
