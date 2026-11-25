#include "Horo/Application/ProjectAiSettings.h"

#include "Horo/AI/AIErrors.h"

#include <algorithm>
#include <array>
#include <limits>
#include <nlohmann/json.hpp>

namespace Horo::Application {
    namespace {
        using Json = nlohmann::json;
        using Scheduler = AI::AiTaskSchedulerSettings;
        using Simulation = AI::AiDecisionSimulationProfile;

        /** @brief Checks that an object contains only keys owned by the selected AI schema. */
        bool HasOnly(const Json &object, const std::span<const std::string_view> keys) {
            return object.is_object() && std::ranges::all_of(object.items(), [&](const auto &item) {
                return std::ranges::find(keys, item.key()) != keys.end();
            });
        }

        /** @brief Copies an optional unsigned numeric field without truncating it on narrower platforms. */
        template <typename T> bool ReadUnsigned(const Json &object, const std::string_view key, T &value) {
            const auto found = object.find(key);
            if (found == object.end())
                return true;
            if (!found->is_number_unsigned() || found->get<std::uint64_t>() > std::numeric_limits<T>::max())
                return false;
            value = found->get<T>();
            return true;
        }

        /** @brief Copies a provided boolean setting without converting strings or numbers. */
        bool ReadFlag(const Json &object, const std::string_view key, bool &value) {
            const auto found = object.find(key);
            if (found == object.end())
                return true;
            if (!found->is_boolean())
                return false;
            value = found->get<bool>();
            return true;
        }

        /** @brief Decodes the closed mode values, preserving the runtime default when omitted. */
        bool ReadMode(const Json &object, Scheduler &scheduler) {
            const auto mode = object.find("mode");
            if (mode == object.end() || *mode == "deterministic")
                return true;
            if (*mode != "bestEffort")
                return false;
            scheduler.mode = AI::AiSchedulingMode::BestEffort;
            return true;
        }

        /** @brief Reads the optional gameplay distance bands used for best-effort scheduling. */
        bool ReadSimulation(const Json &object, Scheduler &scheduler) {
            const auto found = object.find("simulationProfile");
            if (found == object.end())
                return true;
            constexpr std::array<std::string_view, 5> keys{"fullDistanceMillimeters", "reducedDistanceMillimeters",
                                                           "suspensionDistanceMillimeters", "reducedIntervalTicks", "demotionDelayTicks"};
            constexpr std::array members{&Simulation::fullDistanceMillimeters, &Simulation::reducedDistanceMillimeters,
                                         &Simulation::suspensionDistanceMillimeters, &Simulation::reducedIntervalTicks,
                                         &Simulation::demotionDelayTicks};
            if (!HasOnly(*found, keys))
                return false;
            auto &profile = scheduler.simulationProfile.emplace();
            for (std::size_t index = 0; index < keys.size(); ++index) {
                if (!ReadUnsigned(*found, keys[index], profile.*members[index]))
                    return false;
            }
            return true;
        }

        /** @brief Preserves scheduler defaults for omitted budgets and rejects unowned keys. */
        bool ReadScheduler(const Json &object, Scheduler &scheduler) {
            constexpr std::array<std::string_view, 9> keys{"maximumAgents",
                                                           "evaluationsPerTick",
                                                           "workUnitsPerTick",
                                                           "commandsPerTick",
                                                           "workerSubmissionsPerTick",
                                                           "pendingWorkers",
                                                           "starvationTicks",
                                                           "mode",
                                                           "simulationProfile"};
            constexpr std::array budgets{&Scheduler::maximumAgents,   &Scheduler::evaluationsPerTick,       &Scheduler::workUnitsPerTick,
                                         &Scheduler::commandsPerTick, &Scheduler::workerSubmissionsPerTick, &Scheduler::pendingWorkers};
            if (!HasOnly(object, keys))
                return false;
            for (std::size_t index = 0; index < budgets.size(); ++index) {
                if (!ReadUnsigned(object, keys[index], scheduler.*budgets[index]))
                    return false;
            }
            return ReadUnsigned(object, "starvationTicks", scheduler.starvationTicks) && ReadMode(object, scheduler) &&
                   ReadSimulation(object, scheduler);
        }

        /** @brief Reads a bounded per-tick budget shared by a single AI subsystem. */
        bool ReadWorkUnitsLimit(const Json &root, const std::string_view subsystem, std::size_t &limit) {
            const auto found = root.find(subsystem);
            if (found == root.end())
                return true;
            constexpr std::array<std::string_view, 1> keys{"maximumWorkUnitsPerTick"};
            return HasOnly(*found, keys) && ReadUnsigned(*found, keys.front(), limit) && limit > 0 && limit <= Scheduler::HardWorkUnits;
        }

