#include "Horo/Editor/ProposedChangeSet.h"

#include <algorithm>
#include <new>

namespace Horo::Editor::Proposals {
    namespace {
        constexpr std::string_view ReviewWire = "horo-review/1\n";
        constexpr std::string_view UnitCodes = "?+-";
        constexpr std::size_t UndoLimit = 16;

        /** @brief Translates validation failures without reporting source content. */
        template <class T> Result<T> Fail(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Computes a content-free commitment over the canonical review bytes. */
        std::string Commitment(const std::string_view bytes) {
            return FormatSha256(ComputeSha256(std::as_bytes(std::span{bytes})));
        }

        /** @brief Indicates that an observational state needs current evidence before publication. */
        bool HasAcceptance(const std::vector<ReviewState> &states) {
            return std::ranges::find(states, ReviewState::Accepted) != states.end();
        }

        /** @brief Updates a validated selection in a detached decision batch. */
        void SetUnits(const std::span<std::vector<ReviewState>> files, const std::optional<std::size_t> hunk, const ReviewState state) {
            if (hunk) {
                files.front()[*hunk] = state;
                return;
            }
            for (auto &file : files)
                std::ranges::fill(file, state);
        }

        /** @brief Reads unit states in the already-validated, exact proposal shape. */
        bool ReadUnits(std::string_view text, const std::span<std::vector<ReviewState>> files) {
            for (auto &file : files) {
                for (auto &unit : file) {
                    const auto decoded = UnitCodes.find(text.front());
                    if (decoded == std::string_view::npos)
                        return false;
                    unit = static_cast<ReviewState>(decoded);
                    text.remove_prefix(1);
                }
                if (text.front() != '\n')
                    return false;
                text.remove_prefix(1);
            }
            return true;
        }

        /** @brief Returns edit hunks for source validation. Structural changes remain indivisible review units. */
        template <class File> std::span<const TextHunk> Hunks(const File &file) {
            if constexpr (requires { file.hunks; })
                return file.hunks;
            return {};
        }

        /** @brief Requires exact live source evidence for an existing file operation. */
        bool CurrentFile(const FileOperation &operation, const FilePreview &preview) {
            return std::visit([&](const auto &file) {
                using File = std::decay_t<decltype(file)>;
                if constexpr (std::is_same_v<File, CreateFile>) {
                    return !preview.before.Revision() && preview.after == file.text;
                } else {
                    const bool expectsText = !std::is_same_v<File, DeleteFile>;
                    return preview.after.has_value() == expectsText && preview.ValidateSource(file.base, Hunks(file)).HasValue();
                }
            }, operation);
        }
    }  // namespace

    /** @copydoc ReviewDecisions::ReviewDecisions */
    ReviewDecisions::ReviewDecisions(ChangeSet proposal) : proposal_(std::move(proposal)) {
        units_.reserve(proposal_.Files().size());
        for (const auto &file : proposal_.Files()) {
            const auto *edit = std::get_if<EditFile>(&file);
            units_.emplace_back(edit ? edit->hunks.size() : 1, ReviewState::Pending);
        }
    }

    /** @copydoc ReviewDecisions::Contains */
    bool ReviewDecisions::Contains(const ReviewTarget target) const noexcept {
        if (!target.file)
            return !target.hunk;
        if (*target.file >= units_.size())
            return false;
        return !target.hunk ||
               (std::holds_alternative<EditFile>(proposal_.Files()[*target.file]) && *target.hunk < units_[*target.file].size());
    }

    /** @copydoc ReviewDecisions::Matches */
    bool ReviewDecisions::Matches(const WorkspaceEditPlan &plan) const noexcept {
        return plan.proposal == proposal_.Digest() && plan.entries.size() == units_.size();
    }

    /** @copydoc ReviewDecisions::CanAccept */
    bool ReviewDecisions::CanAccept(const WorkspaceEditPlan &plan, const std::size_t file) const {
        const auto *preview = std::get_if<FilePreview>(&plan.entries[file]);
        return preview && CurrentFile(proposal_.Files()[file], *preview);
    }

    /** @brief Revalidates every retained acceptance before a state can become active. */
    bool ReviewDecisions::AcceptancesCurrent(const WorkspaceEditPlan &plan, const Units &units) const {
        for (std::size_t file = 0; file < units.size(); ++file)
            if (HasAcceptance(units[file]) && !CanAccept(plan, file))
                return false;
        return true;
    }

    /** @copydoc ReviewDecisions::HasAcceptedChanges */
    bool ReviewDecisions::HasAcceptedChanges() const noexcept {
        return std::ranges::any_of(units_, HasAcceptance);
    }

