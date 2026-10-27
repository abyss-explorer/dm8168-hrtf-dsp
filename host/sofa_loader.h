/*
 * ======== sofa_loader.h ========
 * HRIR table loader: reads pre-processed hrir_table.bin from filesystem
 * and loads it into SharedRegion for DSP access.
 *
 * Also provides circular trajectory computation for dynamic azimuth switching.
 */

#ifndef SOFA_LOADER_H_
#define SOFA_LOADER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <ti/syslink/Std.h>
#include <ti/syslink/utils/Memory.h>
#include <ti/ipc/SharedRegion.h>

/*
 * HRIR binary file header structure (matches tools/sofa_to_bin.py output)
 */
typedef struct {
    char     magic[4];    /* "HRIR" */
    UInt32   version;     /* 1 */
    UInt32   num_azimuths;/* number of azimuth angles */
    UInt32   hrir_length; /* samples per HRIR */
    UInt32   num_ears;    /* 2 (L/R) */
    UInt32   sample_rate; /* sampling rate in Hz */
} HrirHeader;

/*
 * Trajectory context for circular spatial audio movement.
 * Azimuth advances each frame to create a circular sound source motion.
 */
typedef struct {
    Int     current_frame;     /* current frame index */
    Int     total_frames;      /* total frames to process */
    Int     num_azimuths;      /* number of available azimuths */
    Int     current_azimuth;   /* current azimuth index (0..num_azimuths-1) */
    /* Angular velocity: degrees per frame */
    float   deg_per_frame;
    /* Current azimuth angle in degrees (0-360) */
    float   current_angle;
} TrajectoryCtx;

/*
 * Load HRIR table from binary file into SharedRegion.
 *
 *   bin_path  : path to hrir_table.bin
 *   heap      : SharedRegion #1 heap handle
 *   bufferPtr : base of SharedRegion #1 buffer (audio buffer at offset 0)
 *
 * Returns:
 *   >0 : number of azimuths loaded
 *   -1 : error
 *
 * The HRIR table is placed at SR1_HRIR_TABLE_OFFSET within the shared buffer.
 */
Int sofa_load_hrir_table(const char *bin_path,
                         IHeap_Handle heap,
                         Char *bufferBase);

/*
 * Initialize circular trajectory context.
 *   ctx          : trajectory context to initialize
 *   num_azimuths : number of azimuths in HRIR table
 *   total_frames : total frames to process
 *   revolutions  : number of complete rotations over the entire file
 */
void trajectory_init(TrajectoryCtx *ctx, Int num_azimuths,
                     Int total_frames, float revolutions);

/*
 * Advance trajectory by one frame and return the new azimuth index.
 */
Int trajectory_step(TrajectoryCtx *ctx);

/*
 * Find nearest azimuth index for a given angle in degrees.
 */
Int trajectory_angle_to_index(TrajectoryCtx *ctx, float angle_deg);

#ifdef __cplusplus
}
#endif

#endif /* SOFA_LOADER_H_ */
