#include "Horo/Runtime/Scene/IncrementalSceneCellCook.h"

#include "SceneCellContentHasher.h"

#include <algorithm>
#include <limits>

namespace Horo::Runtime {
    namespace {
        template <class T> Result<T> Failure(const ErrorCodeDescriptor &code) {
            return Result<T>::Failure(MakeError(code));
        }

        /** @brief Validated canonical graph scratch owned only by this synchronous cook
         * attempt. */
        struct DependencyGraph final {
            std::vector<const SceneCellCookDependency *> nodes;
            std::vector<std::vector<std::size_t>> edges;

            std::size_t Find(const Assets::AssetId &id) const {
                const auto found = std::ranges::lower_bound(nodes, id, {}, [](const auto *node) {
                    return node->id;
                });
                return found != nodes.end() && (*found)->id == id ? static_cast<std::size_t>(found - nodes.begin()) : nodes.size();
            }
        };

        /** @brief Rejects cycles with bounded iterative DFS without growing the native
         * call stack. */
        Result<void> ValidateAcyclic(const DependencyGraph &graph, const CancellationToken &cancellation) {
            // Iterative DFS keeps hostile deep graphs off the native call stack.
            std::vector<std::uint8_t> colors(graph.nodes.size());
            std::vector<std::pair<std::size_t, std::size_t>> stack;
            for (std::size_t root = 0; root < graph.nodes.size(); ++root) {
                if (colors[root])
                    continue;
                colors[root] = 1;
                stack.emplace_back(root, 0);
                while (!stack.empty()) {
                    if (cancellation.IsCancellationRequested())
                        return Failure<void>(SceneCellPayloadErrors::Cancelled);
                    auto &[node, cursor] = stack.back();
                    if (cursor == graph.edges[node].size()) {
                        colors[node] = 2;
                        stack.pop_back();
                        continue;
                    }
                    const auto child = graph.edges[node][cursor++];
                    if (colors[child] == 1)
                        return Failure<void>(SceneCellPayloadErrors::Invalid);
                    if (colors[child] == 0) {
                        colors[child] = 1;
                        stack.emplace_back(child, 0);
                    }
                }
            }
            return Result<void>::Success();
        }

        /** @brief Validates bounded complete dependency capture and rejects missing,
         * repeated and cyclic edges. */
        Result<DependencyGraph> BuildGraph(std::span<const SceneCellCookDependency> dependencies, SceneCellCookCacheLimits limits,
                                           const CancellationToken &cancellation) {
            if (dependencies.size() > limits.maximumDependencies)
                return Failure<DependencyGraph>(SceneCellPayloadErrors::CapacityExceeded);
            std::size_t edgeCount{};
            for (const auto &node : dependencies) {
                if (cancellation.IsCancellationRequested())
                    return Failure<DependencyGraph>(SceneCellPayloadErrors::Cancelled);
                if (!node.id.IsValid() || node.revision == 0 || node.content == Sha256Digest{})
                    return Failure<DependencyGraph>(SceneCellPayloadErrors::Invalid);
                if (node.dependencies.size() > limits.maximumDependencyEdges - edgeCount)
                    return Failure<DependencyGraph>(SceneCellPayloadErrors::CapacityExceeded);
                edgeCount += node.dependencies.size();
            }
            DependencyGraph graph;
            for (const auto &node : dependencies)
                graph.nodes.push_back(&node);
            std::ranges::sort(graph.nodes, {}, [](const auto *node) {
                return node->id;
            });
            graph.edges.resize(graph.nodes.size());
            for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
                if (i && graph.nodes[i - 1]->id == graph.nodes[i]->id)
                    return Failure<DependencyGraph>(SceneCellPayloadErrors::Invalid);
                for (const auto &dependency : graph.nodes[i]->dependencies) {
                    if (cancellation.IsCancellationRequested())
                        return Failure<DependencyGraph>(SceneCellPayloadErrors::Cancelled);
                    const auto index = graph.Find(dependency);
                    if (index == graph.nodes.size())
                        return Failure<DependencyGraph>(SceneCellPayloadErrors::Invalid);
                    graph.edges[i].push_back(index);
                }
                std::ranges::sort(graph.edges[i]);
                if (std::ranges::adjacent_find(graph.edges[i]) != graph.edges[i].end())
                    return Failure<DependencyGraph>(SceneCellPayloadErrors::Invalid);
            }
            auto acyclic = ValidateAcyclic(graph, cancellation);
            if (acyclic.HasError())
                return Result<DependencyGraph>::Failure(acyclic.ErrorValue());
            return Result<DependencyGraph>::Success(std::move(graph));
        }

