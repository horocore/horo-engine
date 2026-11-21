#include "HeadlessReplayTimeline.h"

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <utility>

namespace Horo::Application::Internal {
    namespace {
        using Json = nlohmann::json;
        using enum AI::AiReplayBoundary;

        /** @brief Copies only schema-declared decimal identities and bounded integer observations. */
        Json SelectFields(const Json &record, std::initializer_list<const char *> identities,
                          std::initializer_list<const char *> observations) {
            auto selected = Json::object();
            for (const auto *name : identities)
                selected[name] = std::to_string(ReadHeadlessReplayWord(record.at(name)));
            for (const auto *name : observations) {
                const auto &value = record.at(name);
                if (!value.is_number_unsigned() || value > std::numeric_limits<std::uint32_t>::max())
                    throw InvalidHeadlessReplayEvidence{};
                selected[name] = value;
            }
            return selected;
        }

        /** @brief Selects only the stable decision operation and its state from the original trace record. */
        Json DecisionFields(const Json &record) {
            return SelectFields(record, {"graphId", "nodeId", "taskId", "transitionId"},
                                {"kind", "occurrence", "state", "transitionOrdinalOrCount"});
        }

        /** @brief Correlates publication and Blackboard sync to the same cooked query and original publication tick. */
        Json QueryFields(const Json &record) {
            return SelectFields(record, {"queryId", "nodeId", "outputKeyId", "publicationTick"}, {"status"});
        }

        /** @brief Copies the runtime-issued path sequence without the private path geometry. */
        Json NavigationFields(const Json &record) {
            auto fields = SelectFields(record, {"sequence"}, {"pointCount"});
            for (const auto *key : {"status", "stopReason"}) {
                const auto &value = record.at(key);
                if (!value.is_null() && (!value.is_number_unsigned() || value > std::numeric_limits<std::uint8_t>::max()))
                    throw InvalidHeadlessReplayEvidence{};
                fields[key] = value;
            }
            return fields;
        }

        /** @brief Correlates achieved movement without disclosing coordinates or velocities. */
        Json CharacterFields(const Json &record) {
            return SelectFields(record, {"objectId", "componentId", "characterTick"}, {"termination"});
        }

        /** @brief Copies a known Perception publication and explicitly scoped participant counts. */
        Json PerceptionFields(const Json &section) {
            auto fields = SelectFields(section, {"revision"}, {"sourceCount", "listenerCount"});
            fields["digest"] = FormatSha256(ReadHeadlessReplayDigest(section.at("digest")));
            return fields;
        }

        /** @brief Selects authored identities and fixed lifetime from a real post-poll stimulus. */
        Json StimulusFields(const Json &record) {
            auto fields =
                SelectFields(record, {"sourceObjectId", "listenerId", "senseId", "stimulusId", "firstSensedTick", "lastSensedTick"}, {});
            const auto &sensed = record.at("sensed");
            if (!sensed.is_boolean())
                throw InvalidHeadlessReplayEvidence{};
            fields["sensed"] = sensed;
            return fields;
        }

        /** @brief Copies only a declared boundary digest and canonical revision identities. */
        Json DigestFields(const Json &section, std::initializer_list<const char *> identities) {
            auto fields = SelectFields(section, identities, {});
            fields["digest"] = FormatSha256(ReadHeadlessReplayDigest(section.at("digest")));
            return fields;
        }

        /** @brief Selects only the original private body observation's stable public operation key. */
        Json BodyFields(const Json &record) {
            return SelectFields(record, {"objectId", "bodySlot"}, {"activity"});
        }

        /** @brief Selects the stable Gameplay transform recipient and leaves the submitted transform private. */
        Json GameplayFields(const Json &record) {
            return SelectFields(record, {"objectId"}, {});
        }

        /** @brief Reads one exact record sequence and its privacy projection. */
        struct ReplayRecords final {
            const Json *values{};
            Json (*project)(const Json &){};

            [[nodiscard]] std::size_t Size() const noexcept {
                return values ? values->size() : 0;
            }
        };

        /** @brief Looks up the original observation sequence for either the timeline or its divergence locator. */
        ReplayRecords RecordsFor(const Json &frame, const AI::AiReplayBoundary boundary) {
            const auto find = [&](const char *section, const char *key, Json (*project)(const Json &)) -> ReplayRecords {
                const auto found = frame.find(section);
                return found != frame.end() ? ReplayRecords{&found->at(key), project} : ReplayRecords{};
            };
            switch (boundary) {
                case QueryPublication:
                    return find("queryPublication", "records", QueryFields);
                case BlackboardSync:
                    return find("blackboardSync", "records", QueryFields);
                case Decision:
                    return find("decision", "records", DecisionFields);
                case Navigation:
                    return find("navigation", "results", NavigationFields);
                case Gameplay:
                    if (const auto found = frame.find("gameplay"); found != frame.end())
                        return {&found->at("records").at("transforms"), GameplayFields};
                    return {};
                case Physics:
                    return find("physics", "bodies", BodyFields);
                case AchievedCharacterMotion:
                    return find("characterMotion", "records", CharacterFields);
                default:
                    return {};
            }
        }

