/**
 * OpenAL cross platform audio library
 * Copyright (C) 1999-2010 by authors.
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

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cassert>

#include <cmath>
#include <numeric>
#include <algorithm>
#include <functional>

#include "alMain.h"
#include "alAuxEffectSlot.h"
#include "alu.h"
#include "alconfig.h"
#include "ambdec.h"
#include "bformatdec.h"
#include "filters/splitter.h"
#include "uhjfilter.h"
#include "bs2b.h"


constexpr std::array<float,MAX_AMBI_COEFFS> AmbiScale::FromN3D;
constexpr std::array<float,MAX_AMBI_COEFFS> AmbiScale::FromSN3D;
constexpr std::array<float,MAX_AMBI_COEFFS> AmbiScale::FromFuMa;
constexpr std::array<int,MAX_AMBI_COEFFS> AmbiIndex::FromFuMa;
constexpr std::array<int,MAX_AMBI_COEFFS> AmbiIndex::FromACN;
constexpr std::array<int,MAX_AMBI2D_COEFFS> AmbiIndex::From2D;
constexpr std::array<int,MAX_AMBI_COEFFS> AmbiIndex::From3D;


namespace {

inline const char *GetLabelFromChannel(Channel channel)
{
    switch(channel)
    {
        case FrontLeft: return "front-left";
        case FrontRight: return "front-right";
        case FrontCenter: return "front-center";
        case LFE: return "lfe";
        case BackLeft: return "back-left";
        case BackRight: return "back-right";
        case BackCenter: return "back-center";
        case SideLeft: return "side-left";
        case SideRight: return "side-right";

        case UpperFrontLeft: return "upper-front-left";
        case UpperFrontRight: return "upper-front-right";
        case UpperBackLeft: return "upper-back-left";
        case UpperBackRight: return "upper-back-right";
        case LowerFrontLeft: return "lower-front-left";
        case LowerFrontRight: return "lower-front-right";
        case LowerBackLeft: return "lower-back-left";
        case LowerBackRight: return "lower-back-right";

        case Aux0: return "aux-0";
        case Aux1: return "aux-1";
        case Aux2: return "aux-2";
        case Aux3: return "aux-3";
        case Aux4: return "aux-4";
        case Aux5: return "aux-5";
        case Aux6: return "aux-6";
        case Aux7: return "aux-7";
        case Aux8: return "aux-8";
        case Aux9: return "aux-9";
        case Aux10: return "aux-10";
        case Aux11: return "aux-11";
        case Aux12: return "aux-12";
        case Aux13: return "aux-13";
        case Aux14: return "aux-14";
        case Aux15: return "aux-15";

        case InvalidChannel: break;
    }
    return "(unknown)";
}


struct ChannelMap {
    Channel ChanName;
    ALfloat Config[MAX_AMBI2D_COEFFS];
};

bool MakeSpeakerMap(ALCdevice *device, const AmbDecConf *conf, ALsizei (&speakermap)[MAX_OUTPUT_CHANNELS])
{
    auto map_spkr = [device](const AmbDecConf::SpeakerConf &speaker) -> ALsizei
    {
        /* NOTE: AmbDec does not define any standard speaker names, however
         * for this to work we have to by able to find the output channel
         * the speaker definition corresponds to. Therefore, OpenAL Soft
         * requires these channel labels to be recognized:
         *
         * LF = Front left
         * RF = Front right
         * LS = Side left
         * RS = Side right
         * LB = Back left
         * RB = Back right
         * CE = Front center
         * CB = Back center
         *
         * Additionally, surround51 will acknowledge back speakers for side
         * channels, and surround51rear will acknowledge side speakers for
         * back channels, to avoid issues with an ambdec expecting 5.1 to
         * use the side channels when the device is configured for back,
         * and vice-versa.
         */
        Channel ch{};
        if(speaker.Name == "LF")
            ch = FrontLeft;
        else if(speaker.Name == "RF")
            ch = FrontRight;
        else if(speaker.Name == "CE")
            ch = FrontCenter;
        else if(speaker.Name == "LS")
        {
            if(device->FmtChans == DevFmtX51Rear)
                ch = BackLeft;
            else
                ch = SideLeft;
        }
        else if(speaker.Name == "RS")
        {
            if(device->FmtChans == DevFmtX51Rear)
                ch = BackRight;
            else
                ch = SideRight;
        }
        else if(speaker.Name == "LB")
        {
            if(device->FmtChans == DevFmtX51)
                ch = SideLeft;
            else
                ch = BackLeft;
        }
        else if(speaker.Name == "RB")
        {
            if(device->FmtChans == DevFmtX51)
                ch = SideRight;
            else
                ch = BackRight;
        }
        else if(speaker.Name == "CB")
            ch = BackCenter;
        else
        {
            const char *name{speaker.Name.c_str()};
            unsigned int n;
            char c;

            if(sscanf(name, "AUX%u%c", &n, &c) == 1 && n < 16)
                ch = static_cast<Channel>(Aux0+n);
            else
            {
                ERR("AmbDec speaker label \"%s\" not recognized\n", name);
                return -1;
            }
        }
        const int chidx{GetChannelIdxByName(device->RealOut, ch)};
        if(chidx == -1)
            ERR("Failed to lookup AmbDec speaker label %s\n", speaker.Name.c_str());
        return chidx;
    };
    std::transform(conf->Speakers.begin(), conf->Speakers.end(), std::begin(speakermap), map_spkr);
    /* Return success if no invalid entries are found. */
    auto speakermap_end = std::begin(speakermap) + conf->Speakers.size();
    return std::find(std::begin(speakermap), speakermap_end, -1) == speakermap_end;
}


