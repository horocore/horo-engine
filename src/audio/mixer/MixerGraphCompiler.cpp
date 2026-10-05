#include "MixerPlanState.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <queue>
#include <unordered_map>

namespace Horo::Audio {
    namespace {
        /** @brief Preserve a stable graph error with off-callback boundary context. */
        Result<void> Invalid(const char *reason) {
            return Result<void>::Failure(MakeError(AudioErrors::GraphBuildFailed, reason));
        }

        /** @brief Reject malformed pinned build identities before constructing any storage. */
        bool ValidIdentity(const MixerPlanIdentity &id) noexcept {
            return id.owner.IsValid() && id.epoch != 0 && id.generation != 0 && id.assetRevision != 0 && id.catalogRevision != 0 &&
                   id.profileRevision != 0;
        }

        /** @brief Admit the processing format and bounded voice/frame dimensions independently of graph shape. */
        bool ValidDimensions(const MixerCompileProfile &p) noexcept {
            return p.sampleRate >= MinimumAudioSampleRate && p.sampleRate <= MaximumAudioSampleRate &&
                   ValidateAudioChannelLayout(ViewAudioChannelLayout(p.outputLayout)) && p.maximumFrames != 0 &&
                   p.maximumFrames <= MaximumAudioCallbackFrames && p.maximumVoices != 0 && p.maximumVoices <= MaximumAudioHandleSlots;
        }

        /** @brief Admit caller-lowered routing and memory ceilings before checked reservation arithmetic. */
        bool ValidBudgets(const MixerCompileProfile &p) noexcept {
            return p.maximumFanIn != 0 && p.maximumFanIn <= MaximumMixerAssetRoutes && p.maximumFanOut != 0 &&
                   p.maximumFanOut <= MaximumMixerAssetRoutes && p.maximumStorageBytes != 0 &&
                   p.maximumStorageBytes <= MaximumAudioMemoryBytes && p.maximumScratchBytes <= MaximumAudioDSPScratchBytes &&
                   p.maximumSampleOperations != 0 && p.maximumSampleOperations <= (std::uint64_t{1} << 32);
        }

        /** @brief Validate every caller-lowered bound through the shared compiler/runtime admission authority. */
        Result<void> CheckedProfile(const MixerCompileProfile &p) {
            if (!ValidDimensions(p) || !ValidBudgets(p))
                return Invalid("Invalid mixer format, dimensions or resource budgets.");
            return ValidateMixerAssetSchemaLimits(p.schema);
        }

        /** @brief Convert persisted decibels off-callback, rejecting unrepresentable linear headroom. */
        Result<float> Gain(const float db) {
            const auto linear = std::pow(10.0, static_cast<double>(db) / 20.0);
            if (!std::isfinite(linear) || linear > std::numeric_limits<float>::max())
                return Result<float>::Failure(MakeError(AudioErrors::GraphBuildFailed, "Unrepresentable mixer gain."));
            const auto gain = static_cast<float>(linear);
            return Result<float>::Success(std::fpclassify(gain) == FP_SUBNORMAL ? 0.0F : gain);
        }

        /** @brief Checked aligned byte charging before any allocation or pointer construction. */
        Result<std::size_t> Reserve(MixerRenderPlan::State &s, const std::size_t bytes) {
            const std::size_t padded = MixerDetail::Align(bytes);
            if (padded > s.profile.maximumStorageBytes || s.storageBytes > s.profile.maximumStorageBytes - padded)
                return Result<std::size_t>::Failure(MakeError(AudioErrors::GraphBuildFailed, "Mixer storage budget exceeded."));
            const std::size_t offset = s.storageBytes;
            s.storageBytes += padded;
            return Result<std::size_t>::Success(offset);
        }

        /** @brief Charge a conservative operation bound without overflow. */
        Result<void> ChargeWork(std::uint64_t &operations, const std::uint64_t amount, const MixerCompileProfile &p) {
            if (amount > p.maximumSampleOperations || operations > p.maximumSampleOperations - amount)
                return Invalid("Mixer callback-work budget exceeded.");
            operations += amount;
            return Result<void>::Success();
        }

        /** @brief Indexed source topology; unordered lookup never determines processing order. */
        struct Topology final {
            std::vector<const MixerBusDescriptor *> canonical;
            std::unordered_map<std::uint64_t, std::uint32_t> indices;
            std::vector<std::vector<std::uint32_t>> adjacency;
            std::vector<std::uint32_t> indegree;
            std::vector<std::uint32_t> fanOut;
        };

