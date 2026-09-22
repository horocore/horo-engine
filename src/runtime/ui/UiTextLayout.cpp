#include "UiTextLayoutInternal.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <new>
#include <ranges>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace UiTextLayoutInternal {
        /** @brief Scales one logical coordinate with checked ties-to-even rounding. */
        Result<std::int32_t> ScaleValue(const std::int32_t value, const UiTextScale scale) {
            const std::int64_t product = static_cast<std::int64_t>(value) * static_cast<std::int64_t>(scale.value);
            const std::int64_t denominator = UiTextLayoutScaleUnit;
            const std::int64_t quotient = product / denominator;
            const std::int64_t remainder = product % denominator;
            const std::int64_t absoluteRemainder = remainder < 0 ? -remainder : remainder;
            std::int64_t rounded = quotient;
            if (absoluteRemainder * 2 > denominator || (absoluteRemainder * 2 == denominator && (rounded & 1) != 0))
                rounded += product < 0 ? -1 : 1;
            if (rounded < std::numeric_limits<std::int32_t>::min() || rounded > std::numeric_limits<std::int32_t>::max())
                return Failure<std::int32_t>(UiErrors::TextLayoutCapacityExceeded);
            return Result<std::int32_t>::Success(static_cast<std::int32_t>(rounded));
        }

        /** @brief Adds logical coordinates while rejecting signed 32-bit overflow. */
        Result<std::int32_t> AddValue(const std::int64_t left, const std::int64_t right) {
            const auto value = left + right;
            if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max())
                return Failure<std::int32_t>(UiErrors::TextLayoutCapacityExceeded);
            return Result<std::int32_t>::Success(static_cast<std::int32_t>(value));
        }

        /** @brief Reports whether any source lineage component regressed. */
        bool IsOlder(const UiLayoutSourceRevisions &candidate, const UiLayoutSourceRevisions &current) noexcept {
            return candidate.document < current.document || candidate.tree < current.tree || candidate.content < current.content ||
                   candidate.style < current.style || candidate.intrinsic < current.intrinsic || candidate.canvas < current.canvas ||
                   candidate.policy < current.policy;
        }
    }  // namespace UiTextLayoutInternal

    UiTextLayoutResult::Storage::Storage(const UiTextLayoutLimits &limits) {
        lines.reserve(limits.lines);
        glyphs.reserve(limits.glyphs);
        runs.reserve(limits.runs);
    }

    void UiTextLayoutResult::Storage::Reset() noexcept {
        descriptor = {};
        measurement = {};
        overflow = {};
        lines.clear();
        glyphs.clear();
        runs.clear();
    }

    UiTextLayoutEngine::Storage::Storage(const UiTextLayoutEngineDescriptor &source) : descriptor(source) {
        slots.reserve(source.concurrentResults);
        for (std::uint32_t index = 0; index < source.concurrentResults; ++index)
            slots.push_back(std::make_shared<UiTextLayoutResult::Storage>(source.limits));
        scaledClusterAdvances.reserve(source.limits.clusters);
        clusterPrefix.reserve(static_cast<std::size_t>(source.limits.clusters) + 1U);
        softBreaks.reserve(source.limits.clusters);
        linePlans.reserve(source.limits.lines);
        glyphFaces.reserve(source.limits.glyphs);
        ellipsisFaces.reserve(source.limits.glyphs);
    }

    std::shared_ptr<UiTextLayoutResult::Storage> UiTextLayoutEngine::Storage::TryAcquire() noexcept {
        for (std::size_t offset = 0; offset < slots.size(); ++offset) {
            const auto index = (nextSlot + offset) % slots.size();
            if (std::uint64_t expected{}; !slots[index]->leases.compare_exchange_strong(expected, 1))
                continue;
            nextSlot = (index + 1) % slots.size();
            return slots[index];
        }
        return {};
    }

    void UiTextLayoutEngine::Storage::ReleaseSlot(const std::shared_ptr<UiTextLayoutResult::Storage> &slot) const noexcept {
        slot->Reset();
        slot->leases.store(0);
    }

    Result<void> UiTextLayoutEngine::Storage::BuildFaceTable(const UiTextShapedTextView &view, std::vector<UiTextFaceId> &output) const {
        output.resize(view.glyphs.size());
        for (const auto &run : view.runs)
            for (std::uint32_t glyph = run.firstGlyph; glyph < run.firstGlyph + run.glyphCount; ++glyph)
                output[glyph] = run.face;
        return Result<void>::Success();
    }

    Result<std::int64_t> UiTextLayoutEngine::Storage::ShapedWidth(const UiTextShapedTextView &view, const UiTextScale scale) const {
        std::int64_t width{};
        for (const auto &cluster : view.clusters) {
            const auto advance = UiTextLayoutInternal::ScaleValue(cluster.advance.x, scale);
            if (advance.HasError())
                return Result<std::int64_t>::Failure(advance.ErrorValue());
            width += advance.Value();
        }
        return Result<std::int64_t>::Success(width);
    }

    Result<void> UiTextLayoutEngine::Storage::AppendGlyph(UiTextLayoutResult::Storage &slot,
                                                          const UiTextLayoutInternal::GlyphAppend &placement) const {
        if (slot.glyphs.size() >= descriptor.limits.glyphs)
            return UiTextLayoutInternal::Failure(UiErrors::TextLayoutCapacityExceeded);
        if (const auto first = static_cast<std::uint32_t>(slot.glyphs.size());
            slot.runs.empty() || slot.runs.back().face != placement.face || slot.runs.back().line != placement.line ||
            slot.runs.back().ellipsis != placement.ellipsis || slot.runs.back().firstGlyph + slot.runs.back().glyphCount != first) {
            if (slot.runs.size() >= descriptor.limits.runs)
                return UiTextLayoutInternal::Failure(UiErrors::TextLayoutCapacityExceeded);
            slot.runs.emplace_back(placement.face, first, 0, placement.line, placement.ellipsis);
        }
        slot.glyphs.emplace_back(placement.face, placement.glyph, placement.cluster, placement.origin, placement.advance);
        ++slot.runs.back().glyphCount;
        return Result<void>::Success();
    }

    bool UiTextLayoutEngine::Storage::IsDrained() const noexcept {
        return std::ranges::all_of(slots, [](const auto &slot) {
            return slot->leases.load() == 0;
        });
    }

    /** @copydoc UiTextLayoutResult::UiTextLayoutResult(std::shared_ptr<const Storage>) */
    UiTextLayoutResult::UiTextLayoutResult(std::shared_ptr<const Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiTextLayoutResult::~UiTextLayoutResult */
    UiTextLayoutResult::~UiTextLayoutResult() {
        Release();
    }

    /** @copydoc UiTextLayoutResult::UiTextLayoutResult(const UiTextLayoutResult &) */
    UiTextLayoutResult::UiTextLayoutResult(const UiTextLayoutResult &other) noexcept : storage_(other.storage_) {
        Retain();
    }

    /** @copydoc UiTextLayoutResult::operator= */
    UiTextLayoutResult &UiTextLayoutResult::operator=(const UiTextLayoutResult &other) noexcept {
        if (this != &other) {
            UiTextLayoutResult replacement{other};
            *this = std::move(replacement);
        }
        return *this;
    }

    /** @copydoc UiTextLayoutResult::UiTextLayoutResult(UiTextLayoutResult &&) */
    UiTextLayoutResult::UiTextLayoutResult(UiTextLayoutResult &&other) noexcept : storage_(std::move(other.storage_)) {}

    /** @copydoc UiTextLayoutResult::operator= */
    UiTextLayoutResult &UiTextLayoutResult::operator=(UiTextLayoutResult &&other) noexcept {
        if (this != &other) {
            Release();
            storage_ = std::move(other.storage_);
        }
        return *this;
    }

    /** @copydoc UiTextLayoutResult::Retain */
    void UiTextLayoutResult::Retain() const noexcept {
        if (!storage_)
            return;
        auto current = storage_->leases.load();
        while (current != std::numeric_limits<std::uint64_t>::max()) {
            if (storage_->leases.compare_exchange_weak(current, current + 1))
                return;
        }
        std::terminate();
    }

    /** @copydoc UiTextLayoutResult::Release */
    void UiTextLayoutResult::Release() noexcept {
        if (!storage_)
            return;
        storage_->leases.fetch_sub(1);
        storage_.reset();
    }

    /** @copydoc UiTextLayoutResult::Descriptor */
    const UiTextLayoutResultDescriptor &UiTextLayoutResult::Descriptor() const noexcept {
        return storage_->descriptor;
    }

    /** @copydoc UiTextLayoutResult::Measurement */
    const UiLayoutMeasurement &UiTextLayoutResult::Measurement() const noexcept {
        return storage_->measurement;
    }

    /** @copydoc UiTextLayoutResult::Overflow */
    const UiTextLayoutOverflow &UiTextLayoutResult::Overflow() const noexcept {
        return storage_->overflow;
    }

    /** @copydoc UiTextLayoutResult::Lines */
    std::span<const UiTextLayoutLine> UiTextLayoutResult::Lines() const noexcept {
        return storage_->lines;
    }

    /** @copydoc UiTextLayoutResult::Glyphs */
    std::span<const UiTextLayoutGlyph> UiTextLayoutResult::Glyphs() const noexcept {
        return storage_->glyphs;
    }

    /** @copydoc UiTextLayoutResult::Runs */
    std::span<const UiTextLayoutRun> UiTextLayoutResult::Runs() const noexcept {
        return storage_->runs;
    }

    /** @copydoc UiTextLayoutResult::IsValid */
    bool UiTextLayoutResult::IsValid() const noexcept {
        return static_cast<bool>(storage_);
    }

    /** @copydoc UiTextLayoutEngine::Create */
    Result<UiTextLayoutEngine> UiTextLayoutEngine::Create(const UiTextLayoutEngineDescriptor &descriptor) {
        if (!descriptor.IsValid())
            return UiTextLayoutInternal::Failure<UiTextLayoutEngine>(UiErrors::TextLayoutInputInvalid);
        try {
            return Result<UiTextLayoutEngine>::Success(UiTextLayoutEngine{std::make_unique<Storage>(descriptor)});
        } catch (const std::bad_alloc &) {
            return UiTextLayoutInternal::Failure<UiTextLayoutEngine>(UiErrors::TextLayoutCapacityExceeded);
        }
    }

    /** @copydoc UiTextLayoutEngine::UiTextLayoutEngine(std::unique_ptr<Storage>) */
    UiTextLayoutEngine::UiTextLayoutEngine(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiTextLayoutEngine::~UiTextLayoutEngine */
    UiTextLayoutEngine::~UiTextLayoutEngine() {
        Close();
    }

    /** @copydoc UiTextLayoutEngine::UiTextLayoutEngine(UiTextLayoutEngine &&) */
    UiTextLayoutEngine::UiTextLayoutEngine(UiTextLayoutEngine &&other) noexcept = default;

    /** @copydoc UiTextLayoutEngine::operator= */
    UiTextLayoutEngine &UiTextLayoutEngine::operator=(UiTextLayoutEngine &&other) noexcept = default;

    /** @copydoc UiTextLayoutEngine::Layout */
    Result<UiTextLayoutResult> UiTextLayoutEngine::Layout(const UiTextLayoutRequest &request) {
        if (!storage_)
            return UiTextLayoutInternal::Failure<UiTextLayoutResult>(UiErrors::TextLayoutLifecycleUnavailable);
        auto published = storage_->Layout(request);
        if (published.HasError())
            return Result<UiTextLayoutResult>::Failure(published.ErrorValue());
        return Result<UiTextLayoutResult>::Success(UiTextLayoutResult{std::move(published).Value()});
    }

    /** @copydoc UiTextLayoutEngine::Close */
    void UiTextLayoutEngine::Close() noexcept {
        if (!storage_)
            return;
        storage_->lifecycle = UiTextLayoutEngineState::Closed;
        storage_->scaledClusterAdvances.clear();
        storage_->clusterPrefix.clear();
        storage_->softBreaks.clear();
        storage_->linePlans.clear();
        storage_->glyphFaces.clear();
        storage_->ellipsisFaces.clear();
    }

    /** @copydoc UiTextLayoutEngine::Shutdown */
    void UiTextLayoutEngine::Shutdown() noexcept {
        Close();
    }

    /** @copydoc UiTextLayoutEngine::IsDrained */
    bool UiTextLayoutEngine::IsDrained() const noexcept {
        return !storage_ || storage_->IsDrained();
    }

    /** @copydoc UiTextLayoutEngine::State */
    UiTextLayoutEngineState UiTextLayoutEngine::State() const noexcept {
        return storage_ ? storage_->lifecycle : UiTextLayoutEngineState::Closed;
    }
}  // namespace Horo::Runtime::Ui
