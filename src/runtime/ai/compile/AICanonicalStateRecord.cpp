#include "DecisionSourceWire.h"
#include "Horo/AI/AICanonicalState.h"

#include <new>

namespace Horo::AI {
    namespace {
        using Detail::DecisionReader;
        using Detail::DecisionWriter;
        using Encoded = Result<std::vector<std::uint8_t>>;
        using Decoded = Result<AiCanonicalState>;

        constexpr std::size_t RecordHeaderBytes = 8;
        constexpr std::size_t MinimumAgentBytes = 16;

        /** @brief Checks the durable alternatives before accepting the shared Blackboard value representation. */
        bool HasValueShape(const BlackboardValue &value) {
            return std::visit([]<typename Value>(const Value &stored) {
                if constexpr (std::is_same_v<Value, BlackboardScalarValue>) {
                    return true;
                } else if constexpr (std::is_same_v<Value, BlackboardCollectionValue>) {
                    if (stored.count > MaximumBlackboardCollectionElements || stored.elementKind >= BlackboardValueKind::Count)
                        return false;
                    return std::ranges::all_of(std::span{stored.elements}.first(stored.count), [&](const auto &item) {
                        return item.index() == static_cast<std::size_t>(stored.elementKind);
                    });
                } else {
                    return stored.schemaVersion != 0 && stored.serializedType != 0 && stored.size <= MaximumBlackboardOpaqueBytes;
                }
            }, value);
        }

        /** @brief Requires a versioned, strictly ordered Blackboard. The entity wire policy validates references separately. */
        bool IsValid(const BlackboardCanonicalState &state) {
            if (!state.schema.IsValid() || state.schemaVersion == 0 || state.entries.size() > MaximumBlackboardKeys)
                return false;
            BlackboardKeyId previous;
            for (const auto &entry : state.entries) {
                if (entry.key <= previous || (entry.value && !HasValueShape(*entry.value)))
                    return false;
                previous = entry.key;
            }
            return true;
        }

        /** @brief Checks the base-agent shape before transport or destination-specific restore validation. */
        bool IsValid(const AiAgentCanonicalState &agent) {
            if (const auto valid = ValidateAiAgentComponent(agent.authored);
                valid.HasError() || agent.state >= AiCanonicalAgentState::Count)
                return false;
            const bool active = agent.state == AiCanonicalAgentState::Active;
            if (!agent.controller)
                return !active;
            const auto &controller = *agent.controller;
            if (const auto valid = ValidateAiControllerComponent(controller.authored); valid.HasError())
                return false;
            if (active != controller.blackboard.has_value())
                return false;
            return !active || (agent.authored.enabled && controller.authored.enabled &&
                               controller.blackboard->schema == controller.authored.blackboardSchema && IsValid(*controller.blackboard));
        }

        /** @brief Writes the complete ordered Blackboard using the shared value codec. */
        void WriteBlackboard(DecisionWriter &writer, const BlackboardCanonicalState &value, const Detail::WriteBlackboardEntity &entity) {
            writer.IdentityValue(value.schema);
            writer.Unsigned(value.schemaVersion, 4);
            if (!writer.Count(value.entries.size(), MaximumBlackboardKeys))
                return;
            for (const auto &entry : value.entries) {
                writer.IdentityValue(entry.key);
                writer.Unsigned(entry.value.has_value(), 1);
                if (entry.value)
                    Detail::WriteBlackboardValue(writer, *entry.value, entity);
            }
        }

        /** @brief Reads key identities and optional values in saved order. */
        BlackboardCanonicalState ReadBlackboard(DecisionReader &reader, const Detail::ReadBlackboardEntity &entity) {
            BlackboardCanonicalState value{.schema = reader.IdentityValue<BlackboardSchemaId>(),
                                           .schemaVersion = static_cast<std::uint32_t>(reader.Unsigned(4))};
            const auto count = reader.Count(MaximumBlackboardKeys);
            for (std::size_t index = 0; index < count && reader.Valid(); ++index) {
                BlackboardCanonicalEntry entry{.key = reader.IdentityValue<BlackboardKeyId>()};
                if (reader.Boolean())
                    entry.value = Detail::ReadBlackboardValue(reader, entity);
                value.entries.push_back(std::move(entry));
            }
            return value;
        }

