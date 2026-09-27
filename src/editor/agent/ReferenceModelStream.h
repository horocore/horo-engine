#pragma once

#include "Horo/Agent/ModelProvider.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <stop_token>
#include <string_view>

namespace Horo::Agent::Detail {
    enum class Protocol {
        Ollama,
        OpenAI
    };

    inline constexpr std::size_t kMaximumLineBytes = 256U << 10U;

    /** @brief Per-invocation native-frame decoder; callbacks remain on the calling thread. */
    class StreamParser final {
    public:
        StreamParser(Protocol protocol, const ModelEventSink &sink, std::stop_token stop, std::uint64_t invocation);

        bool Line(std::string_view line);
        [[nodiscard]] ModelOutcome Finish();
        [[nodiscard]] bool Invalid() const noexcept;
        [[nodiscard]] bool Cancelled() const noexcept;

    private:
        using Json = nlohmann::json;
        static constexpr std::size_t kMaximumOutputBytes = 8U << 20U;

        bool Emit(const ModelEvent &event);
        bool OllamaFrame(const Json &frame);
        bool OllamaTools(const Json &calls);
        bool OpenAIFrame(const Json &frame);
        bool OpenAITools(const Json &fragments);
        bool ApplyOpenAIFragment(const Json &fragment);

        Protocol m_protocol;
        const ModelEventSink &m_sink;
        std::stop_token m_stop;
        std::uint64_t m_invocation{};
        std::map<std::size_t, ModelToolIntent> m_tools;
        ModelUsage m_usage;
        bool m_done{};
        bool m_error{};
        bool m_cancelled{};
        std::size_t m_outputBytes{};
    };
}  // namespace Horo::Agent::Detail
