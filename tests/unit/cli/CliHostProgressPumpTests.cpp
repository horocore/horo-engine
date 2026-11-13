#include "CliHostProgressPump.h"
#include "Horo/Cli/CliErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <condition_variable>
#include <future>
#include <nlohmann/json.hpp>
#include <sstream>

namespace Horo::Application::Internal {
    namespace {
        /** @brief Builds only the canonical CLI error contribution used by the host presenter. */
        Hosts::ErrorTranslator Translator() {
            const std::array modules{
                ModuleDescriptor{.id = {"horo.cli"}, .version = {1, 0, 0}, .errorDomains = {Cli::CliErrors::ErrorDomain()}}};
            auto registry = BuildErrorCodeRegistry(modules);
            REQUIRE(registry.HasValue());
            const std::array mappings{
                Hosts::ErrorMapping{Cli::CliErrors::HostFailure.domain, Cli::CliErrors::HostFailure.code, Hosts::ExitCategory::Invariant}};
            auto translator = Hosts::ErrorTranslator::Create(std::move(registry).Value(), mappings);
            REQUIRE(translator);
            return std::move(*translator);
        }

        /** @brief Declares the same versioned progress protocol used by native commands. */
        Cli::CliCommandDescriptor Descriptor() {
            return {.path = {{"host", "work"}},
                    .output = {.id = "horo.cli.work",
                               .version = 1,
                               .formats = Cli::CliOutputFormat::Human | Cli::CliOutputFormat::Json | Cli::CliOutputFormat::JsonLines,
                               .progressRecords = true}};
        }

        /** @brief Blocks one output consumer while keeping its storage confined to that writer lane. */
        class SlowBuffer final : public std::stringbuf {
        public:
            bool WaitUntilBlocked() {
                std::unique_lock lock(mutex_);
                return ready_.wait_for(lock, std::chrono::seconds{5}, [this] {
                    return blocked_;
                });
            }

            void Release() {
                std::lock_guard lock(mutex_);
                released_ = true;
                ready_.notify_all();
            }

        protected:
            std::streamsize xsputn(const char *text, const std::streamsize size) override {
                std::unique_lock lock(mutex_);
                blocked_ = true;
                ready_.notify_all();
                ready_.wait(lock, [this] {
                    return released_;
                });
                lock.unlock();
                return std::stringbuf::xsputn(text, size);
            }

        private:
            // Protects the handoff flags shared by the writer and test owner. ReleaseOnExit wakes the writer before join.
            std::mutex mutex_;
            std::condition_variable ready_;
            bool blocked_{};
            bool released_{};
        };

        /** @brief Unblocks the writer before owned producer and presentation threads unwind. */
        struct ReleaseOnExit final {
            SlowBuffer &buffer;

            ~ReleaseOnExit() {
                buffer.Release();
            }
        };

        /** @brief Rejects output deterministically without depending on native pipe capacity. */
        class FailedBuffer final : public std::streambuf {
            std::streamsize xsputn(const char *, std::streamsize) override {
                return 0;
            }

            int_type overflow(int_type) override {
                return traits_type::eof();
            }
        };
    }  // namespace

    TEST_CASE("Native CLI presents progress during dispatch while a slow consumer retains bounded state", "[cli][progress][host]") {
        SlowBuffer buffer;
        std::ostream output{&buffer};
        std::ostringstream diagnostics;
        Cli::CliOutputPresenter presenter(Descriptor(), {1}, Translator(), output, diagnostics, Cli::CliProgressOutputMode::JsonLines,
                                          false);
        Cli::CliInvocationStopController stop{std::chrono::milliseconds{0}};
        Cli::CliProgressMailbox mailbox;
        mailbox.Report({"begin", 0.0F, "working"});
        CliHostProgressPump pump(mailbox, presenter, stop, false);
        std::promise<void> produced;
        auto completed = produced.get_future();
        std::jthread producer;
        ReleaseOnExit release{buffer};
        REQUIRE(buffer.WaitUntilBlocked());
        producer = std::jthread([&] {
            const std::string oversized(8192, 'x');
            for (unsigned index = 0; index < 65'536; ++index)
                mailbox.Report({"work", 0.5F, oversized});
            mailbox.Report({"done", 1.0F, oversized});
            produced.set_value();
        });
        REQUIRE(completed.wait_for(std::chrono::seconds{15}) == std::future_status::ready);
        producer.join();
        const auto retained = mailbox.Take();
        REQUIRE(retained);
        CHECK(retained->message.size() == 4096);
        CHECK(retained->completion == 1.0F);
        buffer.Release();
        REQUIRE(pump.Finish().HasValue());
        CHECK(pump.Finish().HasValue());
        const auto terminal = Cli::CliTerminalResult::Success({.invocation = {1}}, {.fields = {{"done", true}}});
        CHECK(presenter.Complete(terminal).exitCode == 0);
        std::istringstream records{buffer.str()};
        std::string line;
        REQUIRE(std::getline(records, line));
        CHECK(nlohmann::json::parse(line).at("type") == "progress");
        REQUIRE(std::getline(records, line));
        CHECK(nlohmann::json::parse(line).at("type") == "result");
        CHECK(diagnostics.str().empty());
    }

    TEST_CASE("Native CLI progress failure requests cooperative stop and joins its writer", "[cli][progress][host]") {
        FailedBuffer buffer;
        std::ostream output{&buffer};
        std::ostringstream diagnostics;
        Cli::CliOutputPresenter presenter(Descriptor(), {1}, Translator(), output, diagnostics, Cli::CliProgressOutputMode::JsonLines,
                                          false);
        Cli::CliInvocationStopController stop{std::chrono::milliseconds{0}};
        Cli::CliProgressMailbox mailbox;
        mailbox.Report({"work", 0.5F, "working"});
        CliHostProgressPump pump(mailbox, presenter, stop, false);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (!stop.Token().IsCancellationRequested() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        REQUIRE(stop.Token().IsCancellationRequested());
        const auto result = pump.Finish();
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == Cli::CliErrors::HostFailure.code.Value());
        CHECK(presenter.HostFailure().exitCode == 1);
    }
}  // namespace Horo::Application::Internal
