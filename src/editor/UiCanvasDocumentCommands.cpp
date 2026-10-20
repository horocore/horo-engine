#include "Horo/Editor/UiCanvasDocument.h"
#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <limits>
#include <map>
#include <new>
#include <type_traits>
#include <utility>

namespace Horo::Editor {
    namespace {
        using namespace Runtime::Ui;
        constexpr std::size_t MaximumHistory = 128;

        /** @brief Builds typed failures without mutating the document owner. */
        template <typename T = void> Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        /** @brief Locates a detached authored element by stable identity. */
        auto Find(std::vector<UiDocumentElement> &elements, const UiElementId id) {
            return std::ranges::find(elements, id, &UiDocumentElement::id);
        }

        /** @brief Produces bounded parent-first order and enforces the retained-tree depth ceiling. */
        Result<std::vector<UiCanvasHierarchyRow>> Rows(const std::span<const UiDocumentElement> elements) {
            std::map<UiElementId, std::vector<std::size_t>> children;
            for (std::size_t index = 0; index < elements.size(); ++index)
                children[elements[index].parent].push_back(index);
            std::vector<std::pair<std::size_t, std::size_t>> pending;
            pending.reserve(elements.size());
            const auto append = [&](const UiElementId parent, const std::size_t depth) {
                const auto found = children.find(parent);
                if (found != children.end())
                    for (auto index = found->second.rbegin(); index != found->second.rend(); ++index)
                        pending.emplace_back(*index, depth);
            };
            append({}, 0);
            std::vector<UiCanvasHierarchyRow> rows;
            rows.reserve(elements.size());
            while (!pending.empty()) {
                const auto [index, depth] = pending.back();
                pending.pop_back();
                if (depth >= MaximumUiTreeDepth || rows.size() == elements.size())
                    return Failure<std::vector<UiCanvasHierarchyRow>>(UiErrors::DocumentHierarchyInvalid);
                const auto &element = elements[index];
                rows.push_back({element.id, element.parent, depth, element.type.Value()});
                append(element.id, depth + 1);
            }
            if (rows.size() != elements.size())
                return Failure<std::vector<UiCanvasHierarchyRow>>(UiErrors::DocumentHierarchyInvalid);
            return Result<std::vector<UiCanvasHierarchyRow>>::Success(std::move(rows));
        }

        /** @brief Rebuilds one complete document while preserving all unrelated authored domains. */
        Result<UiDocument> Rebuild(const UiDocument &source, const std::vector<UiDocumentElement> &elements) {
            if (source.Revision().Value() == std::numeric_limits<std::uint64_t>::max())
                return Failure<UiDocument>(UiCanvasDocumentErrors::StateExhausted);
            auto rows = Rows(elements);
            if (rows.HasError())
                return Result<UiDocument>::Failure(rows.ErrorValue());
            UiDocumentBuilder builder{source.Id(), UiDocumentRevision::Create(source.Revision().Value() + 1).Value(),
                                      std::ranges::any_of(elements,
                                                          [](const auto &element) {
                return element.presentation.has_value();
            })
                                          ? CurrentUiDocumentSchemaVersion
                                          : source.SchemaVersion()};
            for (const auto &canvas : source.Canvases())
                if (const auto added = builder.AddCanvas(canvas); added.HasError())
                    return Result<UiDocument>::Failure(added.ErrorValue());
            std::map<UiElementId, const UiDocumentElement *> indexed;
            for (const auto &element : elements)
                indexed.emplace(element.id, &element);
            for (const auto &row : rows.Value())
                if (const auto added = builder.AddElement(*indexed.at(row.element)); added.HasError())
                    return Result<UiDocument>::Failure(added.ErrorValue());
            for (const auto &text : source.LocalizedTexts())
                if (const auto added = builder.AddLocalizedText(text); added.HasError())
                    return Result<UiDocument>::Failure(added.ErrorValue());
            for (const auto &asset : source.LocalizedAssets())
                if (const auto added = builder.AddLocalizedAsset(asset); added.HasError())
                    return Result<UiDocument>::Failure(added.ErrorValue());
            for (const auto &dependency : source.Dependencies())
                if (const auto added = builder.RequireAsset(dependency); added.HasError())
                    return Result<UiDocument>::Failure(added.ErrorValue());
            for (const auto &route : source.Routes())
                if (const auto added = builder.AddRoute(route); added.HasError())
                    return Result<UiDocument>::Failure(added.ErrorValue());
            return std::move(builder).Build();
        }

