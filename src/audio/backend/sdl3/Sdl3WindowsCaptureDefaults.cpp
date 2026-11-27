#include "Horo/Audio/AudioDeviceDiscovery.h"
#include "Sdl3CaptureDefaults.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Instantiate the SDK's MMDevice property keys in this translation unit.
// clang-format off
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
// clang-format on

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace Horo::Audio::Backend::Sdl3Detail {
    namespace {
        /** @brief Balances this control call's COM initialization and respects an existing apartment. */
        class ComApartment final {
        public:
            ComApartment() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}

            ~ComApartment() {
                if (SUCCEEDED(result_))
                    CoUninitialize();
            }

            ComApartment(const ComApartment &) = delete;
            ComApartment &operator=(const ComApartment &) = delete;

            [[nodiscard]] bool Ready() const noexcept {
                return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
            }

        private:
            HRESULT result_;
        };

        /** @brief Releases the reference returned by a successful native call. */
        struct ReleaseCom final {
            void operator()(IUnknown *value) const noexcept {
                value->Release();
            }
        };

        template <class T> using ComPtr = std::unique_ptr<T, ReleaseCom>;

        /** @brief Releases one property with its original type and allocator. */
        struct Property final {
            Property() noexcept = default;

            ~Property() {
                PropVariantClear(&value);
            }

            Property(const Property &) = delete;
            Property &operator=(const Property &) = delete;
            PROPVARIANT value{};
        };

        /** @brief Reads a bounded private endpoint identity from the native device. */
        std::wstring EndpointId(IMMDevice &device) {
            LPWSTR raw{};
            const auto status = device.GetId(&raw);
            const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> identity(raw, CoTaskMemFree);
            constexpr std::size_t MaximumIdLength = 1024;
            if (FAILED(status) || !identity)
                return {};
            const auto end = ::wcsnlen(identity.get(), MaximumIdLength + 1);
            return end <= MaximumIdLength ? std::wstring(identity.get(), end) : std::wstring{};
        }

        /** @brief Reads a UTF-8 label only when the full native name fits the catalog's limit. */
        std::string EndpointName(IPropertyStore &properties) {
            Property property;
            if (FAILED(properties.GetValue(PKEY_Device_FriendlyName, &property.value)) || property.value.vt != VT_LPWSTR ||
                !property.value.pwszVal)
                return {};
            std::array<char, 257> utf8{};
            const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, property.value.pwszVal, -1, utf8.data(),
                                                    static_cast<int>(utf8.size()), nullptr, nullptr);
            return length > 1 ? std::string(utf8.data(), static_cast<std::size_t>(length - 1)) : std::string{};
        }

        /** @brief Correlates the same device format property read by SDL's MMDevice adapter. */
        std::optional<AudioProcessingFormat> EndpointFormat(IPropertyStore &properties) {
            Property property;
            if (FAILED(properties.GetValue(PKEY_AudioEngine_DeviceFormat, &property.value)) || property.value.vt != VT_BLOB)
                return std::nullopt;
            const auto &blob = property.value.blob;
            WAVEFORMATEX spec{};
            if (!blob.pBlobData || blob.cbSize < sizeof(spec))
                return std::nullopt;
            std::memcpy(&spec, blob.pBlobData, sizeof(spec));
            if (spec.nChannels != 1 && spec.nChannels != 2)
                return std::nullopt;
            AudioProcessingFormat format{spec.nSamplesPerSec, MakeAudioSpeakerLayout(spec.nChannels == 1 ? AudioSpeakerPreset::Mono
                                                                                                         : AudioSpeakerPreset::Stereo)};
            if (!ValidateAudioProcessingFormat(format))
                return std::nullopt;
            return format;
        }

        /** @brief Keeps incomplete route properties visible to the uniqueness check. */
        struct Endpoint final {
            std::wstring id;
            std::string name;
            std::optional<AudioProcessingFormat> format;
        };

        /** @brief Collects identity and presentation evidence for one active recording endpoint. */
        Endpoint ReadEndpoint(IMMDevice &device) {
            auto id = EndpointId(device);
            IPropertyStore *raw{};
            const auto status = device.OpenPropertyStore(STGM_READ, &raw);
            const ComPtr<IPropertyStore> properties(raw);
            if (FAILED(status) || !properties)
                return {std::move(id), {}, std::nullopt};
            return {std::move(id), EndpointName(*properties), EndpointFormat(*properties)};
        }

        /** @brief Captures one bounded recording-only inventory without retaining native references. */
        std::vector<Endpoint> ReadEndpoints(IMMDeviceEnumerator &enumerator) {
            IMMDeviceCollection *rawCollection{};
            const auto status = enumerator.EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &rawCollection);
            const ComPtr<IMMDeviceCollection> collection(rawCollection);
            UINT size{};
            if (FAILED(status) || !collection || FAILED(collection->GetCount(&size)) || size > MaximumAudioDiscoveredDevices)
                return {};
            std::vector<Endpoint> endpoints;
            endpoints.reserve(size);
            for (UINT index = 0; index < size; ++index) {
                IMMDevice *raw{};
                const auto item = collection->Item(index, &raw);
                const ComPtr<IMMDevice> device(raw);
                if (FAILED(item) || !device)
                    return {};
                endpoints.push_back(ReadEndpoint(*device));
            }
            return endpoints;
        }

        /** @brief Rejects ambiguous or incomplete route properties before assigning a native ID to a label and format. */
        std::optional<CaptureDefaultRoute> UniqueRoute(const std::span<const Endpoint> endpoints, const std::wstring &id) {
            const auto found = std::ranges::find(endpoints, id, &Endpoint::id);
            if (found == endpoints.end() || found->name.empty() || !found->format)
                return std::nullopt;
            const auto ambiguous = [&found](const Endpoint &entry) {
                return entry.name.empty() ||
                       (&entry != &*found && entry.name == found->name && (!entry.format || entry.format == found->format));
            };
            if (std::ranges::any_of(endpoints, ambiguous))
                return std::nullopt;
            return CaptureDefaultRoute{found->name, *found->format};
        }

        /** @brief Resolves one native role against an active inventory from the same observation. */
        std::optional<WindowsCaptureDefault> ReadRole(IMMDeviceEnumerator &enumerator, const ERole role,
                                                      const std::span<const Endpoint> endpoints) {
            IMMDevice *raw{};
            const auto status = enumerator.GetDefaultAudioEndpoint(eCapture, role, &raw);
            const ComPtr<IMMDevice> device(raw);
            if (FAILED(status) || !device)
                return std::nullopt;
            auto id = EndpointId(*device);
            if (id.empty())
                return std::nullopt;
            auto route = UniqueRoute(endpoints, id);
            if (!route)
                return std::nullopt;
            return WindowsCaptureDefault{std::move(id), std::move(*route)};
        }
    }  // namespace

    /** @copydoc ObserveWindowsCaptureDefaults */
    WindowsCaptureDefaults ObserveWindowsCaptureDefaults() {
        const ComApartment apartment;
        if (!apartment.Ready())
            return {};
        IMMDeviceEnumerator *raw{};
        const auto status = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&raw));
        const ComPtr<IMMDeviceEnumerator> enumerator(raw);
        if (FAILED(status) || !enumerator)
            return {};
        const auto endpoints = ReadEndpoints(*enumerator);
        return {ReadRole(*enumerator, eConsole, endpoints), ReadRole(*enumerator, eMultimedia, endpoints),
                ReadRole(*enumerator, eCommunications, endpoints)};
    }
}  // namespace Horo::Audio::Backend::Sdl3Detail
