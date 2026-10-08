#include "EditorActivitySession.h"

#include "EditorActivitySessionState.h"
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

// This C endpoint is target-private, not an exported extension entry point.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(hidden)
#endif
extern "C" {
/** @brief C ABI session adapter; all copied publication and generation validation remain typed. */
HoroExtensionStatus HoroPublishActivitySession(void *context, const HoroEditorActivitySnapshot *snapshot) noexcept {
    if (context == nullptr || snapshot == nullptr)
        return HORO_EXTENSION_ERROR_INVALID_ARGS;
    try {
        return static_cast<Horo::Extensions::EditorActivitySession *>(context)->Publish(*snapshot);
    } catch (...) {
        return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
    }
}
}  // extern "C"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

namespace Horo::Extensions {
    namespace {
        static_assert(std::is_convertible_v<decltype(&HoroPublishActivitySession), decltype(HoroEditorActivitySessionApi::publish)>);

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
            if (const auto contained = path.lexically_relative(root);
                contained.empty() || contained.is_absolute() || *contained.begin() == "..")
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

    /** @brief Copies the exact declared pair and assigns one monotonic provider identity before capability admission. */
    static HoroExtensionStatus CopyDescriptors(const std::shared_ptr<EditorActivitySession> &session,
                                               AssetImporterRegistrationSession &load, const HoroEditorActivityDescriptor *descriptor,
                                               std::string &resource) {
        auto &activity = session->activity.surface;
        auto &drawer = session->drawer.surface;
        if (!Detail::CopyActivityText(descriptor->activityId, activity.id) || !Detail::CopyActivityText(descriptor->drawerId, drawer.id) ||
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
        return HORO_EXTENSION_SUCCESS;
    }

    /** @brief Derives the restricted context allowlists only from the copied initial declarative form. */
    static HoroExtensionStatus CopyFormPermissions(const std::shared_ptr<EditorActivitySession> &session,
                                                   const HoroEditorActivityDescriptor *descriptor) {
        if (descriptor->initialSnapshot.nodeCount > 256 ||
            (descriptor->initialSnapshot.nodeCount != 0 && descriptor->initialSnapshot.nodes == nullptr))
            return HORO_EXTENSION_ERROR_INVALID_ARGS;
        session->drawer.localization.emplace_back(session->activity.surface.labelLocalizationKey);
        for (std::uint32_t index = 0; index < descriptor->initialSnapshot.nodeCount && index < 256; ++index) {
            const auto &node = descriptor->initialSnapshot.nodes[index];
            std::string action;
            std::string label;
            std::string text;
            if (!Detail::CopyActivityText(node.actionId, action) || !Detail::CopyActivityText(node.labelKey, label, 128) ||
                !Detail::CopyActivityText(node.text, text, 4096))
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            if (!action.empty())
                session->drawer.commands.emplace_back(std::move(action));
            if (!label.empty())
                session->drawer.localization.emplace_back(std::move(label));
            if (!text.empty() && (node.flags & HORO_EDITOR_ACTIVITY_TEXT_TECHNICAL) == 0)
                session->drawer.localization.emplace_back(std::move(text));
        }
        auto &keys = session->drawer.localization;
        std::ranges::sort(keys, {}, &EditorSurfaceLocalizationKey::value);
        keys.erase(std::ranges::unique(keys).begin(), keys.end());
        auto &commands = session->drawer.commands;
        std::ranges::sort(commands, {}, &EditorSurfaceCommandId::value);
        commands.erase(std::ranges::unique(commands).begin(), commands.end());
        return HORO_EXTENSION_SUCCESS;
    }

    /** @brief Copies bounded locale messages and rejects duplicate package-local keys before publication. */
    static HoroExtensionStatus CopyMessages(const std::shared_ptr<EditorActivitySession> &session,
                                            const HoroEditorActivityDescriptor *descriptor) {
        std::size_t messageBytes{};
        for (std::uint32_t index = 0; index < descriptor->messageCount; ++index) {
            EditorActivitySession::Message message;
            if (const auto &input = descriptor->messages[index]; !Detail::CopyActivityText(input.locale, message.locale, 32) ||
                                                                 !Detail::CopyActivityText(input.key, message.key, 128) ||
                                                                 !Detail::CopyActivityText(input.value, message.value, 4096) ||
                                                                 message.locale.empty() || message.key.empty() || message.value.empty())
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            messageBytes += message.locale.size() + message.key.size() + message.value.size();
            if (messageBytes > 64U * 1024U || std::ranges::any_of(session->messages, [&message](const auto &existing) {
                return existing.locale == message.locale && existing.key == message.key;
            }))
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            session->messages.push_back(std::move(message));
        }
        if (!session->drawer.commands.empty() && !session->actions.provider.IsAvailable())
            return HORO_EXTENSION_ERROR_INVALID_ARGS;
        return HORO_EXTENSION_SUCCESS;
    }

    /** @brief Validates and copies one complete ABI contribution before admission. */
    static HoroExtensionStatus RegisterEditorActivityImpl(AssetImporterRegistrationSession &load,
                                                          const HoroEditorActivityDescriptor *descriptor,
                                                          HoroEditorActivitySessionApi *api) noexcept {
        if (descriptor == nullptr || api == nullptr || api->structSize < sizeof(HoroEditorActivitySessionApi))
            return HORO_EXTENSION_ERROR_INVALID_ARGS;
        try {
            if (!load.editorHost || !load.manifest || !load.extensionModule || !load.lifetime || !load.retirement ||
                load.lifetime->ownerThread != std::this_thread::get_id() || descriptor->structSize < sizeof(HoroEditorActivityDescriptor) ||
                descriptor->schemaVersion != HORO_EDITOR_ACTIVITY_SCHEMA_VERSION || descriptor->side > HORO_EDITOR_ACTIVITY_BOTTOM ||
                descriptor->group > 2 || descriptor->openByDefault > 1 || descriptor->messageCount > 128 ||
                (descriptor->messageCount != 0 && descriptor->messages == nullptr))
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            auto session = std::make_shared<EditorActivitySession>();
            session->host = load.editorHost;
            session->actions.provider = EditorActivityProviderAction{*descriptor};
            session->retirement = load.retirement;
            std::string resource;
            if (const auto status = CopyDescriptors(session, load, descriptor, resource); status != HORO_EXTENSION_SUCCESS)
                return status;
            const auto generation = load.editorGeneration;
            auto admission = ExtensionCapabilityAdmission::Evaluate({.extensionId = load.manifest->id,
                                                                     .moduleId = load.extensionModule->id,
                                                                     .activationGeneration = generation},
                                                                    {.revision = 1});
            if (admission.HasError())
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            session->admission.emplace(std::move(admission).Value());
            if (CopyFormPermissions(session, descriptor) != HORO_EXTENSION_SUCCESS ||
                CopyMessages(session, descriptor) != HORO_EXTENSION_SUCCESS)
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
            *api = {sizeof(HoroEditorActivitySessionApi), HORO_EDITOR_ACTIVITY_SESSION_VERSION, 31U, session.get(),
                    HoroPublishActivitySession};
            return HORO_EXTENSION_SUCCESS;
        } catch (...) {
            return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
        }
    }

}  // namespace Horo::Extensions

/** @copydoc RegisterExternalEditorActivity */
extern "C" HoroExtensionStatus RegisterExternalEditorActivity(void *context, const HoroEditorActivityDescriptor *descriptor,
                                                              HoroEditorActivitySessionApi *session) noexcept {
    if (!context)
        return HORO_EXTENSION_ERROR_INVALID_ARGS;
    auto &load = *static_cast<Horo::Extensions::AssetImporterRegistrationSession *>(context);
    const auto status = Horo::Extensions::RegisterEditorActivityImpl(load, descriptor, session);
    if (status != HORO_EXTENSION_SUCCESS) {
        load.failed = true;
        try {
            load.error =
                Horo::MakeError(Horo::Extensions::ExtensionErrors::ContributionRejected, "Editor activity ABI publication was rejected.");
        } catch (...) {
            // The typed ABI failure and sticky rejection remain authoritative when diagnostics cannot allocate.
            return status;
        }
    }
    return status;
}

namespace Horo::Extensions {

