#pragma once

/** @file SemanticDocumentHistory.h
 * @brief Shared owner-thread bounded retention of exact semantic document transactions.
 */

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace Horo::Editor {
    /** @brief Finite retention budgets; a transaction must fit independently before eviction. */
    struct SemanticHistoryLimits final {
        std::size_t maximumEntries{128};
        std::size_t maximumBytes{4 * 1024 * 1024};
    };

    /** @brief Shared retention/publication policy; domain owners validate and replay typed semantic values.
     * @details Entry owns before/after semantic deltas, beforeState/afterState and complete bytes.
     * Preparing never changes this owner. Commit swaps a fully reserved candidate without failure.
     * No derived/runtime values or callbacks belong in Entry. Owner-thread use only.
     */
    template <typename Entry> class SemanticDocumentHistory final {
    public:
        /** @brief Creates an empty bounded history. @param limits Finite retention budgets. */
        explicit SemanticDocumentHistory(SemanticHistoryLimits limits = {}) : limits_(limits) {}

        /** @brief Prepares retention, dropping redo and evicting oldest entries only in detached storage.
         * @param entry Complete exact reversible transaction.
         * @return Candidate owner, or null if the transaction cannot fit; allocation exceptions preserve this owner.
         */
        [[nodiscard]] std::unique_ptr<SemanticDocumentHistory> Prepare(Entry entry) const {
            if (limits_.maximumEntries == 0 || entry.bytes > limits_.maximumBytes)
                return {};
            auto candidate = std::make_unique<SemanticDocumentHistory>(limits_);
            candidate->entries_.reserve(limits_.maximumEntries);
            for (std::size_t index = 0; index < cursor_; ++index) {
                candidate->entries_.push_back(entries_[index]);
                candidate->bytes_ += entries_[index].bytes;
            }
            while (!candidate->entries_.empty() &&
                   (candidate->entries_.size() >= limits_.maximumEntries || candidate->bytes_ > limits_.maximumBytes - entry.bytes)) {
                candidate->bytes_ -= candidate->entries_.front().bytes;
                candidate->entries_.erase(candidate->entries_.begin());
            }
            candidate->bytes_ += entry.bytes;
            candidate->entries_.push_back(std::move(entry));
            candidate->cursor_ = candidate->entries_.size();
            return candidate;
        }

        /** @brief Publishes a reserved history together with its domain root. @param candidate Detached prepared owner. */
        void Commit(SemanticDocumentHistory &candidate) noexcept {
            entries_.swap(candidate.entries_);
            std::swap(cursor_, candidate.cursor_);
            std::swap(bytes_, candidate.bytes_);
        }

        /** @brief Borrows the next exact undo/redo transaction. @param redo Direction. @return Entry or null. */
        [[nodiscard]] const Entry *Next(bool redo) const noexcept {
            if (redo)
                return cursor_ < entries_.size() ? &entries_[cursor_] : nullptr;
            return cursor_ ? &entries_[cursor_ - 1] : nullptr;
        }

        /** @brief Advances after a successful domain root commit. @param redo Previously validated replay direction. */
        void DidReplay(bool redo) noexcept {
            redo ? ++cursor_ : --cursor_;
        }

        /** @brief Releases history at explicit replacement/close without changing authored state. */
        void Clear() noexcept {
            entries_.clear();
            cursor_ = 0;
            bytes_ = 0;
        }

        /** @brief Reports retained applied entries. @return Undo count. */
        [[nodiscard]] std::size_t UndoCount() const noexcept {
            return cursor_;
        }

        /** @brief Reports retained unapplied entries. @return Redo count. */
        [[nodiscard]] std::size_t RedoCount() const noexcept {
            return entries_.size() - cursor_;
        }

        /** @brief Reports all retained semantic bytes, including redo. @return Charged bytes. */
        [[nodiscard]] std::size_t Bytes() const noexcept {
            return bytes_;
        }

    private:
        SemanticHistoryLimits limits_;
        std::vector<Entry> entries_;
        std::size_t cursor_{};
        std::size_t bytes_{};
    };
}  // namespace Horo::Editor
