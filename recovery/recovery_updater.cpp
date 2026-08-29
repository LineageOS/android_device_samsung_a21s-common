/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <android-base/properties.h>
#include "edify/expr.h"
#include "otautil/error_code.h"

Value* VerifyBootloader(const char* name, State* state,
                        const std::vector<std::unique_ptr<Expr>>& /*argv*/) {
  std::string bootloader = android::base::GetProperty("ro.boot.bootloader", "");
  if (bootloader.empty()) {
    return ErrorAbort(state, kFileGetPropFailure,
                      "%s() failed to read current bootloader version", name);
  }

  if (bootloader.length() < 9) {
    return ErrorAbort(state, kArgsParsingFailure,
                      "%s() bootloader string too short: %s", name, bootloader.c_str());
  }

  char ninth_char = bootloader[8]; // index 8 = 9th character
  if (ninth_char == 'C') {
    return StringValue("1");
  } else {
    return StringValue("0");
  }
}

void Register_librecovery_updater_exynos850() {
  RegisterFunction("exynos850.verify_bootloader_min", VerifyBootloader);
}
