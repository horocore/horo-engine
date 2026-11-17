/**
 * @copydoc HeadlessMeshCooker.h
 */

#include "HeadlessMeshCooker.h"

#include "../../AssetErrors.h"
#include "Horo/Assets/AssetCook.h"
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Sha256.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

namespace Horo::Assets {
    namespace {
        // ---------------------------------------------------------------------------
        // Payload schema (little-endian):
        //   u32 schema version  = 1
        //   u64 source byte count
        //   32 bytes source SHA-256
        // ---------------------------------------------------------------------------
        constexpr std::uint32_t kPayloadSchemaVersion = 1;

        /**
         * @brief Writes a little-endian u32 into the output buffer.
         */
        void WriteLE32(std::vector<std::uint8_t> &out, std::uint32_t value) {
            for (int i = 0; i < 4; ++i)
                out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
        }

        /**
         * @brief Writes a little-endian u64 into the output buffer.
         */
        void WriteLE64(std::vector<std::uint8_t> &out, std::uint64_t value) {
            for (int i = 0; i < 8; ++i)
                out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
        }

        /**
         * @brief Writes raw bytes into the buffer.
         */
        void WriteBytes(std::vector<std::uint8_t> &out, std::span<const std::uint8_t> bytes) {
            out.insert(out.end(), bytes.begin(), bytes.end());
        }

        class HeadlessMeshCooker final : public ICookerStrategy {
        public:
            /** @brief Declares a source-only producer; all other byte-affecting settings are captured by CacheIdentity. */
            Result<std::optional<std::vector<AssetId>>> InputDependencies(const CookSourceView &,
                                                                          const CancellationToken &) const override {
                return Result<std::optional<std::vector<AssetId>>>::Success(std::vector<AssetId>{});
            }

            [[nodiscard]] Result<CookOutputSink> Cook(const CookSourceView &source, const CancellationToken &cancellation) const override {
                if (cancellation.IsCancellationRequested()) {
                    return Result<CookOutputSink>::Failure(Error{CookErrors::Cancelled.code});
                }

                if (source.bytes.empty()) {
                    return Result<CookOutputSink>::Failure(Error{CookErrors::Cancelled.code});
                }

                if (source.target != AssetCookTargetId::Parse("headless-null").Value()) {
                    return Result<CookOutputSink>::Failure(Error{CookErrors::Cancelled.code});
                }

                CookOutputSink sink;

                // Write schema version (u32 LE)
                WriteLE32(sink.payload, kPayloadSchemaVersion);

                // Write source byte count (u64 LE)
                WriteLE64(sink.payload, static_cast<std::uint64_t>(source.bytes.size()));

                // Write source SHA-256 (32 bytes)
                const auto &digestBytes = source.sourceDigest.bytes;
                WriteBytes(sink.payload, std::span{digestBytes.data(), digestBytes.size()});

                return Result<CookOutputSink>::Success(std::move(sink));
            }
        };
    }  // namespace

    std::shared_ptr<const ICookerStrategy> CreateHeadlessMeshCooker() {
        return std::make_shared<const HeadlessMeshCooker>();
    }

    Result<void> RegisterHeadlessMeshCooker(CookerCatalog &catalog) {
        auto meshType = AssetTypeId::Parse("core.mesh");
        if (meshType.HasError())
            return Result<void>::Failure(meshType.ErrorValue());

        auto nullTarget = AssetCookTargetId::Parse("headless-null");
        if (nullTarget.HasError())
            return Result<void>::Failure(nullTarget.ErrorValue());

        return catalog.Register(CookerContribution{
            .contributionId = "horo.asset-cooker.headless-mesh-validation",
            .assetType = meshType.Value(),
            .targets = {nullTarget.Value()},
            .strategy = CreateHeadlessMeshCooker(),
        });
    }
}  // namespace Horo::Assets
