/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "UdfpsHandler.onyx"

#include <aidl/android/hardware/biometrics/fingerprint/BnFingerprint.h>
#include <android-base/logging.h>
#include <android-base/unique_fd.h>

#include <poll.h>
#include <sys/ioctl.h>
#include <fstream>
#include <thread>

#include <display/drm/mi_disp.h>
#include <touch/xiaomi_touch.h>

#include "UdfpsHandler.h"

#define COMMAND_NIT 10
#define PARAM_NIT_FOD 1
#define PARAM_NIT_NONE 0

#define COMMAND_FOD_PRESS_STATUS 1
#define PARAM_FOD_PRESSED 1
#define PARAM_FOD_RELEASED 0

#define DISP_FEATURE_PATH "/dev/mi_display/disp_feature"
#define TOUCH_DEV_PATH "/dev/xiaomi-touch"

#define FOD_PRESS_STATUS_PATH "/sys/class/touch/touch_dev/fod_press_status"

using ::aidl::android::hardware::biometrics::fingerprint::AcquiredInfo;

namespace {

static bool readBool(int fd) {
    char c;
    int rc;

    rc = lseek(fd, 0, SEEK_SET);
    if (rc) {
        LOG(ERROR) << "failed to seek fd, err: " << rc;
        return false;
    }

    rc = read(fd, &c, sizeof(char));
    if (rc != 1) {
        LOG(ERROR) << "failed to read bool from fd, err: " << rc;
        return false;
    }

    return c != '0';
}

static disp_event_resp* parseDispEvent(int fd) {
    disp_event header;
    ssize_t headerSize = read(fd, &header, sizeof(header));
    if (headerSize < sizeof(header)) {
        LOG(ERROR) << "unexpected display event header size: " << headerSize;
        return nullptr;
    }

    struct disp_event_resp* response =
            reinterpret_cast<struct disp_event_resp*>(malloc(header.length));
    response->base = header;

    int dataLength = response->base.length - sizeof(response->base);
    if (dataLength < 0) {
        LOG(ERROR) << "invalid data length: " << response->base.length;
        return nullptr;
    }

    ssize_t dataSize = read(fd, &response->data, dataLength);
    if (dataSize < dataLength) {
        LOG(ERROR) << "unexpected display event data size: " << dataSize;
        return nullptr;
    }

    return response;
}

struct disp_base displayBasePrimary = {
        .flag = 0,
        .disp_id = MI_DISP_PRIMARY,
};

common_data_t touchDataPrimary = {
        .touch_id = MI_DISP_PRIMARY,
        .cmd = SET_CUR_VALUE,
        .mode = 0,
        .data_len = 1,
        .data_buf = {},
};

}  // anonymous namespace

