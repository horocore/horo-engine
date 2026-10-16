#include "Horo/Application/ProjectCollisionSettings.h"

#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <new>

namespace Horo::Application {
    namespace {
        const ErrorCodeDescriptor Invalid{.domain = ErrorDomainId{"horo.project.collision"},
                                          .code = ErrorCode{"collision.document.invalid"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "The project collision document is invalid.",
                                          .remediationHint = "Repair the exact collision document before saving or activating Physics.",
                                          .userActionable = true};

        /** @brief Admits bounded portable display text without control characters. */
        bool ValidText(const std::string_view text, const std::size_t limit, const bool required) {
            return (!required || !text.empty()) && text.size() <= limit && IsValidUtf8ScalarSequence(text) &&
                   std::ranges::none_of(text, [](const unsigned char c) {
                return c < 0x20 || c == 0x7f;
            });
        }

        /** @brief Requires exactly one display entry for every semantic ID, with no independent definition authority. */
        template <typename Id, typename Definition>
        bool ValidDisplay(std::vector<CollisionDisplayMetadata<Id>> &metadata, const std::span<const Definition> definitions) {
            std::ranges::sort(metadata, {}, &CollisionDisplayMetadata<Id>::id);
            if (metadata.size() != definitions.size())
                return false;
            for (std::size_t i = 0; i < metadata.size(); ++i) {
                if (metadata[i].id != definitions[i].id || !ValidText(metadata[i].name, 128, true) ||
                    !ValidText(metadata[i].description, 1024, false))
                    return false;
            }
            return true;
        }
    }  // namespace

    /** @copydoc ProjectCollisionSettings::Create */
    Result<ProjectCollisionSettings> ProjectCollisionSettings::Create(ProjectCollisionSettingsInput input) {
        try {
            if (input.formatVersion != 1 || input.revision == 0 || !ValidText(input.projectId, 128, true))
                return Result<ProjectCollisionSettings>::Failure(MakeError(Invalid, "Invalid format, project identity or revision."));
            auto normalized = Physics::NormalizedCollisionSchema::Create(input.schema);
            if (normalized.HasError())
                return Result<ProjectCollisionSettings>::Failure(normalized.ErrorValue());
            if (!ValidDisplay(input.layers, normalized.Value().Layers()) || !ValidDisplay(input.profiles, normalized.Value().Profiles()) ||
                !ValidDisplay(input.channels, normalized.Value().QueryChannels()))
                return Result<ProjectCollisionSettings>::Failure(
                    MakeError(Invalid, "Display metadata must cover exactly the typed definitions."));
            auto schema = std::make_shared<const Physics::NormalizedCollisionSchema>(std::move(normalized).Value());
            return Result<ProjectCollisionSettings>::Success(ProjectCollisionSettings{std::move(input), std::move(schema)});
        } catch (const std::bad_alloc &) {
            return Result<ProjectCollisionSettings>::Failure(MakeError(Invalid, "Collision document capacity is unavailable."));
        }
    }

    /** @copydoc ProjectCollisionSettings::ProjectCollisionSettings */
    ProjectCollisionSettings::ProjectCollisionSettings(ProjectCollisionSettingsInput input,
                                                       std::shared_ptr<const Physics::NormalizedCollisionSchema> schema)
        : input_(std::move(input)), schema_(std::move(schema)) {}

    /** @copydoc ProjectCollisionSettings::Input */
    const ProjectCollisionSettingsInput &ProjectCollisionSettings::Input() const noexcept {
        return input_;
    }

    /** @copydoc ProjectCollisionSettings::Schema */
    const std::shared_ptr<const Physics::NormalizedCollisionSchema> &ProjectCollisionSettings::Schema() const noexcept {
        return schema_;
    }

    /** @copydoc ProjectCollisionSettings::Capture */
    Result<Physics::PhysicsWorldSimulationBinding> ProjectCollisionSettings::Capture(const std::uint64_t generation) const {
        if (generation == 0)
            return Result<Physics::PhysicsWorldSimulationBinding>::Failure(
                MakeError(Invalid, "Capture requires an explicit nonzero activation generation."));
        return Result<Physics::PhysicsWorldSimulationBinding>::Success({schema_, generation});
    }
}  // namespace Horo::Application
