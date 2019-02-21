
#include "config.h"

#include <cmath>
#include <array>
#include <vector>
#include <numeric>
#include <algorithm>
#include <functional>

#include "bformatdec.h"
#include "ambdec.h"
#include "filters/splitter.h"
#include "alu.h"

#include "threads.h"
#include "almalloc.h"


namespace {

#define HF_BAND 0
#define LF_BAND 1
static_assert(BFormatDec::sNumBands == 2, "Unexpected BFormatDec::sNumBands");
static_assert(AmbiUpsampler::sNumBands == 2, "Unexpected AmbiUpsampler::sNumBands");

/* These points are in AL coordinates! */
constexpr ALfloat Ambi3DPoints[8][3] = {
    { -0.577350269f,  0.577350269f, -0.577350269f },
    {  0.577350269f,  0.577350269f, -0.577350269f },
    { -0.577350269f,  0.577350269f,  0.577350269f },
    {  0.577350269f,  0.577350269f,  0.577350269f },
    { -0.577350269f, -0.577350269f, -0.577350269f },
    {  0.577350269f, -0.577350269f, -0.577350269f },
    { -0.577350269f, -0.577350269f,  0.577350269f },
    {  0.577350269f, -0.577350269f,  0.577350269f },
};
constexpr ALfloat Ambi3DDecoder[8][MAX_AMBI_COEFFS] = {
    { 0.125f,  0.125f,  0.125f,  0.125f },
    { 0.125f, -0.125f,  0.125f,  0.125f },
    { 0.125f,  0.125f,  0.125f, -0.125f },
    { 0.125f, -0.125f,  0.125f, -0.125f },
    { 0.125f,  0.125f, -0.125f,  0.125f },
    { 0.125f, -0.125f, -0.125f,  0.125f },
    { 0.125f,  0.125f, -0.125f, -0.125f },
    { 0.125f, -0.125f, -0.125f, -0.125f },
};
constexpr ALfloat Ambi3DDecoderHFScale[MAX_AMBI_COEFFS] = {
    2.0f,
    1.15470054f, 1.15470054f, 1.15470054f
};


auto GetAmbiScales(AmbDecScale scaletype) noexcept -> const float(&)[MAX_AMBI_COEFFS]
{
    if(scaletype == AmbDecScale::FuMa) return AmbiScale::FromFuMa;
    if(scaletype == AmbDecScale::SN3D) return AmbiScale::FromSN3D;
    return AmbiScale::FromN3D;
}

} // namespace


