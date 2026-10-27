/*
 * Copyright (c) 2012, Texas Instruments Incorporated
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * *  Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * *  Neither the name of Texas Instruments Incorporated nor the names of
 *    its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 *  ======== AppCommon.h ========
 *
 */

#ifndef AppCommon__include
#define AppCommon__include
#if defined (__cplusplus)
extern "C" {
#endif


/*
 *  ======== Application Configuration ========
 */

/* notify commands 00 - FF */
#define APP_CMD_MASK            0xFF000000

/* used by host and remote device to synchronize states with each other*/
#define APP_CMD_NOP             0x10000000  /* cc------ */
#define APP_CMD_OP_COMPLETE     0x11000000  /* cc------ */
#define APP_SPTR_MASK           0x0000FFFF  /* ----pppp */
#define APP_SPTR_HADDR          0x20000000  /* cc--hhhh */
#define APP_SPTR_LADDR          0x21000000  /* cc--llll */
#define APP_SPTR_ADDR_ACK       0x22000000  /* cc------ */
#define APP_CMD_SHUTDOWN_ACK    0x30000000  /* cc------ */
#define APP_CMD_SHUTDOWN        0x31000000  /* cc------ */
#define APP_CMD_PROCESSING      0x50000000  /* cc-aa-ss : aa=azimuth_idx, ss=buf_size */

/* HRTF extension commands */
#define APP_CMD_LOAD_HRIR       0x60000000  /* cc------ : signal HRIR table ready */
#define APP_CMD_HRIR_LOADED_ACK 0x61000000  /* cc------ : DSP acks HRIR table load */
#define APP_CMD_SET_AZIMUTH     0x70000000  /* cc--aa-- : set current azimuth index */

/* azimuth index in PROCESSING payload: bits [23:16] */
#define APP_AZIMUTH_SHIFT       16
#define APP_AZIMUTH_MASK        0x00FF0000

/* program's shared region number */
#define SHARED_REGION_1         0x1

/*
 * Audio frame size in bytes.
 * CONV_BLOCK_SIZE=512 mono samples per frame (Int16) = 1024 bytes input.
 * Output: 512 stereo frames x 4 bytes = 2048 bytes output.
 * Total shared audio buffer must hold:
 *   - Audio IO: input(1024) + output(2048) = 3072 bytes (at offset 0x0000)
 *   - HRIR table: MAX_AZIMUTHS x 2 x HRIR_LENGTH x 4 bytes (at offset 0x2000)
 *   104 azimuths x 2 x 256 x 4 = 213,504 bytes; with max 710 = ~365KB
 * Use 512KB to leave ample room.
 */
#define CONV_BLOCK_SIZE         512     /* samples per convolution frame */
#define AUDIO_INPUT_SIZE        1024    /* CONV_BLOCK_SIZE x 2 bytes (Int16) */
#define AUDIO_OUTPUT_SIZE       2048    /* CONV_BLOCK_SIZE x 2ch x 2 bytes */
#define BUFFER_SIZE             0x80000 /* 512KB: audio IO + HRIR table */

/* SharedRegion #1 layout offsets (SR1 is ~15MB starting at 0x8E010000) */
#define SR1_AUDIO_BUF_OFFSET    0x0000  /* audio IO buffer: 4KB */
#define SR1_HRIR_TABLE_OFFSET   0x2000  /* HRIR time-domain table start */
/* HRIR table size = NUM_AZIMUTHS x 2 ears x HRIR_LENGTH x 4(float32) */

/* HRTF / convolution parameters */
#define HRIR_LENGTH             256     /* samples per HRIR (truncated) */
#define HRIR_FFT_SIZE           1024    /* FFT block size */
#define MAX_AZIMUTHS            710     /* max azimuth angles in table */

/* Default input/output file paths */
#define AUDIO_IN_PATH           "input_audio.pcm"
#define AUDIO_OUT_PATH          "output_processed.pcm"
#define HRIR_TABLE_PATH         "hrir_table.bin"

/* Legacy FIFO path (kept for backward compatibility) */
#define ALSA_FIFO_PATH          "/tmp/alsa_audio_in"

#define SAMPLE_RATE             48000
#define CHANNELS                2

/* queue error message*/
#define APP_E_FAILURE           0xF0000000
#define APP_E_OVERFLOW          0xF1000000

#if defined (__cplusplus)
}
#endif /* defined (__cplusplus) */
#endif /* AppCommon__include */
