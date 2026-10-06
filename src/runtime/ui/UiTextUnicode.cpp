#include "Horo/Runtime/Ui/UiTextUnicode.h"

#include "Horo/Foundation/Sha256.h"
#include "UiTextShapingInternal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <unicode/ubidi.h>
#include <unicode/ubrk.h>
#include <unicode/uclean.h>
#include <unicode/udata.h>
#include <unicode/uloc.h>
#include <vector>

namespace Horo::Runtime::Ui {
    namespace {
        using TextShapingDetail::Failure;
        // This mutex protects only explicit process-runtime creation/final retirement.
        // No paragraph, shaping, line-layout or immutable query takes it.
        std::mutex runtimeMutex;
        enum class RuntimeAdmission {
            Uninitialized,
            Active,
            Retired
        };
        RuntimeAdmission runtimeAdmission{RuntimeAdmission::Uninitialized};
        constexpr std::size_t DataBytes = 33'107'248;
        constexpr std::string_view DataDigest = "sha256:a3d49eacd189624769dc77457c6aa1789a89219fb217a6d87d5b076c269aaf1f";

        struct alignas(16) DataBlock {
            std::byte bytes[16];
        };

        struct BidiDeleter {
            void operator()(UBiDi *value) const noexcept {
                ubidi_close(value);
            }
        };

        struct BreakDeleter {
            void operator()(UBreakIterator *value) const noexcept {
                ubrk_close(value);
            }
        };

        using BidiOwner = std::unique_ptr<UBiDi, BidiDeleter>;
        using BreakOwner = std::unique_ptr<UBreakIterator, BreakDeleter>;

        /** @brief Owns registration rollback while the caller already holds runtimeMutex. */
        struct InitializationRollback final {
            bool handedOff{};

            ~InitializationRollback() {
                if (!handedOff) {
                    u_cleanup();
                    runtimeAdmission = RuntimeAdmission::Retired;
                }
            }

            InitializationRollback() = default;
            InitializationRollback(const InitializationRollback &) = delete;
            InitializationRollback &operator=(const InitializationRollback &) = delete;
        };

        UBiDiLevel BaseLevel(const UiTextParagraphDirection direction) noexcept {
            if (direction == UiTextParagraphDirection::Auto)
                return UBIDI_DEFAULT_LTR;
            return direction == UiTextParagraphDirection::RightToLeft ? 1 : 0;
        }

        /** @brief Loads packaged dictionary engines at explicit host startup, not on the first frame-hot query. */
        bool WarmDictionaryData() {
            struct Probe {
                const char *locale;
                std::u16string_view text;
            };

            constexpr std::array<Probe, 6> probes{
                {{"th", u"ภาษาไทย"}, {"lo", u"ພາສາລາວ"}, {"km", u"ភាសាខ្មែរ"}, {"my", u"မြန်မာစာ"}, {"ja", u"日本語"}, {"zh", u"中文"}}};
            for (const auto &probe : probes) {
                std::array<UChar, 64> text{};
                std::ranges::copy(probe.text, text.begin());
                UErrorCode status = U_ZERO_ERROR;
                BreakOwner iterator(ubrk_open(UBRK_LINE, probe.locale, text.data(), static_cast<std::int32_t>(probe.text.size()), &status));
                if (U_FAILURE(status) || !iterator)
                    return false;
                for (auto boundary = ubrk_first(iterator.get()); boundary != UBRK_DONE; boundary = ubrk_next(iterator.get())) {
                }
            }
            return true;
        }
    }  // namespace

    struct UiTextUnicodeRuntime::Storage final {
        static std::shared_ptr<Storage> processLease;
        std::unique_ptr<DataBlock[]> data;
        std::atomic<bool> active{true};
        bool initialized{};
    };

    std::shared_ptr<UiTextUnicodeRuntime::Storage> UiTextUnicodeRuntime::Storage::processLease;