        /** @brief Resolve every route once and validate semantic layouts and declared fan budgets. */
        Result<void> RecordEdges(const MixerAssetSchema &asset, const MixerCompileProfile &profile, Topology &t) {
            for (const MixerRouteDescriptor &route : asset.routes) {
                const std::uint32_t src = t.indices.at(route.source.Value());
                const std::uint32_t dst = t.indices.at(route.destination.Value());
                if (t.canonical[src]->layout != t.canonical[dst]->layout)
                    return Invalid("Mixer route requires an unavailable explicit layout conversion.");
                if (!route.enabled)
                    continue;
                t.adjacency[src].push_back(dst);
                ++t.indegree[dst];
                ++t.fanOut[src];
                if (t.indegree[dst] > profile.maximumFanIn || t.fanOut[src] > profile.maximumFanOut)
                    return Invalid("Mixer route fan budget exceeded.");
            }
            return Result<void>::Success();
        }

        /** @brief Kahn order with canonical stable-ID tie breaking, independent of source array order. */
        Result<std::vector<const MixerBusDescriptor *>> Order(const MixerAssetSchema &asset, const MixerCompileProfile &profile) {
            Topology t;
            for (const MixerBusDescriptor &bus : asset.buses)
                t.canonical.push_back(&bus);
            std::ranges::sort(t.canonical, {}, [](const auto *bus) {
                return bus->id.Value();
            });
            t.adjacency.resize(t.canonical.size());
            t.indegree.resize(t.canonical.size());
            t.fanOut.resize(t.canonical.size());
            for (std::uint32_t i = 0; i < t.canonical.size(); ++i)
                t.indices.emplace(t.canonical[i]->id.Value(), i);
            if (const Result<void> edges = RecordEdges(asset, profile, t); edges.HasError())
                return Result<std::vector<const MixerBusDescriptor *>>::Failure(edges.ErrorValue());
            std::priority_queue<std::uint32_t, std::vector<std::uint32_t>, std::greater<>> ready;
            for (std::uint32_t i = 0; i < t.indegree.size(); ++i)
                if (t.indegree[i] == 0)
                    ready.push(i);
            std::vector<const MixerBusDescriptor *> order;
            while (!ready.empty()) {
                const std::uint32_t src = ready.top();
                ready.pop();
                order.push_back(t.canonical[src]);
                for (const std::uint32_t dst : t.adjacency[src])
                    if (--t.indegree[dst] == 0)
                        ready.push(dst);
            }
            if (order.size() != asset.buses.size())
                return Result<std::vector<const MixerBusDescriptor *>>::Failure(MakeError(AudioErrors::GraphBuildFailed, "Cyclic mixer."));
            return Result<std::vector<const MixerBusDescriptor *>>::Success(std::move(order));
        }

        /** @brief Reject additional ports and optional main inputs before accessing singular bindings. */
        bool MainPorts(const AudioDSPNodeDescriptor &d) noexcept {
            return d.inputs.size() == 1 && d.outputs.size() == 1 && d.inputs.front().kind == AudioDSPPortKind::Main &&
                   d.outputs.front().kind == AudioDSPPortKind::Main && !d.inputs.front().optional;
        }

        /** @brief Match all node/port frame limits to the admitted semantic bus format. */
        bool CompatiblePorts(const AudioDSPNodeDescriptor &d, const AudioProcessingFormat &format, const std::uint32_t frames) noexcept {
            return d.inputs.front().format == format && d.outputs.front().format == format && d.maximumFrames >= frames &&
                   d.inputs.front().maximumFrames >= frames && d.outputs.front().maximumFrames >= frames;
        }

        /** @brief Admit only fully declared same-format single-main-port strategies and explicit finite tails. */
        Result<void> ValidateInsert(const AudioDSPNodeDescriptor &d, const AudioProcessingFormat &format, const std::uint32_t frames,
                                    const bool bypassed) {
            if (const Result<void> valid = ValidateAudioDSPNodeDescriptor(d); valid.HasError())
                return valid;
            if (!MainPorts(d))
                return Invalid("Mixer insert requires unsupported ports or same-block control dependencies.");
            if (!CompatiblePorts(d, format, frames))
                return Invalid("Mixer insert format or frame bound differs from its bus.");
            if (d.latencyFrames != 0 || !d.supportsReset || (bypassed && !d.supportsBypass))
                return Invalid("Mixer insert requires unavailable latency compensation, reset or bypass.");
            return Result<void>::Success();
        }

