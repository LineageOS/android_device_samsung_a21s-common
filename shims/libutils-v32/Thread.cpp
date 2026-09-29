/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Thread.h"
#include <utils/StrongPointer.h>
using android::sp;
namespace utils32 {
android::status_t Thread::run(const char* name, int32_t priority, size_t stack) {
    this->forceIncStrong(this);
    return android::Thread::run(name, priority, stack);
}
}  // namespace utils32