        /** @brief Hashes only the canonical transitive closure used by this cell,
         * including projected navigation dependencies. */
        Result<void> HashDependencies(CellCookDetail::ContentHasher &hash, const SceneCellPayloadSource &source,
                                      const DependencyGraph &graph, const CancellationToken &cancellation) {
            std::vector<std::size_t> stack;
            std::vector<bool> visited(graph.nodes.size());
            const auto add = [&](const Assets::AssetId &id) {
                const auto index = graph.Find(id);
                if (index == graph.nodes.size())
                    return false;
                if (!visited[index]) {
                    visited[index] = true;
                    stack.push_back(index);
                }
                return true;
            };
            for (const auto &dependency : source.dependencies)
                if (!add(dependency.id))
                    return Failure<void>(SceneCellPayloadErrors::Invalid);
            for (const auto &entity : source.entities) {
                const auto &surface = entity.components.navigationSurface;
                if (surface && surface->enabled && !add(surface->definition))
                    return Failure<void>(SceneCellPayloadErrors::Invalid);
            }
            while (!stack.empty()) {
                if (cancellation.IsCancellationRequested())
                    return Failure<void>(SceneCellPayloadErrors::Cancelled);
                const auto node = stack.back();
                stack.pop_back();
                for (const auto child : graph.edges[node])
                    if (!visited[child]) {
                        visited[child] = true;
                        stack.push_back(child);
                    }
            }
            hash.Add(static_cast<std::uint64_t>(std::ranges::count(visited, true)));
            for (std::size_t i = 0; i < visited.size(); ++i) {
                if (cancellation.IsCancellationRequested())
                    return Failure<void>(SceneCellPayloadErrors::Cancelled);
                if (!visited[i])
                    continue;
                const auto &node = *graph.nodes[i];
                hash.Fields(node.id, node.content, node.revision, graph.edges[i].size());
                for (const auto child : graph.edges[i])
                    hash.Add(graph.nodes[child]->id);
            }
            return Result<void>::Success();
        }

        /** @brief Canonicalizes unordered requirements and schema capabilities without
         * changing authored entity order. */
        template <class T, class Projection>
        void HashSorted(CellCookDetail::ContentHasher &hash, std::span<const T> values, Projection projection) {
            std::vector<const T *> sorted;
            for (const auto &value : values)
                sorted.push_back(&value);
            std::ranges::sort(sorted, {}, [&](const auto *value) {
                return projection(*value);
            });
            hash.Add(sorted.size());
            for (const auto *value : sorted)
                hash.Add(*value);
        }

        /** @brief Computes the complete separate Scene-baseline key version 1 over
         * actual typed source fields. */
        Result<Sha256Digest> Key(const SceneCellPayloadSource &source, const DependencyGraph &graph, const Sha256Digest &settings,
                                 std::size_t maximumBytes, const CancellationToken &cancellation) {
            CellCookDetail::ContentHasher hash{cancellation, maximumBytes};
            hash.Fields(std::string_view{"Horo.SceneCellCook.Key.v1"}, settings, source.identity, source.entities);
            HashSorted(hash, source.dependencies, [](const auto &d) {
                return std::pair{d.id, d.expectedType};
            });
            HashSorted(hash, source.componentSchemas, [](const auto &s) {
                return std::pair{s.type, s.version};
            });
            HashSorted(hash, source.behaviorSchemas, [](const auto &s) {
                return std::pair{s.type, s.version};
            });
            auto dependencies = HashDependencies(hash, source, graph, cancellation);
            if (dependencies.HasError())
                return Result<Sha256Digest>::Failure(dependencies.ErrorValue());
            return hash.Finish();
        }

