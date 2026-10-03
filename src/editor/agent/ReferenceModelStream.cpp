#include "ReferenceModelStream.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>

namespace Horo::Agent::Detail {
    namespace {
        using Json = nlohmann::json;

        /** @brief Retain a usage count only when the provider reported an unsigned integer. */
        std::optional<std::uint64_t> UsageField(const Json &object, std::string_view key) {
            if (!object.is_object())
                return std::nullopt;
            const auto field = object.find(std::string(key));
            if (field == object.end() || !field->is_number_unsigned())
                return std::nullopt;
            return field->get<std::uint64_t>();
        }
    }  // namespace

    /** @copydoc StreamParser::StreamParser */
    StreamParser::StreamParser(Protocol protocol, const ModelEventSink &sink, std::stop_token stop, std::uint64_t invocation)
        : m_protocol(protocol), m_sink(sink), m_stop(stop), m_invocation(invocation) {}

    /** @copydoc StreamParser::Line */
    bool StreamParser::Line(std::string_view line) {
        if (line.empty() || line == "\r")
            return true;
        if (m_protocol == Protocol::OpenAI) {
            if (!line.starts_with("data:"))
                return true;
            line.remove_prefix(5);
            if (!line.empty() && line.front() == ' ')
                line.remove_prefix(1);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            if (line == "[DONE]") {
                m_done = true;
                return true;
            }
        }
        const Json frame = Json::parse(line, nullptr, false);
        if (frame.is_discarded() || !frame.is_object()) {
            m_error = true;
            return false;
        }
        return m_protocol == Protocol::Ollama ? OllamaFrame(frame) : OpenAIFrame(frame);
    }

    /** @copydoc StreamParser::Finish */
    ModelOutcome StreamParser::Finish() {
        if (m_stop.stop_requested() || m_cancelled)
            return {ModelError{ModelErrorCode::Cancelled, "Model request cancelled", std::nullopt}};
        if (m_error || !m_done)
            return {ModelError{ModelErrorCode::Protocol, "Incomplete or invalid model stream", std::nullopt}};
        for (const auto &[index, tool] : m_tools) {
            (void)index;
            if (const auto arguments = Json::parse(tool.argumentsJson, nullptr, false);
                tool.callId.empty() || tool.name.empty() || !arguments.is_object())
                return {ModelError{ModelErrorCode::Protocol, "Incomplete tool intent", std::nullopt}};
            if (!Emit({ModelEventKind::ToolIntent, {}, tool, {}}))
                break;
        }
        if (m_cancelled)
            return {ModelError{ModelErrorCode::Cancelled, "Model request cancelled", std::nullopt}};
        if ((m_usage.inputTokens.has_value() || m_usage.outputTokens.has_value()) && !Emit({ModelEventKind::Usage, {}, {}, m_usage}))
            return {ModelError{ModelErrorCode::Cancelled, "Model request cancelled", std::nullopt}};
        if (!Emit({ModelEventKind::Completed, {}, {}, {}}))
            return {ModelError{ModelErrorCode::Cancelled, "Model request cancelled", std::nullopt}};
        return {};
    }

    /** @copydoc StreamParser::Invalid */
    bool StreamParser::Invalid() const noexcept {
        return m_error;
    }

    /** @copydoc StreamParser::Cancelled */
    bool StreamParser::Cancelled() const noexcept {
        return m_cancelled;
    }

    /** @copydoc StreamParser::Emit */
    bool StreamParser::Emit(const ModelEvent &event) {
        if (event.kind == ModelEventKind::TextDelta) {
            if (event.text.size() > kMaximumOutputBytes - m_outputBytes) {
                m_error = true;
                return false;
            }
            m_outputBytes += event.text.size();
        }
        if (m_stop.stop_requested() || !m_sink(event)) {
            m_cancelled = true;
            return false;
        }
        return true;
    }

