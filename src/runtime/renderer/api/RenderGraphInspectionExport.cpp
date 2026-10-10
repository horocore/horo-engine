#include "Horo/Runtime/Render/RenderGraphInspection.h"
#include "Horo/Runtime/Render/RenderGraphInspectionErrors.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <initializer_list>
#include <new>
#include <string_view>
#include <type_traits>

namespace Horo::Render {
    namespace {
        /** @brief Internal bounded encoding failure; never escapes the public Result boundary. */
        struct ExportFailure {
            const ErrorCodeDescriptor *descriptor;
        };

        /** @brief Writes finite numeric-only JSON records; no untrusted labels, native payloads or paths are serialized. */
        class GraphJsonWriter {
        public:
            GraphJsonWriter(const std::size_t maxBytes, const std::stop_token cancellation)
                : maxBytes_(maxBytes), cancellation_(cancellation) {
                bytes_.reserve(std::min(maxBytes, std::size_t{4096}));
            }

            void Text(const std::string_view value) {
                if (cancellation_.stop_requested())
                    throw ExportFailure{&RenderGraphInspectionErrors::Cancelled};
                if (value.size() > maxBytes_ - bytes_.size())
                    throw ExportFailure{&RenderGraphInspectionErrors::CapacityExceeded};
                bytes_.append(value);
            }

            void Number(const std::uint64_t value) {
                std::array<char, 20> encoded{};
                const auto result = std::to_chars(encoded.data(), encoded.data() + encoded.size(), value);
                Text({encoded.data(), static_cast<std::size_t>(result.ptr - encoded.data())});
            }

            void Tuple(const std::initializer_list<std::uint64_t> values) {
                Text("[");
                bool first = true;
                for (const auto value : values) {
                    if (!first)
                        Text(",");
                    first = false;
                    Number(value);
                }
                Text("]");
            }

            template <typename T, typename Encode>
            void Records(const std::string_view name, const std::span<const T> records, const Encode &encode) {
                Text(",\"");
                Text(name);
                Text("\":[");
                bool first = true;
                for (const auto &record : records) {
                    if (!first)
                        Text(",");
                    first = false;
                    encode(*this, record);
                }
                Text("]");
            }

            std::string Finish() {
                Text("}\n");
                return std::move(bytes_);
            }

        private:
            std::size_t maxBytes_;
            std::stop_token cancellation_;
            std::string bytes_;
        };

        /** @brief Encodes only Horo enum discriminants, with schema-versioned vocabulary. */
        template <typename T> std::uint64_t Enum(const T value) {
            return static_cast<std::uint64_t>(value);
        }

        /** @brief Emits canonical Horo logical state without native barrier flags. */
        void State(GraphJsonWriter &writer, const RenderGraphLogicalState &state) {
            writer.Tuple({Enum(state.access), Enum(state.operation), Enum(state.scope), Enum(state.layout), state.queue.value});
        }

        /** @brief Emits exact generation-safe resource binding identity; no resource contents are read. */
        void Resource(GraphJsonWriter &writer, const RenderGraphResource &resource) {
            std::uint64_t owner{};
            std::uint64_t slot{};
            std::uint64_t generation{};
            std::visit([&]<typename Binding>(const Binding &binding) {
                if constexpr (!std::is_same_v<Binding, std::monostate>) {
                    owner = binding.owner.value;
                    slot = binding.slot;
                    generation = binding.generation;
                }
            }, resource.binding);
            writer.Tuple(
                {resource.id.value, Enum(resource.kind), Enum(resource.resourceClass), resource.binding.index(), owner, slot, generation});
        }

