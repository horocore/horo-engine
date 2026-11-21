#pragma once

/** @file LspProtocol.h @brief Internal byte framing and JSON-RPC envelope decoding for language servers. */

#include "CanonicalJsonWriter.h"
#include "Horo/Foundation/Result.h"

#include <optional>
#include <string>
#include <string_view>

namespace Horo::Editor::Language {
    inline constexpr std::size_t MaximumLspHeaderBytes = 4096;
    inline constexpr std::size_t MaximumLspBodyBytes = 4 * 1024 * 1024;

    /** @brief Incremental LSP framing for one byte stream, owned by its reader. */
    class LspFrameReader final {
    public:
        /** @brief Consumes bytes through at most one complete message, leaving any suffix in input.
         * @param input Unconsumed bytes, updated as the reader takes ownership.
         * @return A complete body, an empty optional for partial input, or a latched framing failure.
         */
        [[nodiscard]] Result<std::optional<std::string>> Consume(std::string_view &input);

        /** @brief Checks that EOF occurred between frames.
         * @return Success for a clean frame boundary, otherwise a terminal stream error.
         */
        [[nodiscard]] Result<void> EndOfStream();

    private:
        /** @brief Reads the length once the complete bounded header block is available. */
        [[nodiscard]] bool ReadHeader(std::string_view &input);

        std::string header_;
        std::string body_;
        std::size_t bodyLength_{};
        bool failed_{};
    };

    /** @brief Mutually exclusive envelopes allowed on an LSP connection. */
    enum class LspMessageKind {
        Request,
        Notification,
        Result,
        Error
    };

    /** @brief Owns the validated envelope. The adapter still owns method-specific payload validation. */
    struct LspMessage final {
        LspMessageKind kind{};
        JsonEncoding::Detail::OwnedJsonValue envelope;
    };

    /** @brief Decodes one strict, bounded JSON-RPC envelope.
     * @param body Exactly one UTF-8 LSP body.
     * @return An owned envelope or a typed refusal for malformed or over-budget input.
     */
    [[nodiscard]] Result<LspMessage> DecodeLspMessage(std::string_view body);
}  // namespace Horo::Editor::Language