    /** @copydoc StreamParser::OllamaFrame */
    bool StreamParser::OllamaFrame(const Json &frame) {
        if (frame.contains("error")) {
            m_error = true;
            return false;
        }
        if (frame.contains("message") && frame["message"].is_object()) {
            const auto &message = frame["message"];
            if (message.contains("content") && message["content"].is_string()) {
                const std::string text = message["content"].get<std::string>();
                if (!text.empty() && !Emit({ModelEventKind::TextDelta, text, {}, {}}))
                    return false;
            }
            if (message.contains("tool_calls") && message["tool_calls"].is_array() && !OllamaTools(message["tool_calls"]))
                return false;
        }
        if (frame.contains("done") && !frame["done"].is_boolean()) {
            m_error = true;
            return false;
        }
        if (frame.value("done", false)) {
            m_done = true;
            m_usage.inputTokens = UsageField(frame, "prompt_eval_count");
            m_usage.outputTokens = UsageField(frame, "eval_count");
        }
        return true;
    }

    /** @copydoc StreamParser::OllamaTools */
    bool StreamParser::OllamaTools(const Json &calls) {
        for (const auto &call : calls) {
            if (!call.is_object() || !call.contains("function") || !call["function"].is_object()) {
                m_error = true;
                return false;
            }
            const auto &function = call["function"];
            if (!function.value("name", Json()).is_string() || !function.contains("arguments") ||
                (call.contains("id") && !call["id"].is_string())) {
                m_error = true;
                return false;
            }
            std::string callId = call.value("id", std::string{});
            if (callId.empty()) {
                std::ostringstream generated;
                generated << "ollama-" << m_invocation << '-' << m_tools.size();
                callId = generated.str();
            }
            ModelToolIntent tool{std::move(callId), function["name"].get<std::string>(),
                                 function["arguments"].is_string() ? function["arguments"].get<std::string>()
                                                                   : function["arguments"].dump()};
            if (tool.argumentsJson.size() > kMaximumLineBytes || m_tools.size() >= 64) {
                m_error = true;
                return false;
            }
            m_tools.try_emplace(m_tools.size(), std::move(tool));
        }
        return true;
    }

    /** @copydoc StreamParser::OpenAIFrame */
    bool StreamParser::OpenAIFrame(const Json &frame) {
        if (frame.contains("error")) {
            m_error = true;
            return false;
        }
        if (frame.contains("usage") && frame["usage"].is_object()) {
            m_usage.inputTokens = UsageField(frame["usage"], "prompt_tokens");
            m_usage.outputTokens = UsageField(frame["usage"], "completion_tokens");
        }
        if (!frame.contains("choices") || !frame["choices"].is_array())
            return true;
        for (const auto &choice : frame["choices"]) {
            if (!choice.is_object() || !choice.contains("delta") || !choice["delta"].is_object())
                continue;
            const auto &delta = choice["delta"];
            if (delta.contains("content") && delta["content"].is_string()) {
                const std::string text = delta["content"].get<std::string>();
                if (!text.empty() && !Emit({ModelEventKind::TextDelta, text, {}, {}}))
                    return false;
            }
            if (delta.contains("tool_calls") && delta["tool_calls"].is_array() && !OpenAITools(delta["tool_calls"]))
                return false;
        }
        return true;
    }

    /** @copydoc StreamParser::OpenAITools */
    bool StreamParser::OpenAITools(const Json &fragments) {
        return std::ranges::all_of(fragments, [this](const Json &fragment) {
            return ApplyOpenAIFragment(fragment);
        });
    }

    /** @copydoc StreamParser::ApplyOpenAIFragment */
    bool StreamParser::ApplyOpenAIFragment(const Json &fragment) {
        if (!fragment.is_object() || !fragment.value("index", Json()).is_number_unsigned()) {
            m_error = true;
            return false;
        }
        const auto index = fragment["index"].get<std::size_t>();
        if (index >= 64) {
            m_error = true;
            return false;
        }
        auto &tool = m_tools[index];
        if (m_tools.size() > 64) {
            m_error = true;
            return false;
        }
        if (fragment.contains("id") && fragment["id"].is_string())
            tool.callId = fragment["id"].get<std::string>();
        if (fragment.contains("function") && fragment["function"].is_object()) {
            const auto &function = fragment["function"];
            if (function.contains("name") && function["name"].is_string())
                tool.name += function["name"].get<std::string>();
            if (function.contains("arguments") && function["arguments"].is_string())
                tool.argumentsJson += function["arguments"].get<std::string>();
            if (tool.name.size() > 256 || tool.argumentsJson.size() > kMaximumLineBytes) {
                m_error = true;
                return false;
            }
        }
        return true;
    }
}  // namespace Horo::Agent::Detail
