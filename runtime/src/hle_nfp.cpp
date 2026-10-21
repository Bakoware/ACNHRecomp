// nn::nfp (amiibo) at the API level: the library initializes, but there is no NFC reader, so the game sees no
// device and amiibo features stay unavailable. First needed by Animal Crossing: New Horizons (nn::nfp::Initialize
// reaches nfp:user over IPC otherwise, and the stub's empty answer crashes the SDK).
#include <nx/runtime.h>

namespace nx {

namespace {

// nn::nfp::ResultNfcDeviceNotFound: module 115 (nfp), description 64.
constexpr uint32_t kNfcDeviceNotFound = 115 | (64u << 9);

void ok(CPU& c) { c.x[0] = 0; }
void not_found(CPU& c) { c.x[0] = kNfcDeviceNotFound; }

// nn::nfp::State: 0 none, 1 initialized.
bool g_initialized = false;
void Initialize(CPU& c) { g_initialized = true, c.x[0] = 0; }
void Finalize(CPU& c) { g_initialized = false, c.x[0] = 0; }
void GetState(CPU& c) { c.x[0] = g_initialized ? 1 : 0; }

// Result ListDevices(DeviceHandle* out, int* count, int max): no reader.
void ListDevices(CPU& c) {
    if (c.x[1]) st<int32_t>(c.x[1], 0);
    c.x[0] = kNfcDeviceNotFound;
}

// DeviceState GetDeviceState(const DeviceHandle&): 0 (initialized, nothing detected).
void GetDeviceState(CPU& c) { c.x[0] = 0; }

// void AttachAvailabilityChangeEvent(os::SystemEventType*): an event that never fires.
void AttachAvailabilityChangeEvent(CPU& c) {
    static uint64_t create = find_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
    if (!create) return;
    uint64_t saved[3] = {c.x[1], c.x[2], c.x[30]};
    c.x[1] = 0;  // EventClearMode_ManualClear
    c.x[2] = 0;  // not inter-process
    call(c, create);
    c.x[1] = saved[0], c.x[2] = saved[1], c.x[30] = saved[2];
}

}  // namespace

NX_HLE("_ZN2nn3nfp10InitializeEv", Initialize);
NX_HLE("_ZN2nn3nfp8FinalizeEv", Finalize);
NX_HLE("_ZN2nn3nfp8GetStateEv", GetState);
NX_HLE("_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii", ListDevices);
NX_HLE("_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE", GetDeviceState);
NX_HLE("_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE", AttachAvailabilityChangeEvent);
NX_HLE("_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE", not_found);
NX_HLE("_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE", not_found);
NX_HLE("_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE", ok);
NX_HLE("_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE", not_found);
NX_HLE("_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE", not_found);
NX_HLE("_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE", not_found);

}  // namespace nx