void BFormatDec::reset(const AmbDecConf *conf, ALsizei chancount, ALuint srate, const ALsizei (&chanmap)[MAX_OUTPUT_CHANNELS])
{
    static constexpr ALsizei map2DTo3D[MAX_AMBI2D_COEFFS]{ 0,  1, 3,  4, 8,  9, 15 };

    mSamples.clear();
    mSamplesHF = nullptr;
    mSamplesLF = nullptr;

    mNumChannels = chancount;
    mSamples.resize(chancount * 2);
    mSamplesHF = mSamples.data();
    mSamplesLF = mSamplesHF + chancount;

    mEnabled = std::accumulate(std::begin(chanmap), std::begin(chanmap)+conf->Speakers.size(), 0u,
        [](ALuint mask, const ALsizei &chan) noexcept -> ALuint
        { return mask | (1 << chan); }
    );

    mUpSampler[0].XOver.init(400.0f / (float)srate);
    std::fill(std::begin(mUpSampler[0].Gains), std::end(mUpSampler[0].Gains), 0.0f);
    std::fill(std::begin(mUpSampler)+1, std::end(mUpSampler), mUpSampler[0]);

    const bool periphonic{(conf->ChanMask&AMBI_PERIPHONIC_MASK) != 0};
    if(periphonic)
    {
        ALfloat encgains[8][MAX_OUTPUT_CHANNELS]{};
        for(size_t k{0u};k < COUNTOF(Ambi3DPoints);k++)
        {
            ALfloat coeffs[MAX_AMBI_COEFFS];
            CalcDirectionCoeffs(Ambi3DPoints[k], 0.0f, coeffs);
            std::copy(std::begin(coeffs), std::begin(coeffs)+chancount, std::begin(encgains[k]));
        }
        assert(chancount >= 4);
        for(ALsizei i{0};i < 4;i++)
        {
            ALdouble gain{0.0};
            for(size_t k{0u};k < COUNTOF(Ambi3DDecoder);k++)
                gain += (ALdouble)Ambi3DDecoder[k][i] * encgains[k][i];
            mUpSampler[i].Gains[HF_BAND] = (ALfloat)(gain * Ambi3DDecoderHFScale[i]);
            mUpSampler[i].Gains[LF_BAND] = (ALfloat)gain;
        }
    }
    else
    {
        ALfloat encgains[8][MAX_OUTPUT_CHANNELS]{};
        for(size_t k{0u};k < COUNTOF(Ambi3DPoints);k++)
        {
            ALfloat coeffs[MAX_AMBI_COEFFS];
            CalcDirectionCoeffs(Ambi3DPoints[k], 0.0f, coeffs);
            auto ambimap_end = std::begin(map2DTo3D) + chancount;
            std::transform(std::begin(map2DTo3D), ambimap_end, std::begin(encgains[k]),
                [&coeffs](const ALsizei &index) noexcept -> ALfloat
                { ASSUME(index > 0); return coeffs[index]; }
            );
        }
        assert(chancount >= 3);
        for(ALsizei c{0};c < 3;c++)
        {
            const ALsizei i{map2DTo3D[c]};
            ALdouble gain{0.0};
            for(size_t k{0u};k < COUNTOF(Ambi3DDecoder);k++)
                gain += (ALdouble)Ambi3DDecoder[k][i] * encgains[k][c];
            mUpSampler[c].Gains[HF_BAND] = (ALfloat)(gain * Ambi3DDecoderHFScale[i]);
            mUpSampler[c].Gains[LF_BAND] = (ALfloat)gain;
        }
        mUpSampler[3].Gains[HF_BAND] = 0.0f;
        mUpSampler[3].Gains[LF_BAND] = 0.0f;
    }

    const float (&coeff_scale)[MAX_AMBI_COEFFS] = GetAmbiScales(conf->CoeffScale);
    const ALsizei coeff_count{periphonic ? MAX_AMBI_COEFFS : MAX_AMBI2D_COEFFS};

    mMatrix = MatrixU{};
    mDualBand = (conf->FreqBands == 2);
    if(!mDualBand)
    {
        for(size_t i{0u};i < conf->Speakers.size();i++)
        {
            ALfloat (&mtx)[MAX_AMBI_COEFFS] = mMatrix.Single[chanmap[i]];
            for(ALsizei j{0},k{0};j < coeff_count;j++)
            {
                const ALsizei l{periphonic ? j : map2DTo3D[j]};
                if(!(conf->ChanMask&(1<<l))) continue;
                mtx[j] = conf->HFMatrix[i][k] / coeff_scale[l] *
                    ((l>=9) ? conf->HFOrderGain[3] :
                    (l>=4) ? conf->HFOrderGain[2] :
                    (l>=1) ? conf->HFOrderGain[1] : conf->HFOrderGain[0]);
                ++k;
            }
        }
    }
    else
    {
        mXOver[0].init(conf->XOverFreq / (float)srate);
        std::fill(std::begin(mXOver)+1, std::end(mXOver), mXOver[0]);

        const float ratio{std::pow(10.0f, conf->XOverRatio / 40.0f)};
        for(size_t i{0u};i < conf->Speakers.size();i++)
        {
            ALfloat (&mtx)[sNumBands][MAX_AMBI_COEFFS] = mMatrix.Dual[chanmap[i]];
            for(ALsizei j{0},k{0};j < coeff_count;j++)
            {
                const ALsizei l{periphonic ? j : map2DTo3D[j]};
                if(!(conf->ChanMask&(1<<l))) continue;
                mtx[HF_BAND][j] = conf->HFMatrix[i][k] / coeff_scale[l] *
                    ((l>=9) ? conf->HFOrderGain[3] :
                    (l>=4) ? conf->HFOrderGain[2] :
                    (l>=1) ? conf->HFOrderGain[1] : conf->HFOrderGain[0]) * ratio;
                mtx[LF_BAND][j] = conf->LFMatrix[i][k] / coeff_scale[l] *
                    ((l>=9) ? conf->LFOrderGain[3] :
                    (l>=4) ? conf->LFOrderGain[2] :
                    (l>=1) ? conf->LFOrderGain[1] : conf->LFOrderGain[0]) / ratio;
                ++k;
            }
        }
    }
}