        /** @brief Checks topology and exact source fence on every request, including
         * cache hits. */
        Result<void> ValidateInput(const WorldStreaming::WorldPartitionDescriptor &partition, const SceneCellCookInput &input,
                                   SceneCellPayloadLimits limits, std::size_t maximumSchemas) {
            const auto &source = input.source;
            const auto &id = source.identity;
            if (!id.partition.IsValid() || !id.cell.IsValid() || !id.scene.IsValid() || id.revision.value == 0)
                return Failure<void>(SceneCellPayloadErrors::Invalid);
            if (id != input.expected || id.partition != partition.Partition())
                return Failure<void>(SceneCellPayloadErrors::Stale);
            if (std::ranges::none_of(partition.Cells(), [&](const auto &cell) {
                return cell.id == id.cell;
            }))
                return Failure<void>(SceneCellPayloadErrors::Invalid);
            if (source.entities.size() > limits.maximumEntities || source.dependencies.size() > limits.maximumDependencies ||
                source.componentSchemas.size() > maximumSchemas ||
                source.behaviorSchemas.size() > maximumSchemas - source.componentSchemas.size())
                return Failure<void>(SceneCellPayloadErrors::CapacityExceeded);
            return Result<void>::Success();
        }

        /** @brief Validates and orders the complete desired set before any cell candidate is cooked. */
        Result<std::vector<const SceneCellCookInput *>> CaptureInputs(const WorldStreaming::WorldPartitionDescriptor &partition,
                                                                      std::span<const SceneCellCookInput> inputs,
                                                                      SceneCellPayloadLimits payloadLimits, std::size_t maximumSchemas) {
            std::vector<const SceneCellCookInput *> sorted;
            for (const auto &input : inputs) {
                auto valid = ValidateInput(partition, input, payloadLimits, maximumSchemas);
                if (valid.HasError())
                    return Result<std::vector<const SceneCellCookInput *>>::Failure(valid.ErrorValue());
                sorted.push_back(&input);
            }
            std::ranges::sort(sorted, [](const auto *a, const auto *b) {
                return WorldStreaming::StreamingCellCanonicalLess{}(a->source.identity.cell, b->source.identity.cell);
            });
            for (std::size_t i = 1; i < sorted.size(); ++i)
                if (sorted[i - 1]->source.identity.cell == sorted[i]->source.identity.cell)
                    return Failure<std::vector<const SceneCellCookInput *>>(SceneCellPayloadErrors::Invalid);
            return Result<std::vector<const SceneCellCookInput *>>::Success(std::move(sorted));
        }

        /** @brief Detached cell candidate and actual hit/miss disposition for one
         * complete batch. */
        struct CellCookCandidate final {
            SceneCellCookEntry entry;
            bool reused{};
        };

