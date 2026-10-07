#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace Horo {
    /** @brief Cheap immutable observer of cooperative cancellation state. */
    class CancellationToken {
    public:
        /** @brief Creates a token with no cancellation owner or parent. */
        CancellationToken() = default;

        [[nodiscard]] bool IsCancellationRequested() const noexcept {
            for (auto state = m_state; state; state = state->parent) {
                if (state->requested.load())
                    return true;
            }
            return false;
        }

    private:
        struct State {
            State() = default;

        private:
            friend class CancellationToken;
            friend class CancellationSource;
            std::atomic<bool> requested{false};
            std::shared_ptr<const State> parent;
            // Scheduler admission wake targets are deduplicated and weakly owned. No user callback runs on cancellation.
            // Any thread may register/request; wakeMutex protects this list independently of scheduler/record locks.
            mutable std::mutex wakeMutex;

            struct AdmissionWake {
                std::weak_ptr<std::condition_variable> target;
                std::size_t users{};
            };

            mutable std::vector<AdmissionWake> admissionWakes;
        };
        friend class CancellationSource;
        friend class JobSystem;

        /** @brief Owns a scoped registration per ancestor; concurrent waits share a reference-counted weak notification target. */
        class AdmissionWakeLease final {
        public:
            explicit AdmissionWakeLease(std::shared_ptr<std::condition_variable> wake) : wake_(std::move(wake)) {}

            AdmissionWakeLease(const AdmissionWakeLease &) = delete;
            AdmissionWakeLease &operator=(const AdmissionWakeLease &) = delete;

            AdmissionWakeLease(AdmissionWakeLease &&other) noexcept
                : states_(std::move(other.states_)), wake_(std::move(other.wake_)), registered_(std::exchange(other.registered_, 0)) {}

            AdmissionWakeLease &operator=(AdmissionWakeLease &&) = delete;

            ~AdmissionWakeLease() {
                for (std::size_t index = 0; index < registered_; ++index) {
                    const auto &state = states_[index];
                    std::lock_guard lock(state->wakeMutex);
                    std::erase_if(state->admissionWakes, [this](auto &entry) {
                        if (entry.target.lock() == wake_)
                            return --entry.users == 0;
                        return entry.target.expired();
                    });
                }
            }

        private:
            friend class CancellationToken;
            std::vector<std::shared_ptr<const State>> states_;
            std::shared_ptr<std::condition_variable> wake_;
            std::size_t registered_{};
        };

        /** @brief Registers bounded active-wait notification leases without retaining targets after a submission returns. */
        [[nodiscard]] AdmissionWakeLease RegisterAdmissionWake(const std::shared_ptr<std::condition_variable> &wake) const {
            AdmissionWakeLease lease{wake};
            for (auto state = m_state; state; state = state->parent)
                lease.states_.push_back(state);
            for (const auto &state : lease.states_) {
                std::lock_guard lock(state->wakeMutex);
                std::erase_if(state->admissionWakes, [](const auto &entry) {
                    return entry.target.expired();
                });
                const auto found = std::find_if(state->admissionWakes.begin(), state->admissionWakes.end(), [&wake](const auto &entry) {
                    return entry.target.lock() == wake;
                });
                if (found != state->admissionWakes.end())
                    ++found->users;
                else
                    state->admissionWakes.push_back(State::AdmissionWake{wake, 1});
                ++lease.registered_;
            }
            return lease;
        }

        explicit CancellationToken(std::shared_ptr<const State> state) : m_state(std::move(state)) {}

        std::shared_ptr<const State> m_state;
    };

    /** @brief Owner that can request cancellation for its token and derived children. */
    class CancellationSource {
    public:
        CancellationSource() = default;

        explicit CancellationSource(const CancellationToken &parent) {
            m_state->parent = parent.m_state;
        }

        [[nodiscard]] CancellationToken Token() const noexcept {
            return CancellationToken(m_state);
        }

        void RequestCancellation() const noexcept {
            m_state->requested.store(true);
            std::lock_guard lock(m_state->wakeMutex);
            for (const auto &entry : m_state->admissionWakes) {
                if (const auto wake = entry.target.lock())
                    wake->notify_all();
            }
        }

    private:
        std::shared_ptr<CancellationToken::State> m_state{std::make_shared<CancellationToken::State>()};
    };

}  // namespace Horo
