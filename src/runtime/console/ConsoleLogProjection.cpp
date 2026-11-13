#include "ConsoleText.h"
#include "Horo/Debug/ConsolePresentation.h"
#include "Horo/Foundation/Utf8.h"

namespace Horo::Debug {
    namespace {
        /** @brief Matches at most 129 category bytes, preserving exact namespace ownership. */
        bool CategoryMatches(const ConsoleLogFilter &filter, std::string_view category) {
            if (filter.category.empty())
                return true;
            if (filter.categoryMatch == ConsoleLogCategoryMatch::Exact)
                return category == filter.category;
            return category.starts_with(filter.category) &&
                   (category.size() == filter.category.size() || category[filter.category.size()] == '.');
        }

        /** @brief Compares complete bounded payloads; truncated messages or large contexts never collapse ambiguously. */
        bool SameRecord(const Log::StructuredLogRecord &left, const Log::StructuredLogRecord &right) {
            return left.category.size() <= 128 && right.category.size() <= 128 && left.message.size() <= 4096 &&
                   right.message.size() <= 4096 && left.context.size() <= 4096 && right.context.size() <= 4096 &&
                   left.level == right.level && left.category == right.category && left.message == right.message &&
                   left.context == right.context;
        }
    }  // namespace

    /** @copydoc ConsoleStoreOverlays::SetLogFilter */
    Result<void> ConsoleStoreOverlays::SetLogFilter(const ConsoleLogFilter &filter) {
        if (std::this_thread::get_id() != owner_)
            return Result<void>::Failure(MakeCommandError(CommandError::WrongThread));
        if (filter.category.size() > 128 || filter.text.size() > 512 ||
            static_cast<unsigned>(filter.minimumLevel) > static_cast<unsigned>(Log::Level::Off) ||
            static_cast<unsigned>(filter.categoryMatch) > static_cast<unsigned>(ConsoleLogCategoryMatch::NamespacePrefix) ||
            !IsValidUtf8ScalarSequence(filter.category) || !IsValidUtf8ScalarSequence(filter.text))
            return Result<void>::Failure(MakeCommandError(CommandError::Usage));
        if (filter != logFilter_) {
            logFilter_ = filter;
            logFilterDirty_ = true;
        }
        return Result<void>::Success();
    }

    /** @copydoc ConsoleStoreOverlays::RebuildLogRows */
    Result<void> ConsoleStoreOverlays::RebuildLogRows() {
        logScratch_.clear();
        if (logSnapshot_ && logFilter_.minimumLevel != Log::Level::Off) {
            logScratch_.reserve(logSnapshot_->records.size());
            for (const auto &record : logSnapshot_->records) {
                if (!record || static_cast<unsigned>(record->level) > static_cast<unsigned>(Log::Level::Off))
                    return Result<void>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
                if (record->level == Log::Level::Off || record->level < logFilter_.minimumLevel ||
                    !CategoryMatches(logFilter_, record->category))
                    continue;
                const auto message = Detail::ConsoleTextPrefix(record->message, 4096);
                if (message.find(logFilter_.text) == std::string_view::npos)
                    continue;
                const auto category = Detail::ConsoleTextPrefix(record->category, 128);
                if (!IsValidUtf8ScalarSequence(message) || !IsValidUtf8ScalarSequence(category))
                    return Result<void>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
                if (logFilter_.collapseRepeats && !logScratch_.empty() && SameRecord(*logScratch_.back().source, *record))
                    ++logScratch_.back().repetitions;
                else
                    logScratch_.push_back({record, category, message, 1,
                                           category.size() != record->category.size() || message.size() != record->message.size()});
            }
        }
        logRows_.swap(logScratch_);
        ++logProjectionRevision_;
        logScratch_.clear();
        return Result<void>::Success();
    }
}  // namespace Horo::Debug
