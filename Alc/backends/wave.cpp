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

#include "backends/wave.h"

#include <stdlib.h>
#include <stdio.h>
#include <memory.h>
#include <errno.h>

#include <chrono>
#include <thread>
#include <vector>
#include <functional>

#include "alMain.h"
#include "alu.h"
#include "alconfig.h"
#include "compat.h"


namespace {

using std::chrono::seconds;
using std::chrono::milliseconds;
using std::chrono::nanoseconds;

constexpr ALCchar waveDevice[] = "Wave File Writer";

constexpr ALubyte SUBTYPE_PCM[]{
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa,
    0x00, 0x38, 0x9b, 0x71
};
constexpr ALubyte SUBTYPE_FLOAT[]{
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa,
    0x00, 0x38, 0x9b, 0x71
};

constexpr ALubyte SUBTYPE_BFORMAT_PCM[]{
    0x01, 0x00, 0x00, 0x00, 0x21, 0x07, 0xd3, 0x11, 0x86, 0x44, 0xc8, 0xc1,
    0xca, 0x00, 0x00, 0x00
};

constexpr ALubyte SUBTYPE_BFORMAT_FLOAT[]{
    0x03, 0x00, 0x00, 0x00, 0x21, 0x07, 0xd3, 0x11, 0x86, 0x44, 0xc8, 0xc1,
    0xca, 0x00, 0x00, 0x00
};

void fwrite16le(ALushort val, FILE *f)
{
    ALubyte data[2]{ static_cast<ALubyte>(val&0xff), static_cast<ALubyte>((val>>8)&0xff) };
    fwrite(data, 1, 2, f);
}

void fwrite32le(ALuint val, FILE *f)
{
    ALubyte data[4]{ static_cast<ALubyte>(val&0xff), static_cast<ALubyte>((val>>8)&0xff),
        static_cast<ALubyte>((val>>16)&0xff), static_cast<ALubyte>((val>>24)&0xff) };
    fwrite(data, 1, 4, f);
}


struct WaveBackend final : public BackendBase {
    WaveBackend(ALCdevice *device) noexcept : BackendBase{device} { }
    ~WaveBackend() override;

    int mixerProc();

    ALCenum open(const ALCchar *name) override;
    ALCboolean reset() override;
    ALCboolean start() override;
    void stop() override;

    FILE *mFile{nullptr};
    long mDataStart{-1};

    al::vector<ALbyte> mBuffer;

    std::atomic<ALenum> mKillNow{AL_TRUE};
    std::thread mThread;