    /** @copydoc UiTextUnicodeRuntime::Create */
    Result<UiTextUnicodeRuntime> UiTextUnicodeRuntime::Create(const std::span<const std::byte> data) {
        if (std::endian::native != std::endian::little || data.size() != DataBytes)
            return Failure<UiTextUnicodeRuntime>(UiErrors::PayloadInvalid);
        try {
            const auto expected = ParseSha256(DataDigest);
            if (expected.HasError() || ComputeSha256(data) != expected.Value())
                return Failure<UiTextUnicodeRuntime>(UiErrors::PayloadInvalid);
            const std::lock_guard lock(runtimeMutex);
            if (runtimeAdmission != RuntimeAdmission::Uninitialized)
                return Failure<UiTextUnicodeRuntime>(UiErrors::TextLifecycleUnavailable);
            auto storage = std::make_shared<Storage>();
            storage->data = std::make_unique<DataBlock[]>((DataBytes + 15) / 16);
            std::memcpy(storage->data.get(), data.data(), data.size());
            // Declared after storage and before registration: every failure/throw
            // unwinds cleanup before copied bytes, then releases the outer lock.
            InitializationRollback rollback;
            UErrorCode status = U_ZERO_ERROR;
            udata_setCommonData(storage->data.get(), &status);
            if (U_SUCCESS(status))
                u_init(&status);
            if (U_FAILURE(status) || !WarmDictionaryData()) {
                return Failure<UiTextUnicodeRuntime>(UiErrors::TextShapeInvalid);
            }
            auto result = Result<UiTextUnicodeRuntime>::Success(UiTextUnicodeRuntime{storage});
            storage->initialized = true;
            Storage::processLease = storage;
            runtimeAdmission = RuntimeAdmission::Active;
            rollback.handedOff = true;
            return result;
        } catch (const std::bad_alloc &) {
            return Failure<UiTextUnicodeRuntime>(UiErrors::CapacityExceeded);
        }
    }