constexpr ChannelMap MonoCfg[1] = {
    { FrontCenter, { 1.0f } },
}, StereoCfg[2] = {
    { FrontLeft,   { 5.00000000e-1f,  2.88675135e-1f,  5.52305643e-2f } },
    { FrontRight,  { 5.00000000e-1f, -2.88675135e-1f,  5.52305643e-2f } },
}, QuadCfg[4] = {
    { BackLeft,    { 3.53553391e-1f,  2.04124145e-1f, -2.04124145e-1f } },
    { FrontLeft,   { 3.53553391e-1f,  2.04124145e-1f,  2.04124145e-1f } },
    { FrontRight,  { 3.53553391e-1f, -2.04124145e-1f,  2.04124145e-1f } },
    { BackRight,   { 3.53553391e-1f, -2.04124145e-1f, -2.04124145e-1f } },
}, X51SideCfg[4] = {
    { SideLeft,    { 3.33000782e-1f,  1.89084803e-1f, -2.00042375e-1f, -2.12307769e-2f, -1.14579885e-2f } },
    { FrontLeft,   { 1.88542860e-1f,  1.27709292e-1f,  1.66295695e-1f,  7.30571517e-2f,  2.10901184e-2f } },
    { FrontRight,  { 1.88542860e-1f, -1.27709292e-1f,  1.66295695e-1f, -7.30571517e-2f,  2.10901184e-2f } },
    { SideRight,   { 3.33000782e-1f, -1.89084803e-1f, -2.00042375e-1f,  2.12307769e-2f, -1.14579885e-2f } },
}, X51RearCfg[4] = {
    { BackLeft,    { 3.33000782e-1f,  1.89084803e-1f, -2.00042375e-1f, -2.12307769e-2f, -1.14579885e-2f } },
    { FrontLeft,   { 1.88542860e-1f,  1.27709292e-1f,  1.66295695e-1f,  7.30571517e-2f,  2.10901184e-2f } },
    { FrontRight,  { 1.88542860e-1f, -1.27709292e-1f,  1.66295695e-1f, -7.30571517e-2f,  2.10901184e-2f } },
    { BackRight,   { 3.33000782e-1f, -1.89084803e-1f, -2.00042375e-1f,  2.12307769e-2f, -1.14579885e-2f } },
}, X61Cfg[6] = {
    { SideLeft,    { 2.04460341e-1f,  2.17177926e-1f, -4.39996780e-2f, -2.60790269e-2f, -6.87239792e-2f } },
    { FrontLeft,   { 1.58923161e-1f,  9.21772680e-2f,  1.59658796e-1f,  6.66278083e-2f,  3.84686854e-2f } },
    { FrontRight,  { 1.58923161e-1f, -9.21772680e-2f,  1.59658796e-1f, -6.66278083e-2f,  3.84686854e-2f } },
    { SideRight,   { 2.04460341e-1f, -2.17177926e-1f, -4.39996780e-2f,  2.60790269e-2f, -6.87239792e-2f } },
    { BackCenter,  { 2.50001688e-1f,  0.00000000e+0f, -2.50000094e-1f,  0.00000000e+0f,  6.05133395e-2f } },
}, X71Cfg[6] = {
    { BackLeft,    { 2.04124145e-1f,  1.08880247e-1f, -1.88586120e-1f, -1.29099444e-1f,  7.45355993e-2f,  3.73460789e-2f,  0.00000000e+0f } },
    { SideLeft,    { 2.04124145e-1f,  2.17760495e-1f,  0.00000000e+0f,  0.00000000e+0f, -1.49071198e-1f, -3.73460789e-2f,  0.00000000e+0f } },
    { FrontLeft,   { 2.04124145e-1f,  1.08880247e-1f,  1.88586120e-1f,  1.29099444e-1f,  7.45355993e-2f,  3.73460789e-2f,  0.00000000e+0f } },
    { FrontRight,  { 2.04124145e-1f, -1.08880247e-1f,  1.88586120e-1f, -1.29099444e-1f,  7.45355993e-2f, -3.73460789e-2f,  0.00000000e+0f } },
    { SideRight,   { 2.04124145e-1f, -2.17760495e-1f,  0.00000000e+0f,  0.00000000e+0f, -1.49071198e-1f,  3.73460789e-2f,  0.00000000e+0f } },
    { BackRight,   { 2.04124145e-1f, -1.08880247e-1f, -1.88586120e-1f,  1.29099444e-1f,  7.45355993e-2f, -3.73460789e-2f,  0.00000000e+0f } },
};

void InitNearFieldCtrl(ALCdevice *device, ALfloat ctrl_dist, ALsizei order, const ALsizei *RESTRICT chans_per_order)
{
    /* NFC is only used when AvgSpeakerDist is greater than 0, and can only be
     * used when rendering to an ambisonic buffer.
     */
    const char *devname{device->DeviceName.c_str()};
    if(!GetConfigValueBool(devname, "decoder", "nfc", 1) || !(ctrl_dist > 0.0f))
        return;

    device->AvgSpeakerDist = minf(ctrl_dist, 10.0f);
    TRACE("Using near-field reference distance: %.2f meters\n", device->AvgSpeakerDist);

    auto iter = std::copy(chans_per_order, chans_per_order+order+1,
        std::begin(device->NumChannelsPerOrder));
    std::fill(iter, std::end(device->NumChannelsPerOrder), 0);
}

void InitDistanceComp(ALCdevice *device, const AmbDecConf *conf, const ALsizei (&speakermap)[MAX_OUTPUT_CHANNELS])
{
    using namespace std::placeholders;

    const ALfloat maxdist{
        std::accumulate(conf->Speakers.begin(), conf->Speakers.end(), float{0.0f},
            std::bind(maxf, _1, std::bind(std::mem_fn(&AmbDecConf::SpeakerConf::Distance), _2))
        )
    };

    const char *devname{device->DeviceName.c_str()};
    if(!GetConfigValueBool(devname, "decoder", "distance-comp", 1) || !(maxdist > 0.0f))
        return;

    auto srate = static_cast<ALfloat>(device->Frequency);
    size_t total{0u};
    for(size_t i{0u};i < conf->Speakers.size();i++)
    {
        const AmbDecConf::SpeakerConf &speaker = conf->Speakers[i];
        const ALsizei chan{speakermap[i]};

        /* Distance compensation only delays in steps of the sample rate. This
         * is a bit less accurate since the delay time falls to the nearest
         * sample time, but it's far simpler as it doesn't have to deal with
         * phase offsets. This means at 48khz, for instance, the distance delay
         * will be in steps of about 7 millimeters.
         */
        const ALfloat delay{
            std::floor((maxdist - speaker.Distance)/SPEEDOFSOUNDMETRESPERSEC*srate + 0.5f)
        };
        if(delay >= static_cast<ALfloat>(MAX_DELAY_LENGTH))
            ERR("Delay for speaker \"%s\" exceeds buffer length (%f >= %d)\n",
                speaker.Name.c_str(), delay, MAX_DELAY_LENGTH);

        device->ChannelDelay[chan].Length = static_cast<ALsizei>(clampf(
            delay, 0.0f, static_cast<ALfloat>(MAX_DELAY_LENGTH-1)
        ));
        device->ChannelDelay[chan].Gain = speaker.Distance / maxdist;
        TRACE("Channel %u \"%s\" distance compensation: %d samples, %f gain\n", chan,
            speaker.Name.c_str(), device->ChannelDelay[chan].Length,
            device->ChannelDelay[chan].Gain
        );

        /* Round up to the next 4th sample, so each channel buffer starts
         * 16-byte aligned.
         */
        total += RoundUp(device->ChannelDelay[chan].Length, 4);
    }

    if(total > 0)
    {
        device->ChannelDelay.resize(total);
        device->ChannelDelay[0].Buffer = device->ChannelDelay.data();
        auto set_bufptr = [](const DistanceComp::DistData &last, const DistanceComp::DistData &cur) -> DistanceComp::DistData
        {
            DistanceComp::DistData ret{cur};
            ret.Buffer = last.Buffer + RoundUp(last.Length, 4);
            return ret;
        };
        std::partial_sum(device->ChannelDelay.begin(), device->ChannelDelay.end(),
            device->ChannelDelay.begin(), set_bufptr);
    }
}


auto GetAmbiScales(AmbiNorm scaletype) noexcept -> const std::array<float,MAX_AMBI_COEFFS>&
{
    if(scaletype == AmbiNorm::FuMa) return AmbiScale::FromFuMa;
    if(scaletype == AmbiNorm::SN3D) return AmbiScale::FromSN3D;
    return AmbiScale::FromN3D;
}

auto GetAmbiLayout(AmbiLayout layouttype) noexcept -> const std::array<int,MAX_AMBI_COEFFS>&
{
    if(layouttype == AmbiLayout::FuMa) return AmbiIndex::FromFuMa;
    return AmbiIndex::FromACN;
}


