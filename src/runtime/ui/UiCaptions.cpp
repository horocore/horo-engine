#include "Horo/Runtime/Ui/UiCaptions.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace Horo::Runtime::Ui::UiErrors {
    /** @copydoc CaptionInvalid */
    const ErrorCodeDescriptor CaptionInvalid{ErrorDomainId{"horo.runtime_ui"},
                                             ErrorCode{"runtime_ui.caption.invalid"},
                                             ErrorSeverity::Error,
                                             "Caption input is malformed.",
                                             "Use a nonzero session, bounded catalog and validated presentation.",
                                             false,
                                             false};
    /** @copydoc CaptionCapacityExceeded */
    const ErrorCodeDescriptor CaptionCapacityExceeded{ErrorDomainId{"horo.runtime_ui"},
                                                      ErrorCode{"runtime_ui.caption.capacity_exceeded"},
                                                      ErrorSeverity::Error,
                                                      "Caption storage could not be reserved.",
                                                      "Retry setup within the caption storage budget.",
                                                      false,
                                                      false};
}  // namespace Horo::Runtime::Ui::UiErrors

namespace Horo::Runtime::Ui {
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

    namespace {
        /** @brief Validates unique bounded cue identities and positive measured extents before arranging any placement. */
        bool AreMeasuredCaptionsValid(const std::span<const CaptionMeasuredCue> measured) noexcept {
            if (measured.size() > MaximumCaptionCues)
                return false;
            for (std::size_t i = 0; i < measured.size(); ++i) {
                if (measured[i].cue.session == 0 || measured[i].cue.sequence == 0 || !measured[i].extent.IsValid() ||
                    measured[i].extent.width == 0 || measured[i].extent.height == 0)
                    return false;
                for (std::size_t j = 0; j < i; ++j)
                    if (measured[i].cue == measured[j].cue)
                        return false;
            }
            return true;
        }

        /** @brief Checks the fixed-point bounds before narrowing safe-area edges. */
        bool CaptionViewportValid(const UiLogicalRect &viewport) noexcept {
            constexpr auto Limit = std::numeric_limits<std::int32_t>::max();
            return viewport.IsValid() && viewport.extent.width > 0 && viewport.extent.height > 0 &&
                   static_cast<std::int64_t>(viewport.origin.x) + viewport.extent.width <= Limit &&
                   static_cast<std::int64_t>(viewport.origin.y) + viewport.extent.height <= Limit;
        }

        struct CaptionBand final {
            std::int32_t top;
            std::int32_t bottom;
        };

        /** @brief Finds the nearest vertical gap at the chosen horizontal position, with earlier placements retained. */
        std::optional<std::int32_t> FindCaptionGap(const CaptionLayout &layout, const UiLogicalExtent size, const std::int32_t x,
                                                   const std::int32_t preferredY) {
            std::array<CaptionBand, MaximumCaptionCues> storage{};
            std::size_t count = 0;
            for (const auto &placement : std::span{layout.placements}.first(layout.count)) {
                const auto &rect = placement.bounds;
                if (x < rect.origin.x + rect.extent.width && rect.origin.x < x + size.width)
                    storage[count++] = {rect.origin.y, rect.origin.y + rect.extent.height};
            }
            const auto occupied = std::span{storage}.first(count);
            std::ranges::sort(occupied, {}, &CaptionBand::top);
            std::optional<std::int32_t> closest;
            auto bestDistance = std::numeric_limits<std::int64_t>::max();
            const auto consider = [&closest, &bestDistance, preferredY, height = size.height](const std::int32_t start,
                                                                                              const std::int32_t end) {
                if (static_cast<std::int64_t>(end) - start < height)
                    return;
                const auto y = std::clamp(preferredY, start, end - height);
                const auto distance = std::abs(static_cast<std::int64_t>(y) - preferredY);
                if (distance < bestDistance) {
                    closest = y;
                    bestDistance = distance;
                }
            };
            auto cursor = layout.safeArea.origin.y;
            for (const auto band : occupied) {
                consider(cursor, band.top);
                cursor = std::max(cursor, band.bottom);
            }
            consider(cursor, layout.safeArea.origin.y + layout.safeArea.extent.height);
            return closest;
        }

