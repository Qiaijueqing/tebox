// OneUI: placeholders for Samsung vendor HALs that system_server waits for
// without a timeout during boot (ServiceManager.waitForService). A Samsung
// device's vendor provides them; this GSI on this vendor does not, and the
// ones system_ext ships (Knox Guard's kgtaserv) need the phone's TEE. Every
// call is "not implemented": Samsung's clients see a failed call instead of
// blocking the boot. An instance declared in a VINTF manifest is registered
// with VINTF stability, as its clients require.
//
// ISehHyPer (Samsung's DVFS HAL) answers instead: SemDvfsManager returns its
// frequency tables to apps, and the device health service (sdhms) crash-loops
// on a null CPU table. It reports fixed CPU/GPU tables and accepts every
// boost request without doing anything; there is no cpufreq in the guest.
#include <android/binder_ibinder.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/binder_stability.h>
#include <android/log.h>

#include <cstring>
#include <string>

namespace {

// descriptor + "/default", waited for by:
constexpr const char* kServices[] = {
    "vendor.samsung.hardware.tlc.kg.ISehKg",                  // KnoxGuardSeService
    "vendor.samsung.hardware.displayquality.ISehDisplayQuality",  // SemDisplayQualityManagerService
    "vendor.samsung.hardware.khdm.ISehKhdm",                  // services.jar
    "vendor.samsung.hardware.tlc.ddar.ISehDdar",              // services.jar
};

void* onCreate(void* args) { return args; }
void onDestroy(void*) {}
binder_status_t onTransact(AIBinder*, transaction_code_t, const AParcel*, AParcel*) {
    return STATUS_UNKNOWN_TRANSACTION;
}

// vendor.samsung.hardware.hyper.ISehHyPer V2; transaction codes from the
// GSI's vendor.samsung.hardware.hyper-V2-ndk.so.
constexpr const char* kHyPer = "vendor.samsung.hardware.hyper.ISehHyPer";
constexpr const char* kHyPerHash = "a441ea0d6fcf9a8d771c17914859ac86346259a5";
enum : transaction_code_t {
    kAcquire = 1,
    kRelease = 2,
    kGetSupportedFrequency = 3,
    kWriteSysfs = 4,
    kReadSysfs = 5,
    kCheckSysfsIdExist = 6,
    kCheckHintExist = 7,
    kCheckResourceExist = 8,
    kAcquireWithProcName = 9,
    kGetInterfaceHash = 16777214,
    kGetInterfaceVersion = 16777215,
};

// SemDvfsManager resource types: class in the top byte, 0x1001 min / 0x1002
// max in the low bits. Tables are in kHz, highest first, as SemDvfsManager
// expects (Snapdragon 8 Gen 2 values; nothing reads them back from hardware).
constexpr int32_t kResourceMask = 0xff000000;
constexpr int32_t kCpuLittle = 0x11000000;
constexpr int32_t kCpuBig = 0x12000000;
constexpr int32_t kGpu = 0x20000000;
constexpr int32_t kCpuBigFreq[] = {3187200, 2956800, 2726400, 2496000, 2265600, 2035200,
                                   1804800, 1574400, 1344000, 1113600, 883200, 652800};
constexpr int32_t kCpuLittleFreq[] = {2016000, 1785600, 1555200, 1324800, 1094400,
                                      864000,  633600,  403200,  307200};
constexpr int32_t kGpuFreq[] = {719000, 680000, 615000, 550000, 475000,
                                401000, 348000, 295000, 220000};

bool frequencyTable(int32_t type, const int32_t** table, int32_t* count) {
    const int32_t low = type & 0xffff;
    if (low != 0x1001 && low != 0x1002) return false;
    switch (type & kResourceMask) {
        case kCpuBig:
            *table = kCpuBigFreq;
            *count = sizeof(kCpuBigFreq) / sizeof(kCpuBigFreq[0]);
            return true;
        case kCpuLittle:
            *table = kCpuLittleFreq;
            *count = sizeof(kCpuLittleFreq) / sizeof(kCpuLittleFreq[0]);
            return true;
        case kGpu:
            *table = kGpuFreq;
            *count = sizeof(kGpuFreq) / sizeof(kGpuFreq[0]);
            return true;
    }
    return false;
}

binder_status_t writeOk(AParcel* out) {
    AStatus* ok = AStatus_newOk();
    const binder_status_t status = AParcel_writeStatusHeader(out, ok);
    AStatus_delete(ok);
    return status;
}

binder_status_t hyperOnTransact(AIBinder*, transaction_code_t code, const AParcel* in,
                                AParcel* out) {
    int32_t type = 0;
    const int32_t* table = nullptr;
    int32_t count = 0;
    switch (code) {
        case kAcquire:
        case kRelease:
        case kWriteSysfs:
        case kAcquireWithProcName:
            return writeOk(out);
        case kGetSupportedFrequency:
            AParcel_readInt32(in, &type);
            writeOk(out);
            if (!frequencyTable(type, &table, &count)) return AParcel_writeInt32Array(out, nullptr, 0);
            return AParcel_writeInt32Array(out, table, count);
        case kReadSysfs:
            writeOk(out);
            return AParcel_writeString(out, "", 0);
        case kCheckSysfsIdExist:
        case kCheckHintExist:
            writeOk(out);
            return AParcel_writeBool(out, false);
        case kCheckResourceExist:
            AParcel_readInt32(in, &type);
            writeOk(out);
            return AParcel_writeBool(out, frequencyTable(type, &table, &count));
        case kGetInterfaceVersion:
            writeOk(out);
            return AParcel_writeInt32(out, 2);
        case kGetInterfaceHash:
            writeOk(out);
            return AParcel_writeString(out, kHyPerHash, static_cast<int32_t>(strlen(kHyPerHash)));
    }
    return STATUS_UNKNOWN_TRANSACTION;
}

bool publish(const char* descriptor, AIBinder_Class_onTransact transact) {
    const std::string instance = std::string(descriptor) + "/default";
    AIBinder_Class* cls = AIBinder_Class_define(descriptor, onCreate, onDestroy, transact);
    AIBinder* binder = AIBinder_new(cls, nullptr);
    const bool declared = AServiceManager_isDeclared(instance.c_str());
    if (declared) AIBinder_markVintfStability(binder);
    const binder_status_t status = AServiceManager_addService(binder, instance.c_str());
    __android_log_print(status == STATUS_OK ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
                        "samsung-stubs", "register %s (%s): %d", instance.c_str(),
                        declared ? "vintf" : "local", status);
    return status == STATUS_OK;
}

}  // namespace

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(2);
    int registered = 0;
    for (const char* descriptor : kServices) registered += publish(descriptor, onTransact);
    registered += publish(kHyPer, hyperOnTransact);
    if (!registered) return 1;
    ABinderProcess_joinThreadPool();
    return 0;
}
