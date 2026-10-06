#pragma once
#include "EditorActivityAbiConversion.h"
#include "EditorActivitySession.h"
#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Foundation/JobSystem.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <thread>

namespace Horo::Extensions {
    struct EditorActivityHost::State final {
        explicit State(JobSystem &scheduler) : jobs(scheduler) {}

        JobSystem &jobs;
        EditorSurfaceRegistry registry;
        std::vector<std::weak_ptr<EditorActivitySession>> sessions;
        std::vector<EditorActivityProjection> prepared;
        std::uint64_t nextGeneration{1};
        std::uint64_t preparedRevision{};
        bool shutdown{};
    };

    /** @brief Owner-lane copied publication, revoked before the provider's native unload. */
    struct EditorActivitySession final : IExtensionRetirementContribution {
        std::weak_ptr<EditorActivityHost> host;
        std::weak_ptr<ExtensionRetirement> retirement;
        std::thread::id owner{std::this_thread::get_id()};
        EditorSurfaceContextProvider contexts;
        std::optional<ExtensionCapabilityAdmission> admission;
        std::optional<EditorSurfaceRegistration> drawerRegistration;
        std::optional<EditorSurfaceRegistration> activityRegistration;
        EditorSurfaceContextDescriptor drawer;
        EditorSurfaceContextDescriptor activity;

        struct Message {
            std::string locale;
            std::string key;
            std::string value;
        };

        std::vector<Message> messages;
        EditorUiForm form;
        EditorActivityPresentation presentation;
        std::shared_ptr<const EditorSvgIcon> icon;

        struct PendingAction {
            std::string node;
            std::string action;
            std::uint64_t revision{};
        };

        std::optional<PendingAction> pendingAction;
        std::optional<JobHandle> job;

        struct ActionResult {
            std::string drawerId;
            std::string titleKey;
            std::mutex mutex;  // Protects only the bounded worker-to-owner result handoff; provider code runs outside the lock.
            std::optional<EditorUiForm> form;
            EditorActivityPresentation presentation;
            std::uint64_t revision{};
            std::atomic_bool ready{};
        };

        std::shared_ptr<ActionResult> result;
        HoroEditorActivityActionFunc invoke{};
        void *moduleContext{};
        std::uint64_t revision{};
        bool revoked{};
        bool committed{};

        void Revoke() noexcept override {
            revoked = true;
            committed = false;
            pendingAction.reset();
            if (job)
                static_cast<void>(job->RequestCancel());
            if (admission)
                admission->Revoke();
            if (activityRegistration)
                activityRegistration->Reset();
            if (drawerRegistration)
                drawerRegistration->Reset();
            contexts.BeginShutdown();
        }

        /** @brief Copies updates only on the owning lane and rejects stale revisions after teardown. */
        HoroExtensionStatus Publish(const HoroEditorActivitySnapshot &input) {
            if (revoked || owner != std::this_thread::get_id() || input.revision <= revision)
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            auto copied = Detail::CopyActivityForm(input, drawer.surface.id, drawer.surface.labelLocalizationKey);
            if (copied.HasError())
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            const EditorActivityPresentation next{(input.presentationFlags & HORO_EDITOR_ACTIVITY_HIDDEN) == 0,
                                                  (input.presentationFlags & HORO_EDITOR_ACTIVITY_DISABLED) == 0, input.badgeCount};
            if (committed) {
                const auto activeHost = host.lock();
                if (!activeHost ||
                    activeHost->Registry().PublishForm(drawer.surface.provider, drawer.surface.id, copied.Value()).HasError() ||
                    activeHost->Registry().PublishActivity(activity.surface.provider, activity.surface.id, next).HasError())
                    return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            }
            form = std::move(copied).Value();
            presentation = next;
            revision = input.revision;
            return HORO_EXTENSION_SUCCESS;
        }

        Result<void> Commit() {
            const auto activeHost = host.lock();
            if (!activeHost || revoked || !admission)
                return Result<void>::Failure(MakeError(ExtensionErrors::ContributionRejected));
            auto drawerContext = contexts.Attach(drawer, admission->ActivationLease());
            if (drawerContext.HasError())
                return Result<void>::Failure(drawerContext.ErrorValue());
            auto registeredDrawer = activeHost->Registry().Register(std::move(drawerContext).Value());
            if (registeredDrawer.HasError())
                return Result<void>::Failure(registeredDrawer.ErrorValue());
            drawerRegistration.emplace(std::move(registeredDrawer).Value());
            auto activityContext = contexts.Attach(activity, admission->ActivationLease());
            if (activityContext.HasError())
                return Result<void>::Failure(activityContext.ErrorValue());
            auto registeredActivity = activeHost->Registry().Register(std::move(activityContext).Value());
            if (registeredActivity.HasError())
                return Result<void>::Failure(registeredActivity.ErrorValue());
            activityRegistration.emplace(std::move(registeredActivity).Value());
            if (auto published = activeHost->Registry().PublishForm(drawer.surface.provider, drawer.surface.id, form); published.HasError())
                return published;
            if (auto published = activeHost->Registry().PublishActivity(activity.surface.provider, activity.surface.id, presentation);
                published.HasError())
                return published;
            committed = true;
            return Result<void>::Success();
        }
    };

    /** @brief The sole friend that mutates load-time host session ownership. */
    struct EditorActivityHostAccess final {
        static std::uint64_t Generation(EditorActivityHost &host) {
            if (host.state_->shutdown || host.state_->nextGeneration == std::numeric_limits<std::uint64_t>::max())
                return 0;
            return host.state_->nextGeneration++;
        }

        static bool Stage(EditorActivityHost &host, const std::shared_ptr<EditorActivitySession> &session) {
            std::erase_if(host.state_->sessions, [](const auto &entry) {
                return entry.expired();
            });
            if (host.state_->shutdown || host.state_->sessions.size() >= 128)
                return false;
            host.state_->sessions.push_back(session);
            return true;
        }
    };

    /** @brief Applies ready copied results and schedules the next admitted action on the owner lane. */
    void PumpEditorActivityAction(EditorActivitySession &session, JobSystem &jobs, const std::shared_ptr<EditorActivitySession> &owner);
}  // namespace Horo::Extensions