        /** @brief Preserves the speaker's horizontal anchor and uses the bottom center when it is unavailable. */
        std::optional<UiLogicalPoint> FollowSpeakerOrigin(const CaptionLayout &layout, const CaptionMeasuredCue &entry) {
            const auto &safe = layout.safeArea;
            auto x = safe.origin.x + (safe.extent.width - entry.extent.width) / 2;
            auto preferredY = safe.origin.y + safe.extent.height - entry.extent.height;
            if (entry.speaker) {
                const auto requestedX = static_cast<std::int64_t>(entry.speaker->x) - entry.extent.width / 2;
                x = static_cast<std::int32_t>(
                    std::clamp(requestedX, static_cast<std::int64_t>(safe.origin.x),
                               static_cast<std::int64_t>(safe.origin.x) + safe.extent.width - entry.extent.width));
                preferredY = entry.speaker->y;
            }
            if (const auto y = FindCaptionGap(layout, entry.extent, x, preferredY); y.has_value())
                return UiLogicalPoint{x, *y};
            return std::nullopt;
        }

        /** @brief Reserves space for one complete caption and preserves existing placements on failure. */
        std::optional<UiLogicalPoint> PlaceCaption(const CaptionLayout &layout, const CaptionMeasuredCue &entry,
                                                   const CaptionPosition position, CaptionBand &stack) {
            const auto &size = entry.extent;
            if (size.width > layout.safeArea.extent.width || size.height > layout.safeArea.extent.height)
                return std::nullopt;
            if (position == CaptionPosition::FollowSpeaker)
                return FollowSpeakerOrigin(layout, entry);
            if (static_cast<std::int64_t>(stack.bottom) - stack.top < size.height)
                return std::nullopt;
            const auto x = layout.safeArea.origin.x + (layout.safeArea.extent.width - size.width) / 2;
            if (position == CaptionPosition::Top) {
                const auto y = stack.top;
                stack.top += size.height;
                return UiLogicalPoint{x, y};
            }
            stack.bottom -= size.height;
            return UiLogicalPoint{x, stack.bottom};
        }
    }  // namespace

    /** @copydoc CaptionAppearance::IsValid */
    bool CaptionAppearance::IsValid() const noexcept {
        const auto normalized = [](const float value) {
            return std::isfinite(value) && value >= 0 && value <= 1;
        };
        return std::isfinite(fontScale) && fontScale >= 0.5F && fontScale <= 3.0F && std::ranges::all_of(textColor, normalized) &&
               std::ranges::all_of(backgroundColor, normalized) && normalized(backgroundOpacity) &&
               position <= CaptionPosition::FollowSpeaker && std::isfinite(safeAreaMarginPercent) && safeAreaMarginPercent >= 0 &&
               safeAreaMarginPercent <= 25;
    }

    /** @copydoc ArrangeCaptions */
    Result<CaptionLayout> ArrangeCaptions(const UiLogicalRect viewport, const CaptionAppearance &appearance,
                                          const std::span<const CaptionMeasuredCue> measured) {
        if (!CaptionViewportValid(viewport) || !appearance.IsValid() || !AreMeasuredCaptionsValid(measured))
            return Result<CaptionLayout>::Failure(MakeError(UiErrors::CaptionInvalid));
        CaptionLayout output;
        const auto marginX =
            static_cast<std::int32_t>(std::ceil(viewport.extent.width * static_cast<double>(appearance.safeAreaMarginPercent) / 100.0));
        const auto marginY =
            static_cast<std::int32_t>(std::ceil(viewport.extent.height * static_cast<double>(appearance.safeAreaMarginPercent) / 100.0));
        output.safeArea = {{viewport.origin.x + marginX, viewport.origin.y + marginY},
                           {viewport.extent.width - 2 * marginX, viewport.extent.height - 2 * marginY}};
        if (output.safeArea.extent.width <= 0 || output.safeArea.extent.height <= 0)
            return Result<CaptionLayout>::Failure(MakeError(UiErrors::CaptionInvalid));
        if (!appearance.enabled)
            return Result<CaptionLayout>::Success(output);
        CaptionBand stack{output.safeArea.origin.y, output.safeArea.origin.y + output.safeArea.extent.height};
        for (const auto &entry : measured) {
            if (const auto origin = PlaceCaption(output, entry, appearance.position, stack))
                output.placements[output.count++] = {entry.cue, {*origin, entry.extent}};
            else
                ++output.unplaced;
        }
        return Result<CaptionLayout>::Success(output);
    }

