#pragma once

/** @file ExternalProcessBytePump.h @brief Worker-side raw and line-oriented process I/O state. */

#include "Horo/Platform/ExternalProcess.h"

#include <algorithm>
#include <array>

namespace Horo::PlatformInternal {
    /** @brief Owns bounded stderr or unchanged stdout capture for one child stream. */
    class ProcessOutputCapture final {
    public:
        ProcessOutputCapture(const ProcessOutputStream stream, const ExternalProcessRequest &request)
            : stream_(stream), maximum_(std::max<std::size_t>(request.maximumLineBytes, 1U)), outputBudget_(request.maximumOutputBytes),
              callback_(&request.onOutput), raw_(stream == ProcessOutputStream::StandardOutput ? request.byteChannel : nullptr) {}

        /** @brief Preserves raw stdout or decodes and bounds line-oriented capture. */
        bool Append(const std::span<const char> bytes) {
            if (raw_)
                return raw_->AcceptOutput(bytes);
            for (const char value : bytes) {
                if (received_ >= outputBudget_) {
                    truncated_ = true;
                    continue;
                }
                ++received_;
                if (value == '\n')
                    Emit();
                else if (value != '\r') {
                    if (pending_.size() < maximum_)
                        pending_.push_back(value);
                    else
                        truncated_ = true;
                }
            }
            return true;
        }

        /** @brief Flushes a bounded unterminated diagnostic line. */
        void Finish() {
            if (!pending_.empty() || truncated_)
                Emit();
        }

    private:
        void Emit() {
            if (*callback_)
                (*callback_)(ProcessOutputLine{stream_, std::move(pending_), truncated_});
            pending_.clear();
            truncated_ = false;
        }

        ProcessOutputStream stream_;
        std::size_t maximum_;
        std::size_t outputBudget_;
        std::size_t received_{};
        const std::function<void(ProcessOutputLine)> *callback_;
        IProcessByteChannel *raw_;
        std::string pending_;
        bool truncated_{false};
    };

    /** @brief Preserves remaining bytes across partial or temporarily unavailable native writes. */
    class ProcessInputPump final {
    public:
        explicit ProcessInputPump(IProcessByteChannel *const channel) noexcept : channel_(channel) {}

        /** @brief Takes a fresh fixed-size chunk only after the prior chunk was fully written. */
        [[nodiscard]] bool Refill() noexcept {
            if (channel_ && offset_ == size_) {
                offset_ = 0;
                size_ = channel_->TakeInput(buffer_);
                return size_ <= buffer_.size();
            }
            return true;
        }

        /** @brief Exposes the bytes the next native write may consume. */
        [[nodiscard]] std::span<const char> Pending() const noexcept {
            return std::span{buffer_}.subspan(offset_, size_ - offset_);
        }

        /** @brief Advances only by the byte count confirmed by the native write. */
        void Written(const std::size_t count) noexcept {
            offset_ += count;
        }

    private:
        IProcessByteChannel *channel_;
        std::array<char, 16 * 1024> buffer_{};
        std::size_t offset_{};
        std::size_t size_{};
    };
}  // namespace Horo::PlatformInternal
