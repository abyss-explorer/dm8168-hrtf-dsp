/*
 * ======== fft_conv.h ========
 * C674x optimized FFT engine and overlap-save partitioned convolution
 * for HRTF spatial audio processing on DM8168 DSP.
 *
 * Features:
 *   - Radix-2 DIT complex FFT (1024-point)
 *   - Pre-computed twiddle factor table
 *   - Overlap-save partitioned convolution for low-latency HRTF filtering
 *   - Dual-channel (left/right ear) simultaneous processing
 *   - Runtime azimuth switching with pre-computed frequency-domain HRIR
 */

#ifndef FFT_CONV_H_
#define FFT_CONV_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <xdc/std.h>

/*
 * FFT and convolution constants
 * (must match shared/AppCommon.h)
 */
#define FFT_N           1024    /* FFT size */
#define FFT_LOG2N       10      /* log2(1024) */
#define CONV_INPUT_LEN  512     /* new input samples per frame */
#define CONV_HISTORY_LEN 512    /* FFT_N - CONV_INPUT_LEN (overlap region) */
#define HRIR_LEN        256     /* HRIR sample count per ear */

/* Complex float structure for frequency-domain data */
typedef struct {
    float re;
    float im;
} cplx_t;

/*
 * Initialize FFT engine:
 *   - Pre-computes twiddle factor table
 *   - Pre-computes bit-reversal permutation table
 *   Must be called once before any convolution.
 */
void fft_engine_init(void);

/*
 * Compute in-place complex FFT (radix-2 DIT).
 *   x       : input/output array of cplx_t, length = FFT_N
 *   inverse : 0 = forward FFT, 1 = inverse FFT (with 1/N scaling)
 */
void fft_compute(cplx_t *x, int inverse);

/*
 * Perform overlap-save partitioned convolution for one frame.
 *
 *   input_mono : CONV_INPUT_LEN mono Int16 samples (from audio file)
 *   hrir_freq_L: frequency-domain HRIR for left ear (FFT_N cplx_t values)
 *   hrir_freq_R: frequency-domain HRIR for right ear (FFT_N cplx_t values)
 *   output_stereo : output buffer, CONV_INPUT_LEN*2 Int16 (interleaved L/R)
 *   state         : persistent state buffer (CONV_HISTORY_LEN floats)
 *                   Must persist between calls. Initialize to zero.
 *
 * Processing:
 *   1. Append CONV_INPUT_LEN new samples to history buffer
 *   2. FFT the combined (history + new) buffer -> frequency domain
 *   3. Multiply by hrir_freq_L and hrir_freq_R
 *   4. IFFT both results
 *   5. Take last CONV_INPUT_LEN samples as valid output
 *   6. Update history buffer for next frame
 */
void overlap_save_convolve(
    const Int16 *input_mono,
    const cplx_t *hrir_freq_L,
    const cplx_t *hrir_freq_R,
    Int16 *output_stereo,
    float *state
);

/*
 * Convert time-domain HRIR (float samples) to frequency-domain representation.
 *   hrir_time : HRIR_LEN float samples (zero-padded to FFT_N internally)
 *   hrir_freq : output, FFT_N cplx_t values (frequency domain)
 *
 * Used during HRIR table pre-computation.
 */
void hrir_time_to_freq(const float *hrir_time, cplx_t *hrir_freq);

#ifdef __cplusplus
}
#endif

#endif /* FFT_CONV_H_ */
