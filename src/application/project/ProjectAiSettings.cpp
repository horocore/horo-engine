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

        /** @brief Validates the project ceiling for explicitly selected Scene perception budgets. */
        bool ReadPerception(const Json &root, std::size_t &limit) {
            const auto found = root.find("perception");
            if (found == root.end())
                return true;
            constexpr std::array<std::string_view, 1> keys{"maximumWorkUnitsPerTick"};
            return HasOnly(*found, keys) && ReadUnsigned(*found, keys.front(), limit) && limit > 0 && limit <= Scheduler::HardWorkUnits;
        }

        /** @brief Reads only explicitly authorized development operations. */
        bool ReadDevelopment(const Json &root, bool &mutation, bool &capture) {
            const auto debug = root.find("development");
            if (debug == root.end())
                return true;
            constexpr std::array<std::string_view, 2> keys{"liveMutation", "replayCapture"};
            return HasOnly(*debug, keys) && ReadFlag(*debug, "liveMutation", mutation) && ReadFlag(*debug, "replayCapture", capture);
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
        constexpr std::array<std::string_view, 4> keys{"schemaVersion", "scheduler", "perception", "development"};
        std::uint32_t version{};
        if (!HasOnly(root, keys) || !ReadUnsigned(root, "schemaVersion", version) || version != 1)
            return Settings::Failure(MakeError(AI::AIErrors::SchedulerPolicyInvalid, "Invalid project AI settings schema."));
        Scheduler scheduler;
        if (const auto policy = root.find("scheduler"); policy != root.end() && !ReadScheduler(*policy, scheduler))
            return Settings::Failure(MakeError(AI::AIErrors::SchedulerPolicyInvalid, "Invalid project AI scheduler settings."));
        if (const auto validated = AI::AiTaskScheduler::ValidateSettings(scheduler); validated.HasError())
            return Settings::Failure(validated.ErrorValue());
        auto perceptionLimit = Scheduler::HardWorkUnits;
        if (!ReadPerception(root, perceptionLimit))
            return Settings::Failure(MakeError(AI::AIErrors::SchedulerPolicyInvalid, "Invalid project AI perception budget."));
        bool mutation{}, capture{};
        if (!ReadDevelopment(root, mutation, capture))
            return Settings::Failure(MakeError(AI::AIErrors::SchedulerPolicyInvalid, "Invalid project AI development permissions."));
        return Settings::Success(ProjectAiSettings{std::move(scheduler), perceptionLimit, mutation, capture});
    }
}  // namespace Horo::Application