        /** @brief Validates destination containment and resolves the stable insertion position. */
        Result<std::size_t> Destination(std::vector<UiDocumentElement> &elements, const UiElementId parent, const UiElementId before) {
            const auto found = Find(elements, parent);
            if (found == elements.end())
                return Failure<std::size_t>(UiErrors::DocumentHierarchyInvalid);
            const auto *descriptor = UiElementCatalog::Find(found->type.Value());
            if (!descriptor || !descriptor->admitsChildren)
                return Failure<std::size_t>(UiErrors::DocumentHierarchyInvalid);
            if (!before.IsValid())
                return Result<std::size_t>::Success(elements.size());
            const auto next = Find(elements, before);
            if (next == elements.end() || next->parent != parent)
                return Failure<std::size_t>(UiErrors::DocumentHierarchyInvalid);
            return Result<std::size_t>::Success(static_cast<std::size_t>(next - elements.begin()));
        }

        /** @brief Applies a creation to a detached candidate, including migration of legacy canvas-only roots. */
        Result<void> Apply(std::vector<UiDocumentElement> &elements, const UiCanvasCreateElement &command, const UiDocument &source) {
            if (elements.empty()) {
                for (const auto &canvas : source.Canvases()) {
                    auto root = CreateUiElement(UiElementKind::Canvas, canvas.rootElement, {});
                    if (root.HasError())
                        return Result<void>::Failure(root.ErrorValue());
                    elements.push_back(std::move(root).Value());
                }
            }
            if (Find(elements, command.id) != elements.end() || elements.size() >= MaximumUiDocumentElements)
                return Failure(UiErrors::DocumentDuplicateIdentity);
            auto destination = Destination(elements, command.parent, command.before);
            if (destination.HasError())
                return Result<void>::Failure(destination.ErrorValue());
            auto element = CreateUiElement(command.kind, command.id, command.parent);
            if (element.HasError())
                return Result<void>::Failure(element.ErrorValue());
            elements.insert(elements.begin() + static_cast<std::ptrdiff_t>(destination.Value()), std::move(element).Value());
            return Result<void>::Success();
        }

        /** @brief Moves a detached subtree root; complete document validation rejects cycles atomically. */
        Result<void> Apply(std::vector<UiDocumentElement> &elements, const UiCanvasMoveElement &command, const UiDocument &) {
            const auto found = Find(elements, command.element);
            if (found == elements.end() || !found->parent.IsValid() || command.before == command.element ||
                command.parent == command.element)
                return Failure(UiErrors::DocumentHierarchyInvalid);
            UiDocumentElement element = std::move(*found);
            elements.erase(found);
            auto destination = Destination(elements, command.parent, command.before);
            if (destination.HasError())
                return Result<void>::Failure(destination.ErrorValue());
            element.parent = command.parent;
            elements.insert(elements.begin() + static_cast<std::ptrdiff_t>(destination.Value()), std::move(element));
            return Result<void>::Success();
        }

        /** @brief Replaces canonical typed presentation values in a detached candidate. */
        Result<void> Apply(std::vector<UiDocumentElement> &elements, const UiCanvasSetPresentation &command, const UiDocument &) {
            const auto found = Find(elements, command.element);
            if (found == elements.end() || (command.presentation && !command.presentation->IsValid()))
                return Failure(UiCanvasDocumentErrors::InvalidDocument);
            if (!found->properties.empty())
                return Failure(UiCanvasDocumentErrors::InvalidDocument);
            found->presentation = command.presentation;
            return Result<void>::Success();
        }