void InitPanning(ALCdevice *device)
{
    const ChannelMap *chanmap{nullptr};
    ALsizei coeffcount{0};
    ALsizei count{0};

    switch(device->FmtChans)
    {
        case DevFmtMono:
            count = static_cast<ALsizei>(COUNTOF(MonoCfg));
            chanmap = MonoCfg;
            coeffcount = 1;
            break;

        case DevFmtStereo:
            count = static_cast<ALsizei>(COUNTOF(StereoCfg));
            chanmap = StereoCfg;
            coeffcount = 3;
            break;

        case DevFmtQuad:
            count = static_cast<ALsizei>(COUNTOF(QuadCfg));
            chanmap = QuadCfg;
            coeffcount = 3;
            break;

        case DevFmtX51:
            count = static_cast<ALsizei>(COUNTOF(X51SideCfg));
            chanmap = X51SideCfg;
            coeffcount = 5;
            break;

        case DevFmtX51Rear:
            count = static_cast<ALsizei>(COUNTOF(X51RearCfg));
            chanmap = X51RearCfg;
            coeffcount = 5;
            break;

        case DevFmtX61:
            count = static_cast<ALsizei>(COUNTOF(X61Cfg));
            chanmap = X61Cfg;
            coeffcount = 5;
            break;

        case DevFmtX71:
            count = static_cast<ALsizei>(COUNTOF(X71Cfg));
            chanmap = X71Cfg;
            coeffcount = 7;
            break;

        case DevFmtAmbi3D:
            break;
    }

    if(device->FmtChans == DevFmtAmbi3D)
    {
        const char *devname{device->DeviceName.c_str()};
        const std::array<int,MAX_AMBI_COEFFS> &acnmap = GetAmbiLayout(device->mAmbiLayout);
        const std::array<float,MAX_AMBI_COEFFS> &n3dscale = GetAmbiScales(device->mAmbiScale);

        count = (device->mAmbiOrder == 3) ? 16 :
                (device->mAmbiOrder == 2) ? 9 :
                (device->mAmbiOrder == 1) ? 4 : 1;
        std::transform(acnmap.begin(), acnmap.begin()+count, std::begin(device->Dry.AmbiMap),
            [&n3dscale](const ALsizei &acn) noexcept -> BFChannelConfig
            { return BFChannelConfig{1.0f/n3dscale[acn], acn}; }
        );
        device->Dry.NumChannels = count;

        if(device->mAmbiOrder < 2)
        {
            device->FOAOut.AmbiMap = device->Dry.AmbiMap;
            device->FOAOut.NumChannels = 0;
        }
        else
        {
            device->FOAOut.AmbiMap.fill(BFChannelConfig{});
            std::transform(AmbiIndex::From3D.begin(), AmbiIndex::From3D.begin()+4,
                std::begin(device->FOAOut.AmbiMap),
                [](const ALsizei &acn) noexcept { return BFChannelConfig{1.0f, acn}; }
            );
            device->FOAOut.NumChannels = 4;

            device->AmbiUp = al::make_unique<AmbiUpsampler>();
            device->AmbiUp->reset(device->mAmbiOrder,
                400.0f / static_cast<ALfloat>(device->Frequency));
        }

        ALfloat nfc_delay{0.0f};
        if(ConfigValueFloat(devname, "decoder", "nfc-ref-delay", &nfc_delay) && nfc_delay > 0.0f)
        {
            static constexpr ALsizei chans_per_order[MAX_AMBI_ORDER+1]{ 1, 3, 5, 7 };
            nfc_delay = clampf(nfc_delay, 0.001f, 1000.0f);
            InitNearFieldCtrl(device, nfc_delay * SPEEDOFSOUNDMETRESPERSEC,
                              device->mAmbiOrder, chans_per_order);
        }

        device->RealOut.NumChannels = 0;
    }
    else
    {
        ChannelDec chancoeffs[MAX_OUTPUT_CHANNELS]{};
        ALsizei idxmap[MAX_OUTPUT_CHANNELS]{};
        for(ALsizei i{0};i < count;++i)
        {
            const ALint idx{GetChannelIdxByName(device->RealOut, chanmap[i].ChanName)};
            if(idx < 0)
            {
                ERR("Failed to find %s channel in device\n",
                    GetLabelFromChannel(chanmap[i].ChanName));
                continue;
            }
            idxmap[i] = idx;
            std::copy_n(chanmap[i].Config, coeffcount, chancoeffs[i]);
        }

        std::transform(AmbiIndex::From2D.begin(), AmbiIndex::From2D.begin()+coeffcount,
            std::begin(device->Dry.AmbiMap),
            [](const ALsizei &index) noexcept { return BFChannelConfig{1.0f, index}; }
        );
        device->Dry.NumChannels = coeffcount;

        TRACE("Enabling %s-order%s ambisonic decoder\n",
            (coeffcount > 5) ? "third" :
            (coeffcount > 3) ? "second" : "first",
            ""
        );
        device->AmbiDecoder = al::make_unique<BFormatDec>();
        device->AmbiDecoder->reset(coeffcount, 400.0f / static_cast<ALfloat>(device->Frequency),
            count, chancoeffs, idxmap);

        if(coeffcount <= 3)
            device->FOAOut.AmbiMap = device->Dry.AmbiMap;
        else
        {
            const std::array<ALfloat,MAX_AMBI_ORDER+1> scales{AmbiUpsampler::GetHFOrderScales(1,
                (coeffcount > 7) ? 4 :
                (coeffcount > 5) ? 3 :
                (coeffcount > 3) ? 2 : 1)};

            device->FOAOut.AmbiMap[0] = BFChannelConfig{scales[0], AmbiIndex::From2D[0]};
            auto ambimap_iter = std::transform(AmbiIndex::From2D.begin()+1,
                AmbiIndex::From2D.begin()+3, std::begin(device->FOAOut.AmbiMap)+1,
                [&scales](const ALsizei &acn) noexcept { return BFChannelConfig{scales[1], acn}; }
            );
            std::fill(ambimap_iter, std::end(device->FOAOut.AmbiMap), BFChannelConfig{});
        }
        device->FOAOut.NumChannels = 0;

        device->RealOut.NumChannels = device->channelsFromFmt();
    }
}