        /** @brief Writes the base agent and its optional durable controller. */
        void WriteAgent(DecisionWriter &writer, const AiAgentCanonicalState &agent, const Detail::WriteBlackboardEntity &entity) {
            const auto &authored = agent.authored;
            writer.IdentityValue(authored.agent);
            writer.Unsigned(authored.schemaVersion, 4);
            writer.Enumeration(authored.startupPolicy);
            writer.Unsigned(authored.enabled, 1);
            writer.Enumeration(agent.state);
            writer.Unsigned(agent.controller.has_value(), 1);
            if (!agent.controller)
                return;
            const auto &controller = agent.controller->authored;
            writer.IdentityValue(controller.controller);
            writer.IdentityValue(controller.decisionAsset);
            writer.IdentityValue(controller.blackboardSchema);
            writer.Enumeration(controller.decisionKind);
            writer.Unsigned(controller.requiredCapabilities.bits, 4);
            writer.Unsigned(controller.schemaVersion, 4);
            writer.Enumeration(controller.startupPolicy);
            writer.Unsigned(controller.enabled, 1);
            if (agent.controller->blackboard)
                WriteBlackboard(writer, *agent.controller->blackboard, entity);
        }

        /** @brief Reads one base-agent record; active controller records carry an exact Blackboard payload. */
        AiAgentCanonicalState ReadAgent(DecisionReader &reader, const Detail::ReadBlackboardEntity &entity) {
            AiAgentCanonicalState value{.authored = {.agent = reader.IdentityValue<AgentId>(),
                                                     .schemaVersion = static_cast<std::uint32_t>(reader.Unsigned(4)),
                                                     .startupPolicy = reader.Enumeration(AiStartupPolicy::Count),
                                                     .enabled = reader.Boolean()},
                                        .state = reader.Enumeration(AiCanonicalAgentState::Count)};
            if (!reader.Boolean())
                return value;
            value.controller.emplace(
                AiBaseControllerCanonicalState{.authored = {.controller = reader.IdentityValue<ControllerTypeId>(),
                                                            .decisionAsset = reader.IdentityValue<DecisionGraphAssetId>(),
                                                            .blackboardSchema = reader.IdentityValue<BlackboardSchemaId>(),
                                                            .decisionKind = reader.Enumeration(DecisionPlanKind::Count),
                                                            .requiredCapabilities = {static_cast<std::uint32_t>(reader.Unsigned(4))},
                                                            .schemaVersion = static_cast<std::uint32_t>(reader.Unsigned(4)),
                                                            .startupPolicy = reader.Enumeration(AiStartupPolicy::Count),
                                                            .enabled = reader.Boolean()}});
            if (value.state == AiCanonicalAgentState::Active)
                value.controller->blackboard = ReadBlackboard(reader, entity);
            return value;
        }

        /** @brief Materializes a complete, strictly ordered population from the admitted record. */
        Decoded ReadPopulation(DecisionReader &reader, std::size_t count, const Detail::ReadBlackboardEntity &entity, bool materialize) {
            AiCanonicalState state;
            if (materialize)
                state.agents.reserve(count);
            AgentId previous;
            for (std::size_t index = 0; index < count && reader.Valid(); ++index) {
                auto agent = ReadAgent(reader, entity);
                if (!reader.Valid() || agent.authored.agent <= previous || !IsValid(agent))
                    return Decoded::Failure(MakeError(AIErrors::CanonicalStateInvalid));
                previous = agent.authored.agent;
                if (materialize)
                    state.agents.push_back(std::move(agent));
            }
            if (!reader.Complete())
                return Decoded::Failure(MakeError(AIErrors::CanonicalStateInvalid));
            return Decoded::Success(std::move(state));
        }

        /** @brief Checks the durable identity encoding independently from Scene hydration. */
        bool HasIdentity(const AiSavedEntityReference &reference) {
            return reference.generation != 0 && std::ranges::any_of(reference.identity, [](const auto byte) {
                return byte != 0;
            });
        }

        /** @brief Encodes only an entity's admitted persistent identity and durable generation. */
        void WriteEntity(DecisionWriter &writer, const BlackboardStoredEntityReference &value, const AiSaveEntityProjector &project,
                         std::optional<Error> &error) {
            if (!writer.Valid())
                return;
            if (!project || !value.IsValid()) {
                writer.Reject();
                return;
            }
            auto persistent = project(value);
            if (persistent.HasError()) {
                error = std::move(persistent).ErrorValue();
                writer.Reject();
                return;
            }
            const auto &reference = persistent.Value();
            if (!HasIdentity(reference)) {
                writer.Reject();
                return;
            }
            writer.Bytes(reference.identity);
            writer.Unsigned(reference.generation);
        }

        /** @brief Reads the bounded wire identity independently of any runtime reference. */
        AiSavedEntityReference ReadSavedEntity(DecisionReader &reader) {
            AiSavedEntityReference reference;
            const auto identity = reader.Bytes(reference.identity.size());
            std::ranges::copy(identity, reference.identity.begin());
            reference.generation = reader.Unsigned();
            if (!HasIdentity(reference))
                reader.Reject();
            return reference;
        }

