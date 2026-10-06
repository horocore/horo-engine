#include "EditorActivitySession.h"

#include "EditorActivityAbiConversion.h"
#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Foundation/JobSystem.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <thread>
#include <tuple>

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

    namespace {
        HoroExtensionStatus PublishSession(void *context, const HoroEditorActivitySnapshot *snapshot) noexcept {
            if (context == nullptr || snapshot == nullptr)
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            try {
                return static_cast<EditorActivitySession *>(context)->Publish(*snapshot);
            } catch (...) {
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            }
        }

        bool Declared(const AssetImporterRegistrationSession &load, const std::string_view type, const std::string &id) {
            return std::ranges::any_of(load.manifest->contributions, [&](const auto &claim) {
                return claim.type == type && claim.id == id && claim.owningModule == load.extensionModule->id;
            });
        }

        Result<EditorSvgIcon> ReadIcon(const ExtensionManifest &manifest, const std::string &relative) {
            namespace fs = std::filesystem;
            const auto invalid = [] {
                return Result<EditorSvgIcon>::Failure(MakeError(ExtensionErrors::EditorSurfaceDescriptorInvalid));
            };
            if (relative.empty() || relative.size() > 256 || relative.front() == '/' || relative.find("..") != std::string::npos ||
                relative.find('\\') != std::string::npos || relative.find(':') != std::string::npos)
                return invalid();
            std::error_code error;
            const auto root = fs::canonical(manifest.rootPath, error);
            if (error)
                return invalid();
            auto componentPath = root;
            for (const auto &component : fs::path{relative}) {
                componentPath /= component;
                const auto status = fs::symlink_status(componentPath, error);
                if (error || fs::is_symlink(status))
                    return invalid();
            }
            if (!fs::is_regular_file(componentPath, error) || error)
                return invalid();
            const auto path = fs::canonical(componentPath, error);
            if (error)
                return invalid();
            const auto contained = path.lexically_relative(root);
            if (contained.empty() || contained.is_absolute() || *contained.begin() == "..")
                return invalid();
            const auto size = fs::file_size(path, error);
            if (error || size == 0 || size > 64U * 1024U)
                return invalid();
            std::ifstream file{path, std::ios::binary};
            std::string bytes(static_cast<std::size_t>(size), '\0');
            file.read(bytes.data(), static_cast<std::streamsize>(size));
            if (!file || file.peek() != std::char_traits<char>::eof())
                return invalid();
            return RasterizeEditorSvgIcon(bytes);
        }
    }  // namespace

    /** @brief Validates and copies one complete ABI contribution before admission. */
    static HoroExtensionStatus RegisterEditorActivityImpl(void *context, const HoroEditorActivityDescriptor *descriptor,
                                                          HoroEditorActivitySessionApi *api) noexcept {
        if (context == nullptr || descriptor == nullptr || api == nullptr || api->structSize < sizeof(HoroEditorActivitySessionApi))
            return HORO_EXTENSION_ERROR_INVALID_ARGS;
        auto &load = *static_cast<AssetImporterRegistrationSession *>(context);
        try {
            if (!load.editorHost || !load.manifest || !load.extensionModule || !load.lifetime || !load.retirement ||
                load.lifetime->ownerThread != std::this_thread::get_id() || descriptor->structSize < sizeof(HoroEditorActivityDescriptor) ||
                descriptor->schemaVersion != HORO_EDITOR_ACTIVITY_SCHEMA_VERSION || descriptor->side > HORO_EDITOR_ACTIVITY_BOTTOM ||
                descriptor->group > 2 || descriptor->openByDefault > 1 || descriptor->messageCount > 128 ||
                (descriptor->messageCount != 0 && descriptor->messages == nullptr))
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            auto session = std::make_shared<EditorActivitySession>();
            session->host = load.editorHost;
            session->invoke = descriptor->invokeAction;
            session->moduleContext = descriptor->moduleContext;
            session->retirement = load.retirement;
            auto &activity = session->activity.surface;
            auto &drawer = session->drawer.surface;
            std::string resource;
            if (!Detail::CopyActivityText(descriptor->activityId, activity.id) ||
                !Detail::CopyActivityText(descriptor->drawerId, drawer.id) ||
                !Detail::CopyActivityText(descriptor->labelKey, activity.labelLocalizationKey, 128) ||
                !Detail::CopyActivityText(descriptor->tooltipKey, activity.tooltipLocalizationKey, 128) ||
                !Detail::CopyActivityText(descriptor->iconResource, resource) || !Declared(load, "editor.activity_item", activity.id) ||
                !Declared(load, "editor.panel", drawer.id) ||
                std::ranges::find(load.extensionModule->requiredCapabilities, HORO_EDITOR_ACTIVITY_HOST_CAPABILITY) ==
                    load.extensionModule->requiredCapabilities.end())
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            if (load.editorGeneration == 0)
                load.editorGeneration = EditorActivityHostAccess::Generation(*load.editorHost);
            const auto generation = load.editorGeneration;
            if (generation == 0)
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            activity.provider = {load.manifest->id, load.extensionModule->id, generation};
            activity.kind = EditorSurfaceKind::ActivityItem;
            activity.placement = {EditorSurfacePlacementKind::ActivityBar, {}, descriptor->order};
            activity.persistence = EditorSurfacePersistence::Workspace;
            activity.openByDefault = descriptor->openByDefault != 0;
            activity.activity = EditorActivityDestination{static_cast<EditorActivitySide>(descriptor->side),
                                                          static_cast<std::uint8_t>(descriptor->group), drawer.id, resource};
            drawer.provider = activity.provider;
            drawer.kind = EditorSurfaceKind::Panel;
            drawer.labelLocalizationKey = activity.labelLocalizationKey;
            drawer.tooltipLocalizationKey = activity.tooltipLocalizationKey;
            drawer.persistence = EditorSurfacePersistence::Workspace;
            drawer.placement = {EditorSurfacePlacementKind::Workspace, {}, descriptor->order};
            if (ValidateEditorSurfaceDescriptor(activity).HasError() || ValidateEditorSurfaceDescriptor(drawer).HasError())
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            auto admission = ExtensionCapabilityAdmission::Evaluate({.extensionId = load.manifest->id,
                                                                     .moduleId = load.extensionModule->id,
                                                                     .activationGeneration = generation},
                                                                    {.revision = 1});
            if (admission.HasError())
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            session->admission.emplace(std::move(admission).Value());
            if (descriptor->initialSnapshot.nodeCount > 256 ||
                (descriptor->initialSnapshot.nodeCount != 0 && descriptor->initialSnapshot.nodes == nullptr))
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            session->drawer.localization.push_back({activity.labelLocalizationKey});
            for (std::uint32_t index = 0; index < descriptor->initialSnapshot.nodeCount && index < 256; ++index) {
                const auto &node = descriptor->initialSnapshot.nodes[index];
                std::string action;
                std::string label;
                std::string text;
                if (!Detail::CopyActivityText(node.actionId, action) || !Detail::CopyActivityText(node.labelKey, label, 128) ||
                    !Detail::CopyActivityText(node.text, text, 4096))
                    return HORO_EXTENSION_ERROR_INVALID_ARGS;
                if (!action.empty())
                    session->drawer.commands.push_back({std::move(action)});
                if (!label.empty())
                    session->drawer.localization.push_back({std::move(label)});
                if (!text.empty() && (node.flags & HORO_EDITOR_ACTIVITY_TEXT_TECHNICAL) == 0)
                    session->drawer.localization.push_back({std::move(text)});
            }
            auto &keys = session->drawer.localization;
            std::ranges::sort(keys, {}, &EditorSurfaceLocalizationKey::value);
            keys.erase(std::ranges::unique(keys).begin(), keys.end());
            auto &commands = session->drawer.commands;
            std::ranges::sort(commands, {}, &EditorSurfaceCommandId::value);
            commands.erase(std::ranges::unique(commands).begin(), commands.end());
            std::size_t messageBytes{};
            for (std::uint32_t index = 0; index < descriptor->messageCount; ++index) {
                EditorActivitySession::Message message;
                const auto &input = descriptor->messages[index];
                if (!Detail::CopyActivityText(input.locale, message.locale, 32) || !Detail::CopyActivityText(input.key, message.key, 128) ||
                    !Detail::CopyActivityText(input.value, message.value, 4096) || message.locale.empty() || message.key.empty() ||
                    message.value.empty())
                    return HORO_EXTENSION_ERROR_INVALID_ARGS;
                messageBytes += message.locale.size() + message.key.size() + message.value.size();
                if (messageBytes > 64U * 1024U || std::ranges::any_of(session->messages, [&message](const auto &existing) {
                    return existing.locale == message.locale && existing.key == message.key;
                }))
                    return HORO_EXTENSION_ERROR_INVALID_ARGS;
                session->messages.push_back(std::move(message));
            }
            if (!commands.empty() && !session->invoke)
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            auto raster = ReadIcon(*load.manifest, resource);
            if (raster.HasError())
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            session->icon = std::make_shared<EditorSvgIcon>(std::move(raster).Value());
            if (session->Publish(descriptor->initialSnapshot) != HORO_EXTENSION_SUCCESS ||
                !EditorActivityHostAccess::Stage(*load.editorHost, session) ||
                !load.retirement->RegisterContribution(load.extensionModule->id, session))
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            load.lifetime->editorActivities.push_back(session);
            *api = {sizeof(HoroEditorActivitySessionApi), HORO_EDITOR_ACTIVITY_SESSION_VERSION, 31U, session.get(), PublishSession};
            return HORO_EXTENSION_SUCCESS;
        } catch (...) {
            return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
        }
    }

    /** @copydoc RegisterExternalEditorActivity */
    HoroExtensionStatus RegisterExternalEditorActivity(void *context, const HoroEditorActivityDescriptor *descriptor,
                                                       HoroEditorActivitySessionApi *session) noexcept {
        const auto status = RegisterEditorActivityImpl(context, descriptor, session);
        if (context && status != HORO_EXTENSION_SUCCESS) {
            auto &load = *static_cast<AssetImporterRegistrationSession *>(context);
            load.failed = true;
            try {
                load.error = MakeError(ExtensionErrors::ContributionRejected, "Editor activity ABI publication was rejected.");
            } catch (...) {
            }  // Preserve the sticky rejection even if diagnostics cannot allocate.
        }
        return status;
    }

    /** @copydoc CommitEditorActivities */
    Result<void> CommitEditorActivities(const std::span<const std::shared_ptr<ExtensionModuleLifetime>> lifetimes) {
        for (const auto &lifetime : lifetimes) {
            for (const auto &session : lifetime->editorActivities) {
                if (const auto committed = session->Commit(); committed.HasError()) {
                    for (const auto &owner : lifetimes)
                        for (const auto &candidate : owner->editorActivities)
                            candidate->Revoke();
                    return committed;
                }
            }
        }
        return Result<void>::Success();
    }

    EditorActivityHost::EditorActivityHost(JobSystem &jobs) : state_(std::make_unique<State>(jobs)) {}

    EditorActivityHost::~EditorActivityHost() {
        BeginShutdown();
    }

    EditorSurfaceRegistry &EditorActivityHost::Registry() noexcept {
        return state_->registry;
    }

    bool EditorActivityHost::IsLive(const EditorSurfaceProviderIdentity &provider) const noexcept {
        if (state_->shutdown)
            return false;
        for (const auto &weak : state_->sessions)
            if (const auto session = weak.lock(); session && session->committed && !session->revoked &&
                                                  session->activity.surface.provider == provider && session->admission &&
                                                  session->admission->ActivationLease().IsUsable())
                return true;
        return false;
    }

    std::span<const EditorActivityProjection> EditorActivityHost::Prepared() const noexcept {
        return state_->prepared;
    }

    Result<void> EditorActivityHost::QueueAction(const EditorSurfaceProviderIdentity &provider, const std::string_view activityId,
                                                 const std::string_view nodeId, const std::string_view actionId,
                                                 const std::uint64_t revision) {
        for (const auto &weak : state_->sessions) {
            const auto session = weak.lock();
            if (!session || !session->committed || session->revoked || session->activity.surface.id != activityId ||
                session->activity.surface.provider != provider || revision != session->revision || !session->invoke ||
                session->pendingAction || session->job || !session->presentation.visible || !session->presentation.enabled)
                continue;
            for (const auto &node : session->form.nodes) {
                const auto *action = std::get_if<EditorUiActionNode>(&node.payload);
                if (action && action->base.id.value == nodeId && action->action.value == actionId && action->base.enabled) {
                    session->pendingAction = EditorActivitySession::PendingAction{std::string{nodeId}, std::string{actionId}, revision};
                    return Result<void>::Success();
                }
            }
        }
        return Result<void>::Failure(MakeError(ExtensionErrors::ContributionRejected));
    }

    namespace {
        std::uint8_t ActionCancelled(const void *context) {
            return static_cast<std::uint8_t>(static_cast<const CancellationToken *>(context)->IsCancellationRequested());
        }

        HoroExtensionStatus PublishActionResult(void *context, const HoroEditorActivitySnapshot *snapshot) noexcept {
            if (!context || !snapshot)
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            try {
                auto &result = *static_cast<EditorActivitySession::ActionResult *>(context);
                auto copied = Detail::CopyActivityForm(*snapshot, result.drawerId, result.titleKey);
                if (copied.HasError())
                    return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
                const EditorActivityPresentation presentation{(snapshot->presentationFlags & HORO_EDITOR_ACTIVITY_HIDDEN) == 0,
                                                              (snapshot->presentationFlags & HORO_EDITOR_ACTIVITY_DISABLED) == 0,
                                                              snapshot->badgeCount};
                std::lock_guard lock{result.mutex};
                if (result.form || snapshot->revision <= result.revision)
                    return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
                result.form = std::move(copied).Value();
                result.presentation = presentation;
                result.revision = snapshot->revision;
                return HORO_EXTENSION_SUCCESS;
            } catch (...) {
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            }
        }

        void PumpAction(EditorActivitySession &session, JobSystem &jobs, const std::shared_ptr<EditorActivitySession> &owner) {
            if (session.revoked || !session.committed)
                return;
            if (session.result && session.result->ready.load(std::memory_order_acquire)) {
                const auto completed = session.result;
                std::lock_guard lock{completed->mutex};
                if (session.result->form && session.result->revision > session.revision) {
                    if (const auto host = session.host.lock();
                        host &&
                        host->Registry()
                            .PublishForm(session.drawer.surface.provider, session.drawer.surface.id, *session.result->form)
                            .HasValue() &&
                        host->Registry()
                            .PublishActivity(session.activity.surface.provider, session.activity.surface.id, session.result->presentation)
                            .HasValue()) {
                        session.form = std::move(*session.result->form);
                        session.presentation = session.result->presentation;
                        session.revision = session.result->revision;
                    }
                }
                session.job.reset();
                session.result.reset();
            }
            if (!session.pendingAction || session.job)
                return;
            const auto retirement = session.retirement.lock();
            const auto lease = retirement ? retirement->Acquire(session.activity.surface.provider.moduleId, ExtensionLeaseKind::Job,
                                                                session.activity.surface.id, owner)
                                          : nullptr;
            if (!lease) {
                session.pendingAction.reset();
                return;
            }
            auto action = std::move(*session.pendingAction);
            session.pendingAction.reset();
            auto result = std::make_shared<EditorActivitySession::ActionResult>();
            result->drawerId = session.drawer.surface.id;
            result->titleKey = session.drawer.surface.labelLocalizationKey;
            result->revision = session.revision;
            const auto invoke = session.invoke;
            void *const moduleContext = session.moduleContext;
            auto accepted = jobs.SubmitResult({},
                                              [lease, result, action = std::move(action), invoke,
                                               moduleContext](const CancellationToken &cancellation) -> Result<void> {
                if (cancellation.IsCancellationRequested()) {
                    result->ready.store(true, std::memory_order_release);
                    return JobCancelled();
                }
                HoroEditorActivityAction input{sizeof(HoroEditorActivityAction),
                                               {action.node.data(), static_cast<std::uint32_t>(action.node.size())},
                                               {action.action.data(), static_cast<std::uint32_t>(action.action.size())},
                                               action.revision,
                                               {&cancellation, ActionCancelled}};
                const HoroEditorActivitySnapshotSink sink{result.get(), PublishActionResult};
                HoroExtensionStatus status = HORO_EXTENSION_ERROR_INIT_FAILED;
                try {
                    status = invoke(moduleContext, &input, &sink);
                } catch (...) {
                    status = HORO_EXTENSION_ERROR_INIT_FAILED;
                }
                if (status != HORO_EXTENSION_SUCCESS || cancellation.IsCancellationRequested()) {
                    std::lock_guard lock{result->mutex};
                    result->form.reset();
                }
                result->ready.store(true, std::memory_order_release);
                if (cancellation.IsCancellationRequested() || status == HORO_EXTENSION_ERROR_CANCELLED)
                    return JobCancelled();
                if (status != HORO_EXTENSION_SUCCESS)
                    return Result<void>::Failure(MakeError(ExtensionErrors::ContributionRejected));
                return Result<void>::Success();
            });
            if (accepted.HasValue()) {
                session.job.emplace(std::move(accepted).Value());
                session.result = std::move(result);
            }
        }
    }  // namespace

    void EditorActivityHost::Update() {
        for (const auto &weak : state_->sessions)
            if (const auto session = weak.lock())
                PumpAction(*session, state_->jobs, session);
        const auto revision = state_->registry.Revision();
        if (revision == state_->preparedRevision)
            return;
        state_->prepared.clear();
        auto snapshots = state_->registry.Snapshot();
        for (auto &surface : snapshots) {
            if (!surface.descriptor.activity)
                continue;
            for (const auto &weak : state_->sessions) {
                if (const auto session = weak.lock(); session && session->committed && !session->revoked &&
                                                      session->activity.surface.provider == surface.descriptor.provider &&
                                                      session->activity.surface.id == surface.descriptor.id) {
                    const auto drawer = std::ranges::find_if(snapshots, [&surface](const auto &candidate) {
                        return candidate.descriptor.id == surface.descriptor.activity->drawerId &&
                               candidate.descriptor.provider == surface.descriptor.provider;
                    });
                    surface.form = drawer != snapshots.end() ? drawer->form : std::nullopt;
                    state_->prepared.push_back({std::move(surface), session->icon, session->revision});
                    break;
                }
            }
        }
        std::ranges::sort(state_->prepared, [](const auto &left, const auto &right) {
            const auto &a = left.surface.descriptor;
            const auto &b = right.surface.descriptor;
            return std::tie(a.activity->side, a.activity->group, a.placement.order, a.id) <
                   std::tie(b.activity->side, b.activity->group, b.placement.order, b.id);
        });
        state_->preparedRevision = revision;
    }

    std::string_view EditorActivityHost::LocalizedText(const EditorSurfaceProviderIdentity &provider, const std::string_view key,
                                                       const std::string_view locale) const noexcept {
        for (const auto &weak : state_->sessions) {
            if (const auto session = weak.lock();
                session && session->committed && !session->revoked && session->activity.surface.provider == provider) {
                for (const auto &message : session->messages)
                    if (message.key == key && message.locale == locale)
                        return message.value;
                for (const auto &message : session->messages)
                    if (message.key == key && message.locale == "en-US")
                        return message.value;
            }
        }
        return key;
    }

    void EditorActivityHost::BeginShutdown() noexcept {
        if (state_->shutdown)
            return;
        state_->shutdown = true;
        for (const auto &weak : state_->sessions)
            if (const auto session = weak.lock())
                session->Revoke();
        state_->registry.BeginShutdown();
        state_->prepared.clear();
    }
}  // namespace Horo::Extensions
