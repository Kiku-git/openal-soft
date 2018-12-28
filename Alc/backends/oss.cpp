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

#include "backends/oss.h"

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <memory.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <math.h>

#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <algorithm>

#include "alMain.h"
#include "alu.h"
#include "alconfig.h"
#include "ringbuffer.h"
#include "compat.h"

#include <sys/soundcard.h>

/*
 * The OSS documentation talks about SOUND_MIXER_READ, but the header
 * only contains MIXER_READ. Play safe. Same for WRITE.
 */
#ifndef SOUND_MIXER_READ
#define SOUND_MIXER_READ MIXER_READ
#endif
#ifndef SOUND_MIXER_WRITE
#define SOUND_MIXER_WRITE MIXER_WRITE
#endif

#if defined(SOUND_VERSION) && (SOUND_VERSION < 0x040000)
#define ALC_OSS_COMPAT
#endif
#ifndef SNDCTL_AUDIOINFO
#define ALC_OSS_COMPAT
#endif

/*
 * FreeBSD strongly discourages the use of specific devices,
 * such as those returned in oss_audioinfo.devnode
 */
#ifdef __FreeBSD__
#define ALC_OSS_DEVNODE_TRUC
#endif

namespace {

constexpr char DefaultName[] = "OSS Default";
const char *DefaultPlayback{"/dev/dsp"};
const char *DefaultCapture{"/dev/dsp"};

struct DevMap {
    std::string name;
    std::string device_name;

    template<typename StrT0, typename StrT1>
    DevMap(StrT0&& name_, StrT1&& devname_)
      : name{std::forward<StrT0>(name_)}, device_name{std::forward<StrT1>(devname_)}
    { }
};

bool checkName(const al::vector<DevMap> &list, const std::string &name)
{
    return std::find_if(list.cbegin(), list.cend(),
        [&name](const DevMap &entry) -> bool
        { return entry.name == name; }
    ) != list.cend();
}

al::vector<DevMap> PlaybackDevices;
al::vector<DevMap> CaptureDevices;


#ifdef ALC_OSS_COMPAT

#define DSP_CAP_OUTPUT 0x00020000
#define DSP_CAP_INPUT 0x00010000
void ALCossListPopulate(al::vector<DevMap> *devlist, int type)
{
    devlist->emplace_back(DefaultName, (type==DSP_CAP_INPUT) ? DefaultCapture : DefaultPlayback);
}

#else

void ALCossListAppend(al::vector<DevMap> *list, const char *handle, size_t hlen, const char *path, size_t plen)
{
#ifdef ALC_OSS_DEVNODE_TRUC
    for(size_t i{0};i < plen;i++)
    {
        if(path[i] == '.')
        {
            if(strncmp(path + i, handle + hlen + i - plen, plen - i) == 0)
                hlen = hlen + i - plen;
            plen = i;
        }
    }
#endif
    if(handle[0] == '\0')
    {
        handle = path;
        hlen = plen;
    }

    std::string basename{handle, hlen};
    basename.erase(std::find(basename.begin(), basename.end(), '\0'), basename.end());
    std::string devname{path, plen};
    devname.erase(std::find(devname.begin(), devname.end(), '\0'), devname.end());

    auto iter = std::find_if(list->cbegin(), list->cend(),
        [&devname](const DevMap &entry) -> bool
        { return entry.device_name == devname; }
    );
    if(iter != list->cend())
        return;

    int count{1};
    std::string newname{basename};
    while(checkName(PlaybackDevices, newname))
    {
        newname = basename;
        newname += " #";
        newname += std::to_string(++count);
    }

    list->emplace_back(std::move(newname), std::move(devname));
    const DevMap &entry = list->back();

    TRACE("Got device \"%s\", \"%s\"\n", entry.name.c_str(), entry.device_name.c_str());
}

void ALCossListPopulate(al::vector<DevMap> *devlist, int type_flag)
{
    int fd{open("/dev/mixer", O_RDONLY)};
    if(fd < 0)
    {
        TRACE("Could not open /dev/mixer: %s\n", strerror(errno));
        goto done;
    }

    oss_sysinfo si;
    if(ioctl(fd, SNDCTL_SYSINFO, &si) == -1)
    {
        TRACE("SNDCTL_SYSINFO failed: %s\n", strerror(errno));
        goto done;
    }

    for(int i{0};i < si.numaudios;i++)
    {
        oss_audioinfo ai;
        ai.dev = i;
        if(ioctl(fd, SNDCTL_AUDIOINFO, &ai) == -1)
        {
            ERR("SNDCTL_AUDIOINFO (%d) failed: %s\n", i, strerror(errno));
            continue;
        }
        if(!(ai.caps&type_flag) || ai.devnode[0] == '\0')
            continue;

        const char *handle;
        size_t len;
        if(ai.handle[0] != '\0')
        {
            len = strnlen(ai.handle, sizeof(ai.handle));
            handle = ai.handle;
        }
        else
        {
            len = strnlen(ai.name, sizeof(ai.name));
            handle = ai.name;
        }

        ALCossListAppend(devlist, handle, len, ai.devnode,
                         strnlen(ai.devnode, sizeof(ai.devnode)));
    }

done:
    if(fd >= 0)
        close(fd);
    fd = -1;

    const char *defdev{(type_flag==DSP_CAP_INPUT) ? DefaultCapture : DefaultPlayback};
    auto iter = std::find_if(devlist->cbegin(), devlist->cend(),
        [defdev](const DevMap &entry) -> bool
        { return entry.device_name == defdev; }
    );
    if(iter == devlist->cend())
        devlist->insert(devlist->begin(), DevMap{DefaultName, defdev});
    else
    {
        DevMap entry{std::move(*iter)};
        devlist->erase(iter);
        devlist->insert(devlist->begin(), std::move(entry));
    }
    devlist->shrink_to_fit();
}

#endif

int log2i(ALCuint x)
{
    int y = 0;
    while (x > 1)
    {
        x >>= 1;
        y++;
    }
    return y;
}


struct ALCplaybackOSS final : public ALCbackend {
    ALCplaybackOSS(ALCdevice *device) noexcept : ALCbackend{device} { }
    ~ALCplaybackOSS() override;