        /** @brief Construct one owned node and plan its fixed state before any callback publication. */
        Result<void> BuildInsert(MixerRenderPlan::State &s, MixerDetail::BusState &bus, const MixerEffectDescriptor &effect,
                                 IMixerDSPFactory &factory, std::uint64_t &work) {
            auto created = factory.Create(effect, bus.format);
            if (created.HasError())
                return Result<void>::Failure(WrapError(AudioErrors::GraphBuildFailed, created.ErrorValue()));
            MixerDSPStrategy strategy = std::move(created).Value();
            if (!strategy.node || strategy.maximumOperationsPerFrame == 0 ||
                strategy.maximumOperationsPerFrame > s.profile.maximumSampleOperations / s.profile.maximumFrames)
                return Invalid("Mixer factory returned no strategy or no bounded work declaration.");
            MixerDetail::Insert insert;
            insert.descriptor = strategy.node->Descriptor();
            insert.node = std::move(strategy.node);
            insert.effect = effect;
            if (const Result<void> valid = ValidateInsert(insert.descriptor, bus.format, s.profile.maximumFrames, effect.bypassed);
                valid.HasError())
                return valid;
            const std::uint64_t insertWork = strategy.maximumOperationsPerFrame + bus.format.layout.orderedChannels.size() * 2;
            if (const Result<void> charged = ChargeWork(work, insertWork * s.profile.maximumFrames, s.profile); charged.HasError())
                return charged;
            auto offset = Reserve(s, insert.descriptor.memory.stateBytes);
            if (offset.HasError())
                return Result<void>::Failure(offset.ErrorValue());
            insert.stateOffset = offset.Value();
            s.scratchBytes = std::max(s.scratchBytes, insert.descriptor.memory.scratchBytes);
            for (const AudioDSPParameterDescriptor &p : insert.descriptor.parameters)
                insert.parameters.emplace_back(p.identity, p.defaultValue, p.defaultValue, 0);
            bus.inserts.push_back(std::move(insert));
            return Result<void>::Success();
        }

        /** @brief Reserve three disjoint planar taps/work areas and immutable bus metadata. */
        Result<void> BuildBus(MixerRenderPlan::State &s, const MixerBusDescriptor &bus, IMixerDSPFactory *factory, std::uint64_t &work) {
            if (bus.role == MixerBusRole::MasterOutput && bus.layout != s.profile.outputLayout)
                return Invalid("Mixer Master layout differs from the admitted output.");
            auto gain = Gain(bus.defaults.gainDb);
            if (gain.HasError())
                return Result<void>::Failure(gain.ErrorValue());
            const std::size_t planeBytes = MixerDetail::Align(static_cast<std::size_t>(s.profile.maximumFrames) * sizeof(AudioSample));
            const std::size_t tapBytes = planeBytes * bus.layout.orderedChannels.size();
            auto taps = Reserve(s, tapBytes * 3);
            if (taps.HasError())
                return Result<void>::Failure(taps.ErrorValue());
            s.buses.push_back({.id = bus.id,
                               .role = bus.role,
                               .layout = bus.layout,
                               .gain = bus.defaults.muted ? 0.0F : gain.Value(),
                               .paused = bus.defaults.paused,
                               .preFaderOffset = taps.Value(),
                               .postFaderOffset = taps.Value() + tapBytes});
            MixerDetail::BusState processing{.format = {s.profile.sampleRate, bus.layout},
                                             .gain = gain.Value(),
                                             .muted = bus.defaults.muted};
            if (!bus.effects.empty() && factory == nullptr)
                return Invalid("Mixer effect requires an explicit prepared DSP factory.");
            for (const MixerEffectDescriptor &effect : bus.effects) {
                if (const Result<void> valid = BuildInsert(s, processing, effect, *factory, work); valid.HasError())
                    return valid;
            }
            s.processing.push_back(std::move(processing));
            return ChargeWork(work, static_cast<std::uint64_t>(bus.layout.orderedChannels.size()) * s.profile.maximumFrames * 5, s.profile);
        }

