#include "EditorPlaySaveRetirement.h"

#include "EditorPlaySceneHost.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "ProjectGameplayRegistry.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <new>
#include <utility>

namespace Horo::Editor {
    /** @brief Constructs an empty cold slot without allocating a Scene, callback or cancellation source. */
    EditorPlayRetiredUnit::EditorPlayRetiredUnit() = default;
    /** @brief Destroys only empty or actually settled unit fields in their dependency order. */
    EditorPlayRetiredUnit::~EditorPlayRetiredUnit() = default;
    /** @brief Transfers the exact owner bundle without replacing any runtime identity. */
    EditorPlayRetiredUnit::EditorPlayRetiredUnit(EditorPlayRetiredUnit &&) noexcept = default;
    /** @brief Transfers into an empty, reserved physical slot. */
    EditorPlayRetiredUnit &EditorPlayRetiredUnit::operator=(EditorPlayRetiredUnit &&) noexcept = default;

    /** @copydoc EditorPlayRetiredUnit::Poll */
    Result<bool> EditorPlayRetiredUnit::Poll() {
        if (!scene)
            return Result<bool>::Success(true);
        if (sceneClosed_)
            return PollNativeRetirement();
        if (frame == std::numeric_limits<std::uint64_t>::max())
            return Result<bool>::Failure(MakeError(Runtime::SaveErrors::LifecycleUnavailable));
        const Runtime::FrameContext context{.frameNumber = ++frame, .cancellation = cancellation};
        const auto committed = scene->CommitSceneChanges(context);
        if (auto *save = scene->SaveSession(); save) {
            const auto pumped = save->CommitAtSafePoint(context);
            if (!save->IsSettled())
                return pumped.HasError() ? Result<bool>::Failure(pumped.ErrorValue()) : Result<bool>::Success(false);
        }
        if (committed.HasError())
            LOG_WARN("editor.play.save", "Settled Play Scene retired after cancellation diagnostic: %s",
                     committed.ErrorValue().message.c_str());
        if (binding)
            binding->runtime = nullptr;
        scene->Shutdown();
        behaviorOwner.reset();
        sceneClosed_ = true;
        return PollNativeRetirement();
    }

    /** @copydoc EditorPlayRetiredUnit::PollNativeRetirement */
    Result<bool> EditorPlayRetiredUnit::PollNativeRetirement() {
        auto native = scene->PollShutdown(64);
        if (native.HasError())
            return Result<bool>::Failure(native.ErrorValue());
        if (!native.Value())
            return Result<bool>::Success(false);
        scene.reset();
        registry.reset();
        binding.reset();
        return Result<bool>::Success(true);
    }

    /** @copydoc EditorPlaySaveRetirement::EditorPlaySaveRetirement */
    EditorPlaySaveRetirement::EditorPlaySaveRetirement() : EditorPlaySaveRetirement(4) {}

    /** @copydoc EditorPlaySaveRetirement::EditorPlaySaveRetirement */
    EditorPlaySaveRetirement::EditorPlaySaveRetirement(std::size_t capacity) : slots_(capacity) {}

    /** @brief Refuses to destroy a still-owned Play unit outside the explicit application shutdown ordering. */
    EditorPlaySaveRetirement::~EditorPlaySaveRetirement() {
        if (HasPending())
            std::terminate();
    }

