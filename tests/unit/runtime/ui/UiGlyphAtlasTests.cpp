#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiGlyphAtlas.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename Revision> Revision RevisionValue(const std::uint64_t value) {
            const auto revision = Revision::Create(value);
            REQUIRE(revision.HasValue());
            return revision.Value();
        }

        UiOwnershipGeneration Owner() {
            return RevisionValue<UiOwnershipGeneration>(37);
        }

        UiTextFaceId Face() {
            SerializedUiId bytes{};
            bytes.back() = 1;
            const auto face = UiTextFaceId::Create(bytes);
            REQUIRE(face.HasValue());
            return face.Value();
        }

        UiGlyphAtlasGlyphKey Key(const std::uint32_t glyph) {
            return {Face(), glyph, {UiTextScale::Create(UiTextLayoutScaleUnit).Value()}};
        }

        UiGlyphAtlasDescriptor Descriptor(const std::uint32_t maximumFramesInFlight = 2, const std::uint32_t maximumUsesPerFrame = 2) {
            return {Owner(),
                    {8, 4},
                    {4, 4},
                    UiGlyphAtlasRasterFormat::Alpha8,
                    Key(0),
                    {1, 8, 64, maximumFramesInFlight, maximumUsesPerFrame, 1},
                    RevisionValue<UiGlyphAtlasRevision>(1)};
        }

        UiGlyphAtlasRasterData Raster(const UiGlyphAtlasGlyphKey key, std::array<std::byte, 16> &bytes) {
            return {key, UiGlyphAtlasRasterFormat::Alpha8, 4, 4, 4, std::span<const std::byte>{bytes}};
        }

        template <typename Value> void RequireError(const Result<Value> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == expected.code.Value());
        }

        UiGlyphAtlas MakeAtlas() {
            auto created = UiGlyphAtlas::Create(Descriptor());
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        UiGlyphAtlasUploadId CompleteUpload(UiGlyphAtlas &atlas, const UiGlyphAtlasGlyphKey key, std::array<std::byte, 16> &bytes,
                                            const std::uint64_t completionValue) {
            const auto requested = atlas.RequestUpload(Raster(key, bytes));
            REQUIRE(requested.HasValue());
            const auto upload = requested.Value();
            REQUIRE(atlas.DescribeUpload(upload).HasValue());
            REQUIRE(atlas.Payload(upload).HasValue());
            REQUIRE(atlas.MarkSubmitted(upload, {1, completionValue}).HasValue());
            REQUIRE(atlas.Payload(upload).HasError());
            REQUIRE(atlas.Complete(upload).HasValue());
            REQUIRE(atlas.State(upload).Value() == UiGlyphAtlasUploadState::Ready);
            return upload;
        }

        TEST_CASE("Glyph atlas admits bounded uploads and resolves missing glyphs to the resident fallback", "[runtime_ui][glyph_atlas]") {
            auto atlas = MakeAtlas();
            std::array<std::byte, 16> bytes{};
            const auto fallbackUpload = atlas.RequestUpload(Raster(Key(0), bytes));
            REQUIRE(fallbackUpload.HasValue());
            REQUIRE(atlas.Snapshot().pendingEntries == 1);
            REQUIRE(atlas.Snapshot().pendingUploadBytes == bytes.size());

            auto frame = atlas.BeginFrame();
            REQUIRE(frame.HasValue());
            RequireError(atlas.Resolve(frame.Value(), Key(99)), UiErrors::GlyphAtlasFallbackUnavailable);
            REQUIRE(atlas.RetireFrame(frame.Value(), UiGlyphAtlasFrameOutcome::Skipped).HasValue());

            REQUIRE(atlas.MarkSubmitted(fallbackUpload.Value(), {1, 1}).HasValue());
            REQUIRE(atlas.Complete(fallbackUpload.Value()).HasValue());
            const auto lookupFrame = atlas.BeginFrame();
            REQUIRE(lookupFrame.HasValue());
            const auto lookup = atlas.Resolve(lookupFrame.Value(), Key(99));
            REQUIRE(lookup.HasValue());
            REQUIRE(lookup.Value().state == UiGlyphAtlasResolutionState::Fallback);
            REQUIRE(lookup.Value().resolved == Key(0));
            REQUIRE(lookup.Value().IsValid());
            REQUIRE(atlas.RetireFrame(lookupFrame.Value(), UiGlyphAtlasFrameOutcome::Presented).HasValue());
            REQUIRE(atlas.Discard(fallbackUpload.Value()).HasValue());
        }

        TEST_CASE("Glyph atlas preserves frame-pinned entries and applies bounded pressure before eviction",
                  "[runtime_ui][glyph_atlas][eviction]") {
            auto atlas = MakeAtlas();
            std::array<std::byte, 16> fallbackBytes{};
            std::array<std::byte, 16> firstBytes{};
            std::array<std::byte, 16> secondBytes{};
            const auto fallbackUpload = CompleteUpload(atlas, Key(0), fallbackBytes, 1);
            const auto firstUpload = CompleteUpload(atlas, Key(1), firstBytes, 2);

            const auto frame = atlas.BeginFrame();
            REQUIRE(frame.HasValue());
            REQUIRE(atlas.Resolve(frame.Value(), Key(1)).HasValue());
            const auto pressure = atlas.RequestUpload(Raster(Key(2), secondBytes));
            RequireError(pressure, UiErrors::GlyphAtlasPressure);
            REQUIRE(atlas.Snapshot().pressureCount == 1);

            const auto blockedEviction = atlas.Evict({1});
            REQUIRE(blockedEviction.HasValue());
            REQUIRE(blockedEviction.Value().evictedEntries == 0);
            REQUIRE(blockedEviction.Value().pinnedEntries == 1);
            REQUIRE(atlas.RetireFrame(frame.Value(), UiGlyphAtlasFrameOutcome::Presented).HasValue());

            const auto admitted = atlas.RequestUpload(Raster(Key(2), secondBytes));
            REQUIRE(admitted.HasValue());
            REQUIRE(atlas.MarkSubmitted(admitted.Value(), {1, 3}).HasValue());
            REQUIRE(atlas.Complete(admitted.Value()).HasValue());
            const auto eviction = atlas.Evict({1});
            REQUIRE(eviction.HasValue());
            REQUIRE(eviction.Value().evictedEntries == 1);
            REQUIRE(atlas.Discard(fallbackUpload).HasValue());
            REQUIRE(atlas.Discard(firstUpload).HasValue());
            REQUIRE(atlas.Discard(admitted.Value()).HasValue());
        }

        TEST_CASE("Glyph atlas retries failed uploads and deduplicates repeated frame pins", "[runtime_ui][glyph_atlas][retry]") {
            auto atlas = MakeAtlas();
            std::array<std::byte, 16> bytes{};
            const auto failed = atlas.RequestUpload(Raster(Key(1), bytes));
            REQUIRE(failed.HasValue());
            REQUIRE(atlas.Fail(failed.Value(), MakeError(UiErrors::GlyphAtlasUploadInvalid)).HasValue());

            const auto retry = atlas.RequestUpload(Raster(Key(1), bytes));
            REQUIRE(retry.HasValue());
            REQUIRE(retry.Value() != failed.Value());
            REQUIRE(atlas.MarkSubmitted(retry.Value(), {1, 1}).HasValue());
            REQUIRE(atlas.Complete(retry.Value()).HasValue());

            const auto frame = atlas.BeginFrame();
            REQUIRE(frame.HasValue());
            REQUIRE(atlas.Resolve(frame.Value(), Key(1)).HasValue());
            REQUIRE(atlas.Resolve(frame.Value(), Key(1)).HasValue());
            REQUIRE(atlas.Resolve(frame.Value(), Key(1)).HasValue());
            REQUIRE(atlas.Snapshot().pinnedEntries == 1);
            REQUIRE(atlas.RetireFrame(frame.Value(), UiGlyphAtlasFrameOutcome::Presented).HasValue());
            REQUIRE(atlas.Discard(retry.Value()).HasValue());
            RequireError(atlas.State(failed.Value()), UiErrors::GlyphAtlasUploadStale);
        }

        TEST_CASE("Glyph atlas enforces upload, frame, and reset lifecycle ordering", "[runtime_ui][glyph_atlas][lifecycle]") {
            auto atlas = MakeAtlas();
            std::array<std::byte, 16> bytes{};
            const auto pending = atlas.RequestUpload(Raster(Key(0), bytes));
            REQUIRE(pending.HasValue());
            REQUIRE(atlas.IsDrained() == false);
            atlas.StopAdmission();
            REQUIRE(atlas.State() == UiGlyphAtlasState::Closed);
            REQUIRE(atlas.IsDrained());
            REQUIRE(atlas.State(pending.Value()).Value() == UiGlyphAtlasUploadState::Cancelled);
            REQUIRE(atlas.Discard(pending.Value()).HasValue());
            RequireError(atlas.BeginFrame(), UiErrors::GlyphAtlasLifecycleUnavailable);

            auto resetAtlas = MakeAtlas();
            const auto submitted = resetAtlas.RequestUpload(Raster(Key(0), bytes));
            REQUIRE(submitted.HasValue());
            REQUIRE(resetAtlas.MarkSubmitted(submitted.Value(), {1, 1}).HasValue());
            RequireError(resetAtlas.Reset(UiGlyphAtlasResetReason::DeviceLost), UiErrors::GlyphAtlasUploadInFlight);
            REQUIRE(resetAtlas.Cancel(submitted.Value()).HasValue());
            REQUIRE(resetAtlas.IsDrained() == false);
            REQUIRE(resetAtlas.Retire(submitted.Value()).HasValue());
            REQUIRE(resetAtlas.IsDrained());
            REQUIRE(resetAtlas.Discard(submitted.Value()).HasValue());
        }

        TEST_CASE("Glyph atlas reset advances page and upload generations and requires fallback reconstruction",
                  "[runtime_ui][glyph_atlas][reset]") {
            auto atlas = MakeAtlas();
            std::array<std::byte, 16> bytes{};
            const auto fallbackUpload = CompleteUpload(atlas, Key(0), bytes, 1);
            const auto oldPage = atlas.Pages().front();
            const auto oldRevision = atlas.Revision();
            const auto reset = atlas.Reset(UiGlyphAtlasResetReason::Reload);
            REQUIRE(reset.HasValue());
            REQUIRE(reset.Value().previousRevision == oldRevision);
            REQUIRE(reset.Value().revision.Value() == oldRevision.Value() + 1);
            REQUIRE(reset.Value().invalidatedEntries == 1);
            REQUIRE(reset.Value().requiresFallbackUpload);
            REQUIRE(atlas.Pages().front().generation != oldPage.generation);
            RequireError(atlas.State(fallbackUpload), UiErrors::GlyphAtlasUploadStale);

            const auto frame = atlas.BeginFrame();
            REQUIRE(frame.HasValue());
            RequireError(atlas.Resolve(frame.Value(), Key(5)), UiErrors::GlyphAtlasFallbackUnavailable);
            REQUIRE(atlas.RetireFrame(frame.Value(), UiGlyphAtlasFrameOutcome::DeviceLost).HasValue());

            const auto newFallback = atlas.RequestUpload(Raster(Key(0), bytes));
            REQUIRE(newFallback.HasValue());
            REQUIRE(atlas.MarkSubmitted(newFallback.Value(), {1, 2}).HasValue());
            REQUIRE(atlas.Complete(newFallback.Value()).HasValue());
            const auto resolvedFrame = atlas.BeginFrame();
            REQUIRE(resolvedFrame.HasValue());
            REQUIRE(atlas.Resolve(resolvedFrame.Value(), Key(5)).Value().state == UiGlyphAtlasResolutionState::Fallback);
            REQUIRE(atlas.RetireFrame(resolvedFrame.Value(), UiGlyphAtlasFrameOutcome::Presented).HasValue());
        }

        TEST_CASE("Glyph atlas rejects malformed descriptors and stale lifecycle identities", "[runtime_ui][glyph_atlas][validation]") {
            auto invalid = Descriptor();
            invalid.pageExtent.width = 7;
            RequireError(UiGlyphAtlas::Create(invalid), UiErrors::GlyphAtlasInputInvalid);

            auto atlas = MakeAtlas();
            RequireError(atlas.Resolve({}, Key(1)), UiErrors::GlyphAtlasFrameInvalid);
            RequireError(atlas.Evict({0}), UiErrors::GlyphAtlasEvictionInvalid);
            RequireError(atlas.Reset(static_cast<UiGlyphAtlasResetReason>(255)), UiErrors::GlyphAtlasResetInvalid);
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