    int mixerProc();

    int mFd{-1};

    al::vector<ALubyte> mMixData;

    std::atomic<ALenum> mKillNow{AL_TRUE};
    std::thread mThread;
};

void ALCplaybackOSS_Construct(ALCplaybackOSS *self, ALCdevice *device);
void ALCplaybackOSS_Destruct(ALCplaybackOSS *self);
ALCenum ALCplaybackOSS_open(ALCplaybackOSS *self, const ALCchar *name);
ALCboolean ALCplaybackOSS_reset(ALCplaybackOSS *self);
ALCboolean ALCplaybackOSS_start(ALCplaybackOSS *self);
void ALCplaybackOSS_stop(ALCplaybackOSS *self);
DECLARE_FORWARD2(ALCplaybackOSS, ALCbackend, ALCenum, captureSamples, ALCvoid*, ALCuint)
DECLARE_FORWARD(ALCplaybackOSS, ALCbackend, ALCuint, availableSamples)
DECLARE_FORWARD(ALCplaybackOSS, ALCbackend, ClockLatency, getClockLatency)
DECLARE_FORWARD(ALCplaybackOSS, ALCbackend, void, lock)
DECLARE_FORWARD(ALCplaybackOSS, ALCbackend, void, unlock)
DECLARE_DEFAULT_ALLOCATORS(ALCplaybackOSS)
DEFINE_ALCBACKEND_VTABLE(ALCplaybackOSS);


void ALCplaybackOSS_Construct(ALCplaybackOSS *self, ALCdevice *device)
{
    new (self) ALCplaybackOSS{device};
    SET_VTABLE2(ALCplaybackOSS, ALCbackend, self);
}

void ALCplaybackOSS_Destruct(ALCplaybackOSS *self)
{ self->~ALCplaybackOSS(); }

ALCplaybackOSS::~ALCplaybackOSS()
{
    if(mFd != -1)
        close(mFd);
    mFd = -1;
}


int ALCplaybackOSS::mixerProc()
{
    SetRTPriority();
    althrd_setname(MIXER_THREAD_NAME);

    const int frame_size{mDevice->frameSizeFromFmt()};

    ALCplaybackOSS_lock(this);
    while(!mKillNow.load(std::memory_order_acquire) &&
          mDevice->Connected.load(std::memory_order_acquire))
    {
        pollfd pollitem{};
        pollitem.fd = mFd;
        pollitem.events = POLLOUT;

        ALCplaybackOSS_unlock(this);
        int pret{poll(&pollitem, 1, 1000)};
        ALCplaybackOSS_lock(this);
        if(pret < 0)
        {
            if(errno == EINTR || errno == EAGAIN)
                continue;
            ERR("poll failed: %s\n", strerror(errno));
            aluHandleDisconnect(mDevice, "Failed waiting for playback buffer: %s", strerror(errno));
            break;
        }
        else if(pret == 0)
        {
            WARN("poll timeout\n");
            continue;
        }

        ALubyte *write_ptr{mMixData.data()};
        size_t to_write{mMixData.size()};
        aluMixData(mDevice, write_ptr, to_write/frame_size);
        while(to_write > 0 && !mKillNow.load(std::memory_order_acquire))
        {
            ssize_t wrote{write(mFd, write_ptr, to_write)};
            if(wrote < 0)
            {
                if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                    continue;
                ERR("write failed: %s\n", strerror(errno));
                aluHandleDisconnect(mDevice, "Failed writing playback samples: %s",
                                    strerror(errno));
                break;
            }

            to_write -= wrote;
            write_ptr += wrote;
        }
    }
    ALCplaybackOSS_unlock(this);

    return 0;
}


ALCenum ALCplaybackOSS_open(ALCplaybackOSS *self, const ALCchar *name)
{
    ALCdevice *device{self->mDevice};

    const char *devname{DefaultPlayback};
    if(!name)
        name = DefaultName;
    else
    {
        if(PlaybackDevices.empty())
            ALCossListPopulate(&PlaybackDevices, DSP_CAP_OUTPUT);

        auto iter = std::find_if(PlaybackDevices.cbegin(), PlaybackDevices.cend(),
            [&name](const DevMap &entry) -> bool
            { return entry.name == name; }
        );
        if(iter == PlaybackDevices.cend())
            return ALC_INVALID_VALUE;
        devname = iter->device_name.c_str();
    }

    self->mFd = open(devname, O_WRONLY);
    if(self->mFd == -1)
    {
        ERR("Could not open %s: %s\n", devname, strerror(errno));
        return ALC_INVALID_VALUE;
    }

    device->DeviceName = name;
    return ALC_NO_ERROR;
}

ALCboolean ALCplaybackOSS_reset(ALCplaybackOSS *self)
{
    ALCdevice *device{self->mDevice};
    int numFragmentsLogSize;
    int log2FragmentSize;
    unsigned int periods;
    audio_buf_info info;
    ALuint frameSize;
    int numChannels;
    int ossFormat;
    int ossSpeed;
    const char *err;

    switch(device->FmtType)
    {
        case DevFmtByte:
            ossFormat = AFMT_S8;
            break;
        case DevFmtUByte:
            ossFormat = AFMT_U8;
            break;
        case DevFmtUShort:
        case DevFmtInt:
        case DevFmtUInt:
        case DevFmtFloat:
            device->FmtType = DevFmtShort;
            /* fall-through */
        case DevFmtShort:
            ossFormat = AFMT_S16_NE;
            break;
    }

    periods = device->NumUpdates;
    numChannels = device->channelsFromFmt();
    ossSpeed = device->Frequency;
    frameSize = numChannels * device->bytesFromFmt();
    /* According to the OSS spec, 16 bytes (log2(16)) is the minimum. */
    log2FragmentSize = maxi(log2i(device->UpdateSize*frameSize), 4);
    numFragmentsLogSize = (periods << 16) | log2FragmentSize;

#define CHECKERR(func) if((func) < 0) {                                       \
    err = #func;                                                              \
    goto err;                                                                 \
}
    /* Don't fail if SETFRAGMENT fails. We can handle just about anything
     * that's reported back via GETOSPACE */
    ioctl(self->mFd, SNDCTL_DSP_SETFRAGMENT, &numFragmentsLogSize);
    CHECKERR(ioctl(self->mFd, SNDCTL_DSP_SETFMT, &ossFormat));
    CHECKERR(ioctl(self->mFd, SNDCTL_DSP_CHANNELS, &numChannels));
    CHECKERR(ioctl(self->mFd, SNDCTL_DSP_SPEED, &ossSpeed));
    CHECKERR(ioctl(self->mFd, SNDCTL_DSP_GETOSPACE, &info));
    if(0)
    {
    err:
        ERR("%s failed: %s\n", err, strerror(errno));
        return ALC_FALSE;
    }
#undef CHECKERR