    /** @copydoc EditorPlaySaveRetirement::Create */
    Result<std::shared_ptr<EditorPlaySaveRetirement>> EditorPlaySaveRetirement::Create(std::size_t capacity) {
        using Outcome = Result<std::shared_ptr<EditorPlaySaveRetirement>>;
        if (capacity == 0 || capacity > 8)
            return Outcome::Failure(MakeError(Runtime::SaveErrors::CompositionUnsupported));
        try {
            return Outcome::Success(std::shared_ptr<EditorPlaySaveRetirement>(new EditorPlaySaveRetirement(capacity)));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(Runtime::SaveErrors::StorageAllocationFailed));
        }
    }

    /** @copydoc EditorPlaySaveRetirement::Reserve */
    Result<EditorPlaySaveRetirement::Reservation> EditorPlaySaveRetirement::Reserve() {
        using Outcome = Result<Reservation>;
        if (std::this_thread::get_id() != owner_)
            return Outcome::Failure(MakeError(Runtime::SaveErrors::ThreadAffinityViolation));
        if (closed_)
            return Outcome::Failure(MakeError(Runtime::SaveErrors::LifecycleUnavailable));
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            if (slots_[index].state != SlotState::Free)
                continue;
            slots_[index].state = SlotState::Reserved;
            return Outcome::Success(Reservation{shared_from_this(), index});
        }
        return Outcome::Failure(MakeError(Runtime::SaveErrors::OperationInProgress));
    }

    /** @copydoc EditorPlaySaveRetirement::Poll */
    Result<std::size_t> EditorPlaySaveRetirement::Poll() {
        using Outcome = Result<std::size_t>;
        if (std::this_thread::get_id() != owner_)
            return Outcome::Failure(MakeError(Runtime::SaveErrors::ThreadAffinityViolation));
        std::size_t reclaimed{};
        std::optional<Error> failure;
        for (auto &slot : slots_) {
            if (slot.state != SlotState::Retiring)
                continue;
            const auto result = slot.unit.Poll();
            if (result.HasError()) {
                if (!failure)
                    failure = result.ErrorValue();
                continue;
            }
            if (result.Value()) {
                slot.state = SlotState::Free;
                ++reclaimed;
            }
        }
        return failure ? Outcome::Failure(std::move(*failure)) : Outcome::Success(reclaimed);
    }

    /** @copydoc EditorPlaySaveRetirement::Close */
    Result<void> EditorPlaySaveRetirement::Close() {
        if (std::this_thread::get_id() != owner_)
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::ThreadAffinityViolation));
        closed_ = true;
        if (const auto polled = Poll(); polled.HasError())
            return Result<void>::Failure(polled.ErrorValue());
        return HasPending() ? Result<void>::Failure(MakeError(Runtime::SaveErrors::OperationInProgress)) : Result<void>::Success();
    }

    /** @copydoc EditorPlaySaveRetirement::HasPending */
    bool EditorPlaySaveRetirement::HasPending() const noexcept {
        return std::ranges::any_of(slots_, [](const auto &slot) {
            return slot.state == SlotState::Retiring;
        });
    }

    /** @brief Releases only an unused cold reservation; active retirement remains owned by the application. */
    void EditorPlaySaveRetirement::Release(std::size_t index) noexcept {
        if (std::this_thread::get_id() != owner_ || index >= slots_.size())
            std::terminate();
        if (slots_[index].state == SlotState::Reserved)
            slots_[index].state = SlotState::Free;
    }

    /** @brief Pins the explicitly reserved application queue until transfer or unused release. */
    EditorPlaySaveRetirement::Reservation::Reservation(std::shared_ptr<EditorPlaySaveRetirement> owner, std::size_t index) noexcept
        : owner_(std::move(owner)), index_(index) {}

    /** @brief Returns unused admission without releasing an actual retiring Scene. */
    EditorPlaySaveRetirement::Reservation::~Reservation() {
        if (owner_)
            owner_->Release(index_);
    }

    /** @brief Transfers the one reserved physical slot without allocating. */
    EditorPlaySaveRetirement::Reservation::Reservation(Reservation &&other) noexcept
        : owner_(std::move(other.owner_)), index_(other.index_) {}

    /** @brief Releases prior unused admission before adopting the replacement reservation. */
    EditorPlaySaveRetirement::Reservation &EditorPlaySaveRetirement::Reservation::operator=(Reservation &&other) noexcept {
        if (this != &other) {
            if (owner_)
                owner_->Release(index_);
            owner_ = std::move(other.owner_);
            index_ = other.index_;
        }
        return *this;
    }

    /** @copydoc EditorPlaySaveRetirement::Reservation::IsValid */
    bool EditorPlaySaveRetirement::Reservation::IsValid() const noexcept {
        return static_cast<bool>(owner_);
    }

    /** @copydoc EditorPlaySaveRetirement::Reservation::Retain */
    void EditorPlaySaveRetirement::Reservation::Retain(EditorPlayRetiredUnit unit) noexcept {
        if (!owner_ || std::this_thread::get_id() != owner_->owner_ || owner_->slots_[index_].state != SlotState::Reserved)
            std::terminate();
        if (unit.binding && !unit.binding->revoked)
            std::terminate();
        auto &slot = owner_->slots_[index_];
        slot.unit = std::move(unit);
        slot.state = SlotState::Retiring;
        owner_.reset();
    }

    /** @copydoc ClosePlaySaveRetirementAfterJobs */
    void ClosePlaySaveRetirementAfterJobs(EditorPlaySaveRetirement *owner) noexcept {
        if (!owner)
            return;
        // The scheduler has already joined. Only finite Scene owner transitions
        // remain; no worker wait occurs here.
        for (std::size_t phase = 0; phase < Runtime::MaximumSaveParticipantCount + 8; ++phase) {
            try {
                const auto result = owner->Close();
                if (result.HasValue())
                    return;
                if (phase == Runtime::MaximumSaveParticipantCount + 7)
                    std::fprintf(stderr, "Play save retirement failed after scheduler join [%s]: %s\n",
                                 result.ErrorValue().code.Value().c_str(), result.ErrorValue().message.c_str());
            } catch (...) {
                std::fputs("Play save retirement raised after scheduler join; refusing "
                           "premature module teardown.\n",
                           stderr);
                break;
            }
        }
        std::terminate();
    }
}  // namespace Horo::Editor