void InitCustomPanning(ALCdevice *device, const AmbDecConf *conf, const ALsizei (&speakermap)[MAX_OUTPUT_CHANNELS])
{
    if(conf->FreqBands != 1)
        ERR("Basic renderer uses the high-frequency matrix as single-band (xover_freq = %.0fhz)\n",
            conf->XOverFreq);

    ALsizei count;
    if((conf->ChanMask&AMBI_PERIPHONIC_MASK))
    {
        count = (conf->ChanMask > AMBI_2ORDER_MASK) ? 16 :
                (conf->ChanMask > AMBI_1ORDER_MASK) ? 9 : 4;
        std::transform(AmbiIndex::From3D.begin(), AmbiIndex::From3D.begin()+count,
            std::begin(device->Dry.AmbiMap),
            [](const ALsizei &index) noexcept { return BFChannelConfig{1.0f, index}; }
        );
    }
    else
    {
        count = (conf->ChanMask > AMBI_2ORDER_MASK) ? 7 :
                (conf->ChanMask > AMBI_1ORDER_MASK) ? 5 : 3;
        std::transform(AmbiIndex::From2D.begin(), AmbiIndex::From2D.begin()+count,
            std::begin(device->Dry.AmbiMap),
            [](const ALsizei &index) noexcept { return BFChannelConfig{1.0f, index}; }
        );
    }
    device->Dry.NumChannels = count;

    TRACE("Enabling %s-order%s ambisonic decoder\n",
        (conf->ChanMask > AMBI_2ORDER_MASK) ? "third" :
        (conf->ChanMask > AMBI_1ORDER_MASK) ? "second" : "first",
        (conf->ChanMask&AMBI_PERIPHONIC_MASK) ? " periphonic" : ""
    );
    device->AmbiDecoder = al::make_unique<BFormatDec>();
    device->AmbiDecoder->reset(conf, false, count, device->Frequency, speakermap);

    if(conf->ChanMask <= AMBI_1ORDER_MASK)
        device->FOAOut.AmbiMap = device->Dry.AmbiMap;
    else
    {
        const std::array<ALfloat,MAX_AMBI_ORDER+1> scales{AmbiUpsampler::GetHFOrderScales(1,
            (conf->ChanMask > AMBI_3ORDER_MASK) ? 4 :
            (conf->ChanMask > AMBI_2ORDER_MASK) ? 3 :
            (conf->ChanMask > AMBI_1ORDER_MASK) ? 2 : 1)};

        auto ambimap_iter = std::begin(device->FOAOut.AmbiMap);
        if((conf->ChanMask&AMBI_PERIPHONIC_MASK))
        {
            device->FOAOut.AmbiMap[0] = BFChannelConfig{scales[0], AmbiIndex::From3D[0]};
            ambimap_iter = std::transform(AmbiIndex::From3D.begin()+1,
                AmbiIndex::From3D.begin()+4, ambimap_iter+1,
                [&scales](const ALsizei &acn) noexcept { return BFChannelConfig{scales[1], acn}; }
            );
        }
        else
        {
            device->FOAOut.AmbiMap[0] = BFChannelConfig{scales[0], AmbiIndex::From2D[0]};
            ambimap_iter = std::transform(AmbiIndex::From2D.begin()+1,
                AmbiIndex::From2D.begin()+3, ambimap_iter,
                [&scales](const ALsizei &acn) noexcept { return BFChannelConfig{scales[1], acn}; }
            );
        }
        std::fill(ambimap_iter, std::end(device->FOAOut.AmbiMap), BFChannelConfig{});
    }
    device->FOAOut.NumChannels = 0;

    device->RealOut.NumChannels = device->channelsFromFmt();

    InitDistanceComp(device, conf, speakermap);
}

void InitHQPanning(ALCdevice *device, const AmbDecConf *conf, const ALsizei (&speakermap)[MAX_OUTPUT_CHANNELS])
{
    static constexpr ALsizei chans_per_order2d[MAX_AMBI_ORDER+1] = { 1, 2, 2, 2 };
    static constexpr ALsizei chans_per_order3d[MAX_AMBI_ORDER+1] = { 1, 3, 5, 7 };

    ALsizei count;
    if((conf->ChanMask&AMBI_PERIPHONIC_MASK))
    {
        count = (conf->ChanMask > AMBI_2ORDER_MASK) ? 16 :
                (conf->ChanMask > AMBI_1ORDER_MASK) ? 9 : 4;
        std::transform(AmbiIndex::From3D.begin(), AmbiIndex::From3D.begin()+count,
            std::begin(device->Dry.AmbiMap),
            [](const ALsizei &index) noexcept { return BFChannelConfig{1.0f, index}; }
        );
    }
    else
    {
        count = (conf->ChanMask > AMBI_2ORDER_MASK) ? 7 :
                (conf->ChanMask > AMBI_1ORDER_MASK) ? 5 : 3;
        std::transform(AmbiIndex::From2D.begin(), AmbiIndex::From2D.begin()+count,
            std::begin(device->Dry.AmbiMap),
            [](const ALsizei &index) noexcept { return BFChannelConfig{1.0f, index}; }
        );
    }
    device->Dry.NumChannels = count;

    TRACE("Enabling %s-band %s-order%s ambisonic decoder\n",
        (conf->FreqBands == 1) ? "single" : "dual",
        (conf->ChanMask > AMBI_2ORDER_MASK) ? "third" :
        (conf->ChanMask > AMBI_1ORDER_MASK) ? "second" : "first",
        (conf->ChanMask&AMBI_PERIPHONIC_MASK) ? " periphonic" : ""
    );
    device->AmbiDecoder = al::make_unique<BFormatDec>();
    device->AmbiDecoder->reset(conf, true, count, device->Frequency, speakermap);

    if(conf->ChanMask <= AMBI_1ORDER_MASK)
    {
        device->FOAOut.AmbiMap = device->Dry.AmbiMap;
        device->FOAOut.NumChannels = 0;
    }
    else
    {
        device->FOAOut.AmbiMap.fill(BFChannelConfig{});
        if((conf->ChanMask&AMBI_PERIPHONIC_MASK))
        {
            count = 4;
            std::transform(AmbiIndex::From3D.begin(), AmbiIndex::From3D.begin()+count,
                std::begin(device->FOAOut.AmbiMap),
                [](const ALsizei &index) noexcept { return BFChannelConfig{1.0f, index}; }
            );
        }
        else
        {
            count = 3;
            std::transform(AmbiIndex::From2D.begin(), AmbiIndex::From2D.begin()+count,
                std::begin(device->FOAOut.AmbiMap),
                [](const ALsizei &index) noexcept { return BFChannelConfig{1.0f, index}; }
            );
        }
        device->FOAOut.NumChannels = count;
    }

    device->RealOut.NumChannels = device->channelsFromFmt();

    using namespace std::placeholders;
    auto accum_spkr_dist = std::bind(
        std::plus<float>{}, _1, std::bind(std::mem_fn(&AmbDecConf::SpeakerConf::Distance), _2)
    );
    const ALfloat avg_dist{
        std::accumulate(conf->Speakers.begin(), conf->Speakers.end(), float{0.0f},
            accum_spkr_dist) / static_cast<ALfloat>(conf->Speakers.size())
    };
    InitNearFieldCtrl(device, avg_dist,
        (conf->ChanMask > AMBI_2ORDER_MASK) ? 3 :
        (conf->ChanMask > AMBI_1ORDER_MASK) ? 2 : 1,
        (conf->ChanMask&AMBI_PERIPHONIC_MASK) ? chans_per_order3d : chans_per_order2d
    );

    InitDistanceComp(device, conf, speakermap);
}

