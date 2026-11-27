#pragma once

/** @file ProjectAiSettings.h
 * @brief Validated project policy for gameplay AI simulation and development controls.
 */
#include "Horo/AI/AITaskScheduler.h"
#include "Horo/AI/EnvironmentQuerySchema.h"
#include "Horo/AI/LocalLearnedPolicy.h"
#include "Horo/AI/NpcDialogue.h"
#include "Horo/Application/ProjectSourceDocument.h"
#include "Horo/Assets/AssetId.h"
#include "Horo/Foundation/AssetCookTargetId.h"

namespace Horo::Application {
    /** @brief Execution profile supplied by the application composition owner. */
    enum class AiProjectRuntimeKind : std::uint8_t {
        Development,
        Packaged
    };

    /** @brief Operations requiring project authorization in a development profile. */
    enum class AiProjectDebugOperation : std::uint8_t {
        LiveMutation,
        ReplayCapture
    };

    struct ProjectLocalPolicyController final {
        AI::ControllerTypeId controller;
        Assets::AssetId artifact;
        std::vector<AI::BlackboardKeyId> featureKeys;
    };

    /** @brief One explicitly authored Int16LinearV1 package target and its local model bindings. */
    struct ProjectLocalPolicySettings final {
        AssetCookTargetId packageTarget;
        AI::LocalPolicyCookTarget target;
        AI::AiCapabilityDescriptorSet tasks;
        std::optional<AI::BlackboardSchemaId> blackboard;
        std::vector<ProjectLocalPolicyController> controllers;
    };

    /** @brief Stable project-authored identifier for a dialogue interaction. */
    using NpcDialogueTriggerId = AI::AiStableIdentity<struct NpcDialogueTriggerIdentityTag>;

    /** @brief Authored dialogue context for one interaction with an exact NPC identity. */
    struct ProjectNpcDialogueTrigger final {
        NpcDialogueTriggerId id;
        AI::AgentId agent;
        std::vector<AI::NpcDialogueFact> facts; /**< Canonical increasing list of distinct fields. */
    };

    /** @brief Authored local speaker label resolved by the caption presenter. */
    struct ProjectNpcDialogueSpeaker final {
        std::string messageNamespace;
        std::string messageKey;
        std::string fallbackText;
    };

    /** @brief Authored local text identity and duration for one provider-selectable caption. */
    struct ProjectNpcDialogueCaption final {
        AI::NpcDialogueCueId cue;
        std::string messageNamespace;
        std::string messageKey;
        std::string fallbackText;
        std::uint32_t durationTicks{};
        std::optional<ProjectNpcDialogueSpeaker> speaker;
    };

    /** @brief Complete caption catalog and timing policy for one project-selected interaction. */
    struct ProjectNpcDialoguePresentation final {
        NpcDialogueTriggerId trigger;
        AI::NpcDialogueCueId fallback;
        std::uint32_t deadlineTicks{};
        std::uint32_t workerMilliseconds{};
        std::vector<ProjectNpcDialogueCaption> captions; /**< Canonical increasing list of distinct cue IDs. */
    };

    /** @brief Project-authored NPC dialogue request. The host owns endpoint and disclosure approval. */
    struct ProjectNpcDialogueSettings final {
        static constexpr std::size_t MaximumTriggers = 256;

        AI::NpcDialogueRoute route;
        std::vector<AI::NpcDialogueFactId> fields; /**< Canonical increasing set of requested context fields. */
        std::vector<ProjectNpcDialogueTrigger> triggers; /**< Canonical increasing list of distinct trigger IDs. */
        std::optional<ProjectNpcDialoguePresentation> presentation;
    };

    /** @brief Validated authoritative project settings with no renderer or local visualization inputs. */
    class ProjectAiSettings final {
    public:
        /** @brief Returns the scheduler policy admitted by the runtime's validator. */
        [[nodiscard]] const AI::AiTaskSchedulerSettings &Scheduler() const noexcept {
            return scheduler_;
        }

        /** @brief Reports whether the project explicitly selected a scheduler mode. */
        [[nodiscard]] bool HasAuthoredSchedulerMode() const noexcept {
            return authoredSchedulerMode_;
        }

        /** @brief Returns the maximum authored Perception work units a Scene may request per tick. */
        [[nodiscard]] std::size_t PerceptionWorkUnitsLimit() const noexcept {
            return perceptionWorkUnitsLimit_;
        }

        /** @brief Returns the total work units available to environment queries in one authoritative tick. */
        [[nodiscard]] std::size_t QueryWorkUnitsLimit() const noexcept {
            return queryWorkUnitsLimit_;
        }

        /** @brief Returns the authored Scene cache budget for adaptive query execution. */
        [[nodiscard]] const std::optional<AI::QueryResultCacheLimits> &QueryCacheLimits() const noexcept {
            return queryCacheLimits_;
        }

        [[nodiscard]] const std::optional<ProjectLocalPolicySettings> &LocalPolicy() const noexcept {
            return localPolicy_;
        }

        /** @brief Returns the explicitly authored dialogue request, if present. */
        [[nodiscard]] const std::optional<ProjectNpcDialogueSettings> &NpcDialogue() const noexcept {
            return npcDialogue_;
        }

        /** @brief Checks the authored operation permission for a host-selected execution profile.
         * @param profile Exact execution profile supplied by the host.
         * @param operation Explicit operation requested by the application.
         * @return True only for a granted operation in a development profile.
         */
        [[nodiscard]] bool AllowsDebug(AiProjectRuntimeKind profile, AiProjectDebugOperation operation) const noexcept;

    private:
        friend Result<std::optional<ProjectAiSettings>> ResolveProjectAiSettings(const ProjectSourceDocument &);

        ProjectAiSettings() = default;

        AI::AiTaskSchedulerSettings scheduler_;
        std::size_t perceptionWorkUnitsLimit_{AI::AiTaskSchedulerSettings::HardWorkUnits};
        std::size_t queryWorkUnitsLimit_{AI::AiTaskSchedulerSettings::HardWorkUnits};
        std::optional<AI::QueryResultCacheLimits> queryCacheLimits_;
        bool mutation_{};
        bool capture_{};
        bool authoredSchedulerMode_{};
        std::optional<ProjectLocalPolicySettings> localPolicy_;
        std::optional<ProjectNpcDialogueSettings> npcDialogue_;
    };

    /** @brief Resolves versioned settings.ai from the canonical project source without creating a Scene.
     * @param source Strictly parsed project source document.
     * @return Validated authored policy, absence for older projects, or a typed policy diagnostic.
     */
    [[nodiscard]] Result<std::optional<ProjectAiSettings>> ResolveProjectAiSettings(const ProjectSourceDocument &source);
}  // namespace Horo::Application
