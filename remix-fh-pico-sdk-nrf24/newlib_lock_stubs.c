/*
 * newlib_lock_stubs.c — no-op newlib retargeting-lock stubs for bare-metal
 * RP2040 builds with the Arduino pqt-gcc toolchain.
 *
 * The pqt-gcc bundled newlib is built with retargetable locking enabled, so
 * its malloc/stdio require the __retarget_lock_* functions and the __lock___
 * mutex data symbols. The Pico SDK 2.x does not provide them (its own
 * toolchains bundle newlib with internal stubs). This firmware uses newlib
 * only lightly (snprintf under FH_DEBUG_UART, no multithreaded stdio), so
 * plain no-op locks are safe and correct here.
 */

#include <sys/lock.h>

void __retarget_lock_init(_LOCK_T lock) { (void)lock; }
void __retarget_lock_close(_LOCK_T lock) { (void)lock; }
void __retarget_lock_acquire(_LOCK_T lock) { (void)lock; }
void __retarget_lock_release(_LOCK_T lock) { (void)lock; }
int __retarget_lock_try_acquire(_LOCK_T lock) { (void)lock; return 1; }
void __retarget_lock_init_recursive(_LOCK_T lock) { (void)lock; }
void __retarget_lock_close_recursive(_LOCK_T lock) { (void)lock; }
void __retarget_lock_acquire_recursive(_LOCK_T lock) { (void)lock; }
void __retarget_lock_release_recursive(_LOCK_T lock) { (void)lock; }
int __retarget_lock_try_acquire_recursive(_LOCK_T lock) { (void)lock; return 1; }

/* Data objects for newlib's statically allocated mutexes (struct __lock is
 * defined by <sys/lock.h>). */
struct __lock __lock___malloc_recursive_mutex;
struct __lock __lock___sfp_recursive_mutex;