    if(device->channelsFromFmt() != numChannels)
    {
        ERR("Failed to set %s, got %d channels instead\n", DevFmtChannelsString(device->FmtChans), numChannels);
        return ALC_FALSE;
    }

    if(!((ossFormat == AFMT_S8 && device->FmtType == DevFmtByte) ||
         (ossFormat == AFMT_U8 && device->FmtType == DevFmtUByte) ||
         (ossFormat == AFMT_S16_NE && device->FmtType == DevFmtShort)))
    {
        ERR("Failed to set %s samples, got OSS format %#x\n", DevFmtTypeString(device->FmtType), ossFormat);
        return ALC_FALSE;
    }

    device->Frequency = ossSpeed;
    device->UpdateSize = info.fragsize / frameSize;
    device->NumUpdates = info.fragments;

    SetDefaultChannelOrder(device);

    return ALC_TRUE;
}

ALCboolean ALCplaybackOSS_start(ALCplaybackOSS *self)
{
    ALCdevice *device{self->mDevice};

    try {
        self->mMixData.resize(device->UpdateSize * device->frameSizeFromFmt());

        self->mKillNow.store(AL_FALSE);
        self->mThread = std::thread{std::mem_fn(&ALCplaybackOSS::mixerProc), self};
        return ALC_TRUE;
    }
    catch(std::exception& e) {
        ERR("Could not create playback thread: %s\n", e.what());
    }
    catch(...) {
    }
    return ALC_FALSE;
}

