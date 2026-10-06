#include "Horo/Editor/FractureAssetDocument.h"

#include <algorithm>
#include <limits>
#include <new>
#include <ranges>
#include <type_traits>

namespace Horo::Editor {
    namespace {
        /** @brief Measures retained authored sections, including vector element storage and checkpoint leases. */
        std::size_t SourceBytes(const FractureAssetSource &source) noexcept {
            return sizeof(source) + source.settings.sites.size() * sizeof(FractureSourceSite) +
                   source.chunks.size() * sizeof(FractureSourceChunk) + source.contacts.size() * sizeof(FractureSourceContact) +
                   source.materials.size() * sizeof(FractureSourceMaterial);
        }

        /** @brief Measures the complete retained semantic representation before history publication. */
        std::size_t PatchBytes(const FractureSourcePatch &patch) {
            std::size_t bytes = sizeof(patch) + patch.operations.size() * sizeof(FractureSourceOperation);
            for (const auto &operation : patch.operations) {
                bytes += std::visit([](const auto &value) -> std::size_t {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, SetFractureSettings>)
                        return value.value.sites.size() * sizeof(FractureSourceSite);
                    else if constexpr (std::is_same_v<T, ReplaceFractureSource>)
                        return value.value ? SourceBytes(*value.value) : 0;
                    else
                        return 0;
                }, operation);
            }
            return bytes;
        }

        /** @brief Replaces one stable keyed value and records its exact inverse; inserted values remain sorted. */
        template <typename T, typename Key, typename Put, typename Remove>
        void PutValue(std::vector<T> &values, const T &value, Key key, FractureSourcePatch &inverse, Put put, Remove remove) {
            const auto found = std::ranges::lower_bound(values, key(value), {}, key);
            if (found != values.end() && key(*found) == key(value)) {
                inverse.operations.emplace_back(put(*found));
                *found = value;
            } else {
                inverse.operations.emplace_back(remove(key(value)));
                values.insert(found, value);
            }
        }

        /** @brief Deletes one exact stable key and records its prior semantic value; absent deletion is a no-op. */
        template <typename T, typename K, typename Key, typename Put>
        void RemoveValue(std::vector<T> &values, const K keyValue, Key key, FractureSourcePatch &inverse, Put put) {
            const auto found = std::ranges::lower_bound(values, keyValue, {}, key);
            if (found != values.end() && key(*found) == keyValue) {
                inverse.operations.emplace_back(put(*found));
                values.erase(found);
            }
        }

        /** @brief Applies a stable chunk edit and records its exact before-value. */
        template <typename Operation>
        void ApplyChunk(FractureAssetSource &source, const Operation &operation, FractureSourcePatch &inverse) {
            const auto key = [](const FractureSourceChunk &value) {
                return value.id;
            };
            const auto put = [](auto before) {
                return PutFractureChunk{before};
            };
            if constexpr (std::is_same_v<Operation, PutFractureChunk>)
                PutValue(source.chunks, operation.value, key, inverse, put, [](auto id) {
                    return RemoveFractureChunk{id};
                });
            else
                RemoveValue(source.chunks, operation.id, key, inverse, put);
        }

        /** @brief Applies an ordered contact edit and records its exact before-value. */
        template <typename Operation>
        void ApplyContact(FractureAssetSource &source, const Operation &operation, FractureSourcePatch &inverse) {
            const auto key = [](const FractureSourceContact &value) {
                return std::pair{value.low, value.high};
            };
            const auto put = [](auto before) {
                return PutFractureContact{before};
            };
            if constexpr (std::is_same_v<Operation, PutFractureContact>)
                PutValue(source.contacts, operation.value, key, inverse, put, [](auto endpoints) {
                    return RemoveFractureContact{endpoints.first, endpoints.second};
                });
            else
                RemoveValue(source.contacts, std::pair{operation.low, operation.high}, key, inverse, put);
        }

        /** @brief Applies a material-slot edit and records its exact before-value. */
        template <typename Operation>
        void ApplyMaterial(FractureAssetSource &source, const Operation &operation, FractureSourcePatch &inverse) {
            const auto key = [](const FractureSourceMaterial &value) {
                return value.slot;
            };
            const auto put = [](auto before) {
                return PutFractureMaterial{before};
            };
            if constexpr (std::is_same_v<Operation, PutFractureMaterial>)
                PutValue(source.materials, operation.value, key, inverse, put, [](auto slot) {
                    return RemoveFractureMaterial{slot};
                });
            else
                RemoveValue(source.materials, operation.slot, key, inverse, put);
        }

