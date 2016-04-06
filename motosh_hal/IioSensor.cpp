/*
 * Copyright (C) 2016 Motorola Mobility
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

#include <limits>
#include <memory>
#include <chrono>
#include <string.h>
#include "SensorsLog.h"

#include "IioSensor.h"
#include "iio.h"

using namespace std;

vector<shared_ptr<IioSensor>> IioSensor::sensors;
chrono::milliseconds IioSensor::timeout(chrono::seconds(10));

IioSensor::IioSensor(shared_ptr<struct iio_context> iio_ctx, const struct iio_device* dev, int handle) :
    SensorBase("", "", ""), remaining_samples(0),
    iio_ctx(iio_ctx), iio_dev(dev), iio_buf(nullptr) {

    for (unsigned c = 0; c < iio_device_get_channels_count(iio_dev); ++c) {
        struct iio_channel *chan = iio_device_get_channel(iio_dev, c);
        S_LOGD("Chan %d isScan=%d idx=%d id=%s name=%s", c,
                iio_channel_is_scan_element(chan), iio_channel_get_index(chan),
                iio_channel_get_id(chan), iio_channel_get_name(chan));
    }

    // Need to read these first since other attributes depend on them
    iio_device_attr_read_double(iio_dev, "in_scale", &iio_scale);
    iio_device_attr_read_double(iio_dev, "in_offset", &iio_offset);

    // Now populate all the sensor attributes
    sensor.handle       = handle;
    sensor.name         = readIioStr("greybus_name_len", "greybus_name", "Unknown Name");
    //sensor.name       = strdup(iio_device_get_name(iio_dev));
    sensor.vendor       = readIioStr("vendor_len", "vendor", "Unknown Vendor");
    sensor.stringType   = readIioStr("string_type_len", "string_type", "Unknown Type");

    sensor.version                  = readIioInt("greybus_version", 0);
    sensor.type                     = readIioInt("greybus_type", SENSOR_TYPE_DEVICE_PRIVATE_BASE);
    sensor.maxRange                 = convVal(readIioInt<uint32_t>("max_range", 0));
    sensor.resolution               = convVal(readIioInt<uint32_t>("resolution", 0));
    sensor.power                    = readIioInt<uint32_t>("power_uA", 0) * 1e-3; // uA to mA
    sensor.minDelay                 = readIioInt< int32_t>("min_delay_us", 0);
    sensor.maxDelay                 = readIioInt<uint32_t>("max_delay_us", 0);
    sensor.fifoReservedEventCount   = readIioInt<uint32_t>("fifo_rec", 0);
    sensor.fifoMaxEventCount        = readIioInt<uint32_t>("fifo_mec", 0);
    sensor.requiredPermission       = nullptr;
    sensor.flags                    = readIioInt<uint32_t>("flags", 0);
    sensor.reserved[0]              = 0;
    sensor.reserved[1]              = 0;

    // Note: We have no way to communicate to the framework the number of
    // channels (reading_size) for non-standard sensors.
}

IioSensor::~IioSensor() {
    // TODO: In Android-N, send SENSOR_TYPE_DYNAMIC_SENSOR_META to framework to
    // indicate this sensor has disconnected.

    // How do we make sure Android is no longer using these "const *" before
    // deleting them? Need to make it do a getSensorList where we return an
    // empty list first, and then free the pointers.
    free((void*)sensor.name);
    free((void*)sensor.vendor);
    free((void*)sensor.stringType);
    free((void*)sensor.requiredPermission);
}

shared_ptr<struct iio_context> IioSensor::createIioContext() {
    static bool created = false;
    if (created) {
        /** Can only be created once. Boohoo on whoever calls this more than
         * once / process.
         *
         * We can't make iio_ctx a local static variable, since then the
         * destructor will never get called. Making it a global static variable
         * would guarantee the destructor is called, but the undefined static
         * initialization order will cause problems.
         */
        return nullptr;
    } else {
        shared_ptr<struct iio_context> iio_ctx(iio_create_local_context(),
                [](struct iio_context *ptr) {
                    if (ptr) iio_context_destroy(ptr);
                });

        // Since we're configuring our libiio buffers to be non-blocking (i.e. we're doing
        // the poll() outside libiio, this may not be needed.
        if (iio_ctx) {
            iio_context_set_timeout(iio_ctx.get(), chrono::milliseconds(timeout).count());
        }
        created = true;

        // Note: iio_ctx may be NULL if libiio can't read sysfs
        return iio_ctx;
    }
}

