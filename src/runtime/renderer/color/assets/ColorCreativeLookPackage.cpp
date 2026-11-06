#include "ColorCreativeLookPackage.h"

#include "ColorOutputPackageInternal.h"

#include <bit>
#include <cmath>
#include <limits>

namespace Horo::Render::Internal {
    namespace {
        using namespace ColorPackageDetail;
        constexpr std::size_t MaximumPayload = 65U * 65U * 65U * 3U * 4U;

        /** @brief Requires the exact closed object schema; missing keys are subsequently read explicitly. */
        void Fields(const Json &value, std::initializer_list<std::string_view> fields) {
            Require(value.is_object());
            Require(value.size() == fields.size());
            Require(Foundation::HasAllowedFields(value, fields));
        }

        /** @brief Decodes one finite representable source-domain scalar without overflow or implicit nonfinite conversion. */
        float Scalar(const Json &value) {
            Require(value.is_number());
            const auto number = value.get<double>();
            Require(std::isfinite(number));
            Require(std::abs(number) <= std::numeric_limits<float>::max());
            return static_cast<float>(number);
        }

        /** @brief Preserves exact explicit AP1 domain component order. */
        Math::Vec3 Domain(const Json &value) {
            Require(value.is_array());
            Require(value.size() == 3);
            return {Scalar(value[0]), Scalar(value[1]), Scalar(value[2])};
        }

        /** @brief Requires the sole executable creative role, domain policy and interpolation vocabulary. */
        ColorLutInterpolation Interpolation(const Json &descriptor) {
            Require(descriptor.at("role") == "CreativeLook");
            Require(descriptor.at("domainPolicy") == "RejectOutsideV1");
            const auto &value = descriptor.at("interpolation");
            Require(value == "Trilinear" || value == "Tetrahedral");
            return value == "Trilinear" ? ColorLutInterpolation::Trilinear : ColorLutInterpolation::Tetrahedral;
        }

        /** @brief Decodes only public schema2 metadata; factory retains enum, domain and dimension authority. */
        ColorTransformDescriptor Descriptor(const Json &value) {
            Fields(value, {"schemaVersion", "role", "configurationRelease", "configurationSHA256", "dimension", "domainMinimum",
                           "domainMaximum", "interpolation", "domainPolicy"});
            ColorTransformDescriptor descriptor;
            descriptor.schemaVersion = UInt(value.at("schemaVersion"));
            descriptor.configurationRelease = UInt(value.at("configurationRelease"));
            descriptor.configurationDigest = Digest(value.at("configurationSHA256"));
            descriptor.dimension = UInt(value.at("dimension"));
            Require(descriptor.dimension >= 2 && descriptor.dimension <= 65);
            descriptor.domainMinimum = Domain(value.at("domainMinimum"));
            descriptor.domainMaximum = Domain(value.at("domainMaximum"));
            descriptor.interpolation = Interpolation(value);
            return descriptor;
        }

        /** @brief Binds a hash record to one fixed ordinary file; no manifest-provided path is opened. */
        Sha256Digest Record(const Json &value, std::string_view filename, const Bytes &bytes) {
            Fields(value, {"file", "bytes", "sha256"});
            Require(value.at("file") == filename);
            Require(!bytes.empty());
            Require(UInt(value.at("bytes")) == bytes.size());
            const auto digest = Digest(value.at("sha256"));
            Require(Hash(bytes) == digest);
            return digest;
        }

        /** @brief Decodes portable source float bits; the canonical factory rejects every nonfinite sample. */
        std::vector<float> Samples(const Bytes &bytes, std::uint32_t dimension) {
            static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
            const auto count = std::size_t{dimension} * dimension * dimension * 3U;
            Require(bytes.size() == count * 4);
            std::vector<float> samples(count);
            for (std::size_t i = 0; i < count; ++i) {
                std::uint32_t bits{};
                for (unsigned channel = 0; channel < 4; ++channel)
                    bits |= std::uint32_t{bytes[i * 4 + channel]} << (channel * 8);
                samples[i] = std::bit_cast<float>(bits);
            }
            return samples;
        }

        /** @brief Reads exact bounded original files and binds their sole digest records before invoking the canonical factory. */
        ColorCreativeLookBundle Load(const std::filesystem::path &root) {
            const auto canonical = Root(root);
            ColorCreativeLookBundle bundle;
            bundle.files[0] = Read(canonical, "manifest.json", 64U * 1024U);
            const auto manifest = Parse(bundle.files[0]);
            Fields(manifest, {"schemaVersion", "kind", "descriptor", "payload", "reference"});
            Require(UInt(manifest.at("schemaVersion")) == 1);
            Require(manifest.at("kind") == "horo-creative-look-package");
            auto descriptor = Descriptor(manifest.at("descriptor"));
            bundle.files[1] = Read(canonical, "look.rgb.f32le", MaximumPayload);
            bundle.files[2] = Read(canonical, "reference.json", 128U * 1024U);
            descriptor.payloadDigest = Record(manifest.at("payload"), "look.rgb.f32le", bundle.files[1]);
            descriptor.referenceDigest = Record(manifest.at("reference"), "reference.json", bundle.files[2]);
            static_cast<void>(Parse(bundle.files[2]));
            const auto samples = Samples(bundle.files[1], descriptor.dimension);
            bundle.look = Take(CookedColorTransform::Create(descriptor, samples));
            return bundle;
        }
    }  // namespace

    /** @copydoc LoadColorCreativeLookPackage */
    Result<ColorCreativeLookBundle> LoadColorCreativeLookPackage(const std::filesystem::path &root) {
        try {
            return Result<ColorCreativeLookBundle>::Success(Load(root));
        } catch (const ColorPackageDetail::Failure &failure) {
            return Result<ColorCreativeLookBundle>::Failure(failure.error);
        } catch (const std::exception &) {
            return Result<ColorCreativeLookBundle>::Failure(ColorPackageDetail::Invalid());
        }
    }
}  // namespace Horo::Render::Internal
