/*
 * Copyright (C) 2010 Motorola, Inc.
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/select.h>

#include <linux/l3g4200d.h>

#include <cutils/log.h>

#include "l3g4200d_hal.h"

/*****************************************************************************/

GyroSensor::GyroSensor()
    : SensorBase(GYROSCOPE_DEVICE_NAME, "gyroscope"),
      mEnabled(0),
      mInputReader(32),
      mFlushEnabled(0)
{
    memset(&mPendingEvent, 0, sizeof(mPendingEvent));
    memset(&mFlushEvent, 0, sizeof(mFlushEvent));

    mPendingEvent.version = sizeof(sensors_event_t);
    mPendingEvent.sensor = ID_G;
    mPendingEvent.type = SENSOR_TYPE_GYROSCOPE;
    mPendingEvent.gyro.status = SENSOR_STATUS_ACCURACY_HIGH;

    mFlushEvent.version = META_DATA_VERSION;
    mFlushEvent.sensor = 0;
    mFlushEvent.type = SENSOR_TYPE_META_DATA;
    mFlushEvent.reserved0 = 0;
    mFlushEvent.timestamp = 0;
    mFlushEvent.meta_data.what = META_DATA_FLUSH_COMPLETE;
    mFlushEvent.meta_data.sensor = ID_G;


    open_device();

    int flags = 0;
    if (!ioctl(dev_fd, L3G4200D_IOCTL_GET_ENABLE, &flags)) {
        if (flags)  {
            mEnabled = 1;
        }
    }

    if (!mEnabled) {
        close_device();
    }
}

GyroSensor::~GyroSensor() {
}

int GyroSensor::setEnable(int32_t, int en)
{
    int flags = en ? 1 : 0;
    int err = 0;
    if (flags != mEnabled) {
        if (flags) {
            open_device();
        }
        err = ioctl(dev_fd, L3G4200D_IOCTL_SET_ENABLE, &flags);
        err = err<0 ? -errno : 0;
        ALOGE_IF(err, "L3G4200D_IOCTL_SET_ENABLE failed (%s)", strerror(-err));
        if (!err) {
            mEnabled = flags;
        }
        if (!flags) {
            close_device();
        }
    }
    return err;
}

int GyroSensor::getEnable(int32_t)
{
    return mEnabled;
}

int GyroSensor::setDelay(int32_t handle, int64_t ns)
{
    if (ns < 0)
        return -EINVAL;

    int delay = ns / 1000000;

    if (mEnabled == 0 && dev_fd == -1)
        open_device();

    if (ioctl(dev_fd, L3G4200D_IOCTL_SET_DELAY, &delay)) {
        return -errno;
    }

    if (mEnabled == 0)
        close_device();

    return 0;
}

int GyroSensor::readEvents(sensors_event_t* data, int count)
{
    if (count < 1)
        return -EINVAL;

    int numEventReceived = 0;
    input_event const* event;

    if(count && mFlushEnabled) {
        mFlushEnabled = 0;
        *data++ = mFlushEvent;
        count--;
        numEventReceived++;
    }

    ssize_t n = mInputReader.fill(data_fd);
    if (n < 0)
        return n;

    while (count && mInputReader.readEvent(&event)) {
        int type = event->type;
        if (type == EV_REL) {
            processEvent(event->code, event->value);
        } else if (type == EV_SYN) {
            int64_t time = timevalToNano(event->time);
            mPendingEvent.timestamp = time;
            if (mEnabled) {
                *data++ = mPendingEvent;
                count--;
                numEventReceived++;
            }
        } else {
            ALOGE("GyroSensor: unknown event (type=%d, code=%d)",
                    type, event->code);
        }
        mInputReader.next();
    }

    return numEventReceived;
}

void GyroSensor::processEvent(int code, int value)
{
    switch (code) {
        case EVENT_TYPE_GYRO_P:
            mPendingEvent.gyro.y = value * CONVERT_G_P;
            break;
        case EVENT_TYPE_GYRO_R:
            mPendingEvent.gyro.x = value * CONVERT_G_R;
            break;
        case EVENT_TYPE_GYRO_Y:
            mPendingEvent.gyro.z = value * CONVERT_G_Y;
            break;
    }
}

int GyroSensor::flush(int32_t handle)
{
    mFlushEnabled = 1;
    return 0;
}