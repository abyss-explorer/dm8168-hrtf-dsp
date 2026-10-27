/*
 * ======== sofa_loader.c ========
 * Implementation of HRIR table loader and circular trajectory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#include <ti/syslink/Std.h>
#include <ti/ipc/SharedRegion.h>

#include "../shared/AppCommon.h"
#include "sofa_loader.h"

/*
 * Load HRIR table from binary file into SharedRegion.
 *
 * The binary file format (from sofa_to_bin.py):
 *   Header:  "HRIR"(4) + version(4) + num_az(4) + hrir_len(4) + num_ears(4) + sr(4) = 24 bytes
 *   Data:    num_azimuths * 2 * hrir_length * sizeof(float32)
 *
 * We load only the float data into SharedRegion at offset SR1_HRIR_TABLE_OFFSET,
 * and return num_azimuths.
 */
Int sofa_load_hrir_table(const char *bin_path,
                         IHeap_Handle heap,
                         Char *bufferBase)
{
    int fd;
    HrirHeader header;
    ssize_t n_read;
    size_t data_size;
    Char *hrir_dest;
    Int num_az;

    printf("sofa_load_hrir_table: opening %s\n", bin_path);

    fd = open(bin_path, O_RDONLY);
    if (fd < 0) {
        perror("sofa_load_hrir_table: Failed to open HRIR bin file");
        return -1;
    }

    /* Read header */
    n_read = read(fd, &header, sizeof(HrirHeader));
    if (n_read != (ssize_t)sizeof(HrirHeader)) {
        perror("sofa_load_hrir_table: Failed to read header");
        close(fd);
        return -1;
    }

    /* Validate header */
    if (memcmp(header.magic, "HRIR", 4) != 0) {
        printf("sofa_load_hrir_table: Invalid magic (expected 'HRIR')\n");
        close(fd);
        return -1;
    }

    num_az = (Int)header.num_azimuths;
    printf("sofa_load_hrir_table: version=%u, num_azimuths=%d, hrir_len=%u, ears=%u, sr=%u\n",
           (unsigned)header.version,
           num_az,
           (unsigned)header.hrir_length,
           (unsigned)header.num_ears,
           (unsigned)header.sample_rate);

    if (num_az <= 0 || num_az > MAX_AZIMUTHS) {
        printf("sofa_load_hrir_table: Invalid num_azimuths %d (max %d)\n",
               num_az, MAX_AZIMUTHS);
        close(fd);
        return -1;
    }

    if ((Int)header.hrir_length != HRIR_LENGTH) {
        printf("sofa_load_hrir_table: Warning: hrir_length=%u != expected %d, using file value\n",
               (unsigned)header.hrir_length, HRIR_LENGTH);
    }

    /* Calculate data size */
    data_size = (size_t)num_az * 2 * HRIR_LENGTH * sizeof(float);

    /* Destination in SharedRegion buffer (after audio IO area) */
    hrir_dest = bufferBase + SR1_HRIR_TABLE_OFFSET;

    printf("sofa_load_hrir_table: loading %zu bytes of HRIR data to offset 0x%x\n",
           data_size, SR1_HRIR_TABLE_OFFSET);

    /* Read HRIR float data directly into SharedRegion */
    {
        size_t total_read = 0;
        while (total_read < data_size) {
            n_read = read(fd, hrir_dest + total_read, data_size - total_read);
            if (n_read < 0) {
                if (errno == EINTR) continue;
                perror("sofa_load_hrir_table: read error");
                close(fd);
                return -1;
            }
            if (n_read == 0) {
                printf("sofa_load_hrir_table: Unexpected EOF at %zu/%zu bytes\n",
                       total_read, data_size);
                break;
            }
            total_read += n_read;
        }
    }

    close(fd);
    printf("sofa_load_hrir_table: loaded %d azimuths successfully\n", num_az);
    return num_az;
}


/*
 * Trajectory functions for circular spatial audio movement.
 *
 * The azimuth angle advances linearly each frame, making the sound source
 * appear to rotate around the listener's head in a horizontal circle.
 */

void trajectory_init(TrajectoryCtx *ctx, Int num_azimuths,
                     Int total_frames, float revolutions)
{
    ctx->current_frame  = 0;
    ctx->total_frames   = total_frames;
    ctx->num_azimuths   = num_azimuths;
    ctx->current_azimuth = 0;
    ctx->current_angle  = 0.0f;

    /* Compute angular velocity: degrees per frame */
    if (total_frames > 0 && revolutions > 0.0f) {
        ctx->deg_per_frame = (revolutions * 360.0f) / (float)total_frames;
    } else {
        ctx->deg_per_frame = 0.0f;
    }

    printf("trajectory_init: num_az=%d, total_frames=%d, revolutions=%.2f, deg/frame=%.4f\n",
           num_azimuths, total_frames, revolutions, ctx->deg_per_frame);
}

Int trajectory_angle_to_index(TrajectoryCtx *ctx, float angle_deg)
{
    /* Normalize angle to [0, 360) */
    while (angle_deg < 0.0f) angle_deg += 360.0f;
    while (angle_deg >= 360.0f) angle_deg -= 360.0f;

    /* Map angle to azimuth index: assume azimuths are uniformly spaced */
    if (ctx->num_azimuths <= 0)
        return 0;

    /* Compute index: angle/360 * num_azimuths, rounded to nearest */
    float normalized = angle_deg / 360.0f;
    Int idx = (Int)(normalized * (float)ctx->num_azimuths + 0.5f);

    /* Wrap around */
    if (idx >= ctx->num_azimuths)
        idx = ctx->num_azimuths - 1;
    if (idx < 0)
        idx = 0;

    return idx;
}

Int trajectory_step(TrajectoryCtx *ctx)
{
    /* Advance angle */
    ctx->current_angle += ctx->deg_per_frame;

    /* Wrap to [0, 360) */
    while (ctx->current_angle >= 360.0f)
        ctx->current_angle -= 360.0f;
    while (ctx->current_angle < 0.0f)
        ctx->current_angle += 360.0f;

    /* Convert angle to nearest azimuth index */
    ctx->current_azimuth = trajectory_angle_to_index(ctx, ctx->current_angle);

    ctx->current_frame++;

    return ctx->current_azimuth;
}
