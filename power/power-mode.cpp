/*
 * Copyright (C) 2021 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <aidl/android/hardware/power/BnPower.h>
#include <aidl/vendor/xiaomi/hw/touchfeature/ITouchFeature.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android/binder_manager.h>

#include <mutex>
#include <sys/ioctl.h>

#include <touch/xiaomi_touch.h>

#define TOUCH_DEV_PATH "/dev/xiaomi-touch"
#define TOUCH_ID 0

namespace aidl {
namespace google {
namespace hardware {
namespace power {
namespace impl {
namespace pixel {

using ::aidl::android::hardware::power::Mode;
using ::aidl::vendor::xiaomi::hw::touchfeature::ITouchFeature;

static std::shared_ptr<ITouchFeature> getTouchFeatureService() {
  static std::mutex service_mutex;
  static std::shared_ptr<ITouchFeature> service;

  std::lock_guard<std::mutex> lock(service_mutex);
  if (service != nullptr) {
    return service;
  }

  ndk::SpAIBinder binder(AServiceManager_checkService(
      "vendor.xiaomi.hw.touchfeature.ITouchFeature/default"));
  if (!binder.get()) {
    LOG(ERROR) << "Failed to get touchfeature service";
    return nullptr;
  }

  service = ITouchFeature::fromBinder(binder);
  if (service == nullptr) {
    LOG(ERROR) << "Failed to convert touchfeature binder to interface";
  }

  return service;
}

bool isDeviceSpecificModeSupported(Mode type, bool* _aidl_return) {
    switch (type) {
        case Mode::DOUBLE_TAP_TO_WAKE:
        case Mode::GAME:
          *_aidl_return = true;
          return true;
        default:
            return false;
    }
}

bool setDeviceSpecificMode(Mode type, bool enabled) {
    switch (type) {
        case Mode::DOUBLE_TAP_TO_WAKE: {
            int fd = open(TOUCH_DEV_PATH, O_RDWR);
            ioctl(fd, TOUCH_IOC_SELECT_TOUCH_ID, TOUCH_ID);
            common_data_t data = {};
            data.touch_id = TOUCH_ID;
            data.cmd = SET_CUR_VALUE;
            data.mode = Touch_Doubletap_Mode;
            data.data_len = 1;
            data.data_buf[0] = enabled ? 1 : 0;
            ioctl(fd, TOUCH_IOC_COMMON_DATA, &data);
            close(fd);
            return true;
        }
        case Mode::GAME: {
          auto touchfeature = getTouchFeatureService();
          if (touchfeature == nullptr) {
            return false;
          }

          int32_t result = 0;
          const auto gameStatus = touchfeature->setModeValue(
              TOUCH_ID, Touch_Game_Mode, enabled ? 1 : 0, &result);
          if (!gameStatus.isOk()) {
            LOG(ERROR) << "setModeValue failed for GAME: "
                       << gameStatus.getDescription();
            return false;
          }

          if (result < 0) {
            LOG(ERROR) << "setModeValue returned failure for GAME: " << result;
            return false;
          }

          const auto superReportStatus = touchfeature->setModeValue(
              TOUCH_ID, Touch_Super_Report, enabled ? 1 : 0, &result);
          if (!superReportStatus.isOk()) {
            LOG(ERROR) << "setModeValue failed for SUPER_REPORT: "
                       << superReportStatus.getDescription();
            return false;
          }

          if (result < 0) {
            LOG(ERROR) << "setModeValue returned failure for SUPER_REPORT: "
                       << result;
            return false;
          }

          return true;
        }
        default:
            return false;
    }
}

}  // namespace pixel
}  // namespace impl
}  // namespace power
}  // namespace hardware
}  // namespace google
}  // namespace aidl
