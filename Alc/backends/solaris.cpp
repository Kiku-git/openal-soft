/**
 * OpenAL cross platform audio library
 * Copyright (C) 1999-2007 by authors.
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"

#include "backends/solaris.h"

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <memory.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <math.h>

#include <thread>

#include "alMain.h"
#include "alu.h"
#include "alconfig.h"
#include "threads.h"
#include "compat.h"

#include <sys/audioio.h>


struct ALCsolarisBackend final : public ALCbackend {
    int fd{-1};

    ALubyte *mix_data{nullptr};
    int data_size{0};

    std::atomic<ALenum> mKillNow{AL_TRUE};
    std::thread mThread;
};

static int ALCsolarisBackend_mixerProc(ALCsolarisBackend *self);

static void ALCsolarisBackend_Construct(ALCsolarisBackend *self, ALCdevice *device);
static void ALCsolarisBackend_Destruct(ALCsolarisBackend *self);
static ALCenum ALCsolarisBackend_open(ALCsolarisBackend *self, const ALCchar *name);
static ALCboolean ALCsolarisBackend_reset(ALCsolarisBackend *self);
static ALCboolean ALCsolarisBackend_start(ALCsolarisBackend *self);
static void ALCsolarisBackend_stop(ALCsolarisBackend *self);
static DECLARE_FORWARD2(ALCsolarisBackend, ALCbackend, ALCenum, captureSamples, void*, ALCuint)
static DECLARE_FORWARD(ALCsolarisBackend, ALCbackend, ALCuint, availableSamples)
static DECLARE_FORWARD(ALCsolarisBackend, ALCbackend, ClockLatency, getClockLatency)
static DECLARE_FORWARD(ALCsolarisBackend, ALCbackend, void, lock)
static DECLARE_FORWARD(ALCsolarisBackend, ALCbackend, void, unlock)
DECLARE_DEFAULT_ALLOCATORS(ALCsolarisBackend)

DEFINE_ALCBACKEND_VTABLE(ALCsolarisBackend);


static const ALCchar solaris_device[] = "Solaris Default";

static const char *solaris_driver = "/dev/audio";


static void ALCsolarisBackend_Construct(ALCsolarisBackend *self, ALCdevice *device)
{
    new (self) ALCsolarisBackend{};
    ALCbackend_Construct(STATIC_CAST(ALCbackend, self), device);
    SET_VTABLE2(ALCsolarisBackend, ALCbackend, self);
}

static void ALCsolarisBackend_Destruct(ALCsolarisBackend *self)
{
    if(self->fd != -1)
        close(self->fd);
    self->fd = -1;

    free(self->mix_data);
    self->mix_data = nullptr;
    self->data_size = 0;

    ALCbackend_Destruct(STATIC_CAST(ALCbackend, self));
    self->~ALCsolarisBackend();
}


static int ALCsolarisBackend_mixerProc(ALCsolarisBackend *self)
{
    ALCdevice *device{STATIC_CAST(ALCbackend, self)->mDevice};

    SetRTPriority();
    althrd_setname(MIXER_THREAD_NAME);

    const int frame_size{device->frameSizeFromFmt()};

    ALCsolarisBackend_lock(self);
    while(!self->mKillNow.load(std::memory_order_acquire) &&
          device->Connected.load(std::memory_order_acquire))
    {
        pollfd pollitem{};
        pollitem.fd = self->fd;
        pollitem.events = POLLOUT;

        ALCsolarisBackend_unlock(self);
        int pret{poll(&pollitem, 1, 1000)};
        ALCsolarisBackend_lock(self);
        if(pret < 0)
        {
            if(errno == EINTR || errno == EAGAIN)
                continue;
            ERR("poll failed: %s\n", strerror(errno));
            aluHandleDisconnect(device, "Failed to wait for playback buffer: %s", strerror(errno));
            break;
        }
        else if(pret == 0)
        {
            WARN("poll timeout\n");
            continue;
        }

        ALubyte *write_ptr{self->mix_data};
        int to_write{self->data_size};
        aluMixData(device, write_ptr, to_write/frame_size);
        while(to_write > 0 && !self->mKillNow.load())
        {
            ssize_t wrote{write(self->fd, write_ptr, to_write)};
            if(wrote < 0)
            {
                if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                    continue;
                ERR("write failed: %s\n", strerror(errno));
                aluHandleDisconnect(device, "Failed to write playback samples: %s",
                                    strerror(errno));
                break;
            }

            to_write -= wrote;
            write_ptr += wrote;
        }
    }
    ALCsolarisBackend_unlock(self);

    return 0;
}


static ALCenum ALCsolarisBackend_open(ALCsolarisBackend *self, const ALCchar *name)
{
    ALCdevice *device;

    if(!name)
        name = solaris_device;
    else if(strcmp(name, solaris_device) != 0)
        return ALC_INVALID_VALUE;

    self->fd = open(solaris_driver, O_WRONLY);
    if(self->fd == -1)
    {
        ERR("Could not open %s: %s\n", solaris_driver, strerror(errno));
        return ALC_INVALID_VALUE;
    }

    device = STATIC_CAST(ALCbackend,self)->mDevice;
    device->DeviceName = name;

    return ALC_NO_ERROR;
}

static ALCboolean ALCsolarisBackend_reset(ALCsolarisBackend *self)
{
    ALCdevice *device = STATIC_CAST(ALCbackend,self)->mDevice;
    audio_info_t info;
    ALsizei frameSize;
    ALsizei numChannels;

    AUDIO_INITINFO(&info);

    info.play.sample_rate = device->Frequency;

    if(device->FmtChans != DevFmtMono)
        device->FmtChans = DevFmtStereo;
    numChannels = device->channelsFromFmt();
    info.play.channels = numChannels;

    switch(device->FmtType)
    {
        case DevFmtByte:
            info.play.precision = 8;
            info.play.encoding = AUDIO_ENCODING_LINEAR;
            break;
        case DevFmtUByte:
            info.play.precision = 8;
            info.play.encoding = AUDIO_ENCODING_LINEAR8;
            break;
        case DevFmtUShort:
        case DevFmtInt:
        case DevFmtUInt:
        case DevFmtFloat:
            device->FmtType = DevFmtShort;
            /* fall-through */
        case DevFmtShort:
            info.play.precision = 16;
            info.play.encoding = AUDIO_ENCODING_LINEAR;
            break;
    }

    frameSize = numChannels * device->bytesFromFmt();
    info.play.buffer_size = device->UpdateSize*device->NumUpdates * frameSize;

    if(ioctl(self->fd, AUDIO_SETINFO, &info) < 0)
    {
        ERR("ioctl failed: %s\n", strerror(errno));
        return ALC_FALSE;
    }

    if(device->channelsFromFmt() != (ALsizei)info.play.channels)
    {
        ERR("Failed to set %s, got %u channels instead\n", DevFmtChannelsString(device->FmtChans),
            info.play.channels);
        return ALC_FALSE;
    }

    if(!((info.play.precision == 8 && info.play.encoding == AUDIO_ENCODING_LINEAR8 && device->FmtType == DevFmtUByte) ||
         (info.play.precision == 8 && info.play.encoding == AUDIO_ENCODING_LINEAR && device->FmtType == DevFmtByte) ||
         (info.play.precision == 16 && info.play.encoding == AUDIO_ENCODING_LINEAR && device->FmtType == DevFmtShort) ||
         (info.play.precision == 32 && info.play.encoding == AUDIO_ENCODING_LINEAR && device->FmtType == DevFmtInt)))
    {
        ERR("Could not set %s samples, got %d (0x%x)\n", DevFmtTypeString(device->FmtType),
            info.play.precision, info.play.encoding);
        return ALC_FALSE;
    }

    device->Frequency = info.play.sample_rate;
    device->UpdateSize = (info.play.buffer_size/device->NumUpdates) + 1;

    SetDefaultChannelOrder(device);

    free(self->mix_data);
    self->data_size = device->UpdateSize * device->frameSizeFromFmt();
    self->mix_data = static_cast<ALubyte*>(calloc(1, self->data_size));

    return ALC_TRUE;
}

