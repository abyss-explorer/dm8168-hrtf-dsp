/*
 * ======== fft_test.c ========
 * PC-side regression test for the FFT engine in dsp/fft_conv.c.
 *
 * This file is a standalone copy of the FFT engine (fft_engine_init,
 * fft_compute, hrir_time_to_freq) that can be compiled with standard
 * GCC on a host machine — no DSP/xdc dependencies.
 *
 * Build:  gcc -o fft_test fft_test.c -lm
 * Run:    ./fft_test
 *
 * Tests:
 *   1. FFT/IFFT round-trip consistency
 *   2. Unit impulse HRIR convolution (identity filter)
 *   3. Real HRIR data validation (DC gain check)
 *
 * All tests should pass with near-zero error if the butterfly index
 * fix (x[i] -> x[i+k]) is applied correctly.
 */

#include <stdio.h>
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- Constants (must match dsp/fft_conv.h) ---- */
#define FFT_N           1024
#define FFT_LOG2N       10
#define CONV_INPUT_LEN  512
#define CONV_HISTORY_LEN 512
#define HRIR_LEN        256

/* ---- Complex float type ---- */
typedef struct {
    float re;
    float im;
} cplx_t;

/* ================================================================== */
/*  FFT engine — exact copy of dsp/fft_conv.c (with butterfly fix)    */
/* ================================================================== */

static cplx_t twiddle[FFT_N];
static cplx_t twiddle_conj[FFT_N];
static unsigned short bitrev[FFT_N];
static int fft_initialized = 0;

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