void IioSensor::updateSensorList(shared_ptr<struct iio_context> iio_ctx) {
    S_LOGD("+");
    if (! iio_ctx.get()) return;
    //S_LOGD("0x%08x", iio_ctx.get());

    int devCount = iio_context_get_devices_count(iio_ctx.get());

    sensors.clear();
    for (int i = 0; i < devCount; ++i) {
        const struct iio_device *d = iio_context_get_device(iio_ctx.get(), i);
        S_LOGD("Found IIO device %s %s", iio_device_get_name(d), iio_device_get_id(d));

        if (isUsable(d)) {
            sensors.push_back(make_shared<IioSensor>(iio_ctx, d, FIRST_HANDLE + i));

            shared_ptr<IioSensor> s = sensors.back();
            struct sensor_t &sh = s->getHalSensor();
            S_LOGD("Adding greybus IIO device: %d/%d fd=%d %s",
                    sh.handle, FIRST_HANDLE, s->getFd(), sh.name);
        } else {
            S_LOGD("Skipping non-greybus device");
        }
    }
    S_LOGD("sensors = %d", sensors.size());
}

bool IioSensor::isUsable(const struct iio_device *dev) {

    if (iio_device_is_trigger(dev)) return false;

    const char *gb_type = iio_device_find_attr(dev, "greybus_type");
    if (!gb_type) return false; // Only handling greybus sensors for now.

    int nb_channels = iio_device_get_channels_count(dev);
    int out_chan = 0;
    for (int i = 0; i < nb_channels; ++i) {
        struct iio_channel *ch = iio_device_get_channel(dev, i);
        if (iio_channel_is_output(ch)) out_chan++;
    }

    // Expecting at least 3 channels: timestamp, data, sampling frequency.
    // Not sure why sampling is considered an input channel by the kernel.
    return (nb_channels - out_chan) >= 3;
}

const char *IioSensor::readIioStr(const char *lenAttr, const char *strAttr, const char* defaultVal) {
    long long len;
    // We use strdup(defaultVal) so that we can call free() on it without
    // having to keep track of where it came from.
    if (0 == iio_device_attr_read_longlong(iio_dev, lenAttr, &len)) {
        if (len <= 0) return strdup(defaultVal);
        len++; // allow room for null terminator
        char *str = (char *)malloc(len);
        if (str) {
            int res = iio_device_attr_read(iio_dev, strAttr, str, len);
            if (res > 0) {
                return str;
            } else {
                free(str);
            }
        }
    }
    return strdup(defaultVal);
}

void IioSensor::computeChannelOffsets() {
    channel_offset.reserve(iio_device_get_channels_count(iio_dev));

    for (unsigned c = 0; c < iio_device_get_channels_count(iio_dev); ++c) {
        struct iio_channel *chan = iio_device_get_channel(iio_dev, c);
        long idx = iio_channel_get_index(chan);
        if (idx >= 0 && iio_channel_is_scan_element(chan)) {
            channel_offset[idx] =
                reinterpret_cast<uintptr_t>(iio_buffer_first(iio_buf, chan)) -
                reinterpret_cast<uintptr_t>(iio_buffer_start(iio_buf));
        }
    }
}

int IioSensor::readEvents(sensors_event_t* data, int count) {

    /* BIG ASSUMPTION: We (sensors HAL) are the only ones using these IIO
     * devices. No one else is modifying them (enabling/disabling channels)
     * while we're using them. SELinux should prevent anyone else from changing
     * IIO settings.
     *
     * sample_size == iio_buffer_step() when all channels are enabled (which we
     * always do). In general: iio_buffer_step() <= sample_size.
     * */
    ssize_t sample_size = iio_device_get_sample_size(iio_dev);
    int channels = iio_device_get_channels_count(iio_dev);
    uintptr_t start;

    if (channel_offset.size() == 0) {
        computeChannelOffsets();
    }

    if (remaining_samples == 0) {
        ssize_t buffer_bytes = iio_buffer_refill(iio_buf);
        if (buffer_bytes < 0) {
            S_LOGE("Unable to fill buffer: %s", strerror(-buffer_bytes));
            return buffer_bytes;
        }

        start = reinterpret_cast<uintptr_t>(iio_buffer_start(iio_buf));
        remaining_samples = buffer_bytes / sample_size;
    } else {
        start = reinterpret_cast<uintptr_t>(iio_buffer_start(iio_buf)) + (remaining_samples * sample_size);
    }

    ptrdiff_t len = (ptrdiff_t)iio_buffer_end(iio_buf) - start;
    S_LOGD("step=%d sample_size=%lld samples=%d bytes=%lld count=%d",
            iio_buffer_step(iio_buf), sample_size, remaining_samples, len, count);
    //assert(iio_buffer_step(buffer) == sample_size);

    int copied;
    for (copied = 0;
            copied < remaining_samples && copied < count;
            ++copied, start += sample_size) {

        sensors_event_t &d = data[copied];
        d.version   = sizeof(sensors_event_t);
        d.sensor    = sensor.handle;
        d.type      = sensor.type;
        bzero(d.data, sizeof(d.data)); // For debug purposes

        int32_t unscaled;
        for (int c = 0; c < min<int>(channels, IioSensor::MAX_CHANNELS); ++c) {
            struct iio_channel *chan = iio_device_get_channel(iio_dev, c);
            long index = iio_channel_get_index(chan);
            const char *chan_id = iio_channel_get_id(chan);
            if (index >= 0 && chan_id) {
                if (0 == strcmp(chan_id, "timestamp")) {
                    iio_channel_convert(chan, &(d.timestamp), reinterpret_cast<void *>(start + channel_offset[index]));
                } else {
                    iio_channel_convert(chan, &unscaled, reinterpret_cast<void *>(start + channel_offset[index]));
                    d.data[index] = (float)convVal(unscaled);
                }
            }
        }

        d.flags = 0;
    }

    remaining_samples -= copied;
    return copied;
}

