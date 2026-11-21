#include "LspRequests.h"

#include "LspDocumentSync.h"

#include <array>
#include <charconv>
#include <new>

namespace Horo::Editor::Language {
    namespace {
        enum class ParameterShape {
            Document,
            Cursor,
            References,
            Selection
        };

        /** @brief The supported methods and the matching server capability and request shape. */
        struct RequestDescriptor final {
            std::string_view method;
            std::string_view capability;
            ParameterShape shape;
        };

        constexpr std::array<RequestDescriptor, LspFeatureCount> Requests{{
            {"textDocument/diagnostic", "diagnosticProvider", ParameterShape::Document},
            {"textDocument/completion", "completionProvider", ParameterShape::Cursor},
            {"textDocument/hover", "hoverProvider", ParameterShape::Cursor},
            {"textDocument/definition", "definitionProvider", ParameterShape::Cursor},
            {"textDocument/references", "referencesProvider", ParameterShape::References},
            {"textDocument/documentSymbol", "documentSymbolProvider", ParameterShape::Document},
            {"textDocument/codeAction", "codeActionProvider", ParameterShape::Selection},
        }};

        /** @brief Adds the selection and required context for a code-action request. */
        bool AddSelection(nlohmann::json &params, const std::string_view text, const LspSourceQuery query, nlohmann::json start) {
            if (query.length > text.size() - query.offset)
                return false;
            auto end = LspPositionAt(text, query.offset + query.length);
            if (!end)
                return false;
            params["range"] = {{"start", std::move(start)}, {"end", std::move(*end)}};
            params["context"] = {{"diagnostics", nlohmann::json::array()}};
            return true;
        }

        /** @brief Adds method-specific parameters without letting wire JSON choose a method. */
        bool AddQuery(nlohmann::json &params, const std::string_view text, const ParameterShape shape, const LspSourceQuery query) {
            if (shape == ParameterShape::Document)
                return true;
            auto position = LspPositionAt(text, query.offset);
            if (!position)
                return false;
            if (shape == ParameterShape::Selection)
                return AddSelection(params, text, query, std::move(*position));
            params["position"] = std::move(*position);
            if (shape == ParameterShape::References)
                params["context"] = {{"includeDeclaration", query.includeDeclaration}};
            return true;
        }
    }  // namespace

    /** @copydoc ReadLspFeatures */
    LspFeatures ReadLspFeatures(const nlohmann::json &capabilities) {
        LspFeatures result;
        for (std::size_t index = 0; index < Requests.size(); ++index) {
            const auto value = capabilities.find(Requests[index].capability);
            result[index] = value != capabilities.end() && (value->is_object() || *value == true);
        }
        return result;
    }

    /** @copydoc SupportsLspRequest */
    bool SupportsLspRequest(const LspFeatures &features, const SourceLanguageRequestKind kind) noexcept {
        const auto index = static_cast<std::size_t>(kind);
        return index < features.size() && features[index];
    }

    /** @copydoc BuildLspRequest */
    Result<std::string> BuildLspRequest(const std::string_view rootUri, const SourceDocumentSnapshot &source,
                                        const SourceLanguageRequestKind kind, const std::uint64_t id, const LspSourceQuery query) {
        const auto index = static_cast<std::size_t>(kind);
        if (index >= Requests.size())
            return Result<std::string>::Failure(MakeError(SourceDocumentErrors::Invalid));
        try {
            const auto &descriptor = Requests[index];
            nlohmann::json params{{"textDocument", {{"uri", DocumentUri(rootUri, source.Identity().key.source.Value())}}}};
            if (!AddQuery(params, source.Text(), descriptor.shape, query))
                return Result<std::string>::Failure(MakeError(SourceDocumentErrors::Invalid));
            return EncodeLspMessage(
                {{"jsonrpc", "2.0"}, {"id", std::to_string(id)}, {"method", descriptor.method}, {"params", std::move(params)}});
        } catch (const std::bad_alloc &) {
            return Result<std::string>::Failure(MakeError(SourceDocumentErrors::TooLarge));
        }
    }

    /** @copydoc BuildLspCancellation */
    Result<std::string> BuildLspCancellation(const std::uint64_t id) {
        try {
            return EncodeLspMessage({{"jsonrpc", "2.0"}, {"method", "$/cancelRequest"}, {"params", {{"id", std::to_string(id)}}}});
        } catch (const std::bad_alloc &) {
            return Result<std::string>::Failure(MakeError(SourceDocumentErrors::TooLarge));
        }
    }

    /** @copydoc ReadLspRequestId */
    std::optional<std::uint64_t> ReadLspRequestId(const LspMessage &message) {
        const auto &value = message.envelope.Get().at("id");
        if (!value.is_string())
            return std::nullopt;
        const auto &id = value.get_ref<const std::string &>();
        if (id.empty() || id.front() == '0')
            return std::nullopt;
        std::uint64_t result{};
        const auto end = id.data() + id.size();
        const auto parsed = std::from_chars(id.data(), end, result);
        if (parsed.ec != std::errc{} || parsed.ptr != end)
            return std::nullopt;
        return result;
    }
}  // namespace Horo::Editor::Language