void InitHrtfPanning(ALCdevice *device)
{
    /* NOTE: In degrees, and azimuth goes clockwise. */
    static constexpr AngularPoint AmbiPoints[]{
        {  35.264390f,  -45.000000f },
        {  35.264390f,   45.000000f },
        {  35.264390f,  135.000000f },
        {  35.264390f, -135.000000f },
        { -35.264390f,  -45.000000f },
        { -35.264390f,   45.000000f },
        { -35.264390f,  135.000000f },
        { -35.264390f, -135.000000f },
        {   0.000000f,  -20.905157f },
        {   0.000000f,   20.905157f },
        {   0.000000f,  159.094843f },
        {   0.000000f, -159.094843f },
        {  20.905157f,  -90.000000f },
        { -20.905157f,  -90.000000f },
        { -20.905157f,   90.000000f },
        {  20.905157f,   90.000000f },
        {  69.094843f,    0.000000f },
        { -69.094843f,    0.000000f },
        { -69.094843f,  180.000000f },
        {  69.094843f,  180.000000f },
    };
    static constexpr ALfloat AmbiMatrix[][MAX_AMBI_COEFFS]{
        { 5.00000000e-02f,  5.00000000e-02f,  5.00000000e-02f,  5.00000000e-02f,  6.45497224e-02f,  6.45497224e-02f,  0.00000000e+00f,  6.45497224e-02f,  0.00000000e+00f,  1.48264644e-02f,  6.33865691e-02f,  1.01126676e-01f, -7.36485380e-02f, -1.09260065e-02f,  7.08683387e-02f, -1.01622099e-01f },
        { 5.00000000e-02f, -5.00000000e-02f,  5.00000000e-02f,  5.00000000e-02f, -6.45497224e-02f, -6.45497224e-02f,  0.00000000e+00f,  6.45497224e-02f,  0.00000000e+00f, -1.48264644e-02f, -6.33865691e-02f, -1.01126676e-01f, -7.36485380e-02f, -1.09260065e-02f,  7.08683387e-02f, -1.01622099e-01f },
        { 5.00000000e-02f, -5.00000000e-02f,  5.00000000e-02f, -5.00000000e-02f,  6.45497224e-02f, -6.45497224e-02f,  0.00000000e+00f, -6.45497224e-02f,  0.00000000e+00f, -1.48264644e-02f,  6.33865691e-02f, -1.01126676e-01f, -7.36485380e-02f,  1.09260065e-02f,  7.08683387e-02f,  1.01622099e-01f },
        { 5.00000000e-02f,  5.00000000e-02f,  5.00000000e-02f, -5.00000000e-02f, -6.45497224e-02f,  6.45497224e-02f,  0.00000000e+00f, -6.45497224e-02f,  0.00000000e+00f,  1.48264644e-02f, -6.33865691e-02f,  1.01126676e-01f, -7.36485380e-02f,  1.09260065e-02f,  7.08683387e-02f,  1.01622099e-01f },
        { 5.00000000e-02f,  5.00000000e-02f, -5.00000000e-02f,  5.00000000e-02f,  6.45497224e-02f, -6.45497224e-02f,  0.00000000e+00f, -6.45497224e-02f,  0.00000000e+00f,  1.48264644e-02f, -6.33865691e-02f,  1.01126676e-01f,  7.36485380e-02f, -1.09260065e-02f, -7.08683387e-02f, -1.01622099e-01f },
        { 5.00000000e-02f, -5.00000000e-02f, -5.00000000e-02f,  5.00000000e-02f, -6.45497224e-02f,  6.45497224e-02f,  0.00000000e+00f, -6.45497224e-02f,  0.00000000e+00f, -1.48264644e-02f,  6.33865691e-02f, -1.01126676e-01f,  7.36485380e-02f, -1.09260065e-02f, -7.08683387e-02f, -1.01622099e-01f },
        { 5.00000000e-02f, -5.00000000e-02f, -5.00000000e-02f, -5.00000000e-02f,  6.45497224e-02f,  6.45497224e-02f,  0.00000000e+00f,  6.45497224e-02f,  0.00000000e+00f, -1.48264644e-02f, -6.33865691e-02f, -1.01126676e-01f,  7.36485380e-02f,  1.09260065e-02f, -7.08683387e-02f,  1.01622099e-01f },
        { 5.00000000e-02f,  5.00000000e-02f, -5.00000000e-02f, -5.00000000e-02f, -6.45497224e-02f, -6.45497224e-02f,  0.00000000e+00f,  6.45497224e-02f,  0.00000000e+00f,  1.48264644e-02f,  6.33865691e-02f,  1.01126676e-01f,  7.36485380e-02f,  1.09260065e-02f, -7.08683387e-02f,  1.01622099e-01f },
        { 5.00000000e-02f,  3.09016994e-02f,  0.00000000e+00f,  8.09016994e-02f,  6.45497224e-02f,  0.00000000e+00f, -5.59016994e-02f,  0.00000000e+00f,  7.21687836e-02f,  7.76323754e-02f,  0.00000000e+00f, -1.49775925e-01f,  0.00000000e+00f, -2.95083663e-02f,  0.00000000e+00f,  7.76323754e-02f },
        { 5.00000000e-02f, -3.09016994e-02f,  0.00000000e+00f,  8.09016994e-02f, -6.45497224e-02f,  0.00000000e+00f, -5.59016994e-02f,  0.00000000e+00f,  7.21687836e-02f, -7.76323754e-02f,  0.00000000e+00f,  1.49775925e-01f,  0.00000000e+00f, -2.95083663e-02f,  0.00000000e+00f,  7.76323754e-02f },
        { 5.00000000e-02f, -3.09016994e-02f,  0.00000000e+00f, -8.09016994e-02f,  6.45497224e-02f,  0.00000000e+00f, -5.59016994e-02f,  0.00000000e+00f,  7.21687836e-02f, -7.76323754e-02f,  0.00000000e+00f,  1.49775925e-01f,  0.00000000e+00f,  2.95083663e-02f,  0.00000000e+00f, -7.76323754e-02f },
        { 5.00000000e-02f,  3.09016994e-02f,  0.00000000e+00f, -8.09016994e-02f, -6.45497224e-02f,  0.00000000e+00f, -5.59016994e-02f,  0.00000000e+00f,  7.21687836e-02f,  7.76323754e-02f,  0.00000000e+00f, -1.49775925e-01f,  0.00000000e+00f,  2.95083663e-02f,  0.00000000e+00f, -7.76323754e-02f },
        { 5.00000000e-02f,  8.09016994e-02f,  3.09016994e-02f,  0.00000000e+00f,  0.00000000e+00f,  6.45497224e-02f, -3.45491503e-02f,  0.00000000e+00f, -8.44966837e-02f, -4.79794466e-02f,  0.00000000e+00f, -6.77901327e-02f,  3.03448665e-02f,  0.00000000e+00f, -1.65948192e-01f,  0.00000000e+00f },
        { 5.00000000e-02f,  8.09016994e-02f, -3.09016994e-02f,  0.00000000e+00f,  0.00000000e+00f, -6.45497224e-02f, -3.45491503e-02f,  0.00000000e+00f, -8.44966837e-02f, -4.79794466e-02f,  0.00000000e+00f, -6.77901327e-02f, -3.03448665e-02f,  0.00000000e+00f,  1.65948192e-01f,  0.00000000e+00f },
        { 5.00000000e-02f, -8.09016994e-02f, -3.09016994e-02f,  0.00000000e+00f,  0.00000000e+00f,  6.45497224e-02f, -3.45491503e-02f,  0.00000000e+00f, -8.44966837e-02f,  4.79794466e-02f,  0.00000000e+00f,  6.77901327e-02f, -3.03448665e-02f,  0.00000000e+00f,  1.65948192e-01f,  0.00000000e+00f },
        { 5.00000000e-02f, -8.09016994e-02f,  3.09016994e-02f,  0.00000000e+00f,  0.00000000e+00f, -6.45497224e-02f, -3.45491503e-02f,  0.00000000e+00f, -8.44966837e-02f,  4.79794466e-02f,  0.00000000e+00f,  6.77901327e-02f,  3.03448665e-02f,  0.00000000e+00f, -1.65948192e-01f,  0.00000000e+00f },
        { 5.00000000e-02f,  0.00000000e+00f,  8.09016994e-02f,  3.09016994e-02f,  0.00000000e+00f,  0.00000000e+00f,  9.04508497e-02f,  6.45497224e-02f,  1.23279000e-02f,  0.00000000e+00f,  0.00000000e+00f,  0.00000000e+00f,  7.94438918e-02f,  1.12611206e-01f, -2.42115150e-02f,  1.25611822e-01f },
        { 5.00000000e-02f,  0.00000000e+00f, -8.09016994e-02f,  3.09016994e-02f,  0.00000000e+00f,  0.00000000e+00f,  9.04508497e-02f, -6.45497224e-02f,  1.23279000e-02f,  0.00000000e+00f,  0.00000000e+00f,  0.00000000e+00f, -7.94438918e-02f,  1.12611206e-01f,  2.42115150e-02f,  1.25611822e-01f },
        { 5.00000000e-02f,  0.00000000e+00f, -8.09016994e-02f, -3.09016994e-02f,  0.00000000e+00f,  0.00000000e+00f,  9.04508497e-02f,  6.45497224e-02f,  1.23279000e-02f,  0.00000000e+00f,  0.00000000e+00f,  0.00000000e+00f, -7.94438918e-02f, -1.12611206e-01f,  2.42115150e-02f, -1.25611822e-01f },
        { 5.00000000e-02f,  0.00000000e+00f,  8.09016994e-02f, -3.09016994e-02f,  0.00000000e+00f,  0.00000000e+00f,  9.04508497e-02f, -6.45497224e-02f,  1.23279000e-02f,  0.00000000e+00f,  0.00000000e+00f,  0.00000000e+00f,  7.94438918e-02f, -1.12611206e-01f, -2.42115150e-02f, -1.25611822e-01f }
    };
    static constexpr ALfloat AmbiOrderHFGainFOA[MAX_AMBI_ORDER+1]{
        3.16227766e+00f, 1.82574186e+00f
    }, AmbiOrderHFGainHOA[MAX_AMBI_ORDER+1]{
        2.35702260e+00f, 1.82574186e+00f, 9.42809042e-01f
        /* 1.86508671e+00f, 1.60609389e+00f, 1.14205530e+00f, 5.68379553e-01f */
    };
    static constexpr ALsizei IndexMap[9]{ 0, 1, 2, 3, 4, 5, 6, 7, 8 };
    static constexpr ALsizei ChansPerOrder[MAX_AMBI_ORDER+1]{ 1, 3, 5, 0 };
    const ALfloat *AmbiOrderHFGain{AmbiOrderHFGainFOA};
    ALsizei count{4};

    static_assert(COUNTOF(AmbiPoints) == COUNTOF(AmbiMatrix), "Ambisonic HRTF mismatch");

    /* Don't bother with HOA when using full HRTF rendering. Nothing needs it,
     * and it eases the CPU/memory load.
     */
    if(device->mRenderMode != HrtfRender)
    {
        device->AmbiUp = al::make_unique<AmbiUpsampler>();

        AmbiOrderHFGain = AmbiOrderHFGainHOA;
        count = static_cast<ALsizei>(COUNTOF(IndexMap));
    }

    device->mHrtfState = DirectHrtfState::Create(count);

    std::transform(std::begin(IndexMap), std::begin(IndexMap)+count, std::begin(device->Dry.AmbiMap),
        [](const ALsizei &index) noexcept { return BFChannelConfig{1.0f, index}; }
    );
    device->Dry.NumChannels = count;

    if(device->AmbiUp)
    {
        device->FOAOut.AmbiMap.fill(BFChannelConfig{});
        std::transform(std::begin(IndexMap), std::begin(IndexMap)+4, std::begin(device->FOAOut.AmbiMap),
            [](const ALsizei &index) noexcept { return BFChannelConfig{1.0f, index}; }
        );
        device->FOAOut.NumChannels = 4;

        device->AmbiUp->reset(2, 400.0f / static_cast<ALfloat>(device->Frequency));
    }
    else
    {
        device->FOAOut.AmbiMap = device->Dry.AmbiMap;
        device->FOAOut.NumChannels = 0;
    }

    device->RealOut.NumChannels = device->channelsFromFmt();

    BuildBFormatHrtf(device->mHrtf,
        device->mHrtfState.get(), device->Dry.NumChannels, AmbiPoints, AmbiMatrix,
        static_cast<ALsizei>(COUNTOF(AmbiPoints)), AmbiOrderHFGain
    );

    InitNearFieldCtrl(device, device->mHrtf->distance, device->AmbiUp ? 2 : 1,
                      ChansPerOrder);
}

