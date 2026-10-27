/*
 * ======== fft_conv.c ========
 * C674x optimized FFT engine and overlap-save partitioned convolution
 * for HRTF spatial audio processing on DM8168 DSP.
 *
 * Implementation notes:
 *   - Radix-2 DIT FFT with bit-reversal permutation
 *   - Twiddle factors pre-computed in fft_engine_init()
 *   - Overlap-save method: input block = (FFT_N - HRIR_LEN + 1) new samples
 *     padded with (HRIR_LEN - 1) history samples
 *   - For DM8168 C674x @ 1GHz, 1024-pt complex FFT ~ 15k cycles,
 *     total per-frame cost ~ 45k cycles << 10.67ms real-time budget
 */

#include <xdc/std.h>
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "fft_conv.h"

/* ------------------------------------------------------------------ */
/*  Pre-computed tables (in DSP internal memory)                      */
/* ------------------------------------------------------------------ */

/* Twiddle factors: W_N^k = cos(-2*pi*k/N) + j*sin(-2*pi*k/N) */
static cplx_t twiddle[FFT_N];         /* twiddle factors (forward) */
static cplx_t twiddle_conj[FFT_N];   /* conjugate twiddle factors (inverse) */

/* Bit-reversal lookup table */
static unsigned short bitrev[FFT_N];

/* Flag: tables initialized */
static int fft_initialized = 0;

/* ------------------------------------------------------------------ */
/*  Utility: compute bit-reversal index                               */
/* ------------------------------------------------------------------ */
static unsigned short bit_reverse(unsigned short x, int bits)
{
    unsigned short r = 0;
    int i;
    for (i = 0; i < bits; i++) {
        r = (r << 1) | (x & 1);
        x >>= 1;
    }
    return r;
}

/* ------------------------------------------------------------------ */
/*  fft_engine_init: pre-compute twiddle factors and bit-rev table    */
/* ------------------------------------------------------------------ */
void fft_engine_init(void)
{
    int k;

    if (fft_initialized)
        return;

    /* Compute twiddle factors W_N^k = e^{-j*2*pi*k/N} */
    for (k = 0; k < FFT_N; k++) {
        double angle = -2.0 * M_PI * (double)k / (double)FFT_N;
        twiddle[k].re = (float)cos(angle);
        twiddle[k].im = (float)sin(angle);

        /* Conjugate for inverse FFT: W_N^{-k} = e^{+j*2*pi*k/N}
         * Conjugate of (a+jb) is (a-jb), so only flip the imaginary part */
        twiddle_conj[k].re = twiddle[k].re;
        twiddle_conj[k].im = -twiddle[k].im;
    }

    /* Build bit-reversal permutation table */
    for (k = 0; k < FFT_N; k++) {
        bitrev[k] = bit_reverse((unsigned short)k, FFT_LOG2N);
    }

    fft_initialized = 1;
}

