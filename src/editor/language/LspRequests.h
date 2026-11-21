#pragma once

/** @file LspRequests.h @brief Document request and cancellation encoding for the supported LSP features. */

#include "Horo/Editor/SourceLanguageSession.h"
#include "LspProtocol.h"

#include <bitset>

namespace Horo::Editor::Language {
    /** @brief Position or selection in the exact immutable source snapshot being queried. */
    struct LspSourceQuery final {
        std::size_t offset{};
        std::size_t length{};      /**< Optional selection extent for code actions. */
        bool includeDeclaration{}; /**< Reference lookup option. */
    };

    inline constexpr auto LspFeatureCount = static_cast<std::size_t>(SourceLanguageRequestKind::CodeActions) + 1;
    using LspFeatures = std::bitset<LspFeatureCount>;

    /** @brief Inspects server capabilities for the document requests implemented by this adapter. */
    [[nodiscard]] LspFeatures ReadLspFeatures(const nlohmann::json &capabilities);

    /** @brief Checks whether the negotiated features support a particular document request. */
    [[nodiscard]] bool SupportsLspRequest(const LspFeatures &features, SourceLanguageRequestKind kind) noexcept;

    /** @brief Encodes a bound document request and validates all requested byte positions.
     * @param rootUri Validated file root. @param source Current synchronized snapshot.
     * @param kind Requested feature. @param id Local correlation number. @param query Snapshot selection.
     * @return Wire frame or a refusal for an invalid position or unsupported request.
     */
    [[nodiscard]] Result<std::string> BuildLspRequest(std::string_view rootUri, const SourceDocumentSnapshot &source,
                                                      SourceLanguageRequestKind kind, std::uint64_t id, LspSourceQuery query);

    /** @brief Encodes cancellation using the same wire identity as the original request. */
    [[nodiscard]] Result<std::string> BuildLspCancellation(std::uint64_t id);

    /** @brief Recovers the string identity assigned to a document reply. */
    [[nodiscard]] std::optional<std::uint64_t> ReadLspRequestId(const LspMessage &message);
}  // namespace Horo::Editor::Language