void ALCplaybackOSS_stop(ALCplaybackOSS *self)
{
    if(self->mKillNow.exchange(AL_TRUE) || !self->mThread.joinable())
        return;
    self->mThread.join();

    if(ioctl(self->mFd, SNDCTL_DSP_RESET) != 0)
        ERR("Error resetting device: %s\n", strerror(errno));

    self->mMixData.clear();
}


struct ALCcaptureOSS final : public ALCbackend {
    ALCcaptureOSS(ALCdevice *device) noexcept : ALCbackend{device} { }
    ~ALCcaptureOSS() override;

    int recordProc();

    int mFd{-1};

    RingBufferPtr mRing{nullptr};

    std::atomic<ALenum> mKillNow{AL_TRUE};
    std::thread mThread;
};

void ALCcaptureOSS_Construct(ALCcaptureOSS *self, ALCdevice *device);
void ALCcaptureOSS_Destruct(ALCcaptureOSS *self);
ALCenum ALCcaptureOSS_open(ALCcaptureOSS *self, const ALCchar *name);
DECLARE_FORWARD(ALCcaptureOSS, ALCbackend, ALCboolean, reset)
ALCboolean ALCcaptureOSS_start(ALCcaptureOSS *self);
void ALCcaptureOSS_stop(ALCcaptureOSS *self);
ALCenum ALCcaptureOSS_captureSamples(ALCcaptureOSS *self, ALCvoid *buffer, ALCuint samples);
ALCuint ALCcaptureOSS_availableSamples(ALCcaptureOSS *self);
DECLARE_FORWARD(ALCcaptureOSS, ALCbackend, ClockLatency, getClockLatency)
DECLARE_FORWARD(ALCcaptureOSS, ALCbackend, void, lock)
DECLARE_FORWARD(ALCcaptureOSS, ALCbackend, void, unlock)
DECLARE_DEFAULT_ALLOCATORS(ALCcaptureOSS)
DEFINE_ALCBACKEND_VTABLE(ALCcaptureOSS);


void ALCcaptureOSS_Construct(ALCcaptureOSS *self, ALCdevice *device)
{
    new (self) ALCcaptureOSS{device};
    SET_VTABLE2(ALCcaptureOSS, ALCbackend, self);
}

void ALCcaptureOSS_Destruct(ALCcaptureOSS *self)
{ self->~ALCcaptureOSS(); }

ALCcaptureOSS::~ALCcaptureOSS()
{
    if(mFd != -1)
        close(mFd);
    mFd = -1;
}