    /** @brief Publishes a complete decision change with bounded undo history. */
    void ReviewDecisions::Publish(Units units) {
        if (units == units_)
            return;
        undo_.push_back(std::move(units_));
        units_ = std::move(units);
        if (undo_.size() > UndoLimit)
            undo_.pop_front();
    }

    /** @copydoc ReviewDecisions::State */
    Result<ReviewState> ReviewDecisions::State(const ReviewTarget target) const {
        if (!Contains(target))
            return Fail<ReviewState>(Errors::Invalid);
        const auto first = target.file.value_or(0);
        if (target.hunk)
            return Result<ReviewState>::Success(units_[first][*target.hunk]);
        const auto count = target.file ? 1 : units_.size();
        const auto expected = units_[first].front();
        const auto files = std::span{units_}.subspan(first, count);
        const bool uniform = std::ranges::all_of(files, [&](const auto &units) {
            return std::ranges::all_of(units, [&](const auto state) {
                return state == expected;
            });
        });
        return Result<ReviewState>::Success(uniform ? expected : ReviewState::Mixed);
    }

    /** @copydoc ReviewDecisions::Decide */
    Result<void> ReviewDecisions::Decide(const WorkspaceEditPlan &plan, const ReviewTarget target, const ReviewChoice choice) {
        if (!Contains(target) || (choice != ReviewChoice::Accept && choice != ReviewChoice::Reject))
            return Fail<void>(Errors::Invalid);
        if (!Matches(plan))
            return Fail<void>(Errors::StaleBase);
        try {
            auto pending = units_;
            const auto first = target.file.value_or(0);
            const auto end = target.file ? first + 1 : units_.size();
            const auto state = choice == ReviewChoice::Accept ? ReviewState::Accepted : ReviewState::Rejected;
            for (auto file = first; file < end; ++file)
                if (choice == ReviewChoice::Accept && !CanAccept(plan, file))
                    return Fail<void>(Errors::StaleBase);
            SetUnits(std::span{pending}.subspan(first, end - first), target.hunk, state);
            Publish(std::move(pending));
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Fail<void>(Errors::Bounds);
        }
    }

    /** @copydoc ReviewDecisions::Undo */
    Result<bool> ReviewDecisions::Undo(const WorkspaceEditPlan &plan) {
        if (!Matches(plan))
            return Fail<bool>(Errors::StaleBase);
        if (undo_.empty())
            return Result<bool>::Success(false);
        try {
            if (!AcceptancesCurrent(plan, undo_.back()))
                return Fail<bool>(Errors::StaleBase);
        } catch (const std::bad_alloc &) {
            return Fail<bool>(Errors::Bounds);
        }
        units_ = std::move(undo_.back());
        undo_.pop_back();
        return Result<bool>::Success(true);
    }

    /** @copydoc ReviewDecisions::Encode */
    std::string ReviewDecisions::Encode() const {
        std::string encoded{ReviewWire};
        encoded += FormatSha256(proposal_.Digest()) + '\n';
        for (const auto &file : units_) {
            for (const auto state : file)
                encoded += UnitCodes[static_cast<std::size_t>(state)];
            encoded += '\n';
        }
        encoded += Commitment(encoded);
        return encoded;
    }

    /** @copydoc ReviewDecisions::Decode */
    Result<ReviewDecisions> ReviewDecisions::Decode(ChangeSet proposal, const WorkspaceEditPlan &plan, std::string_view text) {
        try {
            if (!text.starts_with(ReviewWire))
                return Fail<ReviewDecisions>(Errors::UnsupportedVersion);
            ReviewDecisions review{std::move(proposal)};
            const auto digest = FormatSha256(review.proposal_.Digest());
            const auto header = std::string{ReviewWire} + digest + '\n';
            if (!review.Matches(plan) || !text.starts_with(header))
                return Fail<ReviewDecisions>(Errors::StaleBase);
            if (text.size() != review.Encode().size())
                return Fail<ReviewDecisions>(Errors::Malformed);
            const auto payload = text.substr(0, text.size() - digest.size());
            if (!text.ends_with(Commitment(payload)))
                return Fail<ReviewDecisions>(Errors::Malformed);
            if (!ReadUnits(payload.substr(header.size()), review.units_))
                return Fail<ReviewDecisions>(Errors::Malformed);
            if (!review.AcceptancesCurrent(plan, review.units_))
                return Fail<ReviewDecisions>(Errors::StaleBase);
            return Result<ReviewDecisions>::Success(std::move(review));
        } catch (const std::bad_alloc &) {
            return Fail<ReviewDecisions>(Errors::Bounds);
        }
    }
}  // namespace Horo::Editor::Proposals