        /** @brief Projects the bounded publication facts when no more specific changed operation is available. */
        Json SummaryFor(const Json &frame, const AI::AiReplayBoundary boundary) {
            const auto digest = [&](const char *section, std::initializer_list<const char *> revisions) -> Json {
                const auto found = frame.find(section);
                return found != frame.end() ? DigestFields(*found, revisions) : Json(nullptr);
            };
            switch (boundary) {
                case SceneInput:
                    return digest("sceneInput", {"revision"});
                case PerceptionInput:
                    if (const auto found = frame.find("perceptionInput"); found != frame.end())
                        return PerceptionFields(*found);
                    return nullptr;
                case QueryPublication:
                    return digest("queryPublication", {"revision"});
                case BlackboardSync:
                    return digest("blackboardSync", {"revision"});
                case Decision: {
                    const auto &decision = frame.at("decision");
                    auto summary = SelectFields(decision, {"rootGraphId"}, {"outcome"});
                    summary["blackboardRevision"] = std::to_string(ReadHeadlessReplayWord(decision.at("blackboard").at("revision")));
                    return summary;
                }
                case Navigation:
                    return digest("navigation", {"topology"});
                case Gameplay:
                    return digest("gameplay", {"revision"});
                case Physics:
                    return digest("physics", {"publicationRevision"});
                case AchievedCharacterMotion:
                    return digest("characterMotion", {});
                case CanonicalAi:
                    return digest("canonicalAi", {"sceneStructureRevision"});
                default:
                    return nullptr;
            }
        }

        /** @brief Shared stable address, independent of timeline retention or wall time. */
        Json EventAddress(const std::string_view tick, const std::string_view agent, const std::string_view boundary,
                          const std::optional<std::size_t> ordinal) {
            return {{"tick", tick}, {"agentId", agent}, {"boundary", boundary}, {"ordinal", ordinal ? Json(*ordinal) : Json(nullptr)}};
        }
    }  // namespace

    HeadlessReplayTimeline::HeadlessReplayTimeline(const std::size_t limit) : limit_(limit) {}

    void HeadlessReplayTimeline::Add(const std::string_view boundary, const std::optional<std::size_t> ordinal, Json fields) {
        if (events_.size() >= limit_) {
            ++dropped_;
            return;
        }
        auto event = EventAddress(tick_, agent_, boundary, ordinal);
        event["operation"] = std::move(fields);
        events_.push_back(std::move(event));
    }

    /** @copydoc HeadlessReplayTimeline::Append */
    void HeadlessReplayTimeline::Append(const Json &frame, const std::uint64_t tick, const std::uint64_t agent) {
        tick_ = std::to_string(tick);
        agent_ = std::to_string(agent);
        for (std::size_t index = 0; index < HeadlessReplayBoundaryNames.size(); ++index) {
            const auto boundary = static_cast<AI::AiReplayBoundary>(index);
            const auto name = HeadlessReplayBoundaryNames[index];
            auto summary = SummaryFor(frame, boundary);
            if (!summary.is_null())
                Add(name, std::nullopt, std::move(summary));
            const auto records = RecordsFor(frame, boundary);
            for (std::size_t ordinal = 0; ordinal < records.Size(); ++ordinal)
                Add(name, ordinal, records.project(records.values->at(ordinal)));
            if (boundary == PerceptionInput) {
                if (const auto stimuli = frame.find("perceptionStimuli"); stimuli != frame.end()) {
                    auto state = SelectFields(*stimuli, {}, {"unresolvedSources"});
                    state["available"] = stimuli->at("available");
                    Add("perception-stimulus", std::nullopt, std::move(state));
                    std::size_t ordinal{};
                    for (const auto &record : stimuli->at("records"))
                        Add("perception-stimulus", ordinal++, StimulusFields(record));
                }
            }
        }
    }

    /** @copydoc HeadlessReplayTimeline::Finish */
    Json HeadlessReplayTimeline::Finish() && {
        return {{"format", "horo-ai-replay-timeline"},
                {"version", 1},
                {"eventLimit", limit_},
                {"droppedEvents", dropped_},
                {"events", std::move(events_)}};
    }

    /** @copydoc LocateHeadlessReplayDifference */
    Json LocateHeadlessReplayDifference(const AI::AiReplayBoundary boundary, const std::uint64_t tick, const std::uint64_t agent,
                                        const Json &recorded, const Json &replayed) {
        const auto left = RecordsFor(recorded, boundary);
        const auto right = RecordsFor(replayed, boundary);
        const auto common = std::min(left.Size(), right.Size());
        std::size_t ordinal{};
        while (ordinal < common && left.values->at(ordinal) == right.values->at(ordinal))
            ++ordinal;
        const bool operationChanged = ordinal < std::max(left.Size(), right.Size());
        auto address =
            EventAddress(std::to_string(tick), std::to_string(agent), HeadlessReplayBoundaryNames[static_cast<std::size_t>(boundary)],
                         operationChanged ? std::optional{ordinal} : std::nullopt);
        const auto project = [&](const ReplayRecords &records, const Json &frame) {
            if (!operationChanged)
                return SummaryFor(frame, boundary);
            return ordinal < records.Size() ? records.project(records.values->at(ordinal)) : Json(nullptr);
        };
        address["recorded"] = project(left, recorded);
        address["replayed"] = project(right, replayed);
        return address;
    }
}  // namespace Horo::Application::Internal
