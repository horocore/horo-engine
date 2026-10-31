#include "AddComponentRegistry.h"

#include <algorithm>
#include <unordered_set>

namespace Horo::Editor {
    namespace {
        const ErrorDomainId domain{"horo.editor.add_component"};
        const ErrorCodeDescriptor invalid{domain, ErrorCode{"add_component.invalid"}, ErrorSeverity::Error,
                                          "Add Component metadata is invalid.",
                                          "Supply a stable identity, label, category and availability predicate."};
        const ErrorCodeDescriptor duplicate{domain, ErrorCode{"add_component.duplicate"}, ErrorSeverity::Error,
                                            "Add Component contribution identities collide.",
                                            "Give each contribution a unique stable identity."};
        constexpr std::array categories{
            AddComponentCategoryDefinition{AddComponentCategory::Rendering, "workspace.inspector.add_rendering"},
            AddComponentCategoryDefinition{AddComponentCategory::Audio, "workspace.inspector.add_audio"},
            AddComponentCategoryDefinition{AddComponentCategory::Gameplay, "workspace.inspector.add_gameplay"},
            AddComponentCategoryDefinition{AddComponentCategory::Scripts, "workspace.inspector.add_scripts"},
            AddComponentCategoryDefinition{AddComponentCategory::Physics, "workspace.physics.title"},
        };

        /** @brief Limits ImGui identity tokens to stable ASCII without display-label separators. */
        bool ValidId(const std::string_view id) {
            return !id.empty() && std::ranges::all_of(id, [](const char c) {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
            });
        }
    }  // namespace

    /** @copydoc AddComponentRegistry::AddComponentRegistry */
    AddComponentRegistry::AddComponentRegistry() {
        for (const auto &definition : BuiltinAddComponentDefinitions())
            AppendStatic(definition);
    }

    /** @copydoc AddComponentRegistry::AppendStatic */
    void AddComponentRegistry::AppendStatic(const BuiltinAddComponentDefinition &definition) {
        const auto action = std::visit([](const auto value) -> AddComponentAction {
            return value;
        }, definition.action);
        entries_[static_cast<std::size_t>(definition.category)].push_back({std::string{definition.id}, std::string{definition.labelKey},
                                                                           action, definition.available, true, false,
                                                                           definition.separatorBefore});
    }

    /** @copydoc AddComponentRegistry::RegisterStatic */
    Result<void> AddComponentRegistry::RegisterStatic(const BuiltinAddComponentDefinition &definition) {
        if (!ValidId(definition.id) || definition.id.starts_with("behavior.") || definition.labelKey.empty() || !definition.available ||
            static_cast<std::size_t>(definition.category) >= entries_.size())
            return Result<void>::Failure(MakeError(invalid));
        if (Find(definition.id))
            return Result<void>::Failure(MakeError(duplicate));
        AppendStatic(definition);
        behaviorRevision_.reset();
        return Result<void>::Success();
    }

    /** @copydoc AddComponentRegistry::SynchronizeBehaviors */
    Result<void> AddComponentRegistry::SynchronizeBehaviors(const std::string_view projectScope, const std::uint64_t revision,
                                                            const std::span<const Gameplay::BehaviorDescriptor> behaviors) {
        if (behaviorRevision_ == revision && projectScope_ == projectScope)
            return Result<void>::Success();
        if (projectScope.empty() && !behaviors.empty())
            return Result<void>::Failure(MakeError(invalid));
        std::unordered_set<std::string> ids;
        std::vector<AddComponentEntry> next;
        auto &scripts = entries_[static_cast<std::size_t>(AddComponentCategory::Scripts)];
        next.reserve(behaviors.size() + scripts.size());
        for (const auto &behavior : behaviors) {
            if (behavior.typeId.Value().empty() || behavior.displayName.empty())
                return Result<void>::Failure(MakeError(invalid));
            const std::string id = "behavior." + behavior.typeId.Value();
            if (!ids.insert(id).second)
                return Result<void>::Failure(MakeError(duplicate));
            next.push_back({id, behavior.displayName, behavior.typeId, nullptr, false, behavior.allowMultiple, false});
        }
        for (const auto &entry : scripts)
            if (!std::holds_alternative<Gameplay::BehaviorTypeId>(entry.action))
                next.push_back(entry);
        std::string scope{projectScope};
        scripts = std::move(next);
        projectScope_ = std::move(scope);
        behaviorRevision_ = revision;
        return Result<void>::Success();
    }

    /** @copydoc AddComponentRegistry::ClearProjectBehaviors */
    void AddComponentRegistry::ClearProjectBehaviors() {
        auto &scripts = entries_[static_cast<std::size_t>(AddComponentCategory::Scripts)];
        std::erase_if(scripts, [](const auto &entry) {
            return std::holds_alternative<Gameplay::BehaviorTypeId>(entry.action);
        });
        projectScope_.clear();
        behaviorRevision_.reset();
    }

    /** @copydoc AddComponentRegistry::Categories */
    std::span<const AddComponentCategoryDefinition> AddComponentRegistry::Categories() noexcept {
        return categories;
    }

    /** @copydoc AddComponentRegistry::Entries */
    std::span<const AddComponentEntry> AddComponentRegistry::Entries(const AddComponentCategory category) const noexcept {
        const auto index = static_cast<std::size_t>(category);
        return index < entries_.size() ? std::span<const AddComponentEntry>{entries_[index]} : std::span<const AddComponentEntry>{};
    }

    /** @copydoc AddComponentRegistry::Find */
    const AddComponentEntry *AddComponentRegistry::Find(const std::string_view id) const noexcept {
        for (const auto &category : entries_)
            for (const auto &entry : category)
                if (entry.id == id)
                    return &entry;
        return nullptr;
    }

    /** @copydoc AddComponentRegistry::IsAvailable */
    bool AddComponentRegistry::IsAvailable(const AddComponentEntry &entry, const AddComponentContext &context) const noexcept {
        if (!context.canIssueCommand)
            return false;
        if (std::holds_alternative<AddComponentWorkflow>(entry.action))
            return context.hasProject && entry.available && entry.available(context);
        if (context.object.effectivelyLocked)
            return false;
        if (const auto *behavior = std::get_if<Gameplay::BehaviorTypeId>(&entry.action))
            return entry.allowMultiple || std::ranges::none_of(context.object.components.behaviors, [&](const auto &component) {
                return component.typeId == *behavior;
            });
        return entry.available && entry.available(context);
    }
}  // namespace Horo::Editor