class XiaomiOnyxUdfpsHandler : public UdfpsHandler {
  public:
    void init(fingerprint_device_t* device) {
        mDevice = device;
        touch_fd_ = android::base::unique_fd(open(TOUCH_DEV_PATH, O_RDWR));
        disp_fd_ = android::base::unique_fd(open(DISP_FEATURE_PATH, O_RDWR));

        ioctl(touch_fd_.get(), TOUCH_IOC_SELECT_TOUCH_ID, MI_DISP_PRIMARY);
        // Thread to notify fingeprint hwmodule about fod presses
        std::thread([this]() {
            android::base::unique_fd fd(open(FOD_PRESS_STATUS_PATH, O_RDONLY));
            if (fd < 0) {
                LOG(ERROR) << "failed to open " << FOD_PRESS_STATUS_PATH << " , err: " << fd;
                return;
            }

            struct pollfd fodPressStatusPoll = {
                    .fd = fd.get(),
                    .events = POLLERR | POLLPRI,
                    .revents = 0,
            };

            while (true) {
                int rc = poll(&fodPressStatusPoll, 1, -1);
                if (rc < 0) {
                    LOG(ERROR) << "failed to poll " << FOD_PRESS_STATUS_PATH << ", err: " << rc;
                    continue;
                }

                bool pressed = readBool(fd);
                mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS,
                                pressed ? PARAM_FOD_PRESSED : PARAM_FOD_RELEASED);

                // Request HBM
                struct disp_local_hbm_req displayLhbmRequest = {
                        .base = displayBasePrimary,
                        .local_hbm_value = pressed ? LHBM_TARGET_BRIGHTNESS_WHITE_1000NIT
                                              : LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP,
                };
                ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &displayLhbmRequest);
            }
        }).detach();

        // Thread to listen for fod ui changes
        std::thread([this]() {
            android::base::unique_fd fd(open(DISP_FEATURE_PATH, O_RDWR));
            if (fd < 0) {
                LOG(ERROR) << "failed to open " << DISP_FEATURE_PATH << " , err: " << fd;
                return;
            }

            // Register for FOD events
            struct disp_event_req displayEventRequest = {
                    .base = displayBasePrimary,
                    .type = MI_DISP_EVENT_FOD,
            };
            if (ioctl(fd.get(), MI_DISP_IOCTL_REGISTER_EVENT, &displayEventRequest) < 0) {
                LOG(ERROR) << "failed to register FOD event";
                return;
            }

            struct pollfd dispEventPoll = {
                    .fd = fd.get(),
                    .events = POLLIN,
                    .revents = 0,
            };

            while (true) {
                int rc = poll(&dispEventPoll, 1, -1);
                if (rc < 0) {
                    LOG(ERROR) << "failed to poll " << DISP_FEATURE_PATH << ", err: " << rc;
                    continue;
                }

                struct disp_event_resp* response = parseDispEvent(fd);
                if (response == nullptr) {
                    continue;
                }

                if (response->base.type != MI_DISP_EVENT_FOD) {
                    LOG(ERROR) << "unexpected display event: " << response->base.type;
                    continue;
                }

                int value = response->data[0];
                LOG(DEBUG) << "received data: " << std::bitset<8>(value);

                bool localHbmUiReady = value & LOCAL_HBM_UI_READY;

                mDevice->extCmd(mDevice, COMMAND_NIT,
                                localHbmUiReady ? PARAM_NIT_FOD : PARAM_NIT_NONE);
            }
        }).detach();
    }

    void onFingerDown(uint32_t /*x*/, uint32_t /*y*/, float /*minor*/, float /*major*/) {
        LOG(INFO) << __func__;
        // Ensure touchscreen is aware of the press state, ideally this is not needed
        setFingerDown(true);
    }

    void onFingerUp() {
        LOG(INFO) << __func__;
        // Ensure touchscreen is aware of the press state, ideally this is not needed
        setFingerDown(false);
    }

    void onAcquired(int32_t result, int32_t vendorCode) {
        LOG(INFO) << __func__ << " result: " << result << " vendorCode: " << vendorCode;
        switch (static_cast<AcquiredInfo>(result)) {
            case AcquiredInfo::GOOD:
            case AcquiredInfo::PARTIAL:
            case AcquiredInfo::INSUFFICIENT:
            case AcquiredInfo::SENSOR_DIRTY:
            case AcquiredInfo::TOO_SLOW:
            case AcquiredInfo::TOO_FAST:
            case AcquiredInfo::TOO_DARK:
            case AcquiredInfo::TOO_BRIGHT:
            case AcquiredInfo::IMMOBILE:
            case AcquiredInfo::LIFT_TOO_SOON:
                setFingerDown(false);
                break;
            default:
                break;
        }
    }

    void cancel() {
        LOG(INFO) << __func__;
    }

  private:
    fingerprint_device_t* mDevice;
    android::base::unique_fd touch_fd_;
    android::base::unique_fd disp_fd_;

    void setFingerDown(bool pressed) {
        common_data_t data = {
            .mode = THP_FOD_DOWNUP_CTL,
            .data_buf = {pressed ? 1 : 0},
        };
        ioctl(touch_fd_.get(), TOUCH_IOC_COMMON_DATA, &data);
    }
};

static UdfpsHandler* create() {
    return new XiaomiOnyxUdfpsHandler();
}

static void destroy(UdfpsHandler* handler) {
    delete handler;
}

extern "C" UdfpsHandlerFactory UDFPS_HANDLER_FACTORY = {
        .create = create,
        .destroy = destroy,
};
