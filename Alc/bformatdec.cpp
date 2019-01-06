
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

using namespace std::placeholders;

#define HF_BAND 0
#define LF_BAND 1
static_assert(BFormatDec::sNumBands == 2, "Unexpected BFormatDec::sNumBands");

constexpr ALfloat Ambi3DDecoderHFScale[MAX_AMBI_ORDER+1] = {
    2.00000000f, 1.15470054f
};
constexpr ALfloat Ambi3DDecoderHFScale2O[MAX_AMBI_ORDER+1] = {
    1.49071198f, 1.15470054f
};
constexpr ALfloat Ambi3DDecoderHFScale3O[MAX_AMBI_ORDER+1] = {
    1.17958441f, 1.01578297f
};

inline auto GetDecoderHFScales(ALsizei order) noexcept -> const ALfloat(&)[MAX_AMBI_ORDER+1]
{
    if(order >= 3) return Ambi3DDecoderHFScale3O;
    if(order == 2) return Ambi3DDecoderHFScale2O;
    return Ambi3DDecoderHFScale;
}

inline auto GetAmbiScales(AmbDecScale scaletype) noexcept -> const std::array<float,MAX_AMBI_COEFFS>&
{
    if(scaletype == AmbDecScale::FuMa) return AmbiScale::FromFuMa;
    if(scaletype == AmbDecScale::SN3D) return AmbiScale::FromSN3D;
    return AmbiScale::FromN3D;
}

} // namespace


