#include "LspResultProjection.h"

#include "Horo/Foundation/Utf8.h"
#include "LspConnection.h"
#include "LspDocumentSync.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Editor::Language {
    namespace {
        using Json = nlohmann::json;
        constexpr std::size_t MaximumItems = 512;
        constexpr std::size_t MaximumTextBytes = 64 * 1024;
        constexpr std::size_t MaximumPositionWork = 8 * 1024 * 1024;

        /** @brief Borrows an object field without throwing on malformed optional payloads. */
        const Json &Field(const Json &object, const std::string_view name) {
            static const Json Missing;
            const auto found = object.find(name);
            return found == object.end() ? Missing : *found;
        }

        /** @brief Bounds individual user-visible or edit strings and rejects embedded NULs. */
        std::optional<std::string_view> Text(const Json &value) {
            if (!value.is_string())
                return std::nullopt;
            const auto &text = value.get_ref<const std::string &>();
            if (text.size() > MaximumTextBytes || text.find('\0') != std::string::npos)
                return std::nullopt;
            return text;
        }

        /** @brief Reads a JSON integer without truncating wide numeric inputs. */
        std::optional<std::int32_t> Integer(const Json &value) {
            constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
            if (value.is_number_unsigned()) {
                const auto number = value.get<std::uint64_t>();
                if (number <= maximum)
                    return static_cast<std::int32_t>(number);
            } else if (value.is_number_integer()) {
                const auto number = value.get<std::int64_t>();
                if (number >= std::numeric_limits<std::int32_t>::min() && number <= maximum)
                    return static_cast<std::int32_t>(number);
            }
            return std::nullopt;
        }

        /** @brief Parses a protocol position with nonnegative signed-32-bit components. */
        std::optional<LanguagePosition> Position(const Json &value) {
            const auto line = Integer(Field(value, "line"));
            const auto character = Integer(Field(value, "character"));
            if (!line || !character || *line < 0 || *character < 0)
                return std::nullopt;
            return LanguagePosition{static_cast<std::uint32_t>(*line), static_cast<std::uint32_t>(*character)};
        }

        /** @brief Parses an ordered protocol selection before looking up any byte offsets. */
        std::optional<std::pair<LanguagePosition, LanguagePosition>> Coordinates(const Json &value) {
            const auto start = Position(Field(value, "start"));
            const auto end = Position(Field(value, "end"));
            if (!start || !end || *end < *start)
                return std::nullopt;
            return std::pair{*start, *end};
        }

        /** @brief Indexes line starts once and bounds aggregate work on pathologically long lines. */
        class SourceCoordinates final {
        public:
            explicit SourceCoordinates(const std::string_view text) : text_(text) {}

            /** @brief Resolves a protocol range without splitting a surrogate pair or CRLF. */
            std::optional<LanguageByteSpan> Span(const Json &value) {
                const auto range = Coordinates(value);
                if (!range)
                    return std::nullopt;
                return Span(range->first, range->second);
            }

            /** @brief Projects an ordered range supplied by a typed language result. */
            std::optional<LanguageByteSpan> Span(const LanguagePosition first, const LanguagePosition last) {
                if (last < first)
                    return std::nullopt;
                if (lines_.empty())
                    IndexLines();
                const auto start = Offset(first);
                const auto end = Offset(last);
                if (!start || !end)
                    return std::nullopt;
                return LanguageByteSpan{*start, *end};
            }

        private:
            /** @brief Handles each supported newline and keeps a final empty line addressable. */
            void IndexLines() {
                lines_.push_back(0);
                for (std::size_t index = 0; index < text_.size(); ++index) {
                    const auto byte = text_[index];
                    if (byte != '\r' && byte != '\n')
                        continue;
                    if (byte == '\r' && index + 1 < text_.size() && text_[index + 1] == '\n')
                        ++index;
                    lines_.push_back(static_cast<std::uint32_t>(index + 1));
                }
            }

            /** @brief Walks past UTF-8 continuation bytes after a checked scalar lead. */
            std::size_t SkipContinuation(std::size_t offset) const noexcept {
                while (offset < text_.size() && (static_cast<unsigned char>(text_[offset]) & 0xC0U) == 0x80U)
                    ++offset;
                return offset;
            }

            /** @brief Walks one bounded line until the requested UTF-16 boundary is found. */
            std::optional<std::size_t> Offset(const LanguagePosition value) {
                if (value.line >= lines_.size())
                    return std::nullopt;
                std::size_t offset = lines_[value.line];
                std::uint32_t column = 0;
                while (column < value.utf16Column) {
                    if (offset == text_.size() || text_[offset] == '\r' || text_[offset] == '\n' || remaining_ == 0)
                        return std::nullopt;
                    --remaining_;
                    const auto byte = static_cast<unsigned char>(text_[offset]);
                    column += byte >= 0xF0U ? 2U : 1U;
                    offset = SkipContinuation(offset + 1);
                }
                if (column != value.utf16Column)
                    return std::nullopt;
                return offset;
            }

            std::string_view text_;
            std::vector<std::uint32_t> lines_;
            std::size_t remaining_{MaximumPositionWork};
        };

        /** @brief Applies a strict per-result list budget before accumulating typed items. */
        template <typename T, typename Project> std::optional<std::vector<T>> Items(const Json &value, Project project) {
            if (!value.is_array() || value.size() > MaximumItems)
                return std::nullopt;
            std::vector<T> output;
            output.reserve(value.size());
            for (const auto &item : value) {
                auto mapped = project(item);
                if (!mapped)
                    return std::nullopt;
                output.push_back(std::move(*mapped));
            }
            return output;
        }

        /** @brief Maps diagnostics onto current source bytes for either protocol delivery mode. */
        std::optional<LanguagePayload> ProjectDiagnosticItems(const Json &value, SourceCoordinates &coordinates) {
            auto items = Items<LanguageDiagnostic>(value, [&](const Json &item) -> std::optional<LanguageDiagnostic> {
                const auto message = Text(Field(item, "message"));
                const auto range = coordinates.Span(Field(item, "range"));
                const auto severity = Integer(Field(item, "severity")).value_or(0);
                if (!message || !range || severity < 0 || severity > static_cast<int>(LanguageSeverity::Hint))
                    return std::nullopt;
                return LanguageDiagnostic{*range, static_cast<LanguageSeverity>(severity), std::string{*message}};
            });
            if (!items)
                return std::nullopt;
            return LanguageDiagnostics{std::move(*items)};
        }

        /** @brief Maps a complete pull response using the same model as push diagnostics. */
        std::optional<LanguagePayload> ProjectDiagnosticReport(const Json &value, SourceCoordinates &coordinates) {
            if (Field(value, "kind") != "full")
                return std::nullopt;
            return ProjectDiagnosticItems(Field(value, "items"), coordinates);
        }

        /** @brief Appends plain or marked hover text as inert display content. */
        bool AppendHover(std::string &text, const Json &item) {
            const auto value = Text(item.is_object() ? Field(item, "value") : item);
            if (!value)
                return false;
            if (!text.empty())
                text += '\n';
            if (value->size() > MaximumTextBytes - std::min(text.size(), MaximumTextBytes))
                return false;
            text += *value;
            return text.size() <= MaximumTextBytes;
        }

        /** @brief Converts LSP hover formats and resolves the optional source range. */
        std::optional<LanguagePayload> ProjectHover(const Json &value, SourceCoordinates &coordinates) {
            LanguageHover output;
            const auto &contents = Field(value, "contents");
            if (contents.is_array()) {
                if (contents.size() > MaximumItems || !std::ranges::all_of(contents, [&](const Json &item) {
                    return AppendHover(output.text, item);
                }))
                    return std::nullopt;
            } else if (!AppendHover(output.text, contents)) {
                return std::nullopt;
            }
            if (value.contains("range")) {
                output.span = coordinates.Span(Field(value, "range"));
                if (!output.span)
                    return std::nullopt;
            }
            return output;
        }

        /** @brief Validates an inert source edit against the exact lease, including BOM preservation. */
        std::optional<LanguageTextEdit> Edit(const Json &value, SourceCoordinates &coordinates, const bool preserveBom) {
            const auto replacement = Text(Field(value, "newText"));
            const auto range = coordinates.Span(Field(value, "range"));
            if (!replacement || !range || (preserveBom && range->start < 3))
                return std::nullopt;
            return LanguageTextEdit{*range, std::string{*replacement}};
        }

        /** @brief Rejects overlapping byte edits, preserving server order at equal insertion points. */
        bool OrderedEdits(std::vector<LanguageTextEdit> &edits) {
            std::ranges::stable_sort(edits, {}, &LanguageTextEdit::span);
            return std::ranges::adjacent_find(edits, [](const auto &left, const auto &right) {
                return left.span.end > right.span.start;
            }) == edits.end();
        }

        /** @brief Contiguous replacement and caret derived from disjoint source edits. */
        struct CombinedSourceEdit final {
            LanguageByteSpan span;
            std::string text;
            std::size_t caret{};
        };

        /** @brief Preserves unchanged bytes and puts the caret after the primary edit or the final edit. */
        std::optional<CombinedSourceEdit> CombineLanguageEdits(const std::string_view source, std::span<const LanguageTextEdit> edits,
                                                               const std::optional<LanguageByteSpan> primary) {
            CombinedSourceEdit combined{.span = {edits.front().span.start, edits.back().span.end}};
            auto cursor = combined.span.start;
            bool foundPrimary = false;
            for (const auto &edit : edits) {
                if (edit.span.start > edit.span.end || edit.span.end > source.size())
                    return std::nullopt;
                combined.text.append(source.substr(cursor, edit.span.start - cursor));
                combined.text.append(edit.replacement);
                if (primary && edit.span == *primary) {
                    if (foundPrimary)
                        return std::nullopt;
                    foundPrimary = true;
                    combined.caret = combined.span.start + combined.text.size();
                }
                cursor = edit.span.end;
            }
            if (primary && !foundPrimary)
                return std::nullopt;
            if (!primary)
                combined.caret = combined.span.start + combined.text.size();
            return combined;
        }

        /** @brief Plans disjoint replacements through the existing single-edit history contract. */
        Result<SourceEditingPlan> PlanLanguageEdits(const SourceDocumentSnapshot &source, const SourceEditingView &view,
                                                    std::vector<LanguageTextEdit> edits, std::optional<LanguageByteSpan> primary) {
            using Output = Result<SourceEditingPlan>;
            if (edits.empty() || !OrderedEdits(edits))
                return Output::Failure(MakeError(SourceDocumentErrors::Invalid));
            const auto replacement = CombineLanguageEdits(source.Text(), edits, primary);
            if (!replacement)
                return Output::Failure(MakeError(SourceDocumentErrors::Invalid));
            auto range = view;
            range.anchor = replacement->span.start;
            range.caret = replacement->span.end;
            auto plan = PlanSourceReplacement(source, range, replacement->text);
            if (plan.HasError())
                return plan;
            auto resolved = std::move(plan).Value();
            resolved.before = view;
            resolved.after.anchor = replacement->caret;
            resolved.after.caret = replacement->caret;
            return Output::Success(std::move(resolved));
        }

        /** @brief Maps one completion item without granting command or snippet execution. */
        std::optional<LanguageCompletionItem> CompletionItem(const Json &value, SourceCoordinates &coordinates, const bool preserveBom) {
            const auto label = Text(Field(value, "label"));
            if (!label || label->empty() || Field(value, "insertTextFormat") == 2)
                return std::nullopt;
            LanguageCompletionItem result{std::string{*label},
                                          std::string{Text(Field(value, "detail")).value_or("")},
                                          std::string{Text(Field(value, "insertText")).value_or(*label)},
                                          {}};
            if (value.contains("additionalTextEdits")) {
                auto additional = Items<LanguageTextEdit>(Field(value, "additionalTextEdits"), [&](const Json &entry) {
                    return Edit(entry, coordinates, preserveBom);
                });
                if (!additional)
                    return std::nullopt;
                result.edits = std::move(*additional);
            }
            if (value.contains("textEdit")) {
                auto main = Edit(Field(value, "textEdit"), coordinates, preserveBom);
                if (!main)
                    return std::nullopt;
                result.primaryEdit = main->span;
                result.edits.push_back(std::move(*main));
            }
            if (!OrderedEdits(result.edits))
                return std::nullopt;
            return result;
        }

        /** @brief Accepts either an item array or a completion list. */
        std::optional<LanguagePayload> Completions(const Json &value, SourceCoordinates &coordinates, const bool preserveBom) {
            auto items = Items<LanguageCompletionItem>(value.is_array() ? value : Field(value, "items"), [&](const Json &item) {
                return CompletionItem(item, coordinates, preserveBom);
            });
            if (!items)
                return std::nullopt;
            return LanguageCompletions{std::move(*items), Field(value, "isIncomplete") == true};
        }

        /** @brief Converts ASCII URI hex without consulting the locale. */
        int Hex(const char byte) noexcept {
            constexpr std::string_view Lower = "0123456789abcdef";
            constexpr std::string_view Upper = "0123456789ABCDEF";
            const auto index = Lower.find(byte);
            if (index != std::string_view::npos)
                return static_cast<int>(index);
            const auto upper = Upper.find(byte);
            return upper == std::string_view::npos ? -1 : static_cast<int>(upper);
        }

        /** @brief Decodes only the relative URI suffix, turning escaped separators into normal validation input. */
        std::optional<std::string> DecodePath(const std::string_view uri) {
            std::string path;
            path.reserve(uri.size());
            for (std::size_t index = 0; index < uri.size(); ++index) {
                if (uri[index] != '%') {
                    path += uri[index];
                    continue;
                }
                if (uri.size() - index < 3)
                    return std::nullopt;
                const int high = Hex(uri[index + 1]);
                const int low = Hex(uri[index + 2]);
                if (high < 0 || low < 0)
                    return std::nullopt;
                path += static_cast<char>(high * 16 + low);
                index += 2;
            }
            return path;
        }

        /** @brief Makes out-of-project destinations inert before any native access is considered. */
        std::optional<SourceDocumentId> ProjectSource(const std::string_view root, const std::string_view uri) {
            const auto prefix = DocumentUri(root, {});
            if (root.empty() || !uri.starts_with(prefix) || uri.find_first_of("?#") != std::string_view::npos)
                return std::nullopt;
            const auto path = DecodePath(uri.substr(prefix.size()));
            if (!path || !IsValidUtf8ScalarSequence(*path))
                return std::nullopt;
            auto id = SourceDocumentId::Parse(*path);
            if (id.HasError())
                return std::nullopt;
            return std::move(id).Value();
        }

        /** @brief Resolves structurally valid in-project Location and LocationLink results. */
        std::optional<LanguageLocation> Location(const Json &value, const std::string_view root) {
            const bool link = value.contains("targetUri");
            const auto uri = Text(Field(value, link ? "targetUri" : "uri"));
            const auto range = Coordinates(Field(value, link ? "targetSelectionRange" : "range"));
            if (!uri || !range)
                return std::nullopt;
            auto source = ProjectSource(root, *uri);
            if (!source)
                return std::nullopt;
            return LanguageLocation{std::move(*source), range->first, range->second};
        }

        /** @brief Maps definition or references without opening or trusting URI destinations. */
        std::optional<LanguagePayload> Navigation(const Json &value, const std::string_view root) {
            LanguageNavigation result;
            if (value.is_object()) {
                auto one = Location(value, root);
                if (one)
                    result.targets.push_back(std::move(*one));
                return result;
            }
            if (!value.is_array() || value.size() > MaximumItems)
                return std::nullopt;
            for (const auto &entry : value) {
                auto target = Location(entry, root);
                if (target)
                    result.targets.push_back(std::move(*target));
            }
            return result;
        }

        /** @brief Resolves a symbol selection inside the current source. */
        std::optional<LanguageSymbol> Symbol(const Json &item, SourceCoordinates &coordinates, const std::string_view uri) {
            const auto name = Text(Field(item, "name"));
            const auto &location = Field(item, "location");
            if (location.is_object() && Text(Field(location, "uri")).value_or("") != uri)
                return std::nullopt;
            const auto selection = coordinates.Span(location.is_object() ? Field(location, "range") : Field(item, "selectionRange"));
            if (!name || !selection)
                return std::nullopt;
            return LanguageSymbol{std::string{*name}, *selection};
        }

        /** @brief Flattens hierarchical symbols while sharing one bound for the entire tree. */
        bool AppendSymbols(LanguageSymbols &output, const Json &value, SourceCoordinates &coordinates, const std::string_view uri) {
            if (!value.is_array() || value.size() > MaximumItems - output.items.size())
                return false;
            for (const auto &item : value) {
                if (output.items.size() == MaximumItems)
                    return false;
                auto symbol = Symbol(item, coordinates, uri);
                if (!symbol)
                    return false;
                output.items.push_back(std::move(*symbol));
                if (item.contains("children") && !AppendSymbols(output, Field(item, "children"), coordinates, uri))
                    return false;
            }
            return true;
        }

        /** @brief Resolves a versioned single-document edit, excluding file operations. */
        const Json *VersionedEdits(const Json &changes, const std::string_view uri, const std::int32_t wireVersion) {
            if (!changes.is_array() || changes.size() != 1)
                return nullptr;
            const auto &change = changes.front();
            const auto &document = Field(change, "textDocument");
            const auto version = Integer(Field(document, "version"));
            if (Text(Field(document, "uri")).value_or("") != uri || !version || *version != wireVersion)
                return nullptr;
            return &Field(change, "edits");
        }

        /** @brief Resolves a single current-document workspace edit without falling back from a stale version. */
        const Json *WorkspaceEdits(const Json &value, const std::string_view uri, const std::int32_t wireVersion) {
            if (value.contains("documentChanges"))
                return VersionedEdits(Field(value, "documentChanges"), uri, wireVersion);
            const auto &changes = Field(value, "changes");
            if (!changes.is_object() || changes.size() != 1)
                return nullptr;
            const auto matching = changes.find(uri);
            return matching == changes.end() ? nullptr : &*matching;
        }

        /** @brief Projects code action titles and current-document edit proposals without executing commands. */
        std::optional<LanguagePayload> ProjectCodeActions(const Json &value, SourceCoordinates &coordinates,
                                                          const SourceDocumentSnapshot &source, const std::string_view uri,
                                                          const std::int32_t wireVersion) {
            auto items = Items<LanguageCodeAction>(value, [&](const Json &entry) -> std::optional<LanguageCodeAction> {
                const auto title = Text(Field(entry, "title"));
                if (!title)
                    return std::nullopt;
                LanguageCodeAction result{std::string{*title}, {}};
                if (entry.contains("disabled"))
                    return result;
                const auto &edit = Field(entry, "edit");
                if (const auto *list = WorkspaceEdits(edit, uri, wireVersion)) {
                    auto edits = Items<LanguageTextEdit>(*list, [&](const Json &item) {
                        return Edit(item, coordinates, source.Metadata().utf8Bom);
                    });
                    if (!edits || !OrderedEdits(*edits))
                        return std::nullopt;
                    result.edits = std::move(*edits);
                }
                return result;
            });
            if (!items)
                return std::nullopt;
            return LanguageCodeActions{std::move(*items)};
        }

        /** @brief Dispatches validated JSON to the correlated feature's model. */
        std::optional<LanguagePayload> Map(const Json &value, const SourceLanguageReply &reply, const std::string_view root,
                                           const std::int32_t wireVersion) {
            if (value.is_null())
                return std::monostate{};
            using enum SourceLanguageRequestKind;
            SourceCoordinates coordinates{reply.source.Text()};
            switch (reply.kind) {
                case Diagnostics:
                    return ProjectDiagnosticReport(value, coordinates);
                case Hover:
                    return ProjectHover(value, coordinates);
                case Completion:
                    return Completions(value, coordinates, reply.source.Metadata().utf8Bom);
                case Definition:
                case References:
                    return Navigation(value, root);
                case Symbols: {
                    LanguageSymbols symbols;
                    if (!AppendSymbols(symbols, value, coordinates, DocumentUri(root, reply.source.Identity().key.source.Value())))
                        return std::nullopt;
                    return symbols;
                }
                case CodeActions:
                    return ProjectCodeActions(value, coordinates, reply.source,
                                              DocumentUri(root, reply.source.Identity().key.source.Value()), wireVersion);
            }
            return std::nullopt;
        }

        /** @brief Keeps a coded server failure as data, separate from a successful feature payload. */
        std::optional<LanguagePayload> MapError(const Json &value) {
            const auto code = Integer(Field(value, "code"));
            const auto message = Text(Field(value, "message"));
            if (!code || !message)
                return std::nullopt;
            return LanguageServerFailure{*code, std::string{*message}};
        }

        /** @brief Projects a diagnostic publication already routed by the connection. */
        std::optional<LanguagePayload> MapPublication(const LspIncoming &incoming) {
            const auto &envelope = incoming.message.envelope.Get();
            const auto &params = Field(envelope, "params");
            if (incoming.reply->kind != SourceLanguageRequestKind::Diagnostics ||
                Text(Field(envelope, "method")).value_or("") != PublishDiagnosticsMethod ||
                Integer(Field(params, "version")) != incoming.wireVersion)
                return std::nullopt;
            SourceCoordinates coordinates{incoming.reply->source.Text()};
            return ProjectDiagnosticItems(Field(params, "diagnostics"), coordinates);
        }

        /** @brief Dispatches responses and revision-bound notifications through one projection boundary. */
        std::optional<LanguagePayload> ProjectEnvelope(const LspIncoming &incoming, const std::string_view rootUri) {
            const auto &envelope = incoming.message.envelope.Get();
            switch (incoming.message.kind) {
                case LspMessageKind::Notification:
                    return MapPublication(incoming);
                case LspMessageKind::Error:
                    return MapError(Field(envelope, "error"));
                case LspMessageKind::Result:
                    return Map(Field(envelope, "result"), *incoming.reply, rootUri, incoming.wireVersion);
                case LspMessageKind::Request:
                    return std::nullopt;
            }
            return std::nullopt;
        }
    }  // namespace

    /** @copydoc PlanLanguageCompletion */
    Result<SourceEditingPlan> PlanLanguageCompletion(const SourceDocumentSnapshot &source, const SourceEditingView &view,
                                                     const LanguageCompletionItem &item) {
        using Output = Result<SourceEditingPlan>;
        if (!source.IsCurrent())
            return Output::Failure(MakeError(SourceDocumentErrors::Stale));
        if (item.edits.empty() && !item.primaryEdit)
            return PlanSourceReplacement(source, view, item.insertText);

        auto edits = item.edits;
        const auto [start, end] = std::minmax(view.anchor, view.caret);
        const auto primary = item.primaryEdit.value_or(LanguageByteSpan{start, end});
        if (!item.primaryEdit)
            edits.push_back({primary, item.insertText});
        return PlanLanguageEdits(source, view, std::move(edits), primary);
    }

    /** @copydoc PlanLanguageCodeAction */
    Result<SourceEditingPlan> PlanLanguageCodeAction(const SourceDocumentSnapshot &source, const SourceEditingView &view,
                                                     const LanguageCodeAction &action) {
        if (!source.IsCurrent())
            return Result<SourceEditingPlan>::Failure(MakeError(SourceDocumentErrors::Stale));
        try {
            return PlanLanguageEdits(source, view, action.edits, std::nullopt);
        } catch (const std::bad_alloc &) {
            return Result<SourceEditingPlan>::Failure(MakeError(SourceDocumentErrors::TooLarge));
        }
    }

    /** @copydoc ResolveLanguageLocation */
    std::optional<LanguageByteSpan> ResolveLanguageLocation(const SourceDocumentSnapshot &source, const LanguageLocation &location) {
        if (!source.IsCurrent() || source.Identity().key.source != location.source || source.Text().size() >= MaximumLspBodyBytes)
            return std::nullopt;
        try {
            auto bytes = SourceCoordinates{source.Text()}.Span(location.start, location.end);
            if (bytes && source.Metadata().utf8Bom && bytes->start < 3)
                return std::nullopt;
            return bytes;
        } catch (const std::bad_alloc &) {
            return std::nullopt;
        }
    }

    /** @copydoc ProjectLspReply */
    Result<LanguageResult> ProjectLspReply(const LspIncoming &incoming, const std::string_view rootUri) {
        using Output = Result<LanguageResult>;
        if (!incoming.reply || !incoming.reply->source.IsCurrent())
            return Output::Failure(MakeError(SourceDocumentErrors::Stale));
        if (incoming.wireVersion <= 0)
            return Output::Failure(MakeError(SourceDocumentErrors::Stale));
        if (incoming.reply->source.Text().size() >= MaximumLspBodyBytes)
            return Output::Failure(MakeError(SourceDocumentErrors::TooLarge));
        try {
            auto payload = ProjectEnvelope(incoming, rootUri);
            if (!payload)
                return Output::Failure(MakeError(SourceDocumentErrors::Invalid));
            return Output::Success({*incoming.reply, std::move(*payload)});
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(SourceDocumentErrors::TooLarge));
        }
    }

    /** @copydoc ReadLspDiagnosticTarget */
    std::optional<LspDiagnosticTarget> ReadLspDiagnosticTarget(const LspMessage &message, const std::string_view rootUri) {
        try {
            const auto &params = Field(message.envelope.Get(), "params");
            const auto version = Integer(Field(params, "version"));
            const auto uri = Text(Field(params, "uri"));
            if (!version || *version <= 0 || !uri)
                return std::nullopt;
            auto source = ProjectSource(rootUri, *uri);
            if (!source)
                return std::nullopt;
            return LspDiagnosticTarget{std::move(*source), *version};
        } catch (const std::bad_alloc &) {
            return std::nullopt;
        }
    }
}  // namespace Horo::Editor::Language