        /** @brief Requires all three independent retention limits when a project opts into adaptive reuse. */
        bool ReadQueryPolicy(const Json &root, std::size_t &work, std::optional<AI::QueryResultCacheLimits> &cache) {
            const auto query = root.find("query");
            if (query == root.end())
                return true;
            constexpr std::array<std::string_view, 2> keys{"maximumWorkUnitsPerTick", "cache"};
            if (!HasOnly(*query, keys) || !ReadUnsigned(*query, keys.front(), work) || work == 0 || work > Scheduler::HardWorkUnits)
                return false;
            const auto authored = query->find("cache");
            if (authored == query->end())
                return true;
            constexpr std::array<std::string_view, 3> limits{"maximumEntries", "maximumBytes", "lifetimeTicks"};
            AI::QueryResultCacheLimits policy;
            if (!HasOnly(*authored, limits) || !ReadUnsigned(*authored, limits[0], policy.maximumEntries) ||
                !ReadUnsigned(*authored, limits[1], policy.maximumBytes) || !ReadUnsigned(*authored, limits[2], policy.lifetimeTicks) ||
                !policy.WithinHardLimits())
                return false;
            cache = policy;
            return true;
        }

        /** @brief Reads only explicitly authorized development operations. */
        bool ReadDevelopment(const Json &root, bool &mutation, bool &capture) {
            const auto debug = root.find("development");
            if (debug == root.end())
                return true;
            constexpr std::array<std::string_view, 2> keys{"liveMutation", "replayCapture"};
            return HasOnly(*debug, keys) && ReadFlag(*debug, "liveMutation", mutation) && ReadFlag(*debug, "replayCapture", capture);
        }

        template <typename Identity> Identity ReadIdentity(const Json &value) {
            if (!value.is_number_unsigned())
                return {};
            const auto id = Identity::Create(value.get<std::uint64_t>());
            return id.HasValue() ? id.Value() : Identity{};
        }

        bool ReadModelTarget(const Json &object, ProjectLocalPolicySettings &policy) {
            if (!object.contains("packageTarget") || !object["packageTarget"].is_string() || !object.contains("interpreter") ||
                object["interpreter"] != "int16-linear-v1" || !object.contains("interpreterTarget"))
                return false;
            const auto target = AssetCookTargetId::Parse(object["packageTarget"].get_ref<const std::string &>());
            if (target.HasError())
                return false;
            policy.packageTarget = target.Value();
            policy.target.id = ReadIdentity<AI::LocalPolicyTargetId>(object["interpreterTarget"]);
            policy.target.supportsInt16LinearV1 = true;
            return policy.target.id.IsValid() && ReadUnsigned(object, "maximumModelBytes", policy.target.maximumModelBytes) &&
                   ReadUnsigned(object, "maximumWorkUnits", policy.target.maximumWorkUnits) &&
                   policy.target.maximumModelBytes >= sizeof(AI::LocalLearnedPolicy) &&
                   policy.target.maximumModelBytes <= 4U * 1024U * 1024U && policy.target.maximumWorkUnits > 0 &&
                   policy.target.maximumWorkUnits <= Scheduler::HardWorkUnits;
        }

        bool ReadModelTasks(const Json &object, ProjectLocalPolicySettings &policy) {
            if (const auto schema = object.find("blackboardSchema"); schema != object.end()) {
                const auto id = ReadIdentity<AI::BlackboardSchemaId>(*schema);
                if (!id.IsValid())
                    return false;
                policy.blackboard = id;
            }
            const auto tasks = object.find("tasks");
            if (tasks == object.end() || !tasks->is_array() || tasks->empty() || tasks->size() > AI::MaximumAiCapabilityTasks)
                return false;
            AI::AiCapabilityDescriptorRegistry registry;
            for (const auto &entry : *tasks) {
                if (registry.Register({ReadIdentity<AI::TaskId>(entry), policy.blackboard}).HasError())
                    return false;
            }
            policy.tasks = registry.Freeze();
            return policy.tasks.Tasks().size() == tasks->size();
        }

        bool ReadModelController(const Json &object, ProjectLocalPolicyController &binding) {
            constexpr std::array<std::string_view, 3> keys{"controller", "artifact", "featureKeys"};
            if (!HasOnly(object, keys) || object.size() != keys.size() || !object["artifact"].is_string())
                return false;
            binding.controller = ReadIdentity<AI::ControllerTypeId>(object["controller"]);
            const auto artifact = Assets::AssetId::Parse(object["artifact"].get_ref<const std::string &>());
            const auto &features = object["featureKeys"];
            if (!binding.controller.IsValid() || artifact.HasError() || !artifact.Value().IsValid() || !features.is_array() ||
                features.empty() || features.size() > AI::LocalLearnedPolicy::MaximumFeatures)
                return false;
            binding.artifact = artifact.Value();
            for (const auto &entry : features) {
                const auto key = ReadIdentity<AI::BlackboardKeyId>(entry);
                if (!key.IsValid())
                    return false;
                binding.featureKeys.push_back(key);
            }
            return true;
        }