/* ------------------------------------------------------------------ */
/*  fft_compute: in-place radix-2 DIT complex FFT                     */
/*                                                                   */
/*  x       : cplx_t array of length FFT_N                            */
/*  inverse : 0=forward, 1=inverse (includes 1/N scaling)             */
/* ------------------------------------------------------------------ */
void fft_compute(cplx_t *x, int inverse)
{
    int i, j, k, a;
    int half_size, step;
    cplx_t w, temp;
    cplx_t *twd;

    if (!fft_initialized)
        fft_engine_init();

    twd = inverse ? twiddle_conj : twiddle;

    /* Bit-reversal permutation */
    for (i = 0; i < FFT_N; i++) {
        j = bitrev[i];
        if (j > i) {
            temp = x[i];
            x[i] = x[j];
            x[j] = temp;
        }
    }

    /* DIT butterfly stages: step = 1, 2, 4, ..., N/2 */
    for (step = 1; step < FFT_N; step <<= 1) {
        half_size = step;

        /* Twiddle stride: for stage with butterfly half-size = step,
         * twiddle index increment = N / (2*step) = N >> (log2(2*step))
         * i.e. tw_idx = k * (FFT_N / (2*step)) = k * (FFT_N >> 1) / step */
        int tw_stride = FFT_N / (2 * step);

        /* For each butterfly group */
        for (i = 0; i < FFT_N; i += (step << 1)) {

            /* Butterflies within group */
            for (k = 0; k < half_size; k++) {
                int tw_idx = k * tw_stride;

                w = twd[tw_idx];

                /* a = index of first operand, j = index of second operand */
                a = i + k;
                j = a + half_size;

                /* temp = w * x[j] */
                temp.re = w.re * x[j].re - w.im * x[j].im;
                temp.im = w.re * x[j].im + w.im * x[j].re;

                /* x[j] = x[a] - temp */
                x[j].re = x[a].re - temp.re;
                x[j].im = x[a].im - temp.im;

                /* x[a] = x[a] + temp */
                x[a].re = x[a].re + temp.re;
                x[a].im = x[a].im + temp.im;
            }
        }
    }

    /* Inverse FFT: scale by 1/N */
    if (inverse) {
        float inv_N = 1.0f / (float)FFT_N;
        for (i = 0; i < FFT_N; i++) {
            x[i].re *= inv_N;
            x[i].im *= inv_N;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  hrir_time_to_freq: convert time-domain HRIR to frequency domain   */
/* ------------------------------------------------------------------ */
void hrir_time_to_freq(const float *hrir_time, cplx_t *hrir_freq)
{
    int i;

    /* Zero-pad HRIR to FFT_N and place in frequency buffer */
    for (i = 0; i < HRIR_LEN; i++) {
        hrir_freq[i].re = hrir_time[i];
        hrir_freq[i].im = 0.0f;
    }
    for (i = HRIR_LEN; i < FFT_N; i++) {
        hrir_freq[i].re = 0.0f;
        hrir_freq[i].im = 0.0f;
    }

    /* Forward FFT -> frequency domain HRIR */
    fft_compute(hrir_freq, 0);
}

/* ------------------------------------------------------------------ */
/*  overlap_save_convolve: one frame of partitioned convolution       */
/*                                                                   */
/*  Overlap-save method:                                              */
/*    - History buffer holds last (FFT_N - CONV_INPUT_LEN) = 512      */
/*      samples from previous frame                                   */
/*    - Build FFT block: [history(512) | new_input(512)] = 1024       */
/*    - FFT the block                                                 */
/*    - Multiply by HRIR frequency domain                             */
/*    - IFFT the result                                                */
/*    - Valid output = last CONV_INPUT_LEN samples                     */
/*    - Update history = last 512 samples of the block                */
/* ------------------------------------------------------------------ */

void overlap_save_convolve(
    const Int16 *input_mono,
    const cplx_t *hrir_freq_L,
    const cplx_t *hrir_freq_R,
    Int16 *output_stereo,
    float *state)
{
    static cplx_t fft_buf[FFT_N];   /* FFT input/output buffer */
    static cplx_t freq_L[FFT_N];    /* frequency domain: left ear */
    static cplx_t freq_R[FFT_N];    /* frequency domain: right ear */
    static cplx_t time_L[FFT_N];    /* time domain: left ear result */
    static cplx_t time_R[FFT_N];    /* time domain: right ear result */
    int i;

    /* Step 1: Build FFT input block = [history | new_input] */
    /* Copy history (CONV_HISTORY_LEN = FFT_N - CONV_INPUT_LEN = 512 samples) */
    for (i = 0; i < CONV_HISTORY_LEN; i++) {
        fft_buf[i].re = state[i];
        fft_buf[i].im = 0.0f;
    }
    /* Append new input samples (CONV_INPUT_LEN = 512) */
    /* 512 (history) + 512 (new input) = 1024 = FFT_N, fills the block exactly */
    for (i = 0; i < CONV_INPUT_LEN; i++) {
        fft_buf[CONV_HISTORY_LEN + i].re = (float)input_mono[i];
        fft_buf[CONV_HISTORY_LEN + i].im = 0.0f;
    }

    /* Step 2: Forward FFT of input block */
    /* fft_buf is aliased by fft_compute, we need a copy for both L and R */
    memcpy(freq_L, fft_buf, sizeof(cplx_t) * FFT_N);
    fft_compute(freq_L, 0);

    /* freq_R = same frequency data (copy) */
    memcpy(freq_R, freq_L, sizeof(cplx_t) * FFT_N);

    /* Step 3: Frequency-domain multiplication */
    /* freq_L *= hrir_freq_L, freq_R *= hrir_freq_R */
    for (i = 0; i < FFT_N; i++) {
        float a_re = freq_L[i].re;
        float a_im = freq_L[i].im;
        float b_re = hrir_freq_L[i].re;
        float b_im = hrir_freq_L[i].im;

        freq_L[i].re = a_re * b_re - a_im * b_im;
        freq_L[i].im = a_re * b_im + a_im * b_re;

        a_re = freq_R[i].re;
        a_im = freq_R[i].im;
        b_re = hrir_freq_R[i].re;
        b_im = hrir_freq_R[i].im;

        freq_R[i].re = a_re * b_re - a_im * b_im;
        freq_R[i].im = a_re * b_im + a_im * b_re;
    }

    /* Step 4: Inverse FFT to get time-domain results */
    memcpy(time_L, freq_L, sizeof(cplx_t) * FFT_N);
    fft_compute(time_L, 1);

    memcpy(time_R, freq_R, sizeof(cplx_t) * FFT_N);
    fft_compute(time_R, 1);

    /* Step 5: Extract valid output (last CONV_INPUT_LEN samples) */
    /* Valid output starts at index HRIR_LEN-1 (255), we take indices */
    /* [CONV_HISTORY_LEN .. FFT_N-1] = [512..1023] = 512 samples */
    for (i = 0; i < CONV_INPUT_LEN; i++) {
        float vL = time_L[CONV_HISTORY_LEN + i].re;
        float vR = time_R[CONV_HISTORY_LEN + i].re;

        /* Clamp to Int16 range */
        if (vL > 32767.0f) vL = 32767.0f;
        if (vL < -32768.0f) vL = -32768.0f;
        if (vR > 32767.0f) vR = 32767.0f;
        if (vR < -32768.0f) vR = -32768.0f;

        /* Interleaved stereo output: L, R, L, R, ... */
        output_stereo[2 * i]     = (Int16)vL;
        output_stereo[2 * i + 1] = (Int16)vR;
    }

    /* Step 6: Update history buffer for next frame */
    /* History = last CONV_HISTORY_LEN samples of the input block */
    for (i = 0; i < CONV_HISTORY_LEN; i++) {
        state[i] = fft_buf[CONV_INPUT_LEN + i].re;
    }
}