        /** @brief Encodes the closed authored graph and exact retained/cull schedule. */
        void Topology(GraphJsonWriter &writer, const RenderGraphInspectionSnapshot &snapshot) {
            writer.Records("passes", snapshot.Passes(), [](GraphJsonWriter &out, const RenderGraphPass &pass) {
                out.Tuple({pass.reference.id.value, Enum(pass.kind), Enum(pass.queue), Enum(pass.cullPolicy)});
            });
            writer.Records("dispositions", snapshot.Dispositions(), [](GraphJsonWriter &out, const RenderGraphPassDisposition &pass) {
                out.Tuple({pass.pass.id.value, Enum(pass.disposition), Enum(pass.reason)});
            });
            writer.Records("execution", snapshot.Execution(), [](GraphJsonWriter &out, const RenderGraphInspectionExecutionPass &pass) {
                out.Tuple({pass.pass.id.value, Enum(pass.kind), pass.queue.value});
            });
            writer.Records("resources", snapshot.Resources(), Resource);
            writer.Records("exports", snapshot.Exports(), [](GraphJsonWriter &out, const RenderGraphResourceExport &resource) {
                out.Tuple({resource.resource.value});
            });
            writer.Records("uses", snapshot.Usages(), [](GraphJsonWriter &out, const RenderGraphResourceUsage &use) {
                out.Tuple({use.pass.id.value, use.resource.value, Enum(use.access), Enum(use.kind)});
            });
            writer.Records("dependencies", snapshot.Dependencies(), [](GraphJsonWriter &out, const RenderGraphDependency &edge) {
                out.Tuple({edge.before.id.value, edge.after.id.value, Enum(edge.kind)});
            });
        }

        /** @brief Preserves logical lifetime, alias and whole-resource barrier proof rather than inferring it from adjacency. */
        void Planning(GraphJsonWriter &writer, const RenderGraphInspectionSnapshot &snapshot) {
            writer.Records("allocations", snapshot.Allocations(),
                           [](GraphJsonWriter &out, const RenderGraphTransientAllocationRequirement &allocation) {
                out.Tuple({allocation.resource.value, allocation.compatibilityClass.value, allocation.slot.value});
            });
            writer.Records("lifetimes", snapshot.Lifetimes(), [](GraphJsonWriter &out, const RenderGraphResourceLifetime &lifetime) {
                out.Tuple({lifetime.resource.value, lifetime.firstPass.id.value, lifetime.lastPass.id.value, lifetime.firstUseIndex,
                           lifetime.lastUseIndex, Enum(lifetime.disposition)});
            });
            writer.Records("aliases", snapshot.Aliases(), [](GraphJsonWriter &out, const RenderGraphAliasOpportunity &alias) {
                out.Tuple({alias.first.value, alias.second.value, alias.compatibilityClass.value});
            });
            writer.Records("transitions", snapshot.Transitions(), [](GraphJsonWriter &out, const RenderGraphTransition &transition) {
                out.Text("[");
                out.Number(transition.resource.value);
                out.Text(",");
                out.Number(transition.before.id.value);
                out.Text(",");
                out.Number(transition.after.id.value);
                out.Text(",");
                out.Number(Enum(transition.hazards));
                out.Text(",");
                State(out, transition.oldState);
                out.Text(",");
                State(out, transition.newState);
                out.Text("]");
            });
            writer.Records("transfers", snapshot.Transfers(), [](GraphJsonWriter &out, const RenderGraphOwnershipTransfer &transfer) {
                out.Tuple({transfer.resource.value, transfer.releaseAfter.id.value, transfer.acquireBefore.id.value,
                           transfer.sourceQueue.value, transfer.destinationQueue.value});
            });
        }
    }  // namespace

    /** @copydoc ExportRenderGraphInspection */
    Result<std::string> ExportRenderGraphInspection(const RenderGraphInspectionSnapshot &snapshot, const std::size_t maxBytes,
                                                    const std::stop_token cancellation) {
        if (maxBytes == 0 || maxBytes > RenderGraphInspectionLimits::HardMaxBytes)
            return Result<std::string>::Failure(MakeError(RenderGraphInspectionErrors::InvalidLimits));
        try {
            GraphJsonWriter writer{maxBytes, cancellation};
            writer.Text(R"({"schema":"horo.render.graph.inspection","version":1,"context":)");
            writer.Tuple(
                {snapshot.Context().renderer.value, snapshot.Context().frame.value, snapshot.Context().revision, snapshot.Owner().value});
            writer.Text(R"(,"coverage":"complete_logical_whole_resource","timing":"unavailable")");
            Topology(writer, snapshot);
            Planning(writer, snapshot);
            return Result<std::string>::Success(writer.Finish());
        } catch (const ExportFailure &failure) {
            return Result<std::string>::Failure(MakeError(*failure.descriptor));
        } catch (const std::bad_alloc &) {
            return Result<std::string>::Failure(MakeError(RenderGraphInspectionErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render
