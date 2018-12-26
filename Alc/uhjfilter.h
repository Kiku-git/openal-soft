#ifndef UHJFILTER_H
#define UHJFILTER_H

#include "AL/al.h"

#include "alMain.h"
#include "almalloc.h"


struct AllPassState {
    ALfloat z[2]{0.0f, 0.0f};
};

/* Encoding 2-channel UHJ from B-Format is done as:
 *
 * S = 0.9396926*W + 0.1855740*X
 * D = j(-0.3420201*W + 0.5098604*X) + 0.6554516*Y
 *
 * Left = (S + D)/2.0
 * Right = (S - D)/2.0
 *
 * where j is a wide-band +90 degree phase shift.
 *
 * The phase shift is done using a Hilbert transform, described here:
 * https://web.archive.org/web/20060708031958/http://www.biochem.oulu.fi/~oniemita/dsp/hilbert/
 * It works using 2 sets of 4 chained filters. The first filter chain produces
 * a phase shift of varying magnitude over a wide range of frequencies, while
 * the second filter chain produces a phase shift 90 degrees ahead of the
 * first over the same range.
 *
 * Combining these two stages requires the use of three filter chains. S-
 * channel output uses a Filter1 chain on the W and X channel mix, while the D-
 * channel output uses a Filter1 chain on the Y channel plus a Filter2 chain on
 * the W and X channel mix. This results in the W and X input mix on the D-
 * channel output having the required +90 degree phase shift relative to the
 * other inputs.
 */

struct Uhj2Encoder {
    AllPassState mFilter1_Y[4];
    AllPassState mFilter2_WX[4];
    AllPassState mFilter1_WX[4];
    ALfloat mLastY{0.0f}, mLastWX{0.0f};

    /* Encodes a 2-channel UHJ (stereo-compatible) signal from a B-Format input
     * signal. The input must use FuMa channel ordering and scaling.
     */
    void encode(ALfloat *LeftOut, ALfloat *RightOut, ALfloat (*InSamples)[BUFFERSIZE], const ALsizei SamplesToDo);

    DEF_NEWDEL(Uhj2Encoder)
};

#endif /* UHJFILTER_H */
