#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Physics/StateRecorder.h>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <type_traits>

namespace Horo::Physics::Detail {
    /** @brief Bounded private recorder bridge. Its bytes NEVER cross the Physics
     * state wire boundary.
     * @details Native SaveState output is decoded into explicitly typed fields.
     * Only fully validated typed fields are encoded back for candidate
     * RestoreState. Short reads zero their destination and retain a failed flag;
     * callers never invoke native restore on unvalidated external bytes.
     */
    class CanonicalPhysicsStateStream final : public JPH::StateRecorder {
    public:
        /** @brief Borrows preallocated native bridge scratch; construction never
         * allocates. */
        explicit CanonicalPhysicsStateStream(std::span<std::byte> storage) noexcept : storage_(storage) {}

        /** @brief Writes only inside admitted scratch and remembers complete-output
         * failure. */
        void WriteBytes(const void *data, std::size_t size) override {
            if (size > storage_.size() - written_) {
                failed_ = true;
                return;
            }
            std::memcpy(storage_.data() + written_, data, size);
            written_ += size;
        }

        /** @brief Reads only initialized bridge bytes; never reads a malformed
         * pointer or unchecked count. */
        void ReadBytes(void *data, std::size_t size) override {
            if (size > written_ - read_) {
                failed_ = true;
                std::memset(data, 0, size);
                return;
            }
            std::memcpy(data, storage_.data() + read_, size);
            read_ += size;
        }

        /** @brief Required native stream state; false success cannot conceal
         * truncation. */
        bool IsFailed() const override {
            return failed_;
        }

        /** @brief Native stream EOF means every initialized byte has been consumed.
         */
        bool IsEOF() const override {
            return read_ == written_;
        }

        /** @brief Rewinds an admitted complete native bridge; caller retains bytes
         * and capacity. */
        void Rewind() noexcept {
            read_ = 0;
        }

        /** @brief Clears the bridge before a new independent capture. */
        void Clear() noexcept {
            written_ = read_ = 0;
            failed_ = false;
        }

        /** @brief Returns the initialized byte prefix for private grammar decoding
         * only. */
        std::span<const std::byte> Bytes() const noexcept {
            return storage_.first(written_);
        }

    private:
        std::span<std::byte> storage_;
        std::size_t written_{}, read_{};
        bool failed_{};
    };
}  // namespace Horo::Physics::Detail