        /** @brief Captures the original semantic move position without retaining document pointers. */
        std::optional<UiCanvasElementCommand> Inverse(const UiDocument &document, const UiCanvasElementCommand &command) {
            if (const auto *presentation = std::get_if<UiCanvasSetPresentation>(&command)) {
                const auto found = std::ranges::find(document.Elements(), presentation->element, &UiDocumentElement::id);
                if (found != document.Elements().end())
                    return UiCanvasElementCommand{UiCanvasSetPresentation{found->id, found->presentation}};
                return std::nullopt;
            }
            const auto *move = std::get_if<UiCanvasMoveElement>(&command);
            if (!move)
                return std::nullopt;
            const auto elements = document.Elements();
            const auto found = std::ranges::find(elements, move->element, &UiDocumentElement::id);
            if (found == elements.end())
                return std::nullopt;
            const auto next = std::find_if(found + 1, elements.end(), [&](const auto &element) {
                return element.parent == found->parent;
            });
            return UiCanvasMoveElement{found->id, found->parent, next == elements.end() ? UiElementId{} : next->id};
        }

        /** @brief Performs locale-independent ASCII folding while preserving non-ASCII UTF-8 bytes. */
        std::string Fold(std::string_view value) {
            std::string folded{value};
            for (char &character : folded)
                if (character >= 'A' && character <= 'Z')
                    character = static_cast<char>(character - 'A' + 'a');
            return folded;
        }
    }  // namespace