    static constexpr inline const char *CurrentPrefix() noexcept { return "WaveBackend::"; }
    DEF_NEWDEL(WaveBackend)
};

WaveBackend::~WaveBackend()
{
    if(mFile)
        fclose(mFile);
    mFile = nullptr;
}

int WaveBackend::mixerProc()
{
    const milliseconds restTime{mDevice->UpdateSize*1000/mDevice->Frequency / 2};

    althrd_setname(MIXER_THREAD_NAME);

    const ALsizei frameSize{mDevice->frameSizeFromFmt()};

    ALint64 done{0};
    auto start = std::chrono::steady_clock::now();
    while(!mKillNow.load(std::memory_order_acquire) &&
          mDevice->Connected.load(std::memory_order_acquire))
    {
        auto now = std::chrono::steady_clock::now();

        /* This converts from nanoseconds to nanosamples, then to samples. */
        ALint64 avail{std::chrono::duration_cast<seconds>((now-start) *
            mDevice->Frequency).count()};
        if(avail-done < mDevice->UpdateSize)
        {
            std::this_thread::sleep_for(restTime);
            continue;
        }
        while(avail-done >= mDevice->UpdateSize)
        {
            lock();
            aluMixData(mDevice, mBuffer.data(), mDevice->UpdateSize);
            unlock();
            done += mDevice->UpdateSize;

            if(!IS_LITTLE_ENDIAN)
            {
                const ALsizei bytesize{mDevice->bytesFromFmt()};
                ALsizei i;

                if(bytesize == 2)
                {
                    ALushort *samples = reinterpret_cast<ALushort*>(mBuffer.data());
                    const auto len = static_cast<ALsizei>(mBuffer.size() / 2);
                    for(i = 0;i < len;i++)
                    {
                        ALushort samp = samples[i];
                        samples[i] = (samp>>8) | (samp<<8);
                    }
                }
                else if(bytesize == 4)
                {
                    ALuint *samples = reinterpret_cast<ALuint*>(mBuffer.data());
                    const auto len = static_cast<ALsizei>(mBuffer.size() / 4);
                    for(i = 0;i < len;i++)
                    {
                        ALuint samp = samples[i];
                        samples[i] = (samp>>24) | ((samp>>8)&0x0000ff00) |
                                     ((samp<<8)&0x00ff0000) | (samp<<24);
                    }
                }
            }

            size_t fs{fwrite(mBuffer.data(), frameSize, mDevice->UpdateSize, mFile)};
            (void)fs;
            if(ferror(mFile))
            {
                ERR("Error writing to file\n");
                aluHandleDisconnect(mDevice, "Failed to write playback samples");
                break;
            }
        }

        /* For every completed second, increment the start time and reduce the
         * samples done. This prevents the difference between the start time
         * and current time from growing too large, while maintaining the
         * correct number of samples to render.
         */
        if(done >= mDevice->Frequency)
        {
            seconds s{done/mDevice->Frequency};
            start += s;
            done -= mDevice->Frequency*s.count();
        }
    }

    return 0;
}

ALCenum WaveBackend::open(const ALCchar *name)
{
    const char *fname{GetConfigValue(nullptr, "wave", "file", "")};
    if(!fname[0]) return ALC_INVALID_VALUE;

    if(!name)
        name = waveDevice;
    else if(strcmp(name, waveDevice) != 0)
        return ALC_INVALID_VALUE;

#ifdef _WIN32
    {
        std::wstring wname = utf8_to_wstr(fname);
        mFile = _wfopen(wname.c_str(), L"wb");
    }
#else
    mFile = fopen(fname, "wb");
#endif
    if(!mFile)
    {
        ERR("Could not open file '%s': %s\n", fname, strerror(errno));
        return ALC_INVALID_VALUE;
    }

    mDevice->DeviceName = name;

    return ALC_NO_ERROR;
}

ALCboolean WaveBackend::reset()
{
    ALuint channels=0, bits=0, chanmask=0;
    int isbformat = 0;
    size_t val;

    fseek(mFile, 0, SEEK_SET);
    clearerr(mFile);

    if(GetConfigValueBool(nullptr, "wave", "bformat", 0))
    {
        mDevice->FmtChans = DevFmtAmbi3D;
        mDevice->mAmbiOrder = 1;
    }

    switch(mDevice->FmtType)
    {
        case DevFmtByte:
            mDevice->FmtType = DevFmtUByte;
            break;
        case DevFmtUShort:
            mDevice->FmtType = DevFmtShort;
            break;
        case DevFmtUInt:
            mDevice->FmtType = DevFmtInt;
            break;
        case DevFmtUByte:
        case DevFmtShort:
        case DevFmtInt:
        case DevFmtFloat:
            break;
    }
    switch(mDevice->FmtChans)
    {
        case DevFmtMono:   chanmask = 0x04; break;
        case DevFmtStereo: chanmask = 0x01 | 0x02; break;
        case DevFmtQuad:   chanmask = 0x01 | 0x02 | 0x10 | 0x20; break;
        case DevFmtX51: chanmask = 0x01 | 0x02 | 0x04 | 0x08 | 0x200 | 0x400; break;
        case DevFmtX51Rear: chanmask = 0x01 | 0x02 | 0x04 | 0x08 | 0x010 | 0x020; break;
        case DevFmtX61: chanmask = 0x01 | 0x02 | 0x04 | 0x08 | 0x100 | 0x200 | 0x400; break;
        case DevFmtX71: chanmask = 0x01 | 0x02 | 0x04 | 0x08 | 0x010 | 0x020 | 0x200 | 0x400; break;
        case DevFmtAmbi3D:
            /* .amb output requires FuMa */
            mDevice->mAmbiOrder = mini(mDevice->mAmbiOrder, 3);
            mDevice->mAmbiLayout = AmbiLayout::FuMa;
            mDevice->mAmbiScale = AmbiNorm::FuMa;
            isbformat = 1;
            chanmask = 0;
            break;
    }
    bits = mDevice->bytesFromFmt() * 8;
    channels = mDevice->channelsFromFmt();

    fputs("RIFF", mFile);
    fwrite32le(0xFFFFFFFF, mFile); // 'RIFF' header len; filled in at close

    fputs("WAVE", mFile);

    fputs("fmt ", mFile);
    fwrite32le(40, mFile); // 'fmt ' header len; 40 bytes for EXTENSIBLE

    // 16-bit val, format type id (extensible: 0xFFFE)
    fwrite16le(0xFFFE, mFile);
    // 16-bit val, channel count
    fwrite16le(channels, mFile);
    // 32-bit val, frequency
    fwrite32le(mDevice->Frequency, mFile);
    // 32-bit val, bytes per second
    fwrite32le(mDevice->Frequency * channels * bits / 8, mFile);
    // 16-bit val, frame size
    fwrite16le(channels * bits / 8, mFile);
    // 16-bit val, bits per sample
    fwrite16le(bits, mFile);
    // 16-bit val, extra byte count
    fwrite16le(22, mFile);
    // 16-bit val, valid bits per sample
    fwrite16le(bits, mFile);
    // 32-bit val, channel mask
    fwrite32le(chanmask, mFile);
    // 16 byte GUID, sub-type format
    val = fwrite((mDevice->FmtType == DevFmtFloat) ?
                 (isbformat ? SUBTYPE_BFORMAT_FLOAT : SUBTYPE_FLOAT) :
                 (isbformat ? SUBTYPE_BFORMAT_PCM : SUBTYPE_PCM), 1, 16, mFile);
    (void)val;

    fputs("data", mFile);
    fwrite32le(0xFFFFFFFF, mFile); // 'data' header len; filled in at close

    if(ferror(mFile))
    {
        ERR("Error writing header: %s\n", strerror(errno));
        return ALC_FALSE;
    }
    mDataStart = ftell(mFile);

    SetDefaultWFXChannelOrder(mDevice);

    const ALuint bufsize{mDevice->frameSizeFromFmt() * mDevice->UpdateSize};
    mBuffer.resize(bufsize);

    return ALC_TRUE;
}

ALCboolean WaveBackend::start()
{
    try {
        mKillNow.store(AL_FALSE, std::memory_order_release);
        mThread = std::thread{std::mem_fn(&WaveBackend::mixerProc), this};
        return ALC_TRUE;
    }
    catch(std::exception& e) {
        ERR("Failed to start mixing thread: %s\n", e.what());
    }
    catch(...) {
    }
    return ALC_FALSE;
}

void WaveBackend::stop()
{
    if(mKillNow.exchange(AL_TRUE, std::memory_order_acq_rel) || !mThread.joinable())
        return;
    mThread.join();

    long size{ftell(mFile)};
    if(size > 0)
    {
        long dataLen{size - mDataStart};
        if(fseek(mFile, mDataStart-4, SEEK_SET) == 0)
            fwrite32le(dataLen, mFile); // 'data' header len
        if(fseek(mFile, 4, SEEK_SET) == 0)
            fwrite32le(size-8, mFile); // 'WAVE' header len
    }
}

} // namespace


bool WaveBackendFactory::init()
{ return true; }

bool WaveBackendFactory::querySupport(BackendType type)
{ return type == BackendType::Playback; }

void WaveBackendFactory::probe(DevProbe type, std::string *outnames)
{
    switch(type)
    {
        case ALL_DEVICE_PROBE:
            /* Includes null char. */
            outnames->append(waveDevice, sizeof(waveDevice));
            break;
        case CAPTURE_DEVICE_PROBE:
            break;
    }
}

BackendPtr WaveBackendFactory::createBackend(ALCdevice *device, BackendType type)
{
    if(type == BackendType::Playback)
        return BackendPtr{new WaveBackend{device}};
    return nullptr;
}

BackendFactory &WaveBackendFactory::getFactory()
{
    static WaveBackendFactory factory{};
    return factory;
}
