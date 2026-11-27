#include "EditorSourcePresentation.h"

namespace Horo::Editor {
    namespace {
        using Presentation = Result<std::shared_ptr<const Language::LanguageSourceFocus>>;

        /** @brief Only the embedded text owner can satisfy these source commands. */
        Result<SourceOpenResult> OpenTextSource(SourceFileOpenService &sources, const SourceDocumentId &source) {
            const auto path = std::filesystem::u8path(source.Value());
            using enum SourceFileKind;
            switch (sources.Classify(path).kind) {
                case NativeSource:
                case HoroScript:
                case ProjectText:
                    return sources.Open({.path = path, .origin = SourceOpenOrigin::Command, .mode = SourceOpenMode::EmbeddedOnly});
                default:
                    return Result<SourceOpenResult>::Failure(MakeError(SourceOpenErrors::Unsupported));
            }
        }

        /** @brief Resolves the requested range against the existing or newly opened canonical text. */
        Presentation PrepareFocus(const SourceOpenResult &result, std::optional<Language::LanguageLocation> target) {
            if (!target)
                return Presentation::Success({});
            target->source = result.location.document;
            const auto selection = Language::ResolveLanguageLocation(*result.sourceSnapshot, *target);
            if (!selection)
                return Presentation::Failure(MakeError(SourceDocumentErrors::Invalid));
            return Presentation::Success(
                std::make_shared<const Language::LanguageSourceFocus>(Language::LanguageSourceFocus{*result.sourceSnapshot, *selection}));
        }

        /** @brief Allows owner presentation only while the request remains live around the host routing callback. */
        Result<void> AdmitPresentation(const SourceOpenResult &result, const std::function<bool(const SourceOpenResult &)> &navigate,
                                       const std::function<bool()> &stop) {
            if (stop())
                return Result<void>::Failure(MakeError(SourceDocumentErrors::Cancelled));
            if (navigate && !navigate(result))
                return Result<void>::Failure(MakeError(SourceOpenErrors::EditorUnavailable));
            return stop() ? Result<void>::Failure(MakeError(SourceDocumentErrors::Cancelled)) : Result<void>::Success();
        }
    }  // namespace

    /** @copydoc PresentEditorSource */
    Result<std::shared_ptr<const Language::LanguageSourceFocus>> PresentEditorSource(
        SourceFileOpenService &sources, WorkspacePanelHost &tabs, const SourceDocumentId &source,
        std::optional<Language::LanguageLocation> target, const std::function<bool(const SourceOpenResult &)> &navigate,
        const std::function<bool()> &stop) {
        if (stop())
            return Presentation::Failure(MakeError(SourceDocumentErrors::Cancelled));
        const auto opened = OpenTextSource(sources, source);
        if (opened.HasError())
            return Presentation::Failure(opened.ErrorValue());
        const auto &result = opened.Value();
        if (!result.document || !result.sourceSnapshot)
            return Presentation::Failure(MakeError(SourceOpenErrors::EditorUnavailable));

        const auto reject = [&](Error error) {
            if (result.document->disposition == DocumentOpenDisposition::Opened)
                static_cast<void>(sources.CloseDocument(result.document->identity.instance));
            return Presentation::Failure(std::move(error));
        };
        auto focus = PrepareFocus(result, std::move(target));
        if (focus.HasError())
            return reject(std::move(focus).ErrorValue());
        if (auto admitted = AdmitPresentation(result, navigate, stop); admitted.HasError())
            return reject(std::move(admitted).ErrorValue());
        const auto shown = tabs.OpenDocument(result.document->identity.key);
        if (shown.HasError())
            return reject(shown.ErrorValue());
        return focus;
    }
}  // namespace Horo::Editor
