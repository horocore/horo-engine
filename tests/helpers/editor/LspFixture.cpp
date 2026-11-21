#include "LspProtocol.h"

#include <iostream>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
    using namespace Horo::Editor::Language;

    /** @brief Reads one request exactly as a stdio language server would. */
    std::optional<LspMessage> ReadMessage() {
        LspFrameReader reader;
        char byte{};
        while (std::cin.get(byte)) {
            auto input = std::string_view{&byte, 1};
            auto frame = reader.Consume(input);
            if (frame.HasError())
                return std::nullopt;
            if (frame.Value()) {
                auto message = DecodeLspMessage(*frame.Value());
                if (message.HasValue())
                    return std::move(message).Value();
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    /** @brief Confirms the expected request or notification was delivered in order. */
    std::optional<LspMessage> Expect(const std::string_view method) {
        auto message = ReadMessage();
        if (!message)
            return std::nullopt;
        const auto &value = message->envelope.Get();
        if (!value.contains("method") || value.at("method").get_ref<const std::string &>() != method)
            return std::nullopt;
        return message;
    }

    /** @brief Emits one complete frame over the child process output. */
    bool Send(const nlohmann::json &message) {
        auto frame = EncodeLspMessage(message);
        if (frame.HasError())
            return false;
        const auto &bytes = frame.Value();
        return static_cast<bool>(std::cout.write(bytes.data(), static_cast<std::streamsize>(bytes.size())).flush());
    }

    /** @brief Returns a standard result associated with the actual incoming ID. */
    bool Respond(const LspMessage &message, nlohmann::json result) {
        return Send({{"jsonrpc", "2.0"}, {"id", message.envelope.Get().at("id")}, {"result", std::move(result)}});
    }

    /** @brief Echoes a diagnostic notification for the actual document and wire version. */
    bool Publish(const LspMessage &open) {
        const auto &document = open.envelope.Get().at("params").at("textDocument");
        return Send(
            {{"jsonrpc", "2.0"},
             {"method", "textDocument/publishDiagnostics"},
             {"params", {{"uri", document.at("uri")}, {"version", document.at("version")}, {"diagnostics", nlohmann::json::array()}}}});
    }

    /** @brief Runs one complete project and source request lifecycle over raw stdio. */
    bool Serve() {
        const auto initialize = Expect("initialize");
        if (!initialize || !Respond(*initialize, {{"capabilities", {{"textDocumentSync", 1}, {"hoverProvider", true}}}}))
            return false;
        if (!Expect("initialized"))
            return false;
        const auto opened = Expect("textDocument/didOpen");
        if (!opened || !Publish(*opened))
            return false;
        const auto request = Expect("textDocument/hover");
        if (!request || !Respond(*request, {{"contents", {{"kind", "plaintext"}, {"value", "hover fixture"}}}}))
            return false;
        const auto shutdown = Expect("shutdown");
        return shutdown && Respond(*shutdown, nullptr) && Expect("exit").has_value();
    }
}  // namespace

int main() {
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) < 0 || _setmode(_fileno(stdout), _O_BINARY) < 0)
        return 4;
#endif
    std::cerr << "language fixture ready\n" << std::flush;
    return Serve() ? 0 : 4;
}