void BFormatDec::reset(const AmbDecConf *conf, bool allow_2band, ALsizei inchans, ALuint srate, const ALsizei (&chanmap)[MAX_OUTPUT_CHANNELS])
{
    mSamples.clear();
    mSamplesHF = nullptr;
    mSamplesLF = nullptr;

    mMatrix = MatrixU{};
    mDualBand = allow_2band && (conf->FreqBands == 2);
    if(!mDualBand)
        mSamples.resize(2);
    else
    {
        mSamples.resize(inchans * 2);
        mSamplesHF = mSamples.data();
        mSamplesLF = mSamplesHF + inchans;
    }
    mNumChannels = inchans;

    mEnabled = std::accumulate(std::begin(chanmap), std::begin(chanmap)+conf->Speakers.size(), 0u,
        [](ALuint mask, const ALsizei &chan) noexcept -> ALuint
        { return mask | (1 << chan); }
    );

    const ALfloat xover_norm{conf->XOverFreq / (float)srate};

    const ALsizei out_order{
        (conf->ChanMask > AMBI_3ORDER_MASK) ? 4 :
        (conf->ChanMask > AMBI_2ORDER_MASK) ? 3 :
        (conf->ChanMask > AMBI_1ORDER_MASK) ? 2 : 1};
    {
        const ALfloat (&hfscales)[MAX_AMBI_ORDER+1] = GetDecoderHFScales(out_order);

        mUpsampler[0].Splitter.init(xover_norm);
        mUpsampler[0].Gains[HF_BAND] = Ambi3DDecoderHFScale[0] / hfscales[0];
        mUpsampler[0].Gains[LF_BAND] = 1.0f;
        mUpsampler[1].Splitter.init(xover_norm);
        mUpsampler[1].Gains[HF_BAND] = Ambi3DDecoderHFScale[1] / hfscales[1];
        mUpsampler[1].Gains[LF_BAND] = 1.0f;
        std::fill(std::begin(mUpsampler)+2, std::end(mUpsampler), mUpsampler[1]);
    }

    const bool periphonic{(conf->ChanMask&AMBI_PERIPHONIC_MASK) != 0};
    const std::array<float,MAX_AMBI_COEFFS> &coeff_scale = GetAmbiScales(conf->CoeffScale);
    const ALsizei coeff_count{periphonic ? MAX_AMBI_COEFFS : MAX_AMBI2D_COEFFS};

    if(!mDualBand)
    {
        for(size_t i{0u};i < conf->Speakers.size();i++)
        {
            ALfloat (&mtx)[MAX_AMBI_COEFFS] = mMatrix.Single[chanmap[i]];
            for(ALsizei j{0},k{0};j < coeff_count;j++)
            {
                const ALsizei l{periphonic ? j : AmbiIndex::From2D[j]};
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
        mXOver[0].init(xover_norm);
        std::fill(std::begin(mXOver)+1, std::end(mXOver), mXOver[0]);

        const float ratio{std::pow(10.0f, conf->XOverRatio / 40.0f)};
        for(size_t i{0u};i < conf->Speakers.size();i++)
        {
            ALfloat (&mtx)[sNumBands][MAX_AMBI_COEFFS] = mMatrix.Dual[chanmap[i]];
            for(ALsizei j{0},k{0};j < coeff_count;j++)
            {
                const ALsizei l{periphonic ? j : AmbiIndex::From2D[j]};
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

void BFormatDec::reset(const ALsizei inchans, const ALfloat xover_norm, const ALsizei chancount, const ChannelDec (&chancoeffs)[MAX_OUTPUT_CHANNELS], const ALsizei (&chanmap)[MAX_OUTPUT_CHANNELS])
{
    mSamples.clear();
    mSamplesHF = nullptr;
    mSamplesLF = nullptr;

    mMatrix = MatrixU{};
    mDualBand = false;
    mSamples.resize(2);
    mNumChannels = inchans;

    mEnabled = std::accumulate(std::begin(chanmap), std::begin(chanmap)+chancount, 0u,
        [](ALuint mask, const ALsizei &chan) noexcept -> ALuint
        { return mask | (1 << chan); }
    );

    const ALsizei out_order{
        (inchans > 7) ? 4 :
        (inchans > 5) ? 3 :
        (inchans > 3) ? 2 : 1};
    {
        const ALfloat (&hfscales)[MAX_AMBI_ORDER+1] = GetDecoderHFScales(out_order);

        mUpsampler[0].Splitter.init(xover_norm);
        mUpsampler[0].Gains[HF_BAND] = Ambi3DDecoderHFScale[0] / hfscales[0];
        mUpsampler[0].Gains[LF_BAND] = 1.0f;
        mUpsampler[1].Splitter.init(xover_norm);
        mUpsampler[1].Gains[HF_BAND] = Ambi3DDecoderHFScale[1] / hfscales[1];
        mUpsampler[1].Gains[LF_BAND] = 1.0f;
        std::fill(std::begin(mUpsampler)+2, std::end(mUpsampler), mUpsampler[1]);
    }

    for(ALsizei i{0};i < chancount;i++)
    {
        const ALfloat (&coeffs)[MAX_AMBI_COEFFS] = chancoeffs[chanmap[i]];
        ALfloat (&mtx)[MAX_AMBI_COEFFS] = mMatrix.Single[chanmap[i]];

        std::copy_n(std::begin(coeffs), inchans, std::begin(mtx));
    }
}


void BFormatDec::process(ALfloat (*OutBuffer)[BUFFERSIZE], const ALsizei OutChannels, const ALfloat (*InSamples)[BUFFERSIZE], const ALsizei SamplesToDo)
{
    ASSUME(OutChannels > 0);
    ASSUME(mNumChannels > 0);

    if(mDualBand)
    {
        for(ALsizei i{0};i < mNumChannels;i++)
            mXOver[i].process(mSamplesHF[i].data(), mSamplesLF[i].data(), InSamples[i],
                              SamplesToDo);

        for(ALsizei chan{0};chan < OutChannels;chan++)
        {
            if(UNLIKELY(!(mEnabled&(1<<chan))))
                continue;

            MixRowSamples(OutBuffer[chan], mMatrix.Dual[chan][HF_BAND],
                &reinterpret_cast<ALfloat(&)[BUFFERSIZE]>(mSamplesHF[0]),
                mNumChannels, 0, SamplesToDo);
            MixRowSamples(OutBuffer[chan], mMatrix.Dual[chan][LF_BAND],
                &reinterpret_cast<ALfloat(&)[BUFFERSIZE]>(mSamplesLF[0]),
                mNumChannels, 0, SamplesToDo);
        }
    }
    else
    {
        for(ALsizei chan{0};chan < OutChannels;chan++)
        {
            if(UNLIKELY(!(mEnabled&(1<<chan))))
                continue;

            MixRowSamples(OutBuffer[chan], mMatrix.Single[chan], InSamples,
                          mNumChannels, 0, SamplesToDo);
        }
    }
}

void BFormatDec::upSample(ALfloat (*OutBuffer)[BUFFERSIZE], const ALfloat (*InSamples)[BUFFERSIZE], const ALsizei InChannels, const ALsizei SamplesToDo)
{
    ASSUME(InChannels > 0);

    /* This up-sampler leverages the differences observed in dual-band higher-
     * order decoder matrices compared to first-order. For the same output
     * channel configuration, the low-frequency matrix has identical
     * coefficients in the shared input channels, while the high-frequency
     * matrix has extra scalars applied to the W channel and X/Y/Z channels.
     * Mixing the first-order content into the higher-order stream, with the
     * appropriate counter-scales applied to the HF response, results in the
     * subsequent higher-order decode generating the same response as a first-
     * order decode.
     */
    for(ALsizei i{0};i < InChannels;i++)
    {
        mUpsampler[i].Splitter.process(mSamples[HF_BAND].data(), mSamples[LF_BAND].data(),
            InSamples[i], SamplesToDo);
        MixRowSamples(OutBuffer[i], mUpsampler[i].Gains,
            &reinterpret_cast<ALfloat(&)[BUFFERSIZE]>(mSamples[0]), sNumBands, 0, SamplesToDo);
    }
}


void AmbiUpsampler::reset(const ALsizei out_order, const ALfloat xover_norm)
{
    const ALfloat (&hfscales)[MAX_AMBI_ORDER+1] = GetDecoderHFScales(out_order);

    mInput[0].Splitter.init(xover_norm);
    mInput[0].Gains[HF_BAND] = Ambi3DDecoderHFScale[0] / hfscales[0];
    mInput[0].Gains[LF_BAND] = 1.0f;
    mInput[1].Splitter.init(xover_norm);
    mInput[1].Gains[HF_BAND] = Ambi3DDecoderHFScale[1] / hfscales[1];
    mInput[1].Gains[LF_BAND] = 1.0f;
    std::fill(std::begin(mInput)+2, std::end(mInput), mInput[1]);
}

void AmbiUpsampler::process(ALfloat (*OutBuffer)[BUFFERSIZE], const ALfloat (*InSamples)[BUFFERSIZE], const ALsizei InChannels, const ALsizei SamplesToDo)
{
    ASSUME(InChannels > 0);

    for(ALsizei i{0};i < InChannels;i++)
    {
        mInput[i].Splitter.process(mSamples[HF_BAND], mSamples[LF_BAND], InSamples[i],
            SamplesToDo);
        MixRowSamples(OutBuffer[i], mInput[i].Gains, mSamples, sNumBands, 0, SamplesToDo);
    }
}
