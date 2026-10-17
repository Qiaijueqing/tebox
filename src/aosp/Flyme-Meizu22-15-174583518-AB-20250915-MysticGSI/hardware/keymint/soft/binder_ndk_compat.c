// Android 15 system: the Android 16 KeyMint NDK libraries in
// /vendor/lib64/keymint call AIBinder_Class_setTransactionCodeToFunctionNameMap
// (libbinder_ndk 36), which only names transactions for debugging. Preloaded
// into the KeyMint service (see its .rc); a no-op is enough.
#include <stddef.h>

typedef struct AIBinder_Class AIBinder_Class;

void AIBinder_Class_setTransactionCodeToFunctionNameMap(AIBinder_Class* clazz,
                                                         const char* const* names, size_t size) {
    (void)clazz;
    (void)names;
    (void)size;
}