        bool ReadLocalPolicy(const Json &root, std::optional<ProjectLocalPolicySettings> &output) {
            const auto object = root.find("localPolicy");
            if (object == root.end())
                return true;
            constexpr std::array<std::string_view, 8>
                keys{"packageTarget",    "interpreter", "interpreterTarget", "maximumModelBytes", "maximumWorkUnits",
                     "blackboardSchema", "tasks",       "controllers"};
            if (!HasOnly(*object, keys))
                return false;
            auto &policy = output.emplace();
            const auto controllers = object->find("controllers");
            if (controllers == object->end() || !controllers->is_array() || controllers->empty() ||
                controllers->size() > AI::MaximumAiCapabilityTasks || !ReadModelTarget(*object, policy) || !ReadModelTasks(*object, policy))
                return false;
            for (const auto &entry : *controllers) {
                ProjectLocalPolicyController binding;
                if (!ReadModelController(entry, binding) ||
                    std::ranges::find(policy.controllers, binding.controller, &ProjectLocalPolicyController::controller) !=
                        policy.controllers.end())
                    return false;
                policy.controllers.push_back(std::move(binding));
            }
            return true;
        }

        /** @brief Admits a bounded, distinct field set and stores it in canonical order. */
        bool ReadDialogueFields(const Json &fields, std::vector<AI::NpcDialogueFactId> &output) {
            if (!fields.is_array() || fields.size() > AI::NpcDialogueTurn::MaximumFacts)
                return false;
            output.reserve(fields.size());
            for (const auto &entry : fields) {
                const auto field = ReadIdentity<AI::NpcDialogueFactId>(entry);
                if (!field.IsValid())
                    return false;
                output.push_back(field);
            }
            std::ranges::sort(output);
            return std::ranges::adjacent_find(output) == output.end();
        }

        /** @brief Admits only authored field and value tokens for one interaction. */
        bool ReadTriggerFacts(const Json &facts, std::vector<AI::NpcDialogueFact> &output) {
            if (!facts.is_array() || facts.size() > AI::NpcDialogueTurn::MaximumFacts)
                return false;
            constexpr std::array<std::string_view, 2> keys{"field", "value"};
            for (const auto &entry : facts) {
                if (!HasOnly(entry, keys) || entry.size() != keys.size())
                    return false;
                const AI::NpcDialogueFact fact{ReadIdentity<AI::NpcDialogueFactId>(entry["field"]),
                                               ReadIdentity<AI::NpcDialogueValueId>(entry["value"])};
                if (!fact.field.IsValid() || !fact.value.IsValid())
                    return false;
                output.push_back(fact);
            }
            std::ranges::sort(output, {}, &AI::NpcDialogueFact::field);
            return std::ranges::adjacent_find(output, {}, &AI::NpcDialogueFact::field) == output.end();
        }

        /** @brief Decodes one trigger and its bound NPC. */
        bool ReadDialogueTrigger(const Json &entry, ProjectNpcDialogueTrigger &trigger) {
            constexpr std::array<std::string_view, 3> keys{"id", "agent", "facts"};
            if (!HasOnly(entry, keys) || entry.size() != keys.size())
                return false;
            trigger.id = ReadIdentity<NpcDialogueTriggerId>(entry["id"]);
            trigger.agent = ReadIdentity<AI::AgentId>(entry["agent"]);
            return trigger.id.IsValid() && trigger.agent.IsValid() && ReadTriggerFacts(entry["facts"], trigger.facts);
        }

        /** @brief Admits the optional bounded trigger catalog independent of trigger ordering in the source. */
        bool ReadDialogueTriggers(const Json &object, std::vector<ProjectNpcDialogueTrigger> &output) {
            const auto triggers = object.find("triggers");
            if (triggers == object.end())
                return true;
            if (!triggers->is_array() || triggers->size() > ProjectNpcDialogueSettings::MaximumTriggers)
                return false;
            output.reserve(triggers->size());
            for (const auto &entry : *triggers) {
                if (!ReadDialogueTrigger(entry, output.emplace_back()))
                    return false;
            }
            std::ranges::sort(output, {}, &ProjectNpcDialogueTrigger::id);
            return std::ranges::adjacent_find(output, {}, &ProjectNpcDialogueTrigger::id) == output.end();
        }