int IioSensor::getFd() const {
    if (iio_buf) {
        return iio_buffer_get_poll_fd(iio_buf);
    } else {
        return -1;
    }
}

int IioSensor::setEnable(int32_t handle, int enabled) {
    int ret = 0;
    S_LOGD("handle=%d enabled=%d iio_buf=%08x", handle, enabled, iio_buf);
    if (! hasSensor(handle)) return -EINVAL;

    if (enabled) {
        // Enable all input channels
        int channels = iio_device_get_channels_count(iio_dev);
        for (int i = 0; i < channels; ++i) {
            struct iio_channel *c = iio_device_get_channel(iio_dev, i);
            if (! iio_channel_is_output(c) &&
                  iio_channel_is_scan_element(c)) {
                iio_channel_enable(c);
            }
        }

        if (!iio_buf) {
            // We must use cyclic=false or else we won't be able to configure
            // the buffer to non-blocking below.
            iio_buf = iio_device_create_buffer(iio_dev, BUFFER_LEN, false);
            S_LOGD("Enabled %d (fd=%d)", iio_buf != NULL, getFd());
            if (!iio_buf) {
                ret = -errno;
                S_LOGE("Failed to create buffer: %s", strerror(errno));
                goto exit;
            }

            // Set the buffer to non-blocking, so libiio doesn't POLLIN.
            // We will do the poll(POLLIN) ourselves.
            iio_buffer_set_blocking_mode(iio_buf, false);
        }
    } else {
        int channels = iio_device_get_channels_count(iio_dev);
        for (int i = 0; i < channels; ++i) {
            struct iio_channel *c = iio_device_get_channel(iio_dev, i);
            if (! iio_channel_is_output(c) &&
                  iio_channel_is_scan_element(c)) {
                iio_channel_disable(c);
            }
        }
        if (iio_buf) {
            iio_buffer_destroy(iio_buf);
            iio_buf = nullptr;
        }
    }

exit:
    return ret;
}

int IioSensor::batch(int32_t handle, int flags, int64_t sampling_period_ns,
        int64_t max_report_latency_ns) {

    (void)flags;
    int res;
    chrono::duration<double, std::nano> period(sampling_period_ns);

    S_LOGD("period=%lld latency=%lld", sampling_period_ns, max_report_latency_ns);

    if (! hasSensor(handle) || period < chrono::microseconds(1)) return -EINVAL;

    res = iio_device_attr_write_longlong(iio_dev, "max_latency_ns", max_report_latency_ns);
    if (res < 0) {
        S_LOGD("Setting max_latency res=%d err=%s", res, res < 0 ? strerror(res) : "NoError");
    }

    double freq = chrono::duration<double>(1.0) / period;
    res = iio_device_attr_write_double(iio_dev, "in_sampling_frequency", freq);
    if (res < 0) {
        S_LOGD("Setting in_sampling_frequency res=%d err=%s", res, res < 0 ? strerror(res) : "NoError");
    }

    return res;
}

int IioSensor::flush(int32_t handle) {
    if (hasSensor(handle)) {
        if ((sensor.flags & REPORTING_MODE_MASK) == SENSOR_FLAG_ONE_SHOT_MODE) {
            // Have to return -EINVAL for one-shot sensors per Android spec
            return -EINVAL;
        } else {
            return iio_device_attr_write_longlong(iio_dev, "flush", 1);
        }
    }
    return -EINVAL;
}

bool IioSensor::hasSensor(int handle) {
    return this->sensor.handle == handle;
}