#ifndef FILTERS_BIQUAD_H
#define FILTERS_BIQUAD_H

#include <cmath>
#include <utility>

#include "AL/al.h"
#include "math_defs.h"


/* Filters implementation is based on the "Cookbook formulae for audio
 * EQ biquad filter coefficients" by Robert Bristow-Johnson
 * http://www.musicdsp.org/files/Audio-EQ-Cookbook.txt
 */
/* Implementation note: For the shelf filters, the specified gain is for the
 * reference frequency, which is the centerpoint of the transition band. This
 * better matches EFX filter design. To set the gain for the shelf itself, use
 * the square root of the desired linear gain (or halve the dB gain).
 */

enum class BiquadType {
    /** EFX-style low-pass filter, specifying a gain and reference frequency. */
    HighShelf,
    /** EFX-style high-pass filter, specifying a gain and reference frequency. */
    LowShelf,
    /** Peaking filter, specifying a gain and reference frequency. */
    Peaking,

    /** Low-pass cut-off filter, specifying a cut-off frequency. */
    LowPass,
    /** High-pass cut-off filter, specifying a cut-off frequency. */
    HighPass,
    /** Band-pass filter, specifying a center frequency. */
    BandPass,
};

class BiquadFilter {
    /* Last two delayed components for direct form II. */
    float z1{0.0f}, z2{0.0f};
    /* Transfer function coefficients "b" (numerator) */
    float b0{1.0f}, b1{0.0f}, b2{0.0f};
    /* Transfer function coefficients "a" (denominator; a0 is pre-applied). */
    float a1{0.0f}, a2{0.0f};

public:
    void clear() noexcept { z1 = z2 = 0.0f; }

    /**
     * Sets the filter state for the specified filter type and its parameters.
     *
     * \param type The type of filter to apply.
     * \param gain The gain for the reference frequency response. Only used by
     *             the Shelf and Peaking filter types.
     * \param f0norm The reference frequency normal (ref_freq / sample_rate).
     *               This is the center point for the Shelf, Peaking, and
     *               BandPass filter types, or the cutoff frequency for the
     *               LowPass and HighPass filter types.
     * \param rcpQ The reciprocal of the Q coefficient for the filter's
     *             transition band. Can be generated from calc_rcpQ_from_slope
     *             or calc_rcpQ_from_bandwidth as needed.
     */
    void setParams(BiquadType type, float gain, float f0norm, float rcpQ);

    void copyParamsFrom(const BiquadFilter &other)
    {
        b0 = other.b0;
        b1 = other.b1;
        b2 = other.b2;
        a1 = other.a1;
        a2 = other.a2;
    }


    void process(float *dst, const float *src, int numsamples);

    void passthru(int numsamples) noexcept
    {
        if(LIKELY(numsamples >= 2))
        {
            z1 = 0.0f;
            z2 = 0.0f;
        }
        else if(numsamples == 1)
        {
            z1 = z2;
            z2 = 0.0f;
        }
    }

    /* Rather hacky. It's just here to support "manual" processing. */
    std::pair<float,float> getComponents() const noexcept
    { return {z1, z2}; }
    void setComponents(float z1_, float z2_) noexcept
    { z1 = z1_; z2 = z2_; }
    float processOne(const float in, float &z1_, float &z2_) const noexcept
    {
        float out{in*b0 + z1_};
        z1_ = in*b1 - out*a1 + z2_;
        z2_ = in*b2 - out*a2;
        return out;
    }
};

/**
 * Calculates the rcpQ (i.e. 1/Q) coefficient for shelving filters, using the
 * reference gain and shelf slope parameter.
 * \param gain 0 < gain
 * \param slope 0 < slope <= 1
 */
inline float calc_rcpQ_from_slope(float gain, float slope)
{
    return std::sqrt((gain + 1.0f/gain)*(1.0f/slope - 1.0f) + 2.0f);
}
/**
 * Calculates the rcpQ (i.e. 1/Q) coefficient for filters, using the normalized
 * reference frequency and bandwidth.
 * \param f0norm 0 < f0norm < 0.5.
 * \param bandwidth 0 < bandwidth
 */
inline ALfloat calc_rcpQ_from_bandwidth(float f0norm, float bandwidth)
{
    float w0 = F_TAU * f0norm;
    return 2.0f*std::sinh(std::log(2.0f)/2.0f*bandwidth*w0/std::sin(w0));
}

#endif /* FILTERS_BIQUAD_H */