void fft_engine_init(void)
{
    int k;

    if (fft_initialized)
        return;

    for (k = 0; k < FFT_N; k++) {
        double angle = -2.0 * M_PI * (double)k / (double)FFT_N;
        twiddle[k].re = (float)cos(angle);
        twiddle[k].im = (float)sin(angle);

        /* Conjugate: only flip imaginary part */
        twiddle_conj[k].re = twiddle[k].re;
        twiddle_conj[k].im = -twiddle[k].im;
    }

    for (k = 0; k < FFT_N; k++)
        bitrev[k] = bit_reverse((unsigned short)k, FFT_LOG2N);

    fft_initialized = 1;
}

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
        int tw_stride = FFT_N / (2 * step);

        for (i = 0; i < FFT_N; i += (step << 1)) {
            for (k = 0; k < half_size; k++) {
                int tw_idx = k * tw_stride;

                w = twd[tw_idx];

                /* a = index of first operand, j = index of second */
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

void hrir_time_to_freq(const float *hrir_time, cplx_t *hrir_freq)
{
    int i;

    for (i = 0; i < HRIR_LEN; i++) {
        hrir_freq[i].re = hrir_time[i];
        hrir_freq[i].im = 0.0f;
    }
    for (i = HRIR_LEN; i < FFT_N; i++) {
        hrir_freq[i].re = 0.0f;
        hrir_freq[i].im = 0.0f;
    }

    fft_compute(hrir_freq, 0);
}

/* ================================================================== */
/*  Test 1: FFT/IFFT round-trip consistency                           */
/* ================================================================== */
static int test_fft_roundtrip(void)
{
    cplx_t x[FFT_N];
    int i;
    float max_err;
    int pass = 1;

    printf("=== Test 1: FFT/IFFT round-trip ===\n");

    /* Fill with impulse: x[0]=1, rest=0. FFT should give all (1,0). */
    x[0].re = 1.0f; x[0].im = 0.0f;
    for (i = 1; i < FFT_N; i++) { x[i].re = 0.0f; x[i].im = 0.0f; }

    fft_engine_init();
    fft_compute(x, 0);  /* forward */

    max_err = 0;
    for (i = 0; i < FFT_N; i++) {
        float err = fabsf(x[i].re - 1.0f) + fabsf(x[i].im);
        if (err > max_err) max_err = err;
    }
    printf("  Forward FFT of impulse: max error = %e (expect ~0)\n", max_err);
    if (max_err > 1e-3f) { printf("  FAIL\n"); pass = 0; }

    fft_compute(x, 1);  /* inverse: should recover [1,0,0,...] */

    max_err = 0;
    for (i = 0; i < FFT_N; i++) {
        float expect = (i == 0) ? 1.0f : 0.0f;
        float err = fabsf(x[i].re - expect) + fabsf(x[i].im);
        if (err > max_err) max_err = err;
    }
    printf("  IFFT round-trip: max error = %e (expect ~0)\n", max_err);
    if (max_err > 1e-3f) { printf("  FAIL\n"); pass = 0; }

    /* Sine wave test */
    for (i = 0; i < FFT_N; i++) {
        x[i].re = (float)(100.0 * sin(2.0 * M_PI * 4.0 * i / FFT_N));
        x[i].im = 0.0f;
    }
    fft_compute(x, 0);

    /* Peaks at bin 4 and bin FFT_N-4=1020 */
    printf("  FFT of sine(k=4): X[4]=(%.2f, %.2f), X[1020]=(%.2f, %.2f)\n",
           x[4].re, x[4].im, x[1020].re, x[1020].im);
    printf("  Expected magnitude at bin 4: ~%.1f\n", 100.0 * FFT_N / 2);

    fft_compute(x, 1);
    max_err = 0;
    for (i = 0; i < FFT_N; i++) {
        float expect = (float)(100.0 * sin(2.0 * M_PI * 4.0 * i / FFT_N));
        float err = fabsf(x[i].re - expect);
        if (err > max_err) max_err = err;
    }
    printf("  IFFT of sine: max error = %e (expect ~0)\n", max_err);
    if (max_err > 1e-2f) { printf("  FAIL\n"); pass = 0; }

    if (pass) printf("  PASS\n");
    return pass;
}

/* ================================================================== */
/*  Test 2: Unit impulse HRIR (identity filter) convolution           */
/* ================================================================== */
static int test_impulse_convolution(void)
{
    cplx_t hrir_freq[FFT_N];
    cplx_t fft_buf[FFT_N], freq_L[FFT_N], time_L[FFT_N];
    float state[CONV_HISTORY_LEN];
    int i;
    short input[CONV_INPUT_LEN];
    float max_err;
    int pass = 1;

    printf("=== Test 2: Unit impulse HRIR (identity filter) ===\n");

    /* HRIR = unit impulse: [1, 0, 0, ...] -> freq domain all (1,0) */
    float impulse[HRIR_LEN];
    impulse[0] = 1.0f;
    for (i = 1; i < HRIR_LEN; i++) impulse[i] = 0.0f;
    hrir_time_to_freq(impulse, hrir_freq);

    max_err = 0;
    for (i = 0; i < FFT_N; i++) {
        float err = fabsf(hrir_freq[i].re - 1.0f) + fabsf(hrir_freq[i].im);
        if (err > max_err) max_err = err;
    }
    printf("  Impulse HRIR freq: max deviation from (1,0) = %e\n", max_err);
    if (max_err > 1e-3f) { printf("  FAIL\n"); pass = 0; }

    /* Input: ramp 0,1,2,...,511 */
    for (i = 0; i < CONV_INPUT_LEN; i++) input[i] = (short)i;
    memset(state, 0, sizeof(state));

    /* Build FFT block: [history(512 zeros) | new_input(512)] */
    for (i = 0; i < CONV_HISTORY_LEN; i++) {
        fft_buf[i].re = state[i]; fft_buf[i].im = 0.0f;
    }
    for (i = 0; i < CONV_INPUT_LEN; i++) {
        fft_buf[CONV_HISTORY_LEN + i].re = (float)input[i];
        fft_buf[CONV_HISTORY_LEN + i].im = 0.0f;
    }

    memcpy(freq_L, fft_buf, sizeof(cplx_t) * FFT_N);
    fft_compute(freq_L, 0);

    /* Multiply by identity HRIR (all 1+0j) */
    for (i = 0; i < FFT_N; i++) {
        float a_re = freq_L[i].re, a_im = freq_L[i].im;
        freq_L[i].re = a_re * hrir_freq[i].re - a_im * hrir_freq[i].im;
        freq_L[i].im = a_re * hrir_freq[i].im + a_im * hrir_freq[i].re;
    }

    memcpy(time_L, freq_L, sizeof(cplx_t) * FFT_N);
    fft_compute(time_L, 1);

    /* With identity filter, output should equal input */
    max_err = 0;
    for (i = 0; i < CONV_INPUT_LEN; i++) {
        float v = time_L[CONV_HISTORY_LEN + i].re;
        float err = fabsf(v - (float)input[i]);
        if (err > max_err) max_err = err;
    }
    printf("  Identity convolution output: max error = %e (expect ~0)\n", max_err);
    if (max_err > 1e-1f) { printf("  FAIL\n"); pass = 0; }

    if (pass) printf("  PASS\n");
    return pass;
}

/* ================================================================== */
/*  Test 3: Real HRIR data validation (DC gain check)                 */
/* ================================================================== */
static int test_real_hrir(void)
{
    int pass = 1;
    int i;
    float sum = 0;
    float hrir_time[HRIR_LEN];
    cplx_t hrir_freq[FFT_N];
    FILE *f;
    size_t n_read;

    printf("=== Test 3: Real HRIR data ===\n");

    f = fopen("hrir_table.bin", "rb");
    if (!f) {
        printf("  SKIP (cannot open hrir_table.bin — run from project root)\n");
        return 1;  /* not a failure, just can't test */
    }

    /* Skip 24-byte header, read az0 L ear HRIR */
    fseek(f, 24, SEEK_SET);
    n_read = fread(hrir_time, sizeof(float), HRIR_LEN, f);
    fclose(f);

    if (n_read != HRIR_LEN) {
        printf("  SKIP (could not read HRIR data)\n");
        return 1;
    }

    printf("  HRIR[0..4]: %.6f %.6f %.6f %.6f %.6f\n",
           hrir_time[0], hrir_time[1], hrir_time[2], hrir_time[3], hrir_time[4]);

    hrir_time_to_freq(hrir_time, hrir_freq);

    /* DC gain = sum of time-domain HRIR = freq[0].re */
    for (i = 0; i < HRIR_LEN; i++) sum += hrir_time[i];
    printf("  HRIR sum (DC gain): %.6f\n", sum);
    printf("  freq[0].re:         %.6f\n", hrir_freq[0].re);

    float err = fabsf(sum - hrir_freq[0].re);
    printf("  |sum - freq[0].re| = %e (expect ~0)\n", err);
    if (err > 1e-3f) { printf("  FAIL\n"); pass = 0; }

    /* Check max frequency magnitude is reasonable (not garbage) */
    float max_mag = 0;
    for (i = 0; i < FFT_N; i++) {
        float mag = sqrtf(hrir_freq[i].re * hrir_freq[i].re +
                          hrir_freq[i].im * hrir_freq[i].im);
        if (mag > max_mag) max_mag = mag;
    }
    printf("  HRIR max freq magnitude: %.6f\n", max_mag);
    /* For a 256-tap HRIR with values ~0.001, max mag should be < ~10 */
    if (max_mag > 1000.0f) {
        printf("  FAIL (magnitude too large, likely FFT bug)\n");
        pass = 0;
    }

    if (pass) printf("  PASS\n");
    return pass;
}

/* ================================================================== */
/*  Main                                                              */
/* ================================================================== */
int main(void)
{
    int results[3];
    int all_pass = 1;
    int n_tests = 0;

    printf("\n");
    printf("========================================\n");
    printf(" FFT Engine Regression Test\n");
    printf(" (FFT_N=%d, HRIR_LEN=%d)\n", FFT_N, HRIR_LEN);
    printf("========================================\n\n");

    results[0] = test_fft_roundtrip();
    printf("\n");
    results[1] = test_impulse_convolution();
    printf("\n");
    results[2] = test_real_hrir();
    printf("\n");

    for (int t = 0; t < 3; t++) {
        if (results[t]) n_tests++;
        else all_pass = 0;
    }

    printf("========================================\n");
    printf(" Results: %d/%d tests passed\n", n_tests, 3);
    if (all_pass)
        printf(" ALL TESTS PASSED\n");
    else
        printf(" SOME TESTS FAILED\n");
    printf("========================================\n");

    return all_pass ? 0 : 1;
}