int ALCcaptureOSS::recordProc()
{
    SetRTPriority();
    althrd_setname(RECORD_THREAD_NAME);

    const int frame_size{mDevice->frameSizeFromFmt()};
    while(!mKillNow.load(std::memory_order_acquire))
    {
        pollfd pollitem{};
        pollitem.fd = mFd;
        pollitem.events = POLLIN;

        int sret{poll(&pollitem, 1, 1000)};
        if(sret < 0)
        {
            if(errno == EINTR || errno == EAGAIN)
                continue;
            ERR("poll failed: %s\n", strerror(errno));
            aluHandleDisconnect(mDevice, "Failed to check capture samples: %s", strerror(errno));
            break;
        }
        else if(sret == 0)
        {
            WARN("poll timeout\n");
            continue;
        }

        auto vec = mRing->getWriteVector();
        if(vec.first.len > 0)
        {
            ssize_t amt{read(mFd, vec.first.buf, vec.first.len*frame_size)};
            if(amt < 0)
            {
                ERR("read failed: %s\n", strerror(errno));
                ALCcaptureOSS_lock(this);
                aluHandleDisconnect(mDevice, "Failed reading capture samples: %s",
                    strerror(errno));
                ALCcaptureOSS_unlock(this);
                break;
            }
            mRing->writeAdvance(amt/frame_size);
        }
    }

    return 0;
}


ALCenum ALCcaptureOSS_open(ALCcaptureOSS *self, const ALCchar *name)
{
    ALCdevice *device{self->mDevice};

    const char *devname{DefaultCapture};
    if(!name)
        name = DefaultName;
    else
    {
        if(CaptureDevices.empty())
            ALCossListPopulate(&CaptureDevices, DSP_CAP_INPUT);

        auto iter = std::find_if(CaptureDevices.cbegin(), CaptureDevices.cend(),
            [&name](const DevMap &entry) -> bool
            { return entry.name == name; }
        );
        if(iter == CaptureDevices.cend())
            return ALC_INVALID_VALUE;
        devname = iter->device_name.c_str();
    }

    self->mFd = open(devname, O_RDONLY);
    if(self->mFd == -1)
    {
        ERR("Could not open %s: %s\n", devname, strerror(errno));
        return ALC_INVALID_VALUE;
    }

    int ossFormat{};
    switch(device->FmtType)
    {
        case DevFmtByte:
            ossFormat = AFMT_S8;
            break;
        case DevFmtUByte:
            ossFormat = AFMT_U8;
            break;
        case DevFmtShort:
            ossFormat = AFMT_S16_NE;
            break;
        case DevFmtUShort:
        case DevFmtInt:
        case DevFmtUInt:
        case DevFmtFloat:
            ERR("%s capture samples not supported\n", DevFmtTypeString(device->FmtType));
            return ALC_INVALID_VALUE;
    }

    int periods{4};
    int numChannels{device->channelsFromFmt()};
    int frameSize{numChannels * device->bytesFromFmt()};
    int ossSpeed{static_cast<int>(device->Frequency)};
    int log2FragmentSize{log2i(device->UpdateSize * device->NumUpdates *
                               frameSize / periods)};

    /* according to the OSS spec, 16 bytes are the minimum */
    log2FragmentSize = std::max(log2FragmentSize, 4);
    int numFragmentsLogSize{(periods << 16) | log2FragmentSize};

    audio_buf_info info;
    const char *err;
#define CHECKERR(func) if((func) < 0) {                                       \
    err = #func;                                                              \
    goto err;                                                                 \
}
    CHECKERR(ioctl(self->mFd, SNDCTL_DSP_SETFRAGMENT, &numFragmentsLogSize));
    CHECKERR(ioctl(self->mFd, SNDCTL_DSP_SETFMT, &ossFormat));
    CHECKERR(ioctl(self->mFd, SNDCTL_DSP_CHANNELS, &numChannels));
    CHECKERR(ioctl(self->mFd, SNDCTL_DSP_SPEED, &ossSpeed));
    CHECKERR(ioctl(self->mFd, SNDCTL_DSP_GETISPACE, &info));
    if(0)
    {
    err:
        ERR("%s failed: %s\n", err, strerror(errno));
        close(self->mFd);
        self->mFd = -1;
        return ALC_INVALID_VALUE;
    }
