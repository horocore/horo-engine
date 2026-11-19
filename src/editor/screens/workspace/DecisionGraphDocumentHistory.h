#pragma once

/** @file DecisionGraphDocumentHistory.h @brief Prepared in-memory publication for one authored Decision document. */
#include "DecisionGraphLayout.h"
#include "Horo/AI/DecisionGraphCompiler.h"
#include "Horo/Application/DecisionAssetCookHost.h"
#include "Horo/Editor/SemanticDocumentHistory.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace Horo::Editor {
    /** @brief Immutable graph/layout pair and canonical semantic digest retained in history. */
    struct DecisionGraphDocumentRoot final {
        std::shared_ptr<const AI::DecisionGraphSource> graph;
        std::shared_ptr<const DecisionGraphLayout> layout;
        Sha256Digest digest;

        bool operator==(const DecisionGraphDocumentRoot &) const noexcept = default;
    };

    /** @brief Captures the session, last accepted source and exact live roots of an edit participant. */
    struct DecisionGraphHistoryFence final {
        DecisionGraphDocumentFence document;
        Sha256Digest acceptedSource;
        DecisionGraphDocumentRoot root;
        bool conflict{};
    };

    /** @brief Retains and prepares semantic history on the Decision workspace owner lane. */
    class DecisionGraphDocumentHistory final {
        struct Entry final {
            DecisionGraphDocumentRoot before;
            DecisionGraphDocumentRoot after;
            std::size_t bytes{};
            std::uint64_t group{};
        };

        using History = SemanticDocumentHistory<Entry>;

    public:
        /** @brief Detached reservation for one complete root swap, owned by exactly one history. */
        class Prepared final {
        public:
            Prepared(Prepared &&) noexcept = default;
            Prepared &operator=(Prepared &&) noexcept = default;

            /** @brief Borrows the exact graph/layout candidate for validation or publication. @return Immutable pending root. */
            [[nodiscard]] const DecisionGraphDocumentRoot &Candidate() const noexcept {
                return candidate_;
            }

        private:
            friend class DecisionGraphDocumentHistory;

            /** @brief Associates the reserved storage or replay cursor with its exact owner and original root. */
            Prepared(const DecisionGraphDocumentHistory *owner, std::uint64_t serial, DecisionGraphHistoryFence expected,
                     DecisionGraphDocumentRoot candidate, std::unique_ptr<History> history, std::optional<bool> redo = {}) noexcept
                : owner_(owner), serial_(serial), expected_(std::move(expected)), candidate_(std::move(candidate)),
                  history_(std::move(history)), redo_(redo) {}

            const DecisionGraphDocumentHistory *owner_;
            std::uint64_t serial_;
            DecisionGraphHistoryFence expected_;
            DecisionGraphDocumentRoot candidate_;
            std::unique_ptr<History> history_;
            std::optional<bool> redo_;
        };

        /** @brief Reserves the history entry for an already semantically validated candidate.
         * @param current Exact current accepted source and owner root. @param candidate Validated replacement.
         * @param bytes Charged canonical graph/layout representations. @param group Nonzero for a coordinated operation.
         * @return Detached reservation or typed stale or budget refusal.
         */
        [[nodiscard]] Result<Prepared> PrepareChange(const DecisionGraphHistoryFence &current, DecisionGraphDocumentRoot candidate,
                                                     std::size_t bytes, std::uint64_t group = 0) const {
            if (!Valid(current) || !candidate.graph || !candidate.layout || (group != 0 && current.conflict))
                return Result<Prepared>::Failure(MakeError(Application::DecisionCookErrors::Stale));
            auto prepared = history_.Prepare({current.root, candidate, bytes, group});
            if (!prepared)
                return Result<Prepared>::Failure(MakeError(Assets::AssetSourceErrors::Limit));
            return Result<Prepared>::Success(Prepared{this, serial_, current, std::move(candidate), std::move(prepared)});
        }

        /** @brief Reserves the exact next replay without moving the current root or history cursor.
         * @param current Live participant state. @param redo Replay direction.
         * @param group Zero for standalone replay, or the exact coordinated operation at this boundary.
         * @return Detached reservation or typed stale/history refusal.
         */
        [[nodiscard]] Result<Prepared> PrepareReplay(const DecisionGraphHistoryFence &current, bool redo, std::uint64_t group = 0) const {
            const auto *entry = history_.Next(redo);
            if (!Valid(current) || !entry || entry->group != group || current.root != (redo ? entry->before : entry->after) ||
                (group != 0 && current.conflict))
                return Result<Prepared>::Failure(MakeError(Application::DecisionCookErrors::Stale));
            return Result<Prepared>::Success(Prepared{this, serial_, current, redo ? entry->after : entry->before, {}, redo});
        }

        /** @brief Checks the reservation again immediately before owner-lane publication.
         * @param prepared Reservation from this participant. @param current Live state, reacquired after validation.
         * @return True only while this is the same unreplaced history boundary and exact accepted document state.
         */
        [[nodiscard]] bool CanPublish(const Prepared &prepared, const DecisionGraphHistoryFence &current) const noexcept {
            return prepared.owner_ == this && prepared.serial_ == serial_ && (prepared.history_ || prepared.redo_) &&
                   prepared.candidate_.graph && prepared.candidate_.layout && Valid(current) &&
                   prepared.expected_.document.instance == current.document.instance &&
                   prepared.expected_.document.revision == current.document.revision &&
                   prepared.expected_.acceptedSource == current.acceptedSource && prepared.expected_.root == current.root &&
                   prepared.expected_.conflict == current.conflict;
        }

        /** @brief Swaps previously reserved history or moves its replay boundary without allocation.
         * @param prepared Exact reservation. @param current Owner state pinned through publication.
         * @return False if the reservation is no longer valid; neither history direction is changed in that case.
         */
        [[nodiscard]] bool Publish(Prepared &prepared, const DecisionGraphHistoryFence &current) noexcept {
            if (!CanPublish(prepared, current))
                return false;
            if (prepared.redo_)
                history_.DidReplay(*prepared.redo_);
            else
                history_.Commit(*prepared.history_);
            prepared.owner_ = nullptr;
            ++serial_;
            return true;
        }

        /** @brief Resolves a coordinated entry at the requested history boundary. @param redo Replay direction. */
        [[nodiscard]] std::optional<std::uint64_t> NextReferenceGroup(bool redo) const noexcept {
            const auto *entry = history_.Next(redo);
            return entry && entry->group ? std::optional{entry->group} : std::nullopt;
        }

        /** @brief Checks whether the requested replay boundary has an entry. @param redo Replay direction. */
        [[nodiscard]] bool HasReplay(bool redo) const noexcept {
            return history_.Next(redo) != nullptr;
        }

    private:
        /** @brief Checks root and exact non-exhausted instance/revision authority. */
        static bool Valid(const DecisionGraphHistoryFence &fence) noexcept {
            return fence.document.instance.IsValid() && fence.document.revision != 0 &&
                   fence.document.revision != std::numeric_limits<std::uint64_t>::max() && fence.root.graph && fence.root.layout;
        }

        History history_;
        std::uint64_t serial_{};
    };
}  // namespace Horo::Editor
