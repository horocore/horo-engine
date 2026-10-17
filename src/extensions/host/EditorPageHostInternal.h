#pragma once
#include "Horo/Extensions/EditorPageHost.h"
#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Foundation/JobSystem.h"

#include <atomic>
#include <limits>
#include <thread>
#include <type_traits>

namespace Horo::Extensions {
    /** @brief Immutable worker result after ready/release; owner reads only after ready/acquire, without provider-held locks. */
    struct EditorPageWork final {
        EditorPageDraft draft;
        EditorUiActionId action;
        EditorPageOperation operation{EditorPageOperation::Validate};
        std::optional<Result<std::vector<EditorPageFinding>>> result;
        std::atomic_bool ready{};
    };

    /** @brief One owner-lane page and its independent worker result; no worker captures this session or borrowed host state. */
    struct EditorPageSession final : IExtensionRetirementContribution {
        explicit EditorPageSession(EditorSurfaceContext view) : context(std::move(view)) {}

        EditorSurfaceContext context;
        std::optional<EditorSurfaceRegistration> registration;
        EditorPageDefinition definition;
        ConfigurationSchema schema;
        std::optional<ConfigurationSnapshot> active;
        EditorPageDraft draft;
        EditorPageState state{EditorPageState::Closed};
        std::uint64_t contextRevision{};
        std::uint64_t revision{1};
        std::optional<EditorUiId> focused;
        std::vector<EditorPageFinding> findings;
        std::optional<Error> failure;
        std::shared_ptr<IEditorPageOperations> operations;
        std::shared_ptr<ExtensionExecutableLease> publication;
        std::weak_ptr<ExtensionRetirement> retirement;
        std::optional<JobHandle> job;
        std::shared_ptr<EditorPageWork> work;
        std::optional<EditorPageDraft> cancelledDraft;
        bool revoked{};
        bool dirty{};

        void Revoke() noexcept override {
            if (revoked)
                return;
            revoked = true;
            state = EditorPageState::Revoked;
            if (job)
                static_cast<void>(job->RequestCancel());
            job.reset();
            work.reset();
            focused.reset();
            if (registration)
                registration->Reset();
            operations.reset();
            publication.reset();
        }
    };

    struct EditorPageHostState final {
        EditorPageHostState(JobSystem &scheduler, EditorSurfaceRegistry &surfaces, IEditorPageStore &persistence,
                            ExtensionAdmissionPolicy evidence)
            : jobs(scheduler), registry(surfaces), store(persistence), policy(std::move(evidence)) {}

        JobSystem &jobs;
        EditorSurfaceRegistry &registry;
        IEditorPageStore &store;
        const ExtensionAdmissionPolicy policy;
        const std::thread::id owner{std::this_thread::get_id()};
        std::vector<std::weak_ptr<EditorPageSession>> sessions;
        std::vector<EditorPageProjection> prepared;
        std::uint64_t nextGeneration{1};
        bool shutdown{};
        bool projectionDirty{true};

        bool OnThread() const noexcept {
            return owner == std::this_thread::get_id();
        }
    };

    /** @brief Sole private load-time bridge to the explicit page host's activation identity sequence. */
    struct EditorPageHostAccess final {
        // Identity issuance increments the owner-only high-water mark; a const host is not an issuing capability.
        static std::uint64_t Generation(EditorPageHost &host) noexcept {  // NOSONAR(cpp:S995): Issues activation identity.
            auto &state = *host.state_;
            if (!state.OnThread() || state.shutdown || state.nextGeneration == std::numeric_limits<std::uint64_t>::max())
                return 0;
            return state.nextGeneration++;
        }
    };

    static_assert(std::is_invocable_v<decltype(&EditorPageHostAccess::Generation), EditorPageHost &>);
    static_assert(!std::is_invocable_v<decltype(&EditorPageHostAccess::Generation), const EditorPageHost &>);

    namespace EditorPageDetail {
        /** @brief Builds canonical control failures without discarding downstream platform/provider errors. */
        template <class T = void> Result<T> Failure(const ErrorCodeDescriptor &error = ExtensionErrors::EditorSurfaceRegistryInvalid) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Resolves one exact live provider generation on the owner lane. */
        std::shared_ptr<EditorPageSession> Find(const EditorPageHostState &host, const EditorSurfaceProviderIdentity &provider,
                                                std::string_view id);
        /** @brief Resolves the canonical source bucket for a single admitted page domain. */
        ConfigurationSourceMap &Source(ConfigurationResolutionRequest &request, SettingScope scope);
        /** @brief Resolves one whole typed candidate with exact constraints, without publication. */
        Result<ConfigurationSnapshot> Candidate(const EditorPageSession &session, const EditorPageDraft &draft,
                                                ConfigurationRevision revision);
        /** @brief Rebuilds bounded draft values and form projection from one canonical committed snapshot. */
        void RestoreDraft(EditorPageSession &session);
        /** @brief Copies the admitted form and overlays only exact typed draft values. */
        EditorUiForm Form(const EditorPageSession &session);
        /** @brief Refreshes copied projections outside draw traversal. */
        void Prepare(EditorPageHostState &host);
        /** @brief Checks primitive focus eligibility and all ancestor admission flags without retaining UI state. */
        bool AdmitsInput(const EditorUiForm &form, const EditorUiNode &node);
        /** @brief Checks exact field count, scope permission and activation evidence before any controller use. */
        Result<void> CheckPermission(const EditorPageHostState &host, const EditorPageSession &session);
        /** @brief Schedules one callback with real native-code and capability leases; no provider callback runs on this owner lane. */
        Result<void> Schedule(EditorPageHostState &host, EditorPageSession &session, EditorPageOperation operation,
                              EditorUiActionId action);
        /** @brief Consumes only ready exact-revision results, validating every finding before durable publication. */
        void Complete(EditorPageHostState &host, EditorPageSession &session);
    }  // namespace EditorPageDetail
}  // namespace Horo::Extensions