void BFormatDec::process(ALfloat (*RESTRICT OutBuffer)[BUFFERSIZE], const ALsizei OutChannels, const ALfloat (*RESTRICT InSamples)[BUFFERSIZE], const ALsizei SamplesToDo)
{
    ASSUME(OutChannels > 0);
    ASSUME(SamplesToDo > 0);

    if(mDualBand)
    {
        for(ALsizei i{0};i < mNumChannels;i++)
            mXOver[i].process(mSamplesHF[i].data(), mSamplesLF[i].data(), InSamples[i],
                              SamplesToDo);

        for(ALsizei chan{0};chan < OutChannels;chan++)
        {
            if(UNLIKELY(!(mEnabled&(1<<chan))))
                continue;

            std::fill(std::begin(mChannelMix), std::begin(mChannelMix)+SamplesToDo, 0.0f);
            MixRowSamples(mChannelMix, mMatrix.Dual[chan][HF_BAND],
                &reinterpret_cast<ALfloat(&)[BUFFERSIZE]>(mSamplesHF[0]),
                mNumChannels, 0, SamplesToDo
            );
            MixRowSamples(mChannelMix, mMatrix.Dual[chan][LF_BAND],
                &reinterpret_cast<ALfloat(&)[BUFFERSIZE]>(mSamplesLF[0]),
                mNumChannels, 0, SamplesToDo
            );

            std::transform(std::begin(mChannelMix), std::begin(mChannelMix)+SamplesToDo,
                OutBuffer[chan], OutBuffer[chan], std::plus<float>());
        }
    }
    else
    {
        for(ALsizei chan{0};chan < OutChannels;chan++)
        {
            if(UNLIKELY(!(mEnabled&(1<<chan))))
                continue;

            std::fill(std::begin(mChannelMix), std::begin(mChannelMix)+SamplesToDo, 0.0f);
            MixRowSamples(mChannelMix, mMatrix.Single[chan], InSamples,
                          mNumChannels, 0, SamplesToDo);

            std::transform(std::begin(mChannelMix), std::begin(mChannelMix)+SamplesToDo,
                OutBuffer[chan], OutBuffer[chan], std::plus<float>());
        }
    }
}

void BFormatDec::upSample(ALfloat (*RESTRICT OutBuffer)[BUFFERSIZE], const ALfloat (*RESTRICT InSamples)[BUFFERSIZE], const ALsizei InChannels, const ALsizei SamplesToDo)
{
    ASSUME(InChannels > 0);
    ASSUME(SamplesToDo > 0);

    /* This up-sampler leverages the differences observed in dual-band higher-
     * order decoder matrices compared to first-order. For the same output
     * channel configuration, the low-frequency matrix has identical
     * coefficients in the shared input channels, while the high-frequency
     * matrix has extra scalars applied to the W channel and X/Y/Z channels.
     * Mixing the first-order content into the higher-order stream with the
     * appropriate counter-scales applied to the HF response results in the
     * subsequent higher-order decode generating the same response as a first-
     * order decode.
     */
    for(ALsizei i{0};i < InChannels;i++)
    {
        /* First, split the first-order components into low and high frequency
         * bands.
         */
        mUpSampler[i].XOver.process(mSamples[HF_BAND].data(), mSamples[LF_BAND].data(),
            InSamples[i], SamplesToDo);

        /* Now write each band to the output. */
        MixRowSamples(OutBuffer[i], mUpSampler[i].Gains,
            &reinterpret_cast<ALfloat(&)[BUFFERSIZE]>(mSamples[0]),
            sNumBands, 0, SamplesToDo);
    }
}


void AmbiUpsampler::reset(const ALCdevice *device)
{
    mXOver[0].init(400.0f / (float)device->Frequency);
    std::fill(std::begin(mXOver)+1, std::end(mXOver), mXOver[0]);

    ALfloat encgains[8][MAX_OUTPUT_CHANNELS];
    for(size_t k{0u};k < COUNTOF(Ambi3DPoints);k++)
    {
        ALfloat coeffs[MAX_AMBI_COEFFS];
        CalcDirectionCoeffs(Ambi3DPoints[k], 0.0f, coeffs);
        ComputePanGains(&device->Dry, coeffs, 1.0f, encgains[k]);
    }

    /* Combine the matrices that do the in->virt and virt->out conversions so
     * we get a single in->out conversion. NOTE: the Encoder matrix (encgains)
     * and output are transposed, so the input channels line up with the rows
     * and the output channels line up with the columns.
     */
    mGains.fill({});
    for(ALsizei i{0};i < 4;i++)
    {
        for(ALsizei j{0};j < device->Dry.NumChannels;j++)
        {
            ALdouble gain{0.0};
            for(size_t k{0u};k < COUNTOF(Ambi3DDecoder);k++)
                gain += (ALdouble)Ambi3DDecoder[k][i] * encgains[k][j];
            mGains[i][HF_BAND][j] = (ALfloat)(gain * Ambi3DDecoderHFScale[i]);
            mGains[i][LF_BAND][j] = (ALfloat)gain;
        }
    }
}

void AmbiUpsampler::process(ALfloat (*RESTRICT OutBuffer)[BUFFERSIZE], const ALsizei OutChannels, const ALfloat (*RESTRICT InSamples)[BUFFERSIZE], const ALsizei SamplesToDo)
{
    for(ALsizei i{0};i < 4;i++)
    {
        mXOver[i].process(mSamples[HF_BAND], mSamples[LF_BAND], InSamples[i], SamplesToDo);

        MixSamples(mSamples[HF_BAND], OutChannels, OutBuffer, mGains[i][HF_BAND].data(),
            mGains[i][HF_BAND].data(), 0, 0, SamplesToDo);
        MixSamples(mSamples[LF_BAND], OutChannels, OutBuffer, mGains[i][LF_BAND].data(),
            mGains[i][LF_BAND].data(), 0, 0, SamplesToDo);
    }
}
