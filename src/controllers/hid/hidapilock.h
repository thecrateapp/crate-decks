#pragma once

#include <mutex>

namespace mixxx::hid {

/// hidapi keeps global state that is not thread-safe: hid_init() and every
/// failed call replace and free its last error string, and hid_enumerate(),
/// hid_open() and hid_open_path() all call hid_init(). Each HidController
/// opens its device in a background thread to read the report descriptor,
/// one thread per device and all at once, while the enumerator may still be
/// listing devices. With several HID devices that freed the same string
/// twice and corrupted the heap. Every call that touches that state, and
/// reading hid_error(nullptr), holds this lock.
///
/// TEST_GAP: the race needs several real HID devices opened from threads at
/// once; it was reproduced and checked on a desktop with glibc's malloc
/// checker (libc_malloc_debug.so, glibc.malloc.check=3).
inline std::mutex& apiMutex() {
    static std::mutex mutex;
    return mutex;
}

} // namespace mixxx::hid
