#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Platform/PlatformErrors.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace std::chrono_literals;

    /** @brief Fixed-capacity receiver with an owned input cursor for real subprocess tests. */
    struct ByteChannel final : IProcessByteChannel {
        explicit ByteChannel(const std::string_view bytes, const std::size_t outputCapacity) : input(bytes), output(outputCapacity, '\0') {}

        std::size_t TakeInput(std::span<char> destination) noexcept override {
            const auto bytes = input.substr(0, destination.size());
            std::ranges::copy(bytes, destination.begin());
            input.remove_prefix(bytes.size());
            return bytes.size();
        }

        bool AcceptOutput(std::span<const char> bytes) noexcept override {
            if (bytes.size() > output.size() - received)
                return false;
            std::ranges::copy(bytes, output.begin() + static_cast<std::ptrdiff_t>(received));
            received += bytes.size();
            return true;
        }

        std::string_view input;
        std::string output;
        std::size_t received{};
    };

    /** @brief Shares native runner settings without a shell or machine-specific fixture dependency. */
    ExternalProcessRequest Request(const char *mode, ByteChannel &channel) {
        ExternalProcessRequest request;
        request.executable = HORO_PROCESS_TEST_CHILD;
        request.arguments = {mode};
        request.timeout = 5s;
        request.gracefulTermination = 100ms;
        request.maximumDrainDuration = 250ms;
        request.byteChannel = &channel;
        return request;
    }

    TEST_CASE("External process byte channel preserves binary input and stdout while stderr remains separate",
              "[unit][platform][process]") {
        constexpr std::size_t TransferBytes = 192 * 1024;
        std::string input;
        input.reserve(TransferBytes);
        for (std::size_t i = 0; i < TransferBytes; ++i)
            input.push_back(static_cast<char>(i % 256));
        ByteChannel channel{input, input.size() * 2};
        auto request = Request("exchange-bytes", channel);
        std::vector<ProcessOutputLine> lines;
        request.onOutput = [&lines](ProcessOutputLine line) {
            lines.push_back(std::move(line));
        };
        NativeExternalProcessRunner runner;
        const auto result = runner.Run(request, {});
        REQUIRE(result.HasValue());
        CHECK(result.Value().exitCode == 0);
        CHECK(result.Value().reason == ProcessTerminationReason::Exited);
        CHECK(channel.input.empty());
        CHECK(channel.received == channel.output.size());
        CHECK(channel.output == std::string(TransferBytes, 'b') + input);
        REQUIRE(lines.size() == 1);
        CHECK(lines.front().stream == ProcessOutputStream::StandardError);
        CHECK(lines.front().text == "raw channel ready");
    }

    TEST_CASE("External process byte channel terminates when the output queue refuses bytes", "[unit][platform][process]") {
        ByteChannel channel{{}, 0};
        auto request = Request("stubborn", channel);
        NativeExternalProcessRunner runner;
        const auto result = runner.Run(request, {});
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == PlatformErrors::ProcessIoFailed.code.Value());
    }

    TEST_CASE("A full external process input pipe cannot prevent timeout", "[unit][platform][process]") {
        const std::string input(4 * 1024 * 1024, 'x');
        ByteChannel channel{input, 4096};
        auto request = Request("stubborn", channel);
        request.timeout = 300ms;
        NativeExternalProcessRunner runner;
        const auto result = runner.Run(request, {});
        REQUIRE(result.HasValue());
        CHECK(result.Value().stopCause == ProcessStopCause::Timeout);
        CHECK_FALSE(channel.input.empty());
    }
}  // namespace