    /** @copydoc UiCanvasDocument::Execute */
    Result<void> UiCanvasDocument::Execute(const UiCanvasDocumentStateId expected, const UiCanvasElementCommand &command) {
        if (closed_)
            return Failure(UiCanvasDocumentErrors::Closed);
        if (expected != currentState_)
            return Failure(UiCanvasDocumentErrors::Conflict);
        if (historyCursor_ == MaximumHistory)
            return Failure(UiErrors::CapacityExceeded);
        const auto next = AdvanceState();
        if (next.HasError())
            return Result<void>::Failure(next.ErrorValue());
        try {
            std::vector<Runtime::Ui::UiDocumentElement> elements{Document().Elements().begin(), Document().Elements().end()};
            const auto applied = std::visit([&](const auto &value) {
                return Apply(elements, value, Document());
            }, command);
            if (applied.HasError())
                return applied;
            auto candidate = Rebuild(Document(), elements);
            if (candidate.HasError())
                return Result<void>::Failure(candidate.ErrorValue());
            if (std::ranges::equal(candidate.Value().Elements(), Document().Elements()))
                return Result<void>::Success();
            const auto afterSelection = std::visit([](const auto &value) {
                if constexpr (std::is_same_v<std::decay_t<decltype(value)>, UiCanvasCreateElement>)
                    return value.id;
                else
                    return value.element;
            }, command);
            HierarchyTransaction transaction{command,       Inverse(Document(), command),  contentState_,
                                             next.Value(),  Document().Elements().empty(), selection_,
                                             afterSelection};
            history_.reserve(MaximumHistory);
            history_.resize(historyCursor_);
            history_.push_back(std::move(transaction));
            ++historyCursor_;
            document_ = std::move(candidate).Value();
            currentState_ = next.Value();
            contentState_ = currentState_;
            selection_ = afterSelection;
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Failure(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiCanvasDocument::RestoreHistory */
    Result<void> UiCanvasDocument::RestoreHistory(const UiCanvasDocumentStateId expected, const bool redo) {
        if (closed_)
            return Failure(UiCanvasDocumentErrors::Closed);
        if (expected != currentState_)
            return Failure(UiCanvasDocumentErrors::Conflict);
        if ((redo && historyCursor_ == history_.size()) || (!redo && historyCursor_ == 0))
            return Failure(UiCanvasDocumentErrors::InvalidDocument);
        const auto next = AdvanceState();
        if (next.HasError())
            return Result<void>::Failure(next.ErrorValue());
        try {
            const auto &transaction = history_[redo ? historyCursor_ : historyCursor_ - 1];
            std::vector<UiDocumentElement> elements{Document().Elements().begin(), Document().Elements().end()};
            Result<void> applied = Result<void>::Success();
            if (redo)
                applied = std::visit([&](const auto &value) {
                    return Apply(elements, value, Document());
                }, transaction.command);
            else if (transaction.inverse)
                applied = std::visit([&](const auto &value) {
                    return Apply(elements, value, Document());
                }, *transaction.inverse);
            else {
                const auto created = std::get<UiCanvasCreateElement>(transaction.command).id;
                std::erase_if(elements, [created](const auto &element) {
                    return element.id == created;
                });
                if (transaction.materializedRoots)
                    elements.clear();
            }
            if (applied.HasError())
                return applied;
            auto candidate = Rebuild(Document(), elements);
            if (candidate.HasError())
                return Result<void>::Failure(candidate.ErrorValue());
            document_ = std::move(candidate).Value();
            currentState_ = next.Value();
            contentState_ = redo ? transaction.afterContent : transaction.beforeContent;
            selection_ = redo ? transaction.afterSelection : transaction.beforeSelection;
            if (redo)
                ++historyCursor_;
            else
                --historyCursor_;
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Failure(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiCanvasDocument::Undo */
    Result<void> UiCanvasDocument::Undo(const UiCanvasDocumentStateId expected) {
        return RestoreHistory(expected, false);
    }

    /** @copydoc UiCanvasDocument::Redo */
    Result<void> UiCanvasDocument::Redo(const UiCanvasDocumentStateId expected) {
        return RestoreHistory(expected, true);
    }

    /** @copydoc UiCanvasDocument::Hierarchy */
    Result<std::vector<UiCanvasHierarchyRow>> UiCanvasDocument::Hierarchy(const std::string_view search) const {
        if (closed_)
            return Failure<std::vector<UiCanvasHierarchyRow>>(UiCanvasDocumentErrors::Closed);
        if (search.size() > 256 || !IsValidUtf8ScalarSequence(search))
            return Failure<std::vector<UiCanvasHierarchyRow>>(UiCanvasDocumentErrors::InvalidDocument);
        try {
            std::vector<UiDocumentElement> legacyRoots;
            if (Document().Elements().empty())
                for (const auto &canvas : Document().Canvases())
                    legacyRoots.push_back(CreateUiElement(UiElementKind::Canvas, canvas.rootElement, {}).Value());
            auto rows = Rows(legacyRoots.empty() ? Document().Elements() : std::span<const UiDocumentElement>{legacyRoots});
            if (rows.HasError() || search.empty())
                return rows;
            const auto query = Fold(search);
            std::vector<bool> visible(rows.Value().size());
            std::vector<std::size_t> ancestors;
            ancestors.reserve(MaximumUiTreeDepth);
            for (std::size_t index = 0; index < rows.Value().size(); ++index) {
                const auto &row = rows.Value()[index];
                ancestors.resize(row.depth);
                if (Fold(row.label).find(query) != std::string::npos) {
                    visible[index] = true;
                    for (const auto ancestor : ancestors)
                        visible[ancestor] = true;
                }
                ancestors.push_back(index);
            }
            std::vector<UiCanvasHierarchyRow> filtered;
            for (std::size_t index = 0; index < rows.Value().size(); ++index)
                if (visible[index])
                    filtered.push_back(std::move(rows.Value()[index]));
            return Result<std::vector<UiCanvasHierarchyRow>>::Success(std::move(filtered));
        } catch (const std::bad_alloc &) {
            return Failure<std::vector<UiCanvasHierarchyRow>>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiCanvasDocument::Select */
    Result<void> UiCanvasDocument::Select(const UiCanvasDocumentStateId expected, const UiElementId element) {
        if (closed_)
            return Failure(UiCanvasDocumentErrors::Closed);
        if (expected != currentState_)
            return Failure(UiCanvasDocumentErrors::Conflict);
        const bool legacyRoot = Document().Elements().empty() && std::ranges::any_of(Document().Canvases(), [element](const auto &canvas) {
            return canvas.rootElement == element;
        });
        if (element.IsValid() && !legacyRoot &&
            std::ranges::find(Document().Elements(), element, &UiDocumentElement::id) == Document().Elements().end())
            return Failure(UiErrors::DocumentHierarchyInvalid);
        selection_ = element;
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
