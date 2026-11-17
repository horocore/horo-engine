#pragma once

/** @file WasapiNativeOwners.h
 * @brief Target-private COM, kernel-handle and notification ownership; no SDK type escapes this target.
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Horo/Audio/Internal/NativeAudioFacts.h"

#include <Windows.h>
#include <atomic>
#include <audioclient.h>
#include <cstdint>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <string>

namespace Horo::Audio::Backend::WasapiDetail {
    /** @brief Balanced MTA admission; an existing STA is an explicit host-affinity failure. */
    struct Apartment final {
        Apartment() = default;
        Apartment(const Apartment &) = delete;
        Apartment &operator=(const Apartment &) = delete;
        HRESULT status{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};

        ~Apartment() {
            if (SUCCEEDED(status))
                CoUninitialize();
        }
    };

    /** @brief Win32 kernel resource owner, destroyed only on detached control or the render worker. */
    struct Handle final {
        Handle() = default;
        Handle(const Handle &) = delete;
        Handle &operator=(const Handle &) = delete;
        HANDLE value{};

        ~Handle() {
            if (value)
                CloseHandle(value);
        }

        void Reset(HANDLE next = nullptr) noexcept {
            if (value)
                CloseHandle(value);
            value = next;
        }
    };

    /** @brief Native mix format allocation returned by WASAPI. */
    struct WaveDelete final {
        void operator()(WAVEFORMATEX *value) const noexcept {
            CoTaskMemFree(value);
        }
    };

    /** @brief Own endpoint ID allocations through every control-side allocation/failure path. */
    struct NativeStringDelete final {
        void operator()(wchar_t *value) const noexcept {
            CoTaskMemFree(value);
        }
    };

    /** @brief Retire native property storage even when presentation string allocation fails. */
    struct PropertyValue final {
        PropertyValue() = default;
        PropertyValue(const PropertyValue &) = delete;
        PropertyValue &operator=(const PropertyValue &) = delete;
        PROPVARIANT value{};

        ~PropertyValue() {
            PropVariantClear(&value);
        }
    };

    /** @brief Notification object owns only immutable endpoint identity and lock-free facts, never a backend pointer. */
    class Notifications final : public IMMNotificationClient {
    public:
        explicit Notifications(std::wstring endpoint) : endpoint_(std::move(endpoint)) {}

        std::atomic<std::uint64_t> changes{};
        NativeAudioLossLatch loss;

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **output) override {
            if (!output)
                return E_POINTER;
            *output = nullptr;
            if (iid != __uuidof(IUnknown) && iid != __uuidof(IMMNotificationClient))
                return E_NOINTERFACE;
            *output = static_cast<IMMNotificationClient *>(this);
            AddRef();
            return S_OK;
        }

        ULONG STDMETHODCALLTYPE AddRef() override {
            return ++references_;
        }

        ULONG STDMETHODCALLTYPE Release() override {
            const auto count = --references_;
            if (!count)
                delete this;
            return count;
        }

        HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD state) override {
            ++changes;
            if (id && endpoint_ == id && state != DEVICE_STATE_ACTIVE)
                loss.Capture(DeviceLossCause::Disconnected);
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override {
            ++changes;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override {
            ++changes;
            if (id && endpoint_ == id)
                loss.Capture(DeviceLossCause::Disconnected);
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole, LPCWSTR) override {
            if (flow == eRender)
                ++changes;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR id, const PROPERTYKEY key) override {
            ++changes;
            if (id && endpoint_ == id && IsEqualPropertyKey(key, PKEY_AudioEngine_DeviceFormat))
                loss.Capture(DeviceLossCause::FormatChanged);
            return S_OK;
        }

    private:
        std::atomic<ULONG> references_{1};
        const std::wstring endpoint_;
    };

}  // namespace Horo::Audio::Backend::WasapiDetail
