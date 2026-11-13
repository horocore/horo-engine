#include "DecisionCompileLimits.h"
#include "DecisionSourceWire.h"
#include "Horo/AI/DecisionPlanCodec.h"

#include <algorithm>
#include <new>

namespace Horo::AI {
    namespace {
        constexpr std::uint64_t SourceMagic = 0x3143525349414f48ULL;
        constexpr std::size_t MaximumBytes = 16U * 1024U * 1024U;

        template <typename T> Result<T> Invalid() {
            return Result<T>::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));
        }

        bool ValidLimits(const DecisionCodecLimits &limits) noexcept {
            return limits.maximumBytes > 0 && limits.maximumBytes <= MaximumBytes && Detail::ValidDecisionLimits(limits.compilation);
        }

        void WriteHeader(Detail::DecisionWriter &writer, const DecisionGraphSource &source) {
            writer.IdentityValue(source.asset);
            writer.Unsigned(source.schemaVersion, 4);
            writer.IdentityValue(source.blackboard);
            writer.Unsigned(source.blackboardVersion.minimum, 4);
            writer.Unsigned(source.blackboardVersion.maximum, 4);
            Detail::WriteLocation(writer, source.location);
            writer.IdentityValue(source.executionTask, true);
            writer.Unsigned(source.body.index(), 1);
        }

        void WriteGraph(Detail::DecisionWriter &writer, const DecisionGraphSource &source) {
            WriteHeader(writer, source);
            std::visit([&](const auto &body) {
                using T = std::decay_t<decltype(body)>;
                if constexpr (std::is_same_v<T, std::shared_ptr<const BehaviorTreeAsset>>) {
                    if (!body) {
                        writer.Reject();
                        return;
                    }
                    Detail::WriteTree(writer, *body);
                } else
                    Detail::WriteMachine(writer, body);
            }, source.body);
        }

        Result<DecisionGraphSource> ReadGraph(Detail::DecisionReader &reader) {
            DecisionGraphSource source;
            source.asset = reader.IdentityValue<DecisionGraphAssetId>();
            source.schemaVersion = static_cast<std::uint32_t>(reader.Unsigned(4));
            source.blackboard = reader.IdentityValue<BlackboardSchemaId>();
            source.blackboardVersion = {static_cast<std::uint32_t>(reader.Unsigned(4)), static_cast<std::uint32_t>(reader.Unsigned(4))};
            source.location = Detail::ReadLocation(reader);
            source.executionTask = reader.IdentityValue<TaskId>(true);
            if (const auto family = reader.Unsigned(1); family == 0) {
                auto tree = Detail::ReadTree(reader);
                if (tree.HasError())
                    return Result<DecisionGraphSource>::Failure(tree.ErrorValue());
                source.body = std::move(tree).Value();
            } else if (family == 1)
                source.body = Detail::ReadMachine(reader);
            else
                return Invalid<DecisionGraphSource>();
            return Result<DecisionGraphSource>::Success(std::move(source));
        }

        Result<std::size_t> SourceNodeCount(const DecisionGraphSource &source);

        Result<std::vector<DecisionGraphSource>> ReadSources(Detail::DecisionReader &reader, std::size_t count,
                                                             const DecisionCompilationLimits &limits) {
            std::vector<DecisionGraphSource> sources;
            std::size_t nodes{};
            sources.reserve(count);
            for (std::size_t index = 0; index < count && reader.Valid(); ++index) {
                auto source = ReadGraph(reader);
                if (source.HasError())
                    return Result<std::vector<DecisionGraphSource>>::Failure(source.ErrorValue());
                auto local = SourceNodeCount(source.Value());
                if (local.HasError())
                    return Result<std::vector<DecisionGraphSource>>::Failure(local.ErrorValue());
                if (local.Value() > DecisionAssetValidationHardLimits::NodesPerAsset || local.Value() > limits.maximumNodes - nodes)
                    return Result<std::vector<DecisionGraphSource>>::Failure(MakeError(AIErrors::DecisionAssetLimitExceeded));
                nodes += local.Value();
                sources.push_back(std::move(source).Value());
            }
            return Result<std::vector<DecisionGraphSource>>::Success(std::move(sources));
        }

        Result<std::size_t> SourceNodeCount(const DecisionGraphSource &source) {
            if (const auto *tree = std::get_if<std::shared_ptr<const BehaviorTreeAsset>>(&source.body)) {
                if (!*tree)
                    return Invalid<std::size_t>();
                return Result<std::size_t>::Success((*tree)->Nodes().size());
            }
            const auto &machine = std::get<StateMachineAssetDescriptor>(source.body);
            if (machine.states.size() > StateMachineLimits::States || machine.transitions.size() > StateMachineLimits::Transitions)
                return Result<std::size_t>::Failure(MakeError(AIErrors::DecisionAssetLimitExceeded));
            auto count = machine.states.size() + machine.transitions.size();
            for (const auto &state : machine.states)
                count += state.entryProvider.has_value() + state.updateProvider.has_value() + state.exitProvider.has_value();
            return Result<std::size_t>::Success(count);
        }

        Result<void> AdmitSourceBounds(std::span<const DecisionGraphSource> sources, const DecisionCompilationLimits &limits) {
            if (sources.empty() || sources.size() > limits.maximumAssets)
                return Result<void>::Failure(MakeError(AIErrors::DecisionAssetLimitExceeded));
            std::size_t total{};
            for (const auto &source : sources) {
                auto count = SourceNodeCount(source);
                if (count.HasError())
                    return Result<void>::Failure(count.ErrorValue());
                if (count.Value() > DecisionAssetValidationHardLimits::NodesPerAsset || count.Value() > limits.maximumNodes - total)
                    return Result<void>::Failure(MakeError(AIErrors::DecisionAssetLimitExceeded));
                total += count.Value();
            }
            return Result<void>::Success();
        }

        Result<std::vector<const DecisionGraphSource *>> CanonicalSources(std::span<const DecisionGraphSource> sources,
                                                                          DecisionGraphAssetId root, std::size_t maximum) {
            using Output = std::vector<const DecisionGraphSource *>;
            if (sources.empty() || sources.size() > maximum || !root.IsValid())
                return Invalid<Output>();
            Output ordered;
            bool rootFound{};
            for (const auto &source : sources) {
                if (!source.asset.IsValid())
                    return Invalid<Output>();
                rootFound = rootFound || source.asset == root;
                ordered.push_back(&source);
            }
            std::ranges::sort(ordered, {}, [](const auto *source) {
                return source->asset;
            });
            if (!rootFound || std::ranges::adjacent_find(ordered, {}, [](const auto *source) {
                return source->asset;
            }) != ordered.end())
                return Invalid<Output>();
            return Result<Output>::Success(std::move(ordered));
        }
    }  // namespace

    Result<std::vector<std::uint8_t>> DecisionPlanCodec::EncodeSource(std::span<const DecisionGraphSource> sources,
                                                                      DecisionGraphAssetId root, DecisionCodecLimits limits) {
        using Output = Result<std::vector<std::uint8_t>>;
        if (!ValidLimits(limits))
            return Invalid<std::vector<std::uint8_t>>();
        try {
            if (auto bounds = AdmitSourceBounds(sources, limits.compilation); bounds.HasError())
                return Output::Failure(bounds.ErrorValue());
            auto ordered = CanonicalSources(sources, root, limits.compilation.maximumAssets);
            if (ordered.HasError())
                return Output::Failure(ordered.ErrorValue());
            Detail::DecisionWriter writer(limits.maximumBytes);
            writer.Unsigned(SourceMagic);
            writer.Unsigned(SourceVersion, 4);
            writer.IdentityValue(root);
            writer.Count(ordered.Value().size(), limits.compilation.maximumAssets);
            for (const auto *source : ordered.Value()) {
                if (!writer.Valid())
                    break;
                WriteGraph(writer, *source);
            }
            if (!writer.Valid())
                return Invalid<std::vector<std::uint8_t>>();
            return Output::Success(std::move(writer).Take());
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(AIErrors::DecisionAssetStorageUnavailable));
        }
    }

    Result<std::vector<DecisionGraphSource>> DecisionPlanCodec::DecodeSource(std::span<const std::uint8_t> bytes,
                                                                             DecisionGraphAssetId &root, DecisionCodecLimits limits) {
        using Output = Result<std::vector<DecisionGraphSource>>;
        if (!ValidLimits(limits) || bytes.size() > limits.maximumBytes)
            return Invalid<std::vector<DecisionGraphSource>>();
        try {
            Detail::DecisionReader reader(bytes);
            if (reader.Unsigned() != SourceMagic || reader.Unsigned(4) != SourceVersion)
                return Invalid<std::vector<DecisionGraphSource>>();
            const auto candidateRoot = reader.IdentityValue<DecisionGraphAssetId>();
            const auto count = reader.Count(limits.compilation.maximumAssets);
            auto captured = ReadSources(reader, count, limits.compilation);
            if (captured.HasError())
                return captured;
            auto sources = std::move(captured).Value();
            if (!reader.Complete())
                return Invalid<std::vector<DecisionGraphSource>>();
            auto canonical = EncodeSource(sources, candidateRoot, limits);
            if (canonical.HasError())
                return Output::Failure(canonical.ErrorValue());
            if (!std::ranges::equal(bytes, canonical.Value()))
                return Invalid<std::vector<DecisionGraphSource>>();
            root = candidateRoot;
            return Output::Success(std::move(sources));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(AIErrors::DecisionAssetStorageUnavailable));
        }
    }
}  // namespace Horo::AI