    /** @brief Rolls back the entire admitted batch after a failed commit, before returning the original error. */
    static void RevokeEditorActivities(const std::span<const std::shared_ptr<ExtensionModuleLifetime>> lifetimes) noexcept {
        for (const auto &owner : lifetimes)
            for (const auto &candidate : owner->editorActivities)
                candidate->Revoke();
    }

    /** @copydoc CommitEditorActivities */
    Result<void> CommitEditorActivities(const std::span<const std::shared_ptr<ExtensionModuleLifetime>> lifetimes) {
        for (const auto &lifetime : lifetimes) {
            for (const auto &session : lifetime->editorActivities) {
                if (const auto committed = session->Commit(); committed.HasError()) {
                    RevokeEditorActivities(lifetimes);
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
        return std::ranges::any_of(state_->sessions, [&provider](const auto &weak) {
            const auto session = weak.lock();
            return session && session->committed && !session->revoked && session->activity.surface.provider == provider &&
                   session->admission && session->admission->ActivationLease().IsUsable();
        });
    }

    std::span<const EditorActivityProjection> EditorActivityHost::Prepared() const noexcept {
        return state_->prepared;
    }

    Result<void> EditorActivityHost::QueueAction(const EditorSurfaceProviderIdentity &provider, const std::string_view activityId,
                                                 const std::string_view nodeId, const std::string_view actionId,
                                                 const std::uint64_t revision) const {
        for (const auto &weak : state_->sessions) {
            const auto session = weak.lock();
            if (!session || !session->committed || session->revoked || session->activity.surface.id != activityId ||
                session->activity.surface.provider != provider || revision != session->revision ||
                !session->actions.provider.IsAvailable() || session->actions.pending || session->actions.job ||
                !session->presentation.visible || !session->presentation.enabled)
                continue;
            for (const auto &node : session->form.nodes) {
                const auto *action = std::get_if<EditorUiActionNode>(&node.payload);
                if (action && action->base.id.value == nodeId && action->action.value == actionId && action->base.enabled) {
                    session->actions.pending = EditorActivitySession::PendingAction{std::string{nodeId}, std::string{actionId}, revision};
                    return Result<void>::Success();
                }
            }
        }
        return Result<void>::Failure(MakeError(ExtensionErrors::ContributionRejected));
    }

    /** @brief Finds the committed owner of a captured activity projection without invoking provider code. */
    static std::shared_ptr<EditorActivitySession> ProjectionOwner(const std::vector<std::weak_ptr<EditorActivitySession>> &sessions,
                                                                  const EditorSurfaceSnapshot &surface) {
        for (const auto &weak : sessions) {
            if (const auto session = weak.lock(); session && session->committed && !session->revoked &&
                                                  session->activity.surface.provider == surface.descriptor.provider &&
                                                  session->activity.surface.id == surface.descriptor.id)
                return session;
        }
        return nullptr;
    }

    void EditorActivityHost::Update() {
        for (const auto &weak : state_->sessions)
            if (const auto session = weak.lock())
                PumpEditorActivityAction(*session, state_->jobs, session);
        const auto revision = state_->registry.Revision();
        if (revision == state_->preparedRevision)
            return;
        state_->prepared.clear();
        auto snapshots = state_->registry.Snapshot();
        for (auto &surface : snapshots) {
            if (!surface.descriptor.activity)
                continue;
            const auto session = ProjectionOwner(state_->sessions, surface);
            if (!session)
                continue;
            const auto drawer = std::ranges::find_if(snapshots, [&surface](const auto &candidate) {
                return candidate.descriptor.id == surface.descriptor.activity->drawerId &&
                       candidate.descriptor.provider == surface.descriptor.provider;
            });
            surface.form = drawer != snapshots.end() ? drawer->form : std::nullopt;
            state_->prepared.emplace_back(std::move(surface), session->icon, session->revision);
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
            const auto session = weak.lock();
            if (!session || !session->committed || session->revoked || session->activity.surface.provider != provider)
                continue;
            for (const auto &message : session->messages)
                if (message.key == key && message.locale == locale)
                    return message.value;
            for (const auto &message : session->messages)
                if (message.key == key && message.locale == "en-US")
                    return message.value;
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
