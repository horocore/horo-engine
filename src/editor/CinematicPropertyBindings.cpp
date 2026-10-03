#include "Horo/Editor/CinematicPropertyBindings.h"

#include "Horo/Cinematic/CinematicErrors.h"

#include <algorithm>
#include <format>
#include <utility>

namespace Horo::Editor {
    /** @copydoc CinematicPropertyProblems::CinematicPropertyProblems */
    CinematicPropertyProblems::CinematicPropertyProblems(DiagnosticsEngine &diagnostics, BuildOutputStore &output, std::string sourcePath,
                                                         const std::span<const PropertyTrackSource> sources)
        : diagnostics_(diagnostics), output_(output), sourcePath_(std::move(sourcePath)), sources_(sources.begin(), sources.end()) {}

    /** @copydoc CinematicPropertyProblems::Sink */
    Cinematic::PropertyDiagnosticSink CinematicPropertyProblems::Sink() noexcept {
        return {this};
    }

    /** @copydoc CinematicPropertyProblems::Publish */
    void CinematicPropertyProblems::Publish(const Cinematic::PropertyBindingDiagnosticEvent &event) {
        if (!event.diagnostic.error.has_value())
            return;
        const auto &error = *event.diagnostic.error;
        const auto source = std::ranges::find(sources_, event.diagnostic.track, &PropertyTrackSource::track);
        const std::uint32_t line = source == sources_.end() ? 0 : source->line;
        const std::uint32_t column = source == sources_.end() ? 0 : source->column;
        const DiagnosticCode code{error.code.Value()};
        const auto severity = DiagnosticSeverityForError(error.severity).value_or(DiagnosticSeverity::Error);
        Diagnostic diagnostic{.code = code,
                              .severity = severity,
                              .message = error.message,
                              .location = {sourcePath_, line, column},
                              .path = std::format("sequence/{}/tracks/{}", event.sequence.stableValue, event.diagnostic.track.stableValue)};
        Error surfaced = error;
        surfaced.diagnostics.push_back(std::move(diagnostic));
        diagnostics_.Report(std::move(surfaced));
        output_.Append({.severity = severity,
                        .stage = "cinematic.property_binding",
                        .code = code,
                        .message = error.message,
                        .source = DiagnosticSourceLocation{sourcePath_, line, column}});
    }

    /** @copydoc InspectorPropertyBindings::InspectorPropertyBindings */
    InspectorPropertyBindings::InspectorPropertyBindings(const Runtime::PropertyBindingRegistry &registry) noexcept : registry_(registry) {}

    /** @copydoc InspectorPropertyBindings::Enumerate */
    Result<std::size_t> InspectorPropertyBindings::Enumerate(const Gameplay::ComponentTypeId &componentType,
                                                             const std::span<const Runtime::PropertyBindingDescriptor *> output) const {
        if (!registry_.IsFrozen())
            return Result<std::size_t>::Failure(MakeError(Cinematic::CinematicErrors::PropertyRegistryUnfrozen));
        std::size_t count{};
        for (const auto &descriptor : registry_.Descriptors()) {
            if (descriptor.componentType != componentType || descriptor.writePolicy == Runtime::PropertyWritePolicy::ReadOnly)
                continue;
            if (count == output.size())
                return Result<std::size_t>::Failure(MakeError(Cinematic::CinematicErrors::PropertyLimitExceeded));
            output[count++] = &descriptor;
        }
        return Result<std::size_t>::Success(count);
    }

    /** @copydoc InspectorPropertyBindings::Read */
    Result<Runtime::PropertyBindingValue> InspectorPropertyBindings::Read(const Cinematic::PropertyBindingTargetSnapshot &target) const {
        if (!registry_.IsFrozen())
            return Result<Runtime::PropertyBindingValue>::Failure(MakeError(Cinematic::CinematicErrors::PropertyRegistryUnfrozen));
        const auto *descriptor = registry_.Find(target.binding);
        if (descriptor == nullptr)
            return Result<Runtime::PropertyBindingValue>::Failure(MakeError(Cinematic::CinematicErrors::PropertyBindingMissing));
        if (!target.targetObject.IsValid() || target.component == nullptr || target.componentRevision == 0)
            return Result<Runtime::PropertyBindingValue>::Failure(MakeError(Cinematic::CinematicErrors::PropertyBindingTargetMissing));
        if (target.componentType != descriptor->componentType)
            return Result<Runtime::PropertyBindingValue>::Failure(MakeError(Cinematic::CinematicErrors::PropertyComponentMismatch));
        return descriptor->getter(target.component);
    }
}  // namespace Horo::Editor
