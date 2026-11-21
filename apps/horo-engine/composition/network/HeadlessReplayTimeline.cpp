#include "HeadlessReplayTimeline.h"

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

        /** @brief Copies only a declared boundary digest and canonical revision identities. */
        Json DigestFields(const Json &section, std::initializer_list<const char *> identities) {
            auto fields = SelectFields(section, identities, {});
            fields["digest"] = FormatSha256(ReadHeadlessReplayDigest(section.at("digest")));
            return fields;
        }
    }  // namespace

    HeadlessReplayTimeline::HeadlessReplayTimeline(const std::size_t limit) : limit_(limit) {}

    void HeadlessReplayTimeline::Add(const std::string_view boundary, const std::size_t ordinal, Json fields) {
        if (events_.size() >= limit_) {
            ++dropped_;
            return;
        }
        events_.push_back(
            {{"tick", tick_}, {"agentId", agent_}, {"boundary", boundary}, {"ordinal", ordinal}, {"operation", std::move(fields)}});
    }

    /** @copydoc HeadlessReplayTimeline::Append */
    void HeadlessReplayTimeline::Append(const Json &frame, const std::uint64_t tick, const std::uint64_t agent) {
        tick_ = std::to_string(tick);
        agent_ = std::to_string(agent);
        const auto appendRecords = [&](AI::AiReplayBoundary boundary, const char *section, const char *key, auto select) {
            const auto found = frame.find(section);
            if (found == frame.end())
                return;
            std::size_t ordinal{};
            for (const auto &record : found->at(key))
                Add(HeadlessReplayBoundaryNames[static_cast<std::size_t>(boundary)], ordinal++, select(record));
        };

        Add(HeadlessReplayBoundaryNames[static_cast<std::size_t>(SceneInput)], 0, DigestFields(frame.at("sceneInput"), {"revision"}));
        if (const auto perception = frame.find("perceptionInput"); perception != frame.end())
            Add(HeadlessReplayBoundaryNames[static_cast<std::size_t>(PerceptionInput)], 0, PerceptionFields(*perception));
        appendRecords(QueryPublication, "queryPublication", "records", QueryFields);
        appendRecords(BlackboardSync, "blackboardSync", "records", QueryFields);
        appendRecords(Decision, "decision", "records", DecisionFields);
        appendRecords(Navigation, "navigation", "results", NavigationFields);
        if (const auto gameplay = frame.find("gameplay"); gameplay != frame.end())
            Add(HeadlessReplayBoundaryNames[static_cast<std::size_t>(Gameplay)], 0, DigestFields(*gameplay, {"revision"}));
        if (const auto physics = frame.find("physics"); physics != frame.end())
            Add(HeadlessReplayBoundaryNames[static_cast<std::size_t>(Physics)], 0, DigestFields(*physics, {"publicationRevision"}));
        appendRecords(AchievedCharacterMotion, "characterMotion", "records", CharacterFields);
        Add(HeadlessReplayBoundaryNames[static_cast<std::size_t>(CanonicalAi)], 0,
            DigestFields(frame.at("canonicalAi"), {"sceneStructureRevision"}));
    }

    /** @copydoc HeadlessReplayTimeline::Finish */
    Json HeadlessReplayTimeline::Finish() && {
        return {{"format", "horo-ai-replay-timeline"},
                {"version", 1},
                {"eventLimit", limit_},
                {"droppedEvents", dropped_},
                {"events", std::move(events_)}};
    }
}  // namespace Horo::Application::Internal