    /** @copydoc CaptionCueQueue::Create */
    Result<std::unique_ptr<CaptionCueQueue>> CaptionCueQueue::Create(const std::uint64_t session, std::vector<LocalizedMessageRef> catalog,
                                                                     const CaptionProducerKind producer) {
        if (session == 0 || producer > CaptionProducerKind::AudioOnly || catalog.empty() || catalog.size() > 4096 ||
            std::ranges::any_of(catalog, [](const auto &text) {
            return !text.IsValid();
        })) {
            return Result<std::unique_ptr<CaptionCueQueue>>::Failure(MakeError(UiErrors::CaptionInvalid));
        }
        try {
            return Result<std::unique_ptr<CaptionCueQueue>>::Success(std::unique_ptr<CaptionCueQueue>(
                new CaptionCueQueue(session, std::make_shared<const std::vector<LocalizedMessageRef>>(std::move(catalog)), producer)));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<CaptionCueQueue>>::Failure(MakeError(UiErrors::CaptionCapacityExceeded));
        }
    }

    /** @copydoc CaptionCueQueue::CaptionCueQueue */
    CaptionCueQueue::CaptionCueQueue(const std::uint64_t session, std::shared_ptr<const std::vector<LocalizedMessageRef>> catalog,
                                     const CaptionProducerKind producer)
        : session_(session), producer_(producer), catalog_(std::move(catalog)) {}

    /** @copydoc CaptionCueQueue::TryEnqueue */
    CaptionAdmission CaptionCueQueue::TryEnqueue(const CaptionCue &cue) noexcept {
        if (cue.id.session != session_)
            return CaptionAdmission::StaleSession;
        if (cue.id.sequence == 0 || cue.text >= catalog_->size() || cue.speaker > catalog_->size() || cue.kind > CaptionCueKind::Music ||
            (producer_ == CaptionProducerKind::AudioOnly && cue.kind == CaptionCueKind::Dialogue) || cue.durationTicks == 0 ||
            cue.startTick > std::numeric_limits<std::uint64_t>::max() - cue.durationTicks)
            return CaptionAdmission::Invalid;
        const auto write = write_.load(std::memory_order_relaxed);
        const auto next = (write + 1) % slots_.size();
        if (next == read_.load(std::memory_order_acquire)) {
            const auto dropped = dropped_.load(std::memory_order_relaxed);
            if (dropped != std::numeric_limits<std::uint32_t>::max())
                dropped_.store(dropped + 1, std::memory_order_relaxed);
            return CaptionAdmission::Full;
        }
        slots_[write] = cue;
        write_.store(static_cast<std::uint32_t>(next), std::memory_order_release);
        return CaptionAdmission::Accepted;
    }