    UiTextUnicodeRuntime::UiTextUnicodeRuntime(std::shared_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    UiTextUnicodeRuntime::~UiTextUnicodeRuntime() {
        Close();
    }

    UiTextUnicodeRuntime::UiTextUnicodeRuntime(UiTextUnicodeRuntime &&other) noexcept = default;

    UiTextUnicodeRuntime &UiTextUnicodeRuntime::operator=(UiTextUnicodeRuntime &&other) noexcept {
        if (this != &other) {
            Close();
            storage_ = std::move(other.storage_);
        }
        return *this;
    }

    void UiTextUnicodeRuntime::Close() noexcept {
        if (storage_)
            storage_->active.store(false);
    }

    bool UiTextUnicodeRuntime::IsActive() const noexcept {
        return storage_ && storage_->active.load();
    }

    /** @copydoc UiTextUnicodeRuntime::Shutdown */
    Result<void> UiTextUnicodeRuntime::Shutdown() {
        Close();
        if (!storage_ || !storage_->initialized)
            return Result<void>::Success();
        const std::lock_guard lock(runtimeMutex);
        // One host lease plus one process pin. Analyzer/analysis leases keep the
        // pin alive until the host retries; their destructors never reset ICU.
        if (storage_.use_count() != 2)
            return Failure(UiErrors::TextShapeStorageExhausted);
        u_cleanup();
        storage_->initialized = false;
        runtimeAdmission = RuntimeAdmission::Retired;
        Storage::processLease.reset();
        return Result<void>::Success();
    }

    struct UiTextUnicodeAnalysis::Storage final {
        std::shared_ptr<UiTextUnicodeRuntime::Storage> runtime;
        BidiOwner paragraph;
        std::string text;
        std::vector<UChar> utf16;
        std::vector<std::uint32_t> scalarByUnit;
        std::vector<UiTextUnicodeScalar> scalars;
        UiTextContentRevision content;
        UiTextLanguage locale;
        UiTextParagraphDirection direction{};
        bool published{};
    };

    struct UiTextUnicodeAnalyzer::Storage final {
        std::shared_ptr<UiTextUnicodeRuntime::Storage> runtime;
        UiTextShaperLimits limits;
        BidiOwner line;
        std::vector<std::int32_t> visualMap;
        std::vector<TextShapingDetail::DecodedScalar> decoded;
        std::vector<std::shared_ptr<UiTextUnicodeAnalysis::Storage>> slots;
        std::shared_ptr<const UiTextUnicodeAnalysis::Storage> cached;
        bool active{true};

        Result<void> Decode(UiTextUnicodeAnalysis::Storage &slot, const std::string_view text) {
            if (const auto result = TextShapingDetail::DecodeText(text, decoded); result.HasError())
                return result;
            if (decoded.size() > limits.maxClusters)
                return Failure(UiErrors::CapacityExceeded);
            slot.utf16.clear();
            slot.scalarByUnit.clear();
            slot.scalars.clear();
            for (const auto &scalar : decoded) {
                const auto first = static_cast<std::uint32_t>(slot.utf16.size());
                const auto index = static_cast<std::uint32_t>(slot.scalars.size());
                if (scalar.value <= 0xFFFF) {
                    slot.utf16.push_back(static_cast<UChar>(scalar.value));
                    slot.scalarByUnit.push_back(index);
                } else {
                    const auto supplementary = scalar.value - 0x10000;
                    slot.utf16.push_back(static_cast<UChar>(0xD800 + (supplementary >> 10)));
                    slot.utf16.push_back(static_cast<UChar>(0xDC00 + (supplementary & 0x3FF)));
                    slot.scalarByUnit.push_back(index);
                    slot.scalarByUnit.push_back(index);
                }
                slot.scalars.push_back(
                    {scalar.byteStart, scalar.byteEnd, first, static_cast<std::uint32_t>(slot.utf16.size()), 0, UiTextUnicodeBreak::None});
            }
            return Result<void>::Success();
        }

        /** @brief Validates that a native break offset ends one complete decoded scalar. */
        static UiTextUnicodeScalar *ScalarEndingAt(UiTextUnicodeAnalysis::Storage &slot, const std::int32_t boundary) noexcept {
            const auto found =
                std::lower_bound(slot.scalars.begin(), slot.scalars.end(), boundary, [](const auto &scalar, const auto offset) {
                return scalar.utf16End < static_cast<std::uint32_t>(offset);
            });
            return found != slot.scalars.end() && found->utf16End == static_cast<std::uint32_t>(boundary) ? &*found : nullptr;
        }

        Result<void> Prepare(UiTextUnicodeAnalysis::Storage &slot, const UiTextLanguage locale, const UiTextParagraphDirection direction) {
            // Validate the same explicit locale for empty and nonempty content;
            // an empty candidate must not publish a tag ICU cannot consume.
            UErrorCode status = U_ZERO_ERROR;
            char localeName[ULOC_FULLNAME_CAPACITY]{};
            std::int32_t parsed{};
            const std::string language(locale.View());
            uloc_forLanguageTag(language.c_str(), localeName, ULOC_FULLNAME_CAPACITY, &parsed, &status);
            if (U_FAILURE(status) || parsed != static_cast<std::int32_t>(language.size()))
                return Failure(UiErrors::LocaleInvalid);
            if (slot.utf16.empty())
                return Result<void>::Success();
            ubidi_setPara(slot.paragraph.get(), slot.utf16.data(), static_cast<std::int32_t>(slot.utf16.size()), BaseLevel(direction),
                          nullptr, &status);
            if (U_FAILURE(status))
                return Failure(UiErrors::TextShapeInvalid);
            const auto *levels = ubidi_getLevels(slot.paragraph.get(), &status);
            if (U_FAILURE(status) || levels == nullptr)
                return Failure(UiErrors::TextShapeInvalid);
            for (auto &scalar : slot.scalars)
                scalar.level = levels[scalar.utf16Start];

            BreakOwner graphemes(
                ubrk_open(UBRK_CHARACTER, "root", slot.utf16.data(), static_cast<std::int32_t>(slot.utf16.size()), &status));
            if (U_FAILURE(status) || !graphemes)
                return Failure(UiErrors::TextShapeInvalid);
            for (auto boundary = ubrk_first(graphemes.get()); (boundary = ubrk_next(graphemes.get())) != UBRK_DONE;) {
                auto *found = ScalarEndingAt(slot, boundary);
                if (found == nullptr)
                    return Failure(UiErrors::TextShapeInvalid);
                found->graphemeEnd = true;
            }

            BreakOwner breaks(ubrk_open(UBRK_LINE, localeName, slot.utf16.data(), static_cast<std::int32_t>(slot.utf16.size()), &status));
            if (U_FAILURE(status) || !breaks)
                return Failure(UiErrors::TextShapeInvalid);
            for (auto boundary = ubrk_first(breaks.get()); (boundary = ubrk_next(breaks.get())) != UBRK_DONE;) {
                auto *found = ScalarEndingAt(slot, boundary);
                if (found == nullptr)
                    return Failure(UiErrors::TextShapeInvalid);
                if (found->graphemeEnd)
                    found->breakAfter =
                        ubrk_getRuleStatus(breaks.get()) >= UBRK_LINE_HARD ? UiTextUnicodeBreak::Mandatory : UiTextUnicodeBreak::Optional;
            }
            return Result<void>::Success();
        }
    };

    Result<UiTextUnicodeAnalyzer> UiTextUnicodeAnalyzer::Create(const UiTextUnicodeRuntime &runtime, const UiTextShaperLimits &limits) {
        if (!runtime.IsActive())
            return Failure<UiTextUnicodeAnalyzer>(UiErrors::TextLifecycleUnavailable);
        if (!limits.IsValid() || limits.concurrentShapes < 2)
            return Failure<UiTextUnicodeAnalyzer>(UiErrors::CapacityExceeded);
        try {
            auto storage = std::make_unique<Storage>();
            storage->runtime = runtime.storage_;
            storage->limits = limits;
            UErrorCode status = U_ZERO_ERROR;
            storage->line.reset(
                ubidi_openSized(static_cast<std::int32_t>(limits.maxInputBytes), static_cast<std::int32_t>(limits.maxRuns), &status));
            storage->visualMap.resize(limits.maxInputBytes);
            storage->decoded.reserve(limits.maxInputBytes);
            storage->slots.reserve(limits.concurrentShapes);
            for (std::uint32_t index = 0; index < limits.concurrentShapes; ++index) {
                auto slot = std::make_shared<UiTextUnicodeAnalysis::Storage>();
                slot->runtime = storage->runtime;
                slot->paragraph.reset(
                    ubidi_openSized(static_cast<std::int32_t>(limits.maxInputBytes), static_cast<std::int32_t>(limits.maxRuns), &status));
                slot->text.reserve(limits.maxInputBytes);
                slot->utf16.reserve(limits.maxInputBytes);
                slot->scalarByUnit.reserve(limits.maxInputBytes);
                slot->scalars.reserve(limits.maxClusters);
                storage->slots.push_back(std::move(slot));
            }
            if (U_FAILURE(status) || !storage->line || std::ranges::any_of(storage->slots, [](const auto &slot) {
                return !slot->paragraph;
            }))
                return Failure<UiTextUnicodeAnalyzer>(UiErrors::CapacityExceeded);
            return Result<UiTextUnicodeAnalyzer>::Success(UiTextUnicodeAnalyzer{std::move(storage)});
        } catch (const std::bad_alloc &) {
            return Failure<UiTextUnicodeAnalyzer>(UiErrors::CapacityExceeded);
        }
    }

    UiTextUnicodeAnalyzer::UiTextUnicodeAnalyzer(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    UiTextUnicodeAnalyzer::~UiTextUnicodeAnalyzer() = default;
    UiTextUnicodeAnalyzer::UiTextUnicodeAnalyzer(UiTextUnicodeAnalyzer &&other) noexcept = default;
    UiTextUnicodeAnalyzer &UiTextUnicodeAnalyzer::operator=(UiTextUnicodeAnalyzer &&other) noexcept = default;

    Result<UiTextUnicodeAnalysis> UiTextUnicodeAnalyzer::Analyze(const std::string_view text, const UiTextContentRevision content,
                                                                 const UiTextLanguage locale, const UiTextParagraphDirection direction) {
        if (!storage_ || !storage_->active || !storage_->runtime->active.load())
            return Failure<UiTextUnicodeAnalysis>(UiErrors::TextLifecycleUnavailable);
        if (!content.IsValid() || locale.IsAuto() || direction >= UiTextParagraphDirection::Count)
            return Failure<UiTextUnicodeAnalysis>(UiErrors::TextInputInvalid);
        if (text.size() > storage_->limits.maxInputBytes)
            return Failure<UiTextUnicodeAnalysis>(UiErrors::CapacityExceeded);
        const auto &cached = storage_->cached;
        if (cached && cached->text == text && cached->content == content && cached->locale == locale && cached->direction == direction)
            return Result<UiTextUnicodeAnalysis>::Success(UiTextUnicodeAnalysis{cached});
        const auto free = std::ranges::find_if(storage_->slots, [](const auto &slot) {
            return slot.use_count() == 1;
        });
        if (free == storage_->slots.end())
            return Failure<UiTextUnicodeAnalysis>(UiErrors::TextShapeStorageExhausted);
        auto &slot = **free;
        slot.published = false;
        try {
            if (const auto decoded = storage_->Decode(slot, text); decoded.HasError())
                return Result<UiTextUnicodeAnalysis>::Failure(decoded.ErrorValue());
            if (const auto prepared = storage_->Prepare(slot, locale, direction); prepared.HasError())
                return Result<UiTextUnicodeAnalysis>::Failure(prepared.ErrorValue());
            slot.text.assign(text);
            slot.content = content;
            slot.locale = locale;
            slot.direction = direction;
            slot.published = true;
            storage_->cached = *free;
            return Result<UiTextUnicodeAnalysis>::Success(UiTextUnicodeAnalysis{*free});
        } catch (const std::bad_alloc &) {
            return Failure<UiTextUnicodeAnalysis>(UiErrors::CapacityExceeded);
        }
    }

    Result<std::uint32_t> UiTextUnicodeAnalyzer::OrderLine(const UiTextUnicodeAnalysis &analysis, const std::uint32_t byteStart,
                                                           const std::uint32_t byteEnd, const std::span<std::uint32_t> output,
                                                           const std::span<std::uint8_t> lineLevels) {
        if (!storage_ || !storage_->active || !storage_->runtime->active.load())
            return Failure<std::uint32_t>(UiErrors::TextLifecycleUnavailable);
        if (!analysis.IsValid() || byteStart > byteEnd || byteEnd > analysis.Text().size() ||
            std::ranges::none_of(storage_->slots, [&analysis](const auto &slot) {
            return slot == analysis.storage_;
        }))
            return Failure<std::uint32_t>(UiErrors::TextInputInvalid);
        const auto &slot = *analysis.storage_;
        const auto first = std::ranges::find(slot.scalars, byteStart, &UiTextUnicodeScalar::byteStart);
        if (byteStart == byteEnd)
            return byteStart == slot.text.size() || first != slot.scalars.end() ? Result<std::uint32_t>::Success(0)
                                                                                : Failure<std::uint32_t>(UiErrors::TextInputInvalid);
        const auto last = std::ranges::find(slot.scalars, byteEnd, &UiTextUnicodeScalar::byteEnd);
        if (first == slot.scalars.end() || last == slot.scalars.end() || last < first)
            return Failure<std::uint32_t>(UiErrors::TextInputInvalid);
        if (output.size() < static_cast<std::size_t>(last - first + 1) ||
            (!lineLevels.empty() && lineLevels.size() < static_cast<std::size_t>(last - first + 1)))
            return Failure<std::uint32_t>(UiErrors::CapacityExceeded);
        UErrorCode status = U_ZERO_ERROR;
        ubidi_setLine(slot.paragraph.get(), static_cast<std::int32_t>(first->utf16Start), static_cast<std::int32_t>(last->utf16End),
                      storage_->line.get(), &status);
        if (U_SUCCESS(status))
            ubidi_getVisualMap(storage_->line.get(), storage_->visualMap.data(), &status);
        if (U_FAILURE(status))
            return Failure<std::uint32_t>(UiErrors::TextShapeInvalid);
        std::uint32_t count{};
        const auto units = last->utf16End - first->utf16Start;
        for (std::uint32_t visual = 0; visual < units; ++visual) {
            const auto logical = storage_->visualMap[visual];
            if (logical < 0 || static_cast<std::uint32_t>(logical) >= units)
                return Failure<std::uint32_t>(UiErrors::TextShapeInvalid);
            const auto scalar = slot.scalarByUnit[first->utf16Start + static_cast<std::uint32_t>(logical)];
            if (count == 0 || output[count - 1] != scalar) {
                if (!lineLevels.empty())
                    lineLevels[count] = ubidi_getLevelAt(storage_->line.get(), logical);
                output[count++] = scalar;
            }
        }
        return Result<std::uint32_t>::Success(count);
    }

    void UiTextUnicodeAnalyzer::Close() noexcept {
        if (storage_)
            storage_->active = false;
    }

    UiTextUnicodeAnalysis::UiTextUnicodeAnalysis(std::shared_ptr<const Storage> storage) noexcept : storage_(std::move(storage)) {}

    std::string_view UiTextUnicodeAnalysis::Text() const noexcept {
        return storage_ ? std::string_view(storage_->text) : std::string_view{};
    }

    std::span<const UiTextUnicodeScalar> UiTextUnicodeAnalysis::Scalars() const noexcept {
        return storage_ ? std::span<const UiTextUnicodeScalar>(storage_->scalars) : std::span<const UiTextUnicodeScalar>{};
    }

    UiTextLanguage UiTextUnicodeAnalysis::Locale() const noexcept {
        return storage_ ? storage_->locale : UiTextLanguage::Auto();
    }

    UiTextContentRevision UiTextUnicodeAnalysis::Content() const noexcept {
        return storage_ ? storage_->content : UiTextContentRevision{};
    }

    bool UiTextUnicodeAnalysis::IsValid() const noexcept {
        return storage_ && storage_->published;
    }
}  // namespace Horo::Runtime::Ui