        /** @brief Build destination-grouped incoming tables in canonical route-ID order. */
        Result<void> BuildRoutes(MixerRenderPlan::State &s, const MixerAssetSchema &asset, std::uint64_t &work) {
            std::unordered_map<std::uint64_t, std::uint32_t> indices;
            for (std::uint32_t i = 0; i < s.buses.size(); ++i)
                indices.emplace(s.buses[i].id.Value(), i);
            s.routeState.reserve(asset.routes.size());
            for (MixerCompiledBus &bus : s.buses) {
                std::vector<const MixerRouteDescriptor *> incoming;
                for (const MixerRouteDescriptor &route : asset.routes)
                    if (route.enabled && route.destination == bus.id)
                        incoming.push_back(&route);
                std::ranges::sort(incoming, {}, [](const auto *route) {
                    return route->id.Value();
                });
                bus.firstIncoming = static_cast<std::uint32_t>(s.routes.size());
                bus.incomingCount = static_cast<std::uint32_t>(incoming.size());
                for (const auto *route : incoming) {
                    auto gain = Gain(route->gainDb);
                    if (gain.HasError())
                        return Result<void>::Failure(gain.ErrorValue());
                    s.routes.emplace_back(route->id, indices.at(route->source.Value()), route->tap, gain.Value());
                    s.routeState.push_back({gain.Value(), route->kind});
                }
                if (const Result<void> charged = ChargeWork(work,
                                                            static_cast<std::uint64_t>(incoming.size()) *
                                                                bus.layout.orderedChannels.size() * s.profile.maximumFrames * 2,
                                                            s.profile);
                    charged.HasError())
                    return charged;
            }
            return Result<void>::Success();
        }

        /** @brief Bind pointer arrays to fixed aligned storage and prepare nodes without publishing partial state. */
        Result<void> PrepareStorage(MixerRenderPlan::State &s) {
            if (s.scratchBytes > s.profile.maximumScratchBytes)
                return Invalid("Mixer DSP scratch budget exceeded.");
            auto scratch = Reserve(s, s.scratchBytes);
            if (scratch.HasError())
                return Result<void>::Failure(scratch.ErrorValue());
            s.scratchOffset = scratch.Value();
            s.storage.reset(static_cast<std::byte *>(::operator new[](s.storageBytes, std::align_val_t{AudioDSPMemoryAlignment})));
            std::ranges::fill(std::span{s.storage.get(), s.storageBytes}, std::byte{});
            const std::size_t stride = MixerDetail::Align(static_cast<std::size_t>(s.profile.maximumFrames) * sizeof(AudioSample));
            for (std::size_t i = 0; i < s.buses.size(); ++i) {
                const MixerCompiledBus &bus = s.buses[i];
                MixerDetail::BusState &processing = s.processing[i];
                const std::size_t tapBytes = stride * bus.layout.orderedChannels.size();
                for (std::size_t c = 0; c < bus.layout.orderedChannels.size(); ++c) {
                    processing.pre[c] = static_cast<AudioSample *>(static_cast<void *>(s.storage.get() + bus.preFaderOffset + stride * c));
                    processing.post[c] =
                        static_cast<AudioSample *>(static_cast<void *>(s.storage.get() + bus.postFaderOffset + stride * c));
                    processing.work[c] =
                        static_cast<AudioSample *>(static_cast<void *>(s.storage.get() + bus.postFaderOffset + tapBytes + stride * c));
                }
                for (MixerDetail::Insert &insert : processing.inserts) {
                    const AudioDSPPrepareContext context{s.profile.maximumFrames,
                                                         {s.storage.get() + insert.stateOffset, insert.descriptor.memory.stateBytes},
                                                         {s.storage.get() + s.scratchOffset, insert.descriptor.memory.scratchBytes}};
                    if (const Result<void> prepared = insert.node->Prepare(context); prepared.HasError())
                        return Result<void>::Failure(WrapError(AudioErrors::GraphBuildFailed, prepared.ErrorValue()));
                }
            }
            return Result<void>::Success();
        }