        /** @brief Prepares an exact hit or real Scene cook without modifying retained
         * owner state. */
        Result<CellCookCandidate> PrepareCell(const WorldStreaming::WorldPartitionDescriptor &partition, const SceneCellCookInput &input,
                                              const DependencyGraph &graph, const Sha256Digest &settings,
                                              SceneCellPayloadLimits payloadLimits, std::size_t maximumKeyBytes,
                                              std::span<const SceneCellCookEntry> entries, const CancellationToken &cancellation) {
            auto key = Key(input.source, graph, settings, maximumKeyBytes, cancellation);
            if (key.HasError())
                return Result<CellCookCandidate>::Failure(key.ErrorValue());
            const auto found = std::ranges::lower_bound(entries, input.source.identity.cell, WorldStreaming::StreamingCellCanonicalLess{},
                                                        [](const auto &entry) {
                return entry.payload->Identity().cell;
            });
            if (found != entries.end() && found->key == key.Value())
                return Result<CellCookCandidate>::Success({{key.Value(), found->payload}, true});
            auto cooked = CookRuntimeSceneCellPayload(partition, input.source, input.expected, payloadLimits, cancellation);
            if (cooked.HasError())
                return Result<CellCookCandidate>::Failure(cooked.ErrorValue());
            auto payload = std::make_shared<const RuntimeSceneCellPayload>(std::move(cooked).Value());
            return Result<CellCookCandidate>::Success({{key.Value(), std::move(payload)}, false});
        }
    }  // namespace

    /** @copydoc IncrementalSceneCellCook::IncrementalSceneCellCook */
    IncrementalSceneCellCook::IncrementalSceneCellCook(SceneCellCookCacheLimits limits) noexcept : limits_(limits) {}

    /** @copydoc IncrementalSceneCellCook::Revision */
    std::uint64_t IncrementalSceneCellCook::Revision() const noexcept {
        return revision_;
    }

    /** @copydoc IncrementalSceneCellCook::Shutdown */
    void IncrementalSceneCellCook::Shutdown() noexcept {
        closed_ = true;
        entries_.clear();
    }

    /** @copydoc IncrementalSceneCellCook::Cook */
    Result<SceneCellCookReport> IncrementalSceneCellCook::Cook(const WorldStreaming::WorldPartitionDescriptor &partition,
                                                               std::span<const SceneCellCookInput> inputs,
                                                               std::span<const SceneCellCookDependency> dependencies,
                                                               const Sha256Digest &settings, std::uint64_t expectedRevision,
                                                               SceneCellPayloadLimits payloadLimits,
                                                               const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return Failure<SceneCellCookReport>(SceneCellPayloadErrors::Cancelled);
        if (closed_ || !limits_.maximumCells || !limits_.maximumDependencies || !limits_.maximumDependencyEdges ||
            !limits_.maximumSchemas || !limits_.maximumKeyBytes || !limits_.maximumRetainedBytes || !payloadLimits.maximumEntities ||
            !payloadLimits.maximumDependencies || !payloadLimits.maximumRetainedBytes || settings == Sha256Digest{})
            return Failure<SceneCellCookReport>(SceneCellPayloadErrors::Invalid);
        if (expectedRevision != revision_)
            return Failure<SceneCellCookReport>(SceneCellPayloadErrors::Stale);
        if (inputs.size() > limits_.maximumCells || revision_ == std::numeric_limits<std::uint64_t>::max())
            return Failure<SceneCellCookReport>(SceneCellPayloadErrors::CapacityExceeded);
        auto graph = BuildGraph(dependencies, limits_, cancellation);
        if (graph.HasError())
            return Result<SceneCellCookReport>::Failure(graph.ErrorValue());
        auto sorted = CaptureInputs(partition, inputs, payloadLimits, limits_.maximumSchemas);
        if (sorted.HasError())
            return Result<SceneCellCookReport>::Failure(sorted.ErrorValue());
        SceneCellCookReport report{.revision = revision_ + 1};
        std::size_t retained{};
        for (const auto *captured : sorted.Value()) {
            const auto &input = *captured;
            auto candidate =
                PrepareCell(partition, input, graph.Value(), settings, payloadLimits, limits_.maximumKeyBytes, entries_, cancellation);
            if (candidate.HasError())
                return Result<SceneCellCookReport>::Failure(candidate.ErrorValue());
            auto prepared = std::move(candidate).Value();
            const auto &payload = prepared.entry.payload;
            if (payload->RetainedBytes() > payloadLimits.maximumRetainedBytes ||
                payload->Definition().AssetDependencies().size() > payloadLimits.maximumDependencies ||
                payload->RetainedBytes() > limits_.maximumRetainedBytes - retained)
                return Failure<SceneCellCookReport>(SceneCellPayloadErrors::CapacityExceeded);
            retained += payload->RetainedBytes();
            report.reused += prepared.reused ? 1 : 0;
            report.cooked += prepared.reused ? 0 : 1;
            report.cells.push_back(std::move(prepared.entry));
        }
        auto replacement = report.cells;
        if (cancellation.IsCancellationRequested())
            return Failure<SceneCellCookReport>(SceneCellPayloadErrors::Cancelled);
        entries_.swap(replacement);
        revision_ = report.revision;
        return Result<SceneCellCookReport>::Success(std::move(report));
    }
}  // namespace Horo::Runtime