        /** @brief Resolves one durable entity only during Scene hydration. */
        BlackboardStoredEntityReference ReadEntity(DecisionReader &reader, const AiSaveEntityResolver *resolve, std::uint32_t version,
                                                   std::optional<Error> &error) {
            if (version != AiCanonicalReferenceRecordVersion || !reader.Valid()) {
                reader.Reject();
                return {};
            }
            const auto reference = ReadSavedEntity(reader);
            // A validation-only parse retains no agent values and never manufactures a runtime reference.
            if (!reader.Valid() || !resolve)
                return {};
            if (!*resolve) {
                reader.Reject();
                return {};
            }
            auto resolved = (*resolve)(reference);
            if (resolved.HasError()) {
                error = std::move(resolved).ErrorValue();
                reader.Reject();
                return {};
            }
            if (!resolved.Value().IsValid())
                reader.Reject();
            return resolved.Value();
        }

        /** @brief Validates wire framing and optionally materializes one complete runtime AI population. */
        Decoded DecodeRecord(std::span<const std::uint8_t> bytes, std::size_t maximumBytes, const AiSaveEntityResolver *resolve,
                             std::optional<std::uint32_t> requiredVersion = {}) {
            if (bytes.size() < RecordHeaderBytes || bytes.size() > maximumBytes)
                return Decoded::Failure(MakeError(AIErrors::CanonicalStateInvalid));
            DecisionReader reader{bytes};
            const auto version = static_cast<std::uint32_t>(reader.Unsigned(4));
            if ((version != AiCanonicalRecordVersion && version != AiCanonicalReferenceRecordVersion) ||
                (requiredVersion && version != *requiredVersion))
                return Decoded::Failure(MakeError(AIErrors::CanonicalSchemaUnsupported));
            const auto count = reader.Count(std::min(MaximumAiSceneAgents, (bytes.size() - RecordHeaderBytes) / MinimumAgentBytes));
            try {
                std::optional<Error> error;
                const Detail::ReadBlackboardEntity entity = [&](DecisionReader &record) {
                    return ReadEntity(record, resolve, version, error);
                };
                auto population = ReadPopulation(reader, count, entity, resolve != nullptr);
                return error ? Decoded::Failure(std::move(*error)) : std::move(population);
            } catch (const std::bad_alloc &) {
                return Decoded::Failure(MakeError(AIErrors::BlackboardStorageUnavailable));
            }
        }
    }  // namespace

    /** @copydoc EncodeAiCanonicalStateRecord */
    Encoded EncodeAiCanonicalStateRecord(const AiCanonicalState &state, const std::size_t maximumBytes,
                                         const AiSaveEntityProjector &project) {
        if (state.version != CurrentAiCanonicalStateVersion)
            return Encoded::Failure(MakeError(AIErrors::CanonicalSchemaUnsupported));
        if (maximumBytes < RecordHeaderBytes || state.agents.size() > MaximumAiSceneAgents)
            return Encoded::Failure(MakeError(AIErrors::CanonicalStateInvalid));
        try {
            DecisionWriter writer{maximumBytes};
            writer.Unsigned(project ? AiCanonicalReferenceRecordVersion : AiCanonicalRecordVersion, 4);
            writer.Unsigned(state.agents.size(), 4);
            AgentId previous;
            std::optional<Error> error;
            const Detail::WriteBlackboardEntity entity = [&](DecisionWriter &record, const BlackboardStoredEntityReference &value) {
                WriteEntity(record, value, project, error);
            };
            for (const auto &agent : state.agents) {
                if (!writer.Valid() || agent.authored.agent <= previous || !IsValid(agent)) {
                    writer.Reject();
                    break;
                }
                previous = agent.authored.agent;
                WriteAgent(writer, agent, entity);
            }
            if (error)
                return Encoded::Failure(std::move(*error));
            if (!writer.Valid())
                return Encoded::Failure(MakeError(AIErrors::CanonicalStateInvalid));
            return Encoded::Success(std::move(writer).Take());
        } catch (const std::bad_alloc &) {
            return Encoded::Failure(MakeError(AIErrors::BlackboardStorageUnavailable));
        }
    }

    /** @copydoc DecodeAiCanonicalStateRecord */
    Decoded DecodeAiCanonicalStateRecord(const std::span<const std::uint8_t> bytes, const std::size_t maximumBytes,
                                         const AiSaveEntityResolver &resolve) {
        return DecodeRecord(bytes, maximumBytes, &resolve);
    }

    /** @copydoc ValidateAiCanonicalStateRecord */
    Result<void> ValidateAiCanonicalStateRecord(std::span<const std::uint8_t> bytes, std::size_t maximumBytes,
                                                std::uint32_t requiredVersion) {
        auto checked = DecodeRecord(bytes, maximumBytes, nullptr, requiredVersion);
        return checked.HasError() ? Result<void>::Failure(std::move(checked).ErrorValue()) : Result<void>::Success();
    }
}  // namespace Horo::AI