        /** @brief Complete every fallible build step while the candidate remains detached. */
        Result<void> Build(MixerRenderPlan::State &s, const MixerAssetSchema &asset, IMixerDSPFactory *factory) {
            if (!ValidIdentity(s.identity))
                return Invalid("Mixer identity/revisions must be nonzero.");
            if (const Result<void> p = CheckedProfile(s.profile); p.HasError())
                return p;
            if (const Result<void> valid = ValidateMixerAssetSchema(asset, s.profile.schema); valid.HasError())
                return Result<void>::Failure(
                    WrapError(AudioErrors::GraphBuildFailed, valid.ErrorValue(), MixerDetail::CycleEvidence(asset)));
            auto order = Order(asset, s.profile);
            if (order.HasError())
                return Result<void>::Failure(order.ErrorValue());
            std::uint64_t work =
                static_cast<std::uint64_t>(s.profile.maximumVoices) *
                (s.profile.outputLayout.orderedChannels.size() * s.profile.maximumFrames * 2 +
                 s.profile.outputLayout.orderedChannels.size() * s.profile.outputLayout.orderedChannels.size() + asset.buses.size());
            if (const Result<void> bound = ChargeWork(work, 0, s.profile); bound.HasError())
                return bound;
            for (const auto *bus : order.Value()) {
                if (const Result<void> built = BuildBus(s, *bus, factory, work); built.HasError())
                    return built;
            }
            if (const Result<void> routes = BuildRoutes(s, asset, work); routes.HasError())
                return routes;
            s.sampleOperations = work;
            s.metadataBytes = s.routeState.capacity() * sizeof(MixerDetail::RouteState);
            if (const auto prepared = PrepareStorage(s); prepared.HasError())
                return prepared;
            if (s.metadataBytes > s.profile.maximumStorageBytes - s.storageBytes)
                return Invalid("Mixer mutable routing storage budget exceeded.");
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc MixerDetail::ValidateProfile */
    Result<void> MixerDetail::ValidateProfile(const MixerCompileProfile &profile) {
        return CheckedProfile(profile);
    }

    /** @copydoc CompileMixerGraph */
    Result<std::unique_ptr<MixerRenderPlan>> CompileMixerGraph(const MixerAssetSchema &asset, const MixerPlanIdentity &identity,
                                                               const MixerCompileProfile &profile, IMixerDSPFactory *factory) {
        try {
            auto state = std::make_unique<MixerRenderPlan::State>();
            state->identity = identity;
            state->profile = profile;
            if (const Result<void> built = Build(*state, asset, factory); built.HasError())
                return Result<std::unique_ptr<MixerRenderPlan>>::Failure(
                    WrapError(AudioErrors::GraphBuildFailed, built.ErrorValue(),
                              std::format("Mixer asset revision {}, generation {}", identity.assetRevision, identity.generation)));
            return Result<std::unique_ptr<MixerRenderPlan>>::Success(
                std::make_unique<MixerRenderPlan>(MixerRenderPlan::ConstructionKey{}, std::move(state)));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<MixerRenderPlan>>::Failure(MakeError(AudioErrors::GraphBuildFailed, "Mixer allocation failed."));
        }
    }

    /** @copydoc MixerRenderPlan::MixerRenderPlan */
    MixerRenderPlan::MixerRenderPlan(ConstructionKey, std::unique_ptr<State> state) : state_(std::move(state)) {}

    /** @copydoc MixerRenderPlan::~MixerRenderPlan */
    MixerRenderPlan::~MixerRenderPlan() = default;

    /** @copydoc MixerRenderPlan::Identity */
    const MixerPlanIdentity &MixerRenderPlan::Identity() const noexcept {
        return state_->identity;
    }

    /** @copydoc MixerRenderPlan::Buses */
    std::span<const MixerCompiledBus> MixerRenderPlan::Buses() const noexcept {
        return state_->buses;
    }

    /** @copydoc MixerRenderPlan::Routes */
    std::span<const MixerCompiledRoute> MixerRenderPlan::Routes() const noexcept {
        return state_->routes;
    }

    /** @copydoc MixerRenderPlan::ResolveBus */
    std::optional<std::uint32_t> MixerRenderPlan::ResolveBus(const AudioBusId id) const noexcept {
        for (std::uint32_t i = 0; i < state_->buses.size(); ++i)
            if (state_->buses[i].id == id)
                return i;
        return std::nullopt;
    }

    /** @copydoc MixerRenderPlan::StorageBytes */
    std::size_t MixerRenderPlan::StorageBytes() const noexcept {
        return state_->storageBytes + state_->metadataBytes;
    }
}  // namespace Horo::Audio