static ALCboolean ALCsolarisBackend_start(ALCsolarisBackend *self)
{
    try {
        self->mKillNow.store(AL_FALSE);
        self->mThread = std::thread(ALCsolarisBackend_mixerProc, self);
        return ALC_TRUE;
    }
    catch(std::exception& e) {
        ERR("Could not create playback thread: %s\n", e.what());
    }
    catch(...) {
    }
    return ALC_FALSE;
}

static void ALCsolarisBackend_stop(ALCsolarisBackend *self)
{
    if(self->mKillNow.exchange(AL_TRUE) || !self->mThread.joinable())
        return;

    self->mThread.join();

    if(ioctl(self->fd, AUDIO_DRAIN) < 0)
        ERR("Error draining device: %s\n", strerror(errno));
}


BackendFactory &SolarisBackendFactory::getFactory()
{
    static SolarisBackendFactory factory{};
    return factory;
}

bool SolarisBackendFactory::init()
{
    ConfigValueStr(nullptr, "solaris", "device", &solaris_driver);
    return true;
}

bool SolarisBackendFactory::querySupport(ALCbackend_Type type)
{ return (type == ALCbackend_Playback); }

void SolarisBackendFactory::probe(DevProbe type, std::string *outnames)
{
    switch(type)
    {
        case ALL_DEVICE_PROBE:
        {
#ifdef HAVE_STAT
            struct stat buf;
            if(stat(solaris_driver, &buf) == 0)
#endif
                outnames->append(solaris_device, sizeof(solaris_device));
        }
        break;

        case CAPTURE_DEVICE_PROBE:
            break;
    }
}

ALCbackend *SolarisBackendFactory::createBackend(ALCdevice *device, ALCbackend_Type type)
{
    if(type == ALCbackend_Playback)
    {
        ALCsolarisBackend *backend;
        NEW_OBJ(backend, ALCsolarisBackend)(device);
        if(!backend) return nullptr;
        return STATIC_CAST(ALCbackend, backend);
    }

    return nullptr;
}