void InitUhjPanning(ALCdevice *device)
{
    static constexpr ALsizei count{3};

    auto acnmap_end = AmbiIndex::FromFuMa.begin() + count;
    std::transform(AmbiIndex::FromFuMa.begin(), acnmap_end, std::begin(device->Dry.AmbiMap),
        [](const ALsizei &acn) noexcept -> BFChannelConfig
        { return BFChannelConfig{1.0f/AmbiScale::FromFuMa[acn], acn}; }
    );
    device->Dry.NumChannels = count;

    device->FOAOut.AmbiMap = device->Dry.AmbiMap;
    device->FOAOut.NumChannels = 0;

    device->RealOut.NumChannels = device->channelsFromFmt();
}

} // namespace


void CalcAmbiCoeffs(const ALfloat y, const ALfloat z, const ALfloat x, const ALfloat spread,
                    ALfloat (&coeffs)[MAX_AMBI_COEFFS])
{
    /* Zeroth-order */
    coeffs[0]  = 1.0f; /* ACN 0 = 1 */
    /* First-order */
    coeffs[1]  = 1.732050808f * y; /* ACN 1 = sqrt(3) * Y */
    coeffs[2]  = 1.732050808f * z; /* ACN 2 = sqrt(3) * Z */
    coeffs[3]  = 1.732050808f * x; /* ACN 3 = sqrt(3) * X */
    /* Second-order */
    coeffs[4]  = 3.872983346f * x * y;             /* ACN 4 = sqrt(15) * X * Y */
    coeffs[5]  = 3.872983346f * y * z;             /* ACN 5 = sqrt(15) * Y * Z */
    coeffs[6]  = 1.118033989f * (z*z*3.0f - 1.0f); /* ACN 6 = sqrt(5)/2 * (3*Z*Z - 1) */
    coeffs[7]  = 3.872983346f * x * z;             /* ACN 7 = sqrt(15) * X * Z */
    coeffs[8]  = 1.936491673f * (x*x - y*y);       /* ACN 8 = sqrt(15)/2 * (X*X - Y*Y) */
    /* Third-order */
    coeffs[9]  =  2.091650066f * y * (x*x*3.0f - y*y);  /* ACN  9 = sqrt(35/8) * Y * (3*X*X - Y*Y) */
    coeffs[10] = 10.246950766f * z * x * y;             /* ACN 10 = sqrt(105) * Z * X * Y */
    coeffs[11] =  1.620185175f * y * (z*z*5.0f - 1.0f); /* ACN 11 = sqrt(21/8) * Y * (5*Z*Z - 1) */
    coeffs[12] =  1.322875656f * z * (z*z*5.0f - 3.0f); /* ACN 12 = sqrt(7)/2 * Z * (5*Z*Z - 3) */
    coeffs[13] =  1.620185175f * x * (z*z*5.0f - 1.0f); /* ACN 13 = sqrt(21/8) * X * (5*Z*Z - 1) */
    coeffs[14] =  5.123475383f * z * (x*x - y*y);       /* ACN 14 = sqrt(105)/2 * Z * (X*X - Y*Y) */
    coeffs[15] =  2.091650066f * x * (x*x - y*y*3.0f);  /* ACN 15 = sqrt(35/8) * X * (X*X - 3*Y*Y) */
    /* Fourth-order */
    /* ACN 16 = sqrt(35)*3/2 * X * Y * (X*X - Y*Y) */
    /* ACN 17 = sqrt(35/2)*3/2 * (3*X*X - Y*Y) * Y * Z */
    /* ACN 18 = sqrt(5)*3/2 * X * Y * (7*Z*Z - 1) */
    /* ACN 19 = sqrt(5/2)*3/2 * Y * Z * (7*Z*Z - 3)  */
    /* ACN 20 = 3/8 * (35*Z*Z*Z*Z - 30*Z*Z + 3) */
    /* ACN 21 = sqrt(5/2)*3/2 * X * Z * (7*Z*Z - 3) */
    /* ACN 22 = sqrt(5)*3/4 * (X*X - Y*Y) * (7*Z*Z - 1) */
    /* ACN 23 = sqrt(35/2)*3/2 * (X*X - 3*Y*Y) * X * Z */
    /* ACN 24 = sqrt(35)*3/8 * (X*X*X*X - 6*X*X*Y*Y + Y*Y*Y*Y) */

    if(spread > 0.0f)
    {
        /* Implement the spread by using a spherical source that subtends the
         * angle spread. See:
         * http://www.ppsloan.org/publications/StupidSH36.pdf - Appendix A3
         *
         * When adjusted for N3D normalization instead of SN3D, these
         * calculations are:
         *
         * ZH0 = -sqrt(pi) * (-1+ca);
         * ZH1 =  0.5*sqrt(pi) * sa*sa;
         * ZH2 = -0.5*sqrt(pi) * ca*(-1+ca)*(ca+1);
         * ZH3 = -0.125*sqrt(pi) * (-1+ca)*(ca+1)*(5*ca*ca - 1);
         * ZH4 = -0.125*sqrt(pi) * ca*(-1+ca)*(ca+1)*(7*ca*ca - 3);
         * ZH5 = -0.0625*sqrt(pi) * (-1+ca)*(ca+1)*(21*ca*ca*ca*ca - 14*ca*ca + 1);
         *
         * The gain of the source is compensated for size, so that the
         * loudness doesn't depend on the spread. Thus:
         *
         * ZH0 = 1.0f;
         * ZH1 = 0.5f * (ca+1.0f);
         * ZH2 = 0.5f * (ca+1.0f)*ca;
         * ZH3 = 0.125f * (ca+1.0f)*(5.0f*ca*ca - 1.0f);
         * ZH4 = 0.125f * (ca+1.0f)*(7.0f*ca*ca - 3.0f)*ca;
         * ZH5 = 0.0625f * (ca+1.0f)*(21.0f*ca*ca*ca*ca - 14.0f*ca*ca + 1.0f);
         */
        ALfloat ca = std::cos(spread * 0.5f);
        /* Increase the source volume by up to +3dB for a full spread. */
        ALfloat scale = std::sqrt(1.0f + spread/al::MathDefs<float>::Tau());

        ALfloat ZH0_norm = scale;
        ALfloat ZH1_norm = 0.5f * (ca+1.f) * scale;
        ALfloat ZH2_norm = 0.5f * (ca+1.f)*ca * scale;
        ALfloat ZH3_norm = 0.125f * (ca+1.f)*(5.f*ca*ca-1.f) * scale;

        /* Zeroth-order */
        coeffs[0]  *= ZH0_norm;
        /* First-order */
        coeffs[1]  *= ZH1_norm;
        coeffs[2]  *= ZH1_norm;
        coeffs[3]  *= ZH1_norm;
        /* Second-order */
        coeffs[4]  *= ZH2_norm;
        coeffs[5]  *= ZH2_norm;
        coeffs[6]  *= ZH2_norm;
        coeffs[7]  *= ZH2_norm;
        coeffs[8]  *= ZH2_norm;
        /* Third-order */
        coeffs[9]  *= ZH3_norm;
        coeffs[10] *= ZH3_norm;
        coeffs[11] *= ZH3_norm;
        coeffs[12] *= ZH3_norm;
        coeffs[13] *= ZH3_norm;
        coeffs[14] *= ZH3_norm;
        coeffs[15] *= ZH3_norm;
    }
}


