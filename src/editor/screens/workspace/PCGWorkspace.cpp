#include "editor/screens/workspace/PCGWorkspace.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Horo::Editor {
    namespace {
        /** @brief Returns a stable canvas rejection without modifying canonical or presentation state. */
        template <class T> Result<T> Reject(PCGDocumentCommandFailureReason reason) {
            return Result<T>::Failure(MakeError(PCGDocumentError(reason)));
        }

        /** @brief Resolves a stable node in one immutable publication. */
        bool HasNode(const PCG::PCGGraphSourceData &data, PCG::NodeId id) {
            return std::ranges::find(data.nodes, id, &PCG::PCGGraphNode::id) != data.nodes.end();
        }

        /** @brief Finite presentation bounds prevent malformed widget requests from poisoning subsequent frames. */
        bool PositionValid(Math::Vec2 position) {
            return Math::IsFinite(position) && std::abs(position.x) <= 1'000'000.0F && std::abs(position.y) <= 1'000'000.0F;
        }

        /** @brief Allocates after the greatest live identity with explicit exhaustion rejection. */
        template <class Id> Result<Id> Next(std::uint64_t value) {
            if (value == std::numeric_limits<std::uint64_t>::max())
                return Reject<Id>(PCGDocumentCommandFailureReason::Exhausted);
            return Id::Create(value + 1);
        }

        /** @brief Expands node creation from a host-owned schema, assigning globally unique pin identities. */
        Result<void> AddNode(PCGDocumentCommand &command, const PCGEditorRequest &request, const PCG::PCGGraphSourceData &data,
                             std::span<const PCGPaletteEntry> palette) {
            const auto recipe = std::ranges::find(palette, request.type, [](const auto &entry) {
                return entry.prototype.type;
            });
            if (recipe == palette.end())
                return Reject<void>(PCGDocumentCommandFailureReason::Invalid);
            std::uint64_t nodeId{}, pinId{};
            for (const auto &node : data.nodes) {
                nodeId = std::max(nodeId, node.id.Value());
                for (const auto &pin : node.pins)
                    pinId = std::max(pinId, pin.id.Value());
            }
            const auto next = Next<PCG::NodeId>(nodeId);
            if (next.HasError())
                return Result<void>::Failure(next.ErrorValue());
            auto node = recipe->prototype;
            node.id = next.Value();
            for (auto &pin : node.pins) {
                const auto id = Next<PCG::PinId>(pinId);
                if (id.HasError())
                    return Result<void>::Failure(id.ErrorValue());
                pin.id = id.Value();
                pinId = pin.id.Value();
            }
            command.operations.emplace_back(PutPCGNode{std::move(node)});
            return Result<void>::Success();
        }

        /** @brief Closes every incident reference in one atomic, undoable removal transaction. */
        void RemoveSelected(PCGDocumentCommand &command, const PCGCanvasState &state, const PCG::PCGGraphSourceData &data) {
            const auto selected = [&](PCG::NodeId id) {
                return std::ranges::find(state.selection, id) != state.selection.end();
            };
            for (const auto &edge : data.edges)
                if (selected(edge.sourceNode) || selected(edge.targetNode))
                    command.operations.emplace_back(RemovePCGEdge{edge.id});
            for (const auto &input : data.exposedInputs)
                if (selected(input.node))
                    command.operations.emplace_back(RemovePCGInput{input.id});
            for (const auto id : state.selection)
                command.operations.emplace_back(RemovePCGNode{id});
        }
    }  // namespace

    /** @copydoc ValidatePCGCanvas */
    Result<void> ValidatePCGCanvas(const PCGCanvasState &state, const PCG::PCGGraphAsset &source) {
        const auto &data = source.Data();
        if (!PositionValid(state.pan) || !PositionValid(state.dragOrigin) || !std::isfinite(state.zoom) || state.zoom < 0.25F ||
            state.zoom > 2.0F || state.filter.size() > 128 || state.placements.size() != data.nodes.size() ||
            state.selection.size() > data.nodes.size() || state.selection.size() > 256)
            return Reject<void>(PCGDocumentCommandFailureReason::Limit);
        std::vector<PCG::NodeId> ids;
        ids.reserve(state.placements.size());
        for (const auto &placement : state.placements) {
            if (!HasNode(data, placement.node) || !PositionValid(placement.position))
                return Reject<void>(PCGDocumentCommandFailureReason::Invalid);
            ids.push_back(placement.node);
        }
        std::ranges::sort(ids);
        if (std::ranges::adjacent_find(ids) != ids.end())
            return Reject<void>(PCGDocumentCommandFailureReason::Invalid);
        ids = state.selection;
        std::ranges::sort(ids);
        if (std::ranges::adjacent_find(ids) != ids.end() || std::ranges::any_of(ids, [&](auto id) {
            return !HasNode(data, id);
        }))
            return Reject<void>(PCGDocumentCommandFailureReason::Invalid);
        if (state.draggingNode && !HasNode(data, *state.draggingNode))
            return Reject<void>(PCGDocumentCommandFailureReason::Invalid);
        if (state.connectingNode.has_value() != state.connectingPin.has_value())
            return Reject<void>(PCGDocumentCommandFailureReason::Invalid);
        if (state.connectingNode) {
            const auto node = std::ranges::find(data.nodes, *state.connectingNode, &PCG::PCGGraphNode::id);
            if (node == data.nodes.end())
                return Reject<void>(PCGDocumentCommandFailureReason::Invalid);
            const auto pin = std::ranges::find(node->pins, *state.connectingPin, &PCG::PCGGraphPin::id);
            if (pin == node->pins.end() || pin->direction != PCG::PCGPinDirection::Output)
                return Reject<void>(PCGDocumentCommandFailureReason::Invalid);
        }
        return Result<void>::Success();
    }

    /** @copydoc CancelPCGCanvasGesture */
    void CancelPCGCanvasGesture(PCGCanvasState &state) {
        if (state.draggingNode)
            for (auto &placement : state.placements)
                if (placement.node == *state.draggingNode)
                    placement.position = state.dragOrigin;
        state.draggingNode.reset();
        state.connectingNode.reset();
        state.connectingPin.reset();
    }

    /** @copydoc RefreshPCGCanvas */
    void RefreshPCGCanvas(PCGWorkspaceDocument &document, const PCGDocumentSnapshot &snapshot) {
        auto &state = document.canvas;
        if (document.snapshot.session != snapshot.session)
            state = {};
        if (document.snapshot.revision != snapshot.revision || document.snapshot.catalogRevision != snapshot.catalogRevision) {
            CancelPCGCanvasGesture(state);
        }
        const auto &data = snapshot.source->Data();
        std::erase_if(state.placements, [&](const auto &entry) {
            return !HasNode(data, entry.node);
        });
        std::erase_if(state.selection, [&](auto id) {
            return !HasNode(data, id);
        });
        for (std::size_t index = 0; index < data.nodes.size(); ++index) {
            const auto id = data.nodes[index].id;
            if (std::ranges::find(state.placements, id, &PCGNodePlacement::node) == state.placements.end())
                state.placements.push_back({id, {static_cast<float>(index % 4) * 280.0F, static_cast<float>(index / 4) * 240.0F}});
        }
        document.snapshot = snapshot;
    }

    /** @copydoc PreparePCGEdit */
    Result<PCGDocumentCommand> PreparePCGEdit(const PCGEditorRequest &request, const PCG::PCGGraphAsset &source,
                                              std::span<const PCGPaletteEntry> palette) {
        if (!request.fence.operations.empty())
            return Reject<PCGDocumentCommand>(PCGDocumentCommandFailureReason::Invalid);
        auto command = request.fence;
        const auto &data = source.Data();
        switch (request.action) {
            case PCGEditorAction::CreateNode:
                if (auto added = AddNode(command, request, data, palette); added.HasError())
                    return Result<PCGDocumentCommand>::Failure(added.ErrorValue());
                break;
            case PCGEditorAction::RemoveSelection:
                RemoveSelected(command, request.presentation, data);
                break;
            case PCGEditorAction::Connect: {
                std::uint64_t maximum{};
                for (const auto &edge : data.edges)
                    maximum = std::max(maximum, edge.id.Value());
                const auto id = Next<PCG::EdgeId>(maximum);
                if (id.HasError())
                    return Result<PCGDocumentCommand>::Failure(id.ErrorValue());
                command.operations.emplace_back(
                    PutPCGEdge{{id.Value(), request.sourceNode, request.sourcePin, request.targetNode, request.targetPin}});
                break;
            }
            case PCGEditorAction::Disconnect:
                command.operations.emplace_back(RemovePCGEdge{request.edge});
                break;
            default:
                return Reject<PCGDocumentCommand>(PCGDocumentCommandFailureReason::Invalid);
        }
        if (command.operations.empty() || command.operations.size() > 256)
            return Reject<PCGDocumentCommand>(PCGDocumentCommandFailureReason::Limit);
        return Result<PCGDocumentCommand>::Success(std::move(command));
    }
}  // namespace Horo::Editor
