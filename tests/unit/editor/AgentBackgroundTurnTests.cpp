#include "AgentBackgroundTurn.h"
#include "AgentTurn.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <semaphore>
#include <stdexcept>
#include <variant>

namespace Horo::Agent {
    namespace {
        using namespace std::chrono_literals;
        using BackgroundTurn = AgentBackgroundTurn<std::variant<ProviderDiscovery, AgentTurnResult>>;
        using State = BackgroundTurn::CompletionState;

        /** @brief Pumps the same non-blocking owner-lane boundary as the editor. */
        std::optional<BackgroundTurn::Completion> Await(BackgroundTurn &owner) {
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            do {
                if (auto result = owner.Poll())
                    return result;
                std::this_thread::sleep_for(1ms);
            } while (std::chrono::steady_clock::now() < deadline);
            return std::nullopt;
        }

        /** @brief Simulates stoppable provider work that may still return a late response. */
        BackgroundTurn::Operation WaitForStop(std::promise<bool> stopped) {
            return BackgroundTurn::Operation{[stopped = std::move(stopped)](std::stop_token stop) mutable -> BackgroundTurn::Reply {
                std::mutex mutex;
                std::unique_lock lock{mutex};
                std::condition_variable_any pending;
                (void)pending.wait_for(lock, stop, 5s, [] {
                    return false;
                });
                stopped.set_value(stop.stop_requested());
                return ProviderDiscovery{.id = "retired provider"};
            }};
        }
    }  // namespace

    TEST_CASE("Editor-owned agent work publishes independently of presentation polling", "[unit][editor][agent][background]") {
        BackgroundTurn owner;
        std::promise<void> registered;
        auto finished = registered.get_future();
        REQUIRE(
            owner.Start(BackgroundTurn::Operation{[done = std::move(registered)](std::stop_token stop) mutable -> BackgroundTurn::Reply {
            done.set_value();
            return ProviderDiscovery{.id = stop.stop_requested() ? "stopped" : "available"};
        }}));
        REQUIRE(finished.wait_for(5s) == std::future_status::ready);
        CHECK(owner.Busy());
        CHECK_FALSE(owner.Start(BackgroundTurn::Operation{[](std::stop_token) -> BackgroundTurn::Reply {
            return ProviderDiscovery{};
        }}));
        const auto result = Await(owner);
        REQUIRE(result);
        CHECK(result->state == State::Ready);
        REQUIRE(result->reply);
        CHECK(std::get<ProviderDiscovery>(*result->reply).id == "available");
        CHECK_FALSE(owner.Busy());
        CHECK_FALSE(owner.Poll());
    }

    TEST_CASE("Agent background retirement cancels and suppresses old provider replies", "[unit][editor][agent][background]") {
        BackgroundTurn owner;
        std::promise<bool> cancelled;
        auto observed = cancelled.get_future();
        REQUIRE(owner.Start(WaitForStop(std::move(cancelled))));
        State expected{};
        SECTION("the user stops the current turn") {
            REQUIRE(owner.CanCancel());
            owner.Cancel();
            expected = State::Cancelled;
        }
        SECTION("the workspace is replaced") {
            owner.Abandon();
            expected = State::Abandoned;
        }
        CHECK_FALSE(owner.CanCancel());
        REQUIRE(observed.wait_for(5s) == std::future_status::ready);
        CHECK(observed.get());
        const auto result = Await(owner);
        REQUIRE(result);
        CHECK(result->state == expected);
        CHECK_FALSE(result->reply);

        REQUIRE(owner.Start(BackgroundTurn::Operation{[](std::stop_token) -> BackgroundTurn::Reply {
            return ProviderDiscovery{.id = "fresh provider"};
        }}));
        const auto fresh = Await(owner);
        REQUIRE(fresh);
        CHECK(fresh->state == State::Ready);
        REQUIRE(fresh->reply);
        CHECK(std::get<ProviderDiscovery>(*fresh->reply).id == "fresh provider");
    }

    TEST_CASE("Agent completion waits for provider captures to retire on the worker", "[unit][editor][agent][background]") {
        struct Capture final {
            std::thread::id &retirementThread;
            std::binary_semaphore &retiring;
            std::binary_semaphore &release;

            Capture(std::thread::id &thread, std::binary_semaphore &started, std::binary_semaphore &ready) noexcept
                : retirementThread(thread), retiring(started), release(ready) {}

            Capture(const Capture &) = delete;
            Capture &operator=(const Capture &) = delete;

            ~Capture() noexcept {
                retirementThread = std::this_thread::get_id();
                retiring.release();
                static_cast<void>(release.try_acquire_for(5s));
            }
        };

        std::thread::id retirementThread;
        std::binary_semaphore retiring{0};
        std::binary_semaphore release{0};
        BackgroundTurn owner;
        REQUIRE(owner.Start(BackgroundTurn::Operation{
            [capture = std::make_unique<Capture>(retirementThread, retiring, release)](std::stop_token) -> BackgroundTurn::Reply {
            return ProviderDiscovery{.id = "retiring"};
        }}));

        REQUIRE(retiring.try_acquire_for(5s));
        CHECK(retirementThread != std::this_thread::get_id());
        CHECK(owner.Busy());
        CHECK_FALSE(owner.Poll());
        release.release();
        const auto completed = Await(owner);
        REQUIRE(completed);
        CHECK(completed->state == State::Ready);
        CHECK_FALSE(owner.Busy());
    }

    TEST_CASE("Agent background shutdown joins cooperative work and reports provider exceptions", "[unit][editor][agent][background]") {
        std::promise<bool> cancelled;
        auto observed = cancelled.get_future();
        {
            BackgroundTurn owner;
            REQUIRE(owner.Start(WaitForStop(std::move(cancelled))));
        }
        REQUIRE(observed.wait_for(0s) == std::future_status::ready);
        CHECK(observed.get());

        BackgroundTurn owner;
        REQUIRE(owner.Start(BackgroundTurn::Operation{[](std::stop_token) -> BackgroundTurn::Reply {
            throw std::runtime_error{"provider disconnected"};
        }}));
        const auto result = Await(owner);
        REQUIRE(result);
        CHECK(result->state == State::Failed);
        CHECK_FALSE(result->reply);
        CHECK_FALSE(owner.Busy());
    }
}  // namespace Horo::Agent