void ComputePanningGainsBF(const BFChannelConfig *chanmap, ALsizei numchans, const ALfloat*RESTRICT coeffs, ALfloat ingain, ALfloat (&gains)[MAX_OUTPUT_CHANNELS])
{
    ASSUME(numchans > 0);
    auto iter = std::transform(chanmap, chanmap+numchans, std::begin(gains),
        [coeffs,ingain](const BFChannelConfig &chanmap) noexcept -> ALfloat
        {
            ASSUME(chanmap.Index >= 0);
            return chanmap.Scale * coeffs[chanmap.Index] * ingain;
        }
    );
    std::fill(iter, std::end(gains), 0.0f);
}

void ComputePanGains(const ALeffectslot *slot, const ALfloat*RESTRICT coeffs, ALfloat ingain, ALfloat (&gains)[MAX_OUTPUT_CHANNELS])
{ ComputePanningGainsBF(slot->ChanMap, slot->NumChannels, coeffs, ingain, gains); }


void aluInitRenderer(ALCdevice *device, ALint hrtf_id, HrtfRequestMode hrtf_appreq, HrtfRequestMode hrtf_userreq)
{
    /* Hold the HRTF the device last used, in case it's used again. */
    HrtfEntry *old_hrtf{device->mHrtf};

    device->mHrtfState = nullptr;
    device->mHrtf = nullptr;
    device->HrtfName.clear();
    device->mRenderMode = NormalRender;

    device->Dry.AmbiMap.fill(BFChannelConfig{});
    device->Dry.NumChannels = 0;
    std::fill(std::begin(device->NumChannelsPerOrder), std::end(device->NumChannelsPerOrder), 0);

    device->AvgSpeakerDist = 0.0f;
    device->ChannelDelay.clear();

    device->AmbiDecoder = nullptr;
    device->AmbiUp = nullptr;
    device->Stablizer = nullptr;

    if(device->FmtChans != DevFmtStereo)
    {
        if(old_hrtf)
            old_hrtf->DecRef();
        old_hrtf = nullptr;
        if(hrtf_appreq == Hrtf_Enable)
            device->HrtfStatus = ALC_HRTF_UNSUPPORTED_FORMAT_SOFT;

        const char *layout{nullptr};
        switch(device->FmtChans)
        {
            case DevFmtQuad: layout = "quad"; break;
            case DevFmtX51: /* fall-through */
            case DevFmtX51Rear: layout = "surround51"; break;
            case DevFmtX61: layout = "surround61"; break;
            case DevFmtX71: layout = "surround71"; break;
            /* Mono, Stereo, and Ambisonics output don't use custom decoders. */
            case DevFmtMono:
            case DevFmtStereo:
            case DevFmtAmbi3D:
                break;
        }

        const char *devname{device->DeviceName.c_str()};
        ALsizei speakermap[MAX_OUTPUT_CHANNELS];
        AmbDecConf *pconf{nullptr};
        AmbDecConf conf{};
        if(layout)
        {
            const char *fname;
            if(ConfigValueStr(devname, "decoder", layout, &fname))
            {
                if(!conf.load(fname))
                    ERR("Failed to load layout file %s\n", fname);
                else if(conf.Speakers.size() > MAX_OUTPUT_CHANNELS)
                    ERR("Unsupported speaker count " SZFMT " (max %d)\n", conf.Speakers.size(),
                        MAX_OUTPUT_CHANNELS);
                else if(conf.ChanMask > AMBI_3ORDER_MASK)
                    ERR("Unsupported channel mask 0x%04x (max 0x%x)\n", conf.ChanMask,
                        AMBI_3ORDER_MASK);
                else if(MakeSpeakerMap(device, &conf, speakermap))
                    pconf = &conf;
            }
        }

        if(!pconf)
            InitPanning(device);
        else if(GetConfigValueBool(devname, "decoder", "hq-mode", 0))
            InitHQPanning(device, pconf, speakermap);
        else
            InitCustomPanning(device, pconf, speakermap);

        /* Enable the stablizer only for formats that have front-left, front-
         * right, and front-center outputs.
         */
        switch(device->FmtChans)
        {
        case DevFmtX51:
        case DevFmtX51Rear:
        case DevFmtX61:
        case DevFmtX71:
            if(GetConfigValueBool(devname, nullptr, "front-stablizer", 0))
            {
                auto stablizer = al::make_unique<FrontStablizer>();
                /* Initialize band-splitting filters for the front-left and
                 * front-right channels, with a crossover at 5khz (could be
                 * higher).
                 */
                const ALfloat scale{static_cast<ALfloat>(5000.0 / device->Frequency)};

                stablizer->LFilter.init(scale);
                stablizer->RFilter = stablizer->LFilter;

                /* Initialize all-pass filters for all other channels. */
                stablizer->APFilter[0].init(scale);
                std::fill(std::begin(stablizer->APFilter)+1, std::end(stablizer->APFilter),
                    stablizer->APFilter[0]);

                device->Stablizer = std::move(stablizer);
            }
            break;
        case DevFmtMono:
        case DevFmtStereo:
        case DevFmtQuad:
        case DevFmtAmbi3D:
            break;
        }
        TRACE("Front stablizer %s\n", device->Stablizer ? "enabled" : "disabled");

        return;
    }

    device->AmbiDecoder = nullptr;

    bool headphones{device->IsHeadphones != AL_FALSE};
    if(device->Type != Loopback)
    {
        const char *mode;
        if(ConfigValueStr(device->DeviceName.c_str(), nullptr, "stereo-mode", &mode))
        {
            if(strcasecmp(mode, "headphones") == 0)
                headphones = true;
            else if(strcasecmp(mode, "speakers") == 0)
                headphones = false;
            else if(strcasecmp(mode, "auto") != 0)
                ERR("Unexpected stereo-mode: %s\n", mode);
        }
    }

    if(hrtf_userreq == Hrtf_Default)
    {
        bool usehrtf = (headphones && hrtf_appreq != Hrtf_Disable) ||
                       (hrtf_appreq == Hrtf_Enable);
        if(!usehrtf) goto no_hrtf;

        device->HrtfStatus = ALC_HRTF_ENABLED_SOFT;
        if(headphones && hrtf_appreq != Hrtf_Disable)
            device->HrtfStatus = ALC_HRTF_HEADPHONES_DETECTED_SOFT;
    }
    else
    {
        if(hrtf_userreq != Hrtf_Enable)
        {
            if(hrtf_appreq == Hrtf_Enable)
                device->HrtfStatus = ALC_HRTF_DENIED_SOFT;
            goto no_hrtf;
        }
        device->HrtfStatus = ALC_HRTF_REQUIRED_SOFT;
    }

    if(device->HrtfList.empty())
        device->HrtfList = EnumerateHrtf(device->DeviceName.c_str());

    if(hrtf_id >= 0 && static_cast<size_t>(hrtf_id) < device->HrtfList.size())
    {
        const EnumeratedHrtf &entry = device->HrtfList[hrtf_id];
        HrtfEntry *hrtf{GetLoadedHrtf(entry.hrtf)};
        if(hrtf && hrtf->sampleRate == device->Frequency)
        {
            device->mHrtf = hrtf;
            device->HrtfName = entry.name;
        }
        else if(hrtf)
            hrtf->DecRef();
    }

    if(!device->mHrtf)
    {
        auto find_hrtf = [device](const EnumeratedHrtf &entry) -> bool
        {
            HrtfEntry *hrtf{GetLoadedHrtf(entry.hrtf)};
            if(!hrtf) return false;
            if(hrtf->sampleRate != device->Frequency)
            {
                hrtf->DecRef();
                return false;
            }
            device->mHrtf = hrtf;
            device->HrtfName = entry.name;
            return true;
        };
        std::find_if(device->HrtfList.cbegin(), device->HrtfList.cend(), find_hrtf);
    }

    if(device->mHrtf)
    {
        if(old_hrtf)
            old_hrtf->DecRef();
        old_hrtf = nullptr;

        device->mRenderMode = HrtfRender;
        const char *mode;
        if(ConfigValueStr(device->DeviceName.c_str(), nullptr, "hrtf-mode", &mode))
        {
            if(strcasecmp(mode, "full") == 0)
                device->mRenderMode = HrtfRender;
            else if(strcasecmp(mode, "basic") == 0)
                device->mRenderMode = NormalRender;
            else
                ERR("Unexpected hrtf-mode: %s\n", mode);
        }

        TRACE("%s HRTF rendering enabled, using \"%s\"\n",
            ((device->mRenderMode == HrtfRender) ? "Full" : "Basic"), device->HrtfName.c_str()
        );
        InitHrtfPanning(device);
        return;
    }
    device->HrtfStatus = ALC_HRTF_UNSUPPORTED_FORMAT_SOFT;

no_hrtf:
    if(old_hrtf)
        old_hrtf->DecRef();
    old_hrtf = nullptr;
    TRACE("HRTF disabled\n");

    device->mRenderMode = StereoPair;

    int bs2blevel{((headphones && hrtf_appreq != Hrtf_Disable) ||
                   (hrtf_appreq == Hrtf_Enable)) ? 5 : 0};
    if(device->Type != Loopback)
        ConfigValueInt(device->DeviceName.c_str(), nullptr, "cf_level", &bs2blevel);
    if(bs2blevel > 0 && bs2blevel <= 6)
    {
        device->Bs2b = al::make_unique<bs2b>();
        bs2b_set_params(device->Bs2b.get(), bs2blevel, device->Frequency);
        TRACE("BS2B enabled\n");
        InitPanning(device);
        return;
    }

    TRACE("BS2B disabled\n");

    const char *mode;
    if(ConfigValueStr(device->DeviceName.c_str(), nullptr, "stereo-encoding", &mode))
    {
        if(strcasecmp(mode, "uhj") == 0)
            device->mRenderMode = NormalRender;
        else if(strcasecmp(mode, "panpot") != 0)
            ERR("Unexpected stereo-encoding: %s\n", mode);
    }
    if(device->mRenderMode == NormalRender)
    {
        device->Uhj_Encoder = al::make_unique<Uhj2Encoder>();
        TRACE("UHJ enabled\n");
        InitUhjPanning(device);
        return;
    }

    TRACE("UHJ disabled\n");
    InitPanning(device);
}


void aluInitEffectPanning(ALeffectslot *slot)
{
    const size_t count{countof(slot->ChanMap)};
    auto acnmap_end = AmbiIndex::From3D.begin() + count;
    std::transform(AmbiIndex::From3D.begin(), acnmap_end, std::begin(slot->ChanMap),
        [](const ALsizei &acn) noexcept { return BFChannelConfig{1.0f, acn}; }
    );
    slot->NumChannels = static_cast<ALsizei>(count);
}