    /** @copydoc CaptionCueQueue::TryDequeue */
    bool CaptionCueQueue::TryDequeue(CaptionCue &cue) noexcept {
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire))
            return false;
        cue = slots_[read];
        read_.store(static_cast<std::uint32_t>((read + 1) % slots_.size()), std::memory_order_release);
        return true;
    }

    /** @copydoc CaptionCueQueue::Catalog */
    std::shared_ptr<const std::vector<LocalizedMessageRef>> CaptionCueQueue::Catalog() const noexcept {
        return catalog_;
    }

    /** @copydoc CaptionCueQueue::Session */
    std::uint64_t CaptionCueQueue::Session() const noexcept {
        return session_;
    }

    /** @copydoc CaptionCueQueue::Producer */
    CaptionProducerKind CaptionCueQueue::Producer() const noexcept {
        return producer_;
    }

    /** @copydoc CaptionCueQueue::Dropped */
    std::uint32_t CaptionCueQueue::Dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

    /** @copydoc CaptionTimeline::Reset */
    Result<void> CaptionTimeline::Reset(const std::uint64_t session) {
        if (session != 0 && session <= lastSession_)
            return Result<void>::Failure(MakeError(UiErrors::CaptionInvalid));
        ClearSession(session);
        if (session != 0)
            lastSession_ = session;
        return Result<void>::Success();
    }

    /** @copydoc CaptionTimeline::ClearSession */
    void CaptionTimeline::ClearSession(const std::uint64_t session) noexcept {
        active_ = {};
        count_ = 0;
        tick_ = 0;
        highestSequence_ = 0;
        seen_ = 0;
        session_ = session;
    }

    /** @copydoc CaptionTimeline::BeginFrame */
    void CaptionTimeline::BeginFrame(const std::uint64_t tick) noexcept {
        if (tick < tick_)
            ClearSession(0);
        tick_ = tick;
        std::size_t retained = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            if (active_[i].cue.startTick + active_[i].cue.durationTicks > tick_) {
                if (retained != i)
                    active_[retained] = std::move(active_[i]);
                ++retained;
            }
        }
        for (auto i = retained; i < count_; ++i)
            active_[i] = {};
        count_ = retained;
    }

    /** @copydoc CaptionTimeline::Drain */
    std::size_t CaptionTimeline::Drain(CaptionCueQueue &queue) noexcept {
        std::size_t dropped = 0;
        CaptionCue cue;
        for (std::size_t i = 0; i < MaximumCaptionCues && queue.TryDequeue(cue); ++i) {
            if (session_ == 0 || cue.id.session != session_)
                continue;
            if (cue.id.sequence > highestSequence_) {
                const auto shift = cue.id.sequence - highestSequence_;
                seen_ = shift >= 64 ? 0 : seen_ << shift;
                highestSequence_ = cue.id.sequence;
            }
            const auto age = highestSequence_ - cue.id.sequence;
            if (age >= 64 || (seen_ & (std::uint64_t{1} << age)) != 0)
                continue;
            seen_ |= std::uint64_t{1} << age;
            if (cue.startTick + cue.durationTicks <= tick_)
                continue;
            if (count_ == active_.size()) {
                ++dropped;
                continue;
            }
            active_[count_++] = {cue, queue.Catalog(), queue.Producer(), {}};
        }
        std::sort(active_.begin(), active_.begin() + count_, [](const auto &a, const auto &b) {
            if (a.cue.startTick != b.cue.startTick)
                return a.cue.startTick < b.cue.startTick;
            return a.cue.id < b.cue.id;
        });
        return dropped;
    }

    /** @copydoc CaptionTimeline::ObserveAudioTiming */
    Result<std::size_t> CaptionTimeline::ObserveAudioTiming(const std::span<const CaptionTimingObservation> observations) {
        if (observations.size() > MaximumCaptionCues)
            return Result<std::size_t>::Failure(MakeError(UiErrors::CaptionInvalid));
        std::size_t matched = 0;
        for (const auto &observation : observations) {
            if (session_ == 0 || observation.id.session != session_ || observation.id.sequence == 0)
                continue;
            for (std::size_t i = 0; i < count_; ++i) {
                auto &active = active_[i];
                if (active.producer == CaptionProducerKind::Semantic && active.cue.id == observation.id && !active.observedAudioStart) {
                    active.observedAudioStart = observation.observedStartTick;
                    ++matched;
                    break;
                }
            }
        }
        return Result<std::size_t>::Success(matched);
    }

    /** @copydoc CaptionTimeline::Cues */
    std::span<const CaptionActiveCue> CaptionTimeline::Cues() const noexcept {
        return {active_.data(), count_};
    }

    /** @copydoc CaptionTimeline::Tick */
    std::uint64_t CaptionTimeline::Tick() const noexcept {
        return tick_;
    }
}  // namespace Horo::Runtime::Ui
