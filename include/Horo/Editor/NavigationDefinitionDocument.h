#pragma once
/** @file NavigationDefinitionDocument.h
 * @brief Asset-rooted navigation authoring session; never a second project profile catalog.
 */
#include "Horo/Assets/AssetId.h"
#include "Horo/Navigation/NavigationDefinitionSerialization.h"
#include "Horo/Navigation/NavigationSettingsErrors.h"

#include <memory>
#include <thread>

namespace Horo::Editor {
    class NavigationSettingsWorkflow;

    /** @brief Exact immutable document state; host-issued session never enters authored bytes. */
    struct NavigationDefinitionSnapshot final {
        Assets::AssetId asset;
        std::uint64_t session{};
        std::uint64_t revision{};
        std::uint64_t state{};
        std::shared_ptr<const Navigation::NavigationDefinition> definition;
    };

    /** @brief Detached exact source-save ticket; cook/bake cannot acknowledge it. */
    class NavigationDefinitionSave final {
    public:
        /** @brief Returns the captured canonical source record. @return Immutable record. */
        [[nodiscard]] const Navigation::NavigationAuthoredRecord &Record() const noexcept {
            return record_;
        }

        /** @brief Returns the exact accepted source publication revision. @return Nonzero source revision. */
        [[nodiscard]] std::uint64_t ExpectedSourceRevision() const noexcept {
            return expectedSourceRevision_;
        }

    private:
        friend class NavigationDefinitionDocument;
        NavigationDefinitionSnapshot snapshot_;
        std::uint64_t expectedSourceRevision_{};
        Navigation::NavigationAuthoredRecord record_;
        std::weak_ptr<const void> owner_;
    };

    /** @brief One canonical writable session with bounded semantic undo/redo and source-publication receipts.
     * @details Host admits one session per asset; immutable leases may outlive Close. No job, provider, runtime or GUI state is owned.
     */
    class NavigationDefinitionDocument final {
    public:
        /** @brief Opens a validated accepted source. @param asset Sidecar identity. @param session Fresh nonzero opening.
         * @param sourceRevision Accepted nonzero Assets publication revision. @param definition Sole source authority.
         * @return Clean owner-thread document or typed invalid/capacity failure.
         */
        [[nodiscard]] static Result<NavigationDefinitionDocument> Open(Assets::AssetId asset, std::uint64_t session,
                                                                       std::uint64_t sourceRevision,
                                                                       Navigation::NavigationDefinition definition);
        NavigationDefinitionDocument(NavigationDefinitionDocument &&other) noexcept;
        /** @brief Revokes borrowed workflow handles before releasing document state. */
        ~NavigationDefinitionDocument();
        NavigationDefinitionDocument &operator=(NavigationDefinitionDocument &&) = delete;
        NavigationDefinitionDocument(const NavigationDefinitionDocument &) = delete;
        NavigationDefinitionDocument &operator=(const NavigationDefinitionDocument &) = delete;
        /** @brief Leases exact authored state. @return Owner snapshot or closed/thread failure. */
        [[nodiscard]] Result<NavigationDefinitionSnapshot> Snapshot() const;

        /** @brief Reports unsaved semantic content. @return True until an exact save receipt catches up. */
        [[nodiscard]] bool IsDirty() const noexcept {
            return state_ != savedState_;
        }

        /** @brief Captures canonical source record without clearing dirty state. @param recordId Durable NAVDEF01 record identity.
         * @return Owned exact ticket for Assets atomic source publication.
         */
        [[nodiscard]] Result<NavigationDefinitionSave> CaptureSave(Navigation::NavigationAuthoredRecordId recordId) const;
        /** @brief Accepts a host-proven durable source receipt; later edits remain dirty.
         * @param save Submitted exact ticket. @param publishedRevision New Assets revision. @return Success or stale/closed failure.
         */
        [[nodiscard]] Result<void> AcknowledgeSave(const NavigationDefinitionSave &save, std::uint64_t publishedRevision);
        /** @brief Replays retained semantic content without rerunning a generator.
         * @param expected Exact owner revision. @param redo Replay forward instead of undo. @return New snapshot or zero-mutation failure.
         */
        [[nodiscard]] Result<NavigationDefinitionSnapshot> Replay(const NavigationDefinitionSnapshot &expected, bool redo);

        /** @brief Closes admission and leaves issued immutable leases valid. */
        void Close() noexcept {
            closed_ = true;
            if (lifetime_)
                lifetime_->owner = nullptr;
        }

    private:
        friend class NavigationSettingsWorkflow;

        struct Lifetime final {
            NavigationDefinitionDocument *owner{};
        };

        std::shared_ptr<Lifetime> lifetime_;

        struct History final {
            std::shared_ptr<const Navigation::NavigationDefinition> source;
            std::uint64_t state{};
            std::size_t bytes{};
            bool coordinated{};
        };

        struct Prepared final {
            History next;
            std::vector<History> undo;
            std::vector<History> redo;
            bool replay{};
        };

        NavigationDefinitionDocument() = default;
        /** @brief Checks owner-thread/session/revision before a transaction. */
        [[nodiscard]] Result<void> Admit(const NavigationDefinitionSnapshot &expected) const;
        /** @brief Reserves the complete successor and history before any Scene publication. */
        [[nodiscard]] Result<Prepared> Prepare(const NavigationDefinitionSnapshot &expected, Navigation::NavigationDefinitionInput input,
                                               bool coordinated = false) const;
        /** @brief Stages the exact retained semantic inverse/redo without publication. */
        [[nodiscard]] Result<Prepared> PrepareReplay(const NavigationDefinitionSnapshot &expected, bool redo) const;
        /** @brief Swaps prevalidated state without allocation/callbacks after all owner fences pass. */
        void Publish(Prepared &prepared) noexcept;
        Assets::AssetId asset_;
        std::uint64_t session_{};
        std::uint64_t revision_{1};
        std::uint64_t state_{1};
        std::uint64_t savedState_{1};
        std::uint64_t nextState_{2};
        std::uint64_t sourceRevision_{};
        std::shared_ptr<const Navigation::NavigationDefinition> source_;
        std::vector<History> undo_;
        std::vector<History> redo_;
        std::thread::id owner_{std::this_thread::get_id()};
        bool closed_{};
    };

    /** @brief Copies the sole source into an editable detached candidate. @param source Validated definition. @return Owned authored
     * values. */
    [[nodiscard]] Navigation::NavigationDefinitionInput CopyNavigationDefinition(const Navigation::NavigationDefinition &source);
}  // namespace Horo::Editor