#undef CHECKERR

    if(device->channelsFromFmt() != numChannels)
    {
        ERR("Failed to set %s, got %d channels instead\n", DevFmtChannelsString(device->FmtChans), numChannels);
        close(self->mFd);
        self->mFd = -1;
        return ALC_INVALID_VALUE;
    }

    if(!((ossFormat == AFMT_S8 && device->FmtType == DevFmtByte) ||
         (ossFormat == AFMT_U8 && device->FmtType == DevFmtUByte) ||
         (ossFormat == AFMT_S16_NE && device->FmtType == DevFmtShort)))
    {
        ERR("Failed to set %s samples, got OSS format %#x\n", DevFmtTypeString(device->FmtType), ossFormat);
        close(self->mFd);
        self->mFd = -1;
        return ALC_INVALID_VALUE;
    }

    self->mRing = CreateRingBuffer(device->UpdateSize*device->NumUpdates, frameSize, false);
    if(!self->mRing)
    {
        ERR("Ring buffer create failed\n");
        close(self->mFd);
        self->mFd = -1;
        return ALC_OUT_OF_MEMORY;
    }

    device->DeviceName = name;
    return ALC_NO_ERROR;
}

ALCboolean ALCcaptureOSS_start(ALCcaptureOSS *self)
{
    try {
        self->mKillNow.store(AL_FALSE);
        self->mThread = std::thread{std::mem_fn(&ALCcaptureOSS::recordProc), self};
        return ALC_TRUE;
    }
    catch(std::exception& e) {
        ERR("Could not create record thread: %s\n", e.what());
    }
    catch(...) {
    }
    return ALC_FALSE;
}

void ALCcaptureOSS_stop(ALCcaptureOSS *self)
{
    if(self->mKillNow.exchange(AL_TRUE) || !self->mThread.joinable())
        return;

    self->mThread.join();

    if(ioctl(self->mFd, SNDCTL_DSP_RESET) != 0)
        ERR("Error resetting device: %s\n", strerror(errno));
}

ALCenum ALCcaptureOSS_captureSamples(ALCcaptureOSS *self, ALCvoid *buffer, ALCuint samples)
{
    RingBuffer *ring{self->mRing.get()};
    ring->read(buffer, samples);
    return ALC_NO_ERROR;
}

ALCuint ALCcaptureOSS_availableSamples(ALCcaptureOSS *self)
{
    RingBuffer *ring{self->mRing.get()};
    return ring->readSpace();
}

} // namespace


BackendFactory &OSSBackendFactory::getFactory()
{
    static OSSBackendFactory factory{};
    return factory;
}

bool OSSBackendFactory::init()
{
    ConfigValueStr(nullptr, "oss", "device", &DefaultPlayback);
    ConfigValueStr(nullptr, "oss", "capture", &DefaultCapture);

    return true;
}

void OSSBackendFactory::deinit()
{
    PlaybackDevices.clear();
    CaptureDevices.clear();
}

bool OSSBackendFactory::querySupport(ALCbackend_Type type)
{ return (type == ALCbackend_Playback || type == ALCbackend_Capture); }

void OSSBackendFactory::probe(DevProbe type, std::string *outnames)
{
    auto add_device = [outnames](const DevMap &entry) -> void
    {
#ifdef HAVE_STAT
        struct stat buf;
        if(stat(entry.device_name.c_str(), &buf) == 0)
#endif
        {
            /* Includes null char. */
            outnames->append(entry.name.c_str(), entry.name.length()+1);
        }
    };

    switch(type)
    {
        case ALL_DEVICE_PROBE:
            PlaybackDevices.clear();
            ALCossListPopulate(&PlaybackDevices, DSP_CAP_OUTPUT);
            std::for_each(PlaybackDevices.cbegin(), PlaybackDevices.cend(), add_device);
            break;

        case CAPTURE_DEVICE_PROBE:
            CaptureDevices.clear();
            ALCossListPopulate(&CaptureDevices, DSP_CAP_INPUT);
            std::for_each(CaptureDevices.cbegin(), CaptureDevices.cend(), add_device);
            break;
    }
}

ALCbackend *OSSBackendFactory::createBackend(ALCdevice *device, ALCbackend_Type type)
{
    if(type == ALCbackend_Playback)
    {
        ALCplaybackOSS *backend;
        NEW_OBJ(backend, ALCplaybackOSS)(device);
        return backend;
    }
    if(type == ALCbackend_Capture)
    {
        ALCcaptureOSS *backend;
        NEW_OBJ(backend, ALCcaptureOSS)(device);
        return backend;
    }

    return nullptr;
}