        /** @brief Dispatches typed intent only to detached storage, preserving inverse values in operation order. */
        void ApplyOperation(FractureAssetSource &source, const FractureSourceOperation &operation, FractureSourcePatch &inverse) {
            std::visit([&](const auto &value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, SetFractureSettings>) {
                    inverse.operations.emplace_back(SetFractureSettings{source.settings});
                    source.settings = value.value;
                } else if constexpr (std::is_same_v<T, PutFractureChunk> || std::is_same_v<T, RemoveFractureChunk>) {
                    ApplyChunk(source, value, inverse);
                } else if constexpr (std::is_same_v<T, PutFractureContact> || std::is_same_v<T, RemoveFractureContact>) {
                    ApplyContact(source, value, inverse);
                } else if constexpr (std::is_same_v<T, PutFractureMaterial> || std::is_same_v<T, RemoveFractureMaterial>) {
                    ApplyMaterial(source, value, inverse);
                } else if constexpr (std::is_same_v<T, SetFractureDamage>) {
                    inverse.operations.emplace_back(SetFractureDamage{source.damage});
                    source.damage = value.value;
                } else {
                    inverse.operations.emplace_back(ReplaceFractureSource{std::make_shared<const FractureAssetSource>(source)});
                    source = *value.value;
                }
            }, operation);
        }

        /** @brief Validates incoming variable storage before copying and freezes external checkpoint aliases. */
        Result<FractureSourcePatch> CopyPatch(const FractureSourcePatch &patch, const Destruction::FractureAssetId asset,
                                              const std::size_t maximumBytes) {
            if (patch.operations.size() > MaximumFracturePatchOperations)
                return Result<FractureSourcePatch>::Failure(MakeError(FractureDocumentErrors::LimitExceeded));
            // Admit all variable-size input before allocating any retained checkpoint copies.
            for (const auto &operation : patch.operations) {
                if (const auto *settings = std::get_if<SetFractureSettings>(&operation);
                    settings && settings->value.sites.size() > Destruction::DestructionHardLimits::ChunksPerDestructible)
                    return Result<FractureSourcePatch>::Failure(MakeError(FractureDocumentErrors::LimitExceeded));
                if (const auto *replacement = std::get_if<ReplaceFractureSource>(&operation)) {
                    if (!replacement->value || replacement->value->asset != asset)
                        return Result<FractureSourcePatch>::Failure(MakeError(FractureDocumentErrors::WrongDocument));
                    const auto valid = ValidateFractureAssetSource(*replacement->value);
                    if (valid.HasError())
                        return Result<FractureSourcePatch>::Failure(valid.ErrorValue());
                }
            }
            if (PatchBytes(patch) > maximumBytes)
                return Result<FractureSourcePatch>::Failure(MakeError(FractureDocumentErrors::HistoryBudgetExceeded));
            FractureSourcePatch copy;
            copy.operations.reserve(patch.operations.size());
            for (const auto &operation : patch.operations) {
                if (const auto *replacement = std::get_if<ReplaceFractureSource>(&operation)) {
                    copy.operations.emplace_back(ReplaceFractureSource{std::make_shared<const FractureAssetSource>(*replacement->value)});
                } else {
                    copy.operations.push_back(operation);
                }
            }
            return Result<FractureSourcePatch>::Success(std::move(copy));
        }

        /** @brief Produces a completely validated detached source and reverse-ordered semantic undo patch. */
        Result<std::shared_ptr<const FractureAssetSource>> Stage(const FractureAssetSource &source, const FractureSourcePatch &patch,
                                                                 FractureSourcePatch &inverse, const CancellationToken &cancellation) {
            auto candidate = std::make_shared<FractureAssetSource>(source);
            for (const auto &operation : patch.operations) {
                if (cancellation.IsCancellationRequested())
                    return Result<std::shared_ptr<const FractureAssetSource>>::Failure(MakeError(FractureDocumentErrors::Cancelled));
                ApplyOperation(*candidate, operation, inverse);
            }
            const auto valid = ValidateFractureAssetSource(*candidate);
            if (valid.HasError())
                return Result<std::shared_ptr<const FractureAssetSource>>::Failure(valid.ErrorValue());
            std::ranges::reverse(inverse.operations);
            return Result<std::shared_ptr<const FractureAssetSource>>::Success(std::move(candidate));
        }
    }  // namespace

    /** @copydoc FractureAssetDocument::Open */
    Result<FractureAssetDocument> FractureAssetDocument::Open(FractureAssetSource source, const std::uint64_t sourceRevision,
                                                              FractureDocumentSession session, FractureDocumentHistoryLimits limits) {
        if (!session.IsValid() || sourceRevision == 0 || limits.maximumEntries == 0 || limits.maximumEntries > 128 ||
            limits.maximumBytes == 0 || limits.maximumBytes > 16 * MaximumFractureSourceBytes)
            return Result<FractureAssetDocument>::Failure(MakeError(FractureDocumentErrors::LimitExceeded));
        const auto valid = ValidateFractureAssetSource(source);
        if (valid.HasError())
            return Result<FractureAssetDocument>::Failure(valid.ErrorValue());
        try {
            FractureAssetDocument document;
            document.source_ = std::make_shared<const FractureAssetSource>(std::move(source));
            document.sourceRevision_ = sourceRevision;
            document.session_ = session;
            document.limits_ = limits;
            return Result<FractureAssetDocument>::Success(std::move(document));
        } catch (const std::bad_alloc &) {
            return Result<FractureAssetDocument>::Failure(MakeError(FractureDocumentErrors::LimitExceeded));
        }
    }

    /** @copydoc FractureAssetDocument::Snapshot */
    FractureDocumentSnapshot FractureAssetDocument::Snapshot() const noexcept {
        return {session_, revision_, state_, source_};
    }

    /** @copydoc FractureAssetDocument::Admit */
    Result<void> FractureAssetDocument::Admit(const FractureDocumentEditContext &context) const {
        const auto fail = [](const ErrorCodeDescriptor &error) {
            return Result<void>::Failure(MakeError(error));
        };
        if (closed_ || !source_)
            return fail(FractureDocumentErrors::Closed);
        if (context.session != session_)
            return fail(FractureDocumentErrors::WrongDocument);
        if (context.revision != revision_)
            return fail(FractureDocumentErrors::StaleRevision);
        if (!context.canEdit)
            return fail(FractureDocumentErrors::AuthorityDenied);
        if (context.cancellation.IsCancellationRequested())
            return fail(FractureDocumentErrors::Cancelled);
        return Result<void>::Success();
    }

    /** @copydoc FractureAssetDocument::Apply */
    Result<FractureDocumentChange> FractureAssetDocument::Apply(const FractureSourcePatch &patch,
                                                                const FractureDocumentEditContext &context) {
        const auto admitted = Admit(context);
        if (admitted.HasError())
            return Result<FractureDocumentChange>::Failure(admitted.ErrorValue());
        try {
            auto copied = CopyPatch(patch, source_->asset, limits_.maximumBytes);
            if (copied.HasError())
                return Result<FractureDocumentChange>::Failure(copied.ErrorValue());
            FractureSourcePatch inverse;
            auto candidate = Stage(*source_, copied.Value(), inverse, context.cancellation);
            if (candidate.HasError())
                return Result<FractureDocumentChange>::Failure(candidate.ErrorValue());
            if (*candidate.Value() == *source_)
                return Result<FractureDocumentChange>::Success(FractureDocumentChange::Unchanged);
            if (revision_.Value() == std::numeric_limits<std::uint64_t>::max() || nextState_ == std::numeric_limits<std::uint64_t>::max())
                return Result<FractureDocumentChange>::Failure(MakeError(FractureDocumentErrors::Exhausted));
            const std::size_t cost = PatchBytes(inverse) + PatchBytes(copied.Value()) + sizeof(HistoryEntry);
            if (cost > limits_.maximumBytes)
                return Result<FractureDocumentChange>::Failure(MakeError(FractureDocumentErrors::HistoryBudgetExceeded));
            // Prepare the complete next history branch before the no-fail source swap.
            std::size_t total = cost;
            std::size_t first = cursor_;
            while (first > 0 && cursor_ - first + 1 < limits_.maximumEntries && history_[first - 1].bytes <= limits_.maximumBytes - total) {
                total += history_[--first].bytes;
            }
            std::vector<HistoryEntry> history(history_.begin() + static_cast<std::ptrdiff_t>(first),
                                              history_.begin() + static_cast<std::ptrdiff_t>(cursor_));
            const auto nextState = FractureDocumentStateId::Create(nextState_).Value();
            history.push_back({std::move(inverse), std::move(copied.Value()), state_, nextState, cost});
            if (context.cancellation.IsCancellationRequested())
                return Result<FractureDocumentChange>::Failure(MakeError(FractureDocumentErrors::Cancelled));
            source_ = std::move(candidate.Value());
            history_ = std::move(history);
            cursor_ = history_.size();
            state_ = nextState;
            ++nextState_;
            revision_ = FractureDocumentRevision::Create(revision_.Value() + 1).Value();
            return Result<FractureDocumentChange>::Success(FractureDocumentChange::Committed);
        } catch (const std::bad_alloc &) {
            return Result<FractureDocumentChange>::Failure(MakeError(FractureDocumentErrors::HistoryBudgetExceeded));
        }
    }

    /** @copydoc FractureAssetDocument::Replay */
    Result<FractureDocumentChange> FractureAssetDocument::Replay(const bool redo, const FractureDocumentEditContext &context) {
        const auto admitted = Admit(context);
        if (admitted.HasError())
            return Result<FractureDocumentChange>::Failure(admitted.ErrorValue());
        if ((redo && cursor_ == history_.size()) || (!redo && cursor_ == 0))
            return Result<FractureDocumentChange>::Success(FractureDocumentChange::Unchanged);
        if (revision_.Value() == std::numeric_limits<std::uint64_t>::max())
            return Result<FractureDocumentChange>::Failure(MakeError(FractureDocumentErrors::Exhausted));
        try {
            const auto &entry = history_[redo ? cursor_ : cursor_ - 1];
            FractureSourcePatch ignored;
            auto candidate = Stage(*source_, redo ? entry.after : entry.before, ignored, context.cancellation);
            if (candidate.HasError())
                return Result<FractureDocumentChange>::Failure(candidate.ErrorValue());
            if (context.cancellation.IsCancellationRequested())
                return Result<FractureDocumentChange>::Failure(MakeError(FractureDocumentErrors::Cancelled));
            source_ = std::move(candidate.Value());
            state_ = redo ? entry.afterState : entry.beforeState;
            cursor_ = redo ? cursor_ + 1 : cursor_ - 1;
            revision_ = FractureDocumentRevision::Create(revision_.Value() + 1).Value();
            return Result<FractureDocumentChange>::Success(FractureDocumentChange::Committed);
        } catch (const std::bad_alloc &) {
            return Result<FractureDocumentChange>::Failure(MakeError(FractureDocumentErrors::HistoryBudgetExceeded));
        }
    }

    /** @copydoc FractureAssetDocument::Undo */
    Result<FractureDocumentChange> FractureAssetDocument::Undo(const FractureDocumentEditContext &context) {
        return Replay(false, context);
    }

    /** @copydoc FractureAssetDocument::Redo */
    Result<FractureDocumentChange> FractureAssetDocument::Redo(const FractureDocumentEditContext &context) {
        return Replay(true, context);
    }

    /** @copydoc FractureAssetDocument::CaptureSave */
    Result<FractureDocumentSave> FractureAssetDocument::CaptureSave() const {
        if (closed_ || !source_)
            return Result<FractureDocumentSave>::Failure(MakeError(FractureDocumentErrors::Closed));
        auto bytes = EncodeFractureAssetSource(*source_);
        if (bytes.HasError())
            return Result<FractureDocumentSave>::Failure(bytes.ErrorValue());
        FractureDocumentSave save;
        save.snapshot_ = Snapshot();
        save.expectedSourceRevision_ = sourceRevision_;
        save.bytes_ = std::move(bytes.Value());
        return Result<FractureDocumentSave>::Success(std::move(save));
    }

    /** @copydoc FractureAssetDocument::AcknowledgeSave */
    Result<void> FractureAssetDocument::AcknowledgeSave(const FractureDocumentSave &save, const std::uint64_t publishedSourceRevision) {
        if (closed_ || !source_)
            return Result<void>::Failure(MakeError(FractureDocumentErrors::Closed));
        if (save.snapshot_.session != session_ || !save.snapshot_.source || save.snapshot_.source->asset != source_->asset)
            return Result<void>::Failure(MakeError(FractureDocumentErrors::WrongDocument));
        if (save.snapshot_.state == savedState_ && publishedSourceRevision == sourceRevision_ &&
            publishedSourceRevision > save.expectedSourceRevision_)
            return Result<void>::Success();
        if (save.expectedSourceRevision_ != sourceRevision_ || publishedSourceRevision <= sourceRevision_)
            return Result<void>::Failure(MakeError(FractureDocumentErrors::PublicationConflict));
        savedState_ = save.snapshot_.state;
        sourceRevision_ = publishedSourceRevision;
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