        /** @brief Reads typed dialogue config without admitting a URL, credential or arbitrary text field. */
        bool ReadNpcDialogue(const Json &root, std::optional<ProjectNpcDialogueSettings> &output) {
            const auto object = root.find("npcDialogue");
            if (object == root.end())
                return true;
            constexpr std::array<std::string_view, 5> keys{"provider", "destination", "version", "fields", "triggers"};
            if (!HasOnly(*object, keys) || !object->contains("provider") || !object->contains("destination") || !object->contains("fields"))
                return false;
            auto &dialogue = output.emplace();
            auto &route = dialogue.route;
            route.provider = ReadIdentity<AI::DecisionProviderId>((*object)["provider"]);
            route.destination = ReadIdentity<AI::NpcDialogueDestinationId>((*object)["destination"]);
            return route.provider.IsValid() && route.destination.IsValid() && ReadUnsigned(*object, "version", route.version) &&
                   route.version == AI::NpcDialogueWireVersion && ReadDialogueFields((*object)["fields"], dialogue.fields) &&
                   ReadDialogueTriggers(*object, dialogue.triggers);
        }

        /** @brief Reads and validates budgets under their owning scheduler, Perception and query rules. */
        Result<void> ReadBudgets(const Json &root, Scheduler &scheduler, std::size_t &perceptionLimit, std::size_t &queryLimit,
                                 std::optional<AI::QueryResultCacheLimits> &queryCache) {
            const auto policy = root.find("scheduler");
            if (policy != root.end() && !ReadScheduler(*policy, scheduler))
                return Result<void>::Failure(MakeError(AI::AIErrors::SchedulerPolicyInvalid, "Invalid project AI scheduler settings."));
            if (auto validated = AI::AiTaskScheduler::ValidateSettings(scheduler); validated.HasError())
                return validated;
            if (!ReadWorkUnitsLimit(root, "perception", perceptionLimit))
                return Result<void>::Failure(MakeError(AI::AIErrors::SchedulerPolicyInvalid, "Invalid project AI perception budget."));
            if (!ReadQueryPolicy(root, queryLimit, queryCache))
                return Result<void>::Failure(MakeError(AI::AIErrors::SchedulerPolicyInvalid, "Invalid project AI query budget."));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ProjectAiSettings::AllowsDebug */
    bool ProjectAiSettings::AllowsDebug(const AiProjectRuntimeKind profile, const AiProjectDebugOperation operation) const noexcept {
        if (profile != AiProjectRuntimeKind::Development)
            return false;
        switch (operation) {
            case AiProjectDebugOperation::LiveMutation:
                return mutation_;
            case AiProjectDebugOperation::ReplayCapture:
                return capture_;
        }
        return false;
    }

    /** @copydoc ResolveProjectAiSettings */
    Result<std::optional<ProjectAiSettings>> ResolveProjectAiSettings(const ProjectSourceDocument &source) {
        using Settings = Result<std::optional<ProjectAiSettings>>;
        if (!source.aiSettingsSource)
            return Settings::Success(std::nullopt);
        const auto root = Json::parse(*source.aiSettingsSource, nullptr, false);
        constexpr std::array<std::string_view, 7> keys{"schemaVersion", "scheduler", "perception", "query", "development", "localPolicy",
                                                       "npcDialogue"};
        std::uint32_t version{};
        if (!HasOnly(root, keys) || !ReadUnsigned(root, "schemaVersion", version) || version != 1)
            return Settings::Failure(MakeError(AI::AIErrors::SchedulerPolicyInvalid, "Invalid project AI settings schema."));
        ProjectAiSettings settings;
        if (auto budgets = ReadBudgets(root, settings.scheduler_, settings.perceptionWorkUnitsLimit_, settings.queryWorkUnitsLimit_,
                                       settings.queryCacheLimits_); budgets.HasError())
            return Settings::Failure(std::move(budgets).ErrorValue());
        const auto policy = root.find("scheduler");
        settings.authoredSchedulerMode_ = policy != root.end() && policy->contains("mode");
        if (!ReadDevelopment(root, settings.mutation_, settings.capture_))
            return Settings::Failure(MakeError(AI::AIErrors::SchedulerPolicyInvalid, "Invalid project AI development permissions."));
        if (!ReadLocalPolicy(root, settings.localPolicy_))
            return Settings::Failure(MakeError(AI::AIErrors::SchedulerPolicyInvalid, "Invalid project AI local policy."));
        if (!ReadNpcDialogue(root, settings.npcDialogue_))
            return Settings::Failure(MakeError(AI::AIErrors::SchedulerPolicyInvalid, "Invalid project NPC dialogue policy."));
        return Settings::Success(std::move(settings));
    }
}  // namespace Horo::Application
