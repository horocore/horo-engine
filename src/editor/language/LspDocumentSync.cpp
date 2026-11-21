#include "LspDocumentSync.h"

#include <limits>
#include <new>

namespace Horo::Editor::Language {
    namespace {
        /** @brief Constructs the wire name from a host URI and a validated relative source path. */
        std::string DocumentUri(const std::string_view root, const std::string_view path) {
            constexpr std::string_view Unescaped = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~/";
            constexpr std::string_view Hex = "0123456789ABCDEF";
            std::string uri{root};
            if (!uri.ends_with('/'))
                uri += '/';
            for (const char value : path) {
                if (Unescaped.find(value) != std::string_view::npos) {
                    uri += value;
                } else {
                    const auto byte = static_cast<unsigned char>(value);
                    uri += '%';
                    uri += Hex[byte >> 4U];
                    uri += Hex[byte & 0x0FU];
                }
            }
            return uri;
        }

        /** @brief Computes the exclusive UTF-16 end position of a validated UTF-8 source snapshot. */
        nlohmann::json EndPosition(const std::string_view text) {
            std::int32_t line{};
            std::int32_t column{};
            for (std::size_t i = 0; i < text.size(); ++i) {
                const auto byte = static_cast<unsigned char>(text[i]);
                if (byte == '\n' || byte == '\r') {
                    ++line;
                    column = 0;
                    if (byte == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
                        ++i;
                } else if ((byte & 0xC0U) != 0x80U) {
                    column += byte >= 0xF0U ? 2 : 1;
                }
            }
            return {{"line", line}, {"character", column}};
        }

        /** @brief Assembles one adapter-owned text document notification. */
        Result<std::string> Notify(const std::string_view method, nlohmann::json params) {
            return EncodeLspMessage({{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}});
        }

        /** @brief Sends the complete current bytes when introducing a document URI. */
        Result<std::string> Open(std::string uri, const std::string_view languageId, const SourceDocumentSnapshot &source) {
            return Notify("textDocument/didOpen", {{"textDocument",
                                                    {{"uri", std::move(uri)},
                                                     {"languageId", languageId},
                                                     {"version", source.Revision()},
                                                     {"text", source.Text()}}}});
        }

        /** @brief Uses a single replacement over the previous peer baseline for incremental servers. */
        Result<std::string> Change(std::string uri, const SourceDocumentSnapshot &source, const SourceDocumentSnapshot &previous,
                                   const LspTextSync mode) {
            nlohmann::json content{{"text", source.Text()}};
            if (mode == LspTextSync::Incremental)
                content["range"] = {{"start", {{"line", 0}, {"character", 0}}}, {"end", EndPosition(previous.Text())}};
            return Notify("textDocument/didChange", {{"textDocument", {{"uri", std::move(uri)}, {"version", source.Revision()}}},
                                                     {"contentChanges", nlohmann::json::array({std::move(content)})}});
        }

        /** @brief Accepts an absolute project file URI with no query or fragment. */
        bool ValidRoot(const std::string_view uri) noexcept {
            constexpr std::string_view FileScheme = "file://";
            return uri.starts_with(FileScheme) && uri.find('/', FileScheme.size()) != std::string_view::npos &&
                   uri.find_first_of("?#") == std::string_view::npos;
        }

        /** @brief Resolves the current and prior names once before assembling wire frames. */
        Result<LspDocumentUpdate> PrepareUpdate(const std::string_view rootUri, const std::string_view languageId,
                                                const SourceDocumentSnapshot &current,
                                                const std::optional<SourceDocumentSnapshot> &previous, const LspTextSync mode) {
            using Output = Result<LspDocumentUpdate>;
            const auto currentId = current.Identity().key.source;
            const auto previousId = previous ? previous->Identity().key.source : SourceDocumentId{};
            auto uri = DocumentUri(rootUri, currentId.Value());
            const bool replacesUri = previous && previousId != currentId;
            auto frame =
                previous && !replacesUri ? Change(std::move(uri), current, *previous, mode) : Open(std::move(uri), languageId, current);
            if (frame.HasError())
                return Output::Failure(std::move(frame).ErrorValue());
            LspDocumentUpdate update{std::nullopt, std::move(frame).Value()};
            if (replacesUri) {
                auto close = BuildLspDocumentClose(rootUri, previousId);
                if (close.HasError())
                    return Output::Failure(std::move(close).ErrorValue());
                update.close = std::move(close).Value();
            }
            return Output::Success(std::move(update));
        }
    }  // namespace

    /** @copydoc BuildLspDocumentClose */
    Result<std::string> BuildLspDocumentClose(const std::string_view rootUri, const SourceDocumentId &source) {
        if (!ValidRoot(rootUri) || !source.IsValid())
            return Result<std::string>::Failure(MakeError(SourceDocumentErrors::Invalid));
        try {
            return Notify("textDocument/didClose", {{"textDocument", {{"uri", DocumentUri(rootUri, source.Value())}}}});
        } catch (const std::bad_alloc &) {
            return Result<std::string>::Failure(MakeError(SourceDocumentErrors::TooLarge));
        }
    }

    /** @copydoc BuildLspDocumentUpdate */
    Result<LspDocumentUpdate> BuildLspDocumentUpdate(const std::string_view rootUri, const std::string_view languageId,
                                                     const SourceDocumentSnapshot &current,
                                                     const std::optional<SourceDocumentSnapshot> &previous, const LspTextSync mode) {
        using Output = Result<LspDocumentUpdate>;
        if (mode == LspTextSync::Disabled || !ValidRoot(rootUri) || languageId.empty() || languageId.size() > 128)
            return Output::Failure(MakeError(SourceDocumentErrors::Invalid));
        if (current.Revision() > std::numeric_limits<std::int32_t>::max() || current.Text().size() >= MaximumLspBodyBytes)
            return Output::Failure(MakeError(SourceDocumentErrors::TooLarge));
        try {
            return PrepareUpdate(rootUri, languageId, current, previous, mode);
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(SourceDocumentErrors::TooLarge));
        }
    }
}  // namespace Horo::Editor::Language
