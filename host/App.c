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
 *  ======== App.c ========
 *
 */

/* host header files */
#include <stdio.h>
#include <unistd.h>
#include <semaphore.h>
#include <time.h>     /* by Guanqing 20210520  */
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <string.h>
#include <time.h>

/* package header files */
#include <ti/syslink/Std.h>     /* must be first */
#include <ti/syslink/utils/Memory.h>
#include <ti/ipc/Notify.h>
#include <ti/ipc/SharedRegion.h>
#include <sys/stat.h>
#include <errno.h>


/* local header files */
#include "../shared/AppCommon.h"
#include "../shared/SystemCfg.h"
#include "App.h"
#include "sofa_loader.h"

/* max number of outstanding commands minus one */
#define QUEUESIZE   8   

/* queue structure */
typedef struct 
{
    UInt32              queue[QUEUESIZE];   /* command queue */
    UInt                head;               /* queue head pointer */
    UInt                tail;               /* queue tail pointer */
    UInt32              error;              /* error flag */
    sem_t               semH;               /* handle to object above  */
} Event_Queue;   

/* module structure */
typedef struct {
    Event_Queue     eventQueue;
    UInt16          remoteProcId;
    UInt16          lineId;     /* notify line id */
    UInt32          eventId;    /* notify event id */
    Char*           bufferPtr;  /* string buffer */
} App_Module;

/* private functions */
static UInt32 App_waitForEvent(Event_Queue* eventQueue);
static Void App_notifyCB( UInt16 procId, UInt16 lineId, UInt32 eventId, 
        UArg arg, UInt32 payload);
static long long get_duration_us(struct timespec start, struct timespec end) {
    return (long long)(end.tv_sec - start.tv_sec) * 1000000LL + 
           (long long)(end.tv_nsec - start.tv_nsec) / 1000LL;
}

        
/* private data */
static App_Module Module;

/*
 *  ======== App_create ========
 *  1. create sync object
 *  2. register notify callback
 *  3. wait until remote core has also register notify callback
 *  4. create buffer from shared region 1 heap
 */

Int App_create(UInt16 remoteProcId)
{
    Int status;
    int retStatus;
    IHeap_Handle heap;
    
    printf("--> App_create:\n");
    
    /* setting default values */
    Module.eventQueue.head      = 0;               
    Module.eventQueue.tail      = 0;              
    Module.eventQueue.error     = 0;       
    Module.lineId               = SystemCfg_LineId;
    Module.eventId              = SystemCfg_EventId;
    Module.remoteProcId         = remoteProcId;
    
    /* 1. create sync object */
    retStatus = sem_init(&Module.eventQueue.semH, 0, 0);
    if(retStatus == -1) {
        printf("App_create: Could not initialize a semaphore\n");
        status = -1;
        goto leave;
    }

    /* 2. register notify callback */
    status = Notify_registerEvent(Module.remoteProcId, Module.lineId, 
            Module.eventId, App_notifyCB, (UArg) &Module.eventQueue);

    if (status < 0) {
        printf("App_create: Host failed to register event\n");
        goto leave;
    }

    /* 3. wait until remote core has also registered notify callback */
    do {
        status = Notify_sendEvent(Module.remoteProcId, Module.lineId,
                Module.eventId,APP_CMD_NOP, TRUE);

        if (status == Notify_E_EVTNOTREGISTERED) {
            usleep(100);
        }
        
    } while (status == Notify_E_EVTNOTREGISTERED);

    if (status != Notify_S_SUCCESS) {
        printf("App_create: Waiting for remote core to register has failed\n");
        status = -1;
        goto leave;
    }

    /* get handle to shared region 1 heap */
    heap = (IHeap_Handle) SharedRegion_getHeap(SHARED_REGION_1);

    if (heap == NULL) {
        status = -1;
        printf("App_create: Shared region heap does not exist\n");
        goto leave;
    }

    /* 4. create buffer from shared region 1 heap */
    Module.bufferPtr = (Char *) Memory_calloc(heap,BUFFER_SIZE,0,NULL);
    

    if(Module.bufferPtr == NULL) {
        printf("App_create: Failed to create buffer from shared region 1 "
                "heap\n");
        status = -1;
        goto leave;
    }

    printf("App_create: Host is ready\n");

leave: 
    printf("<-- App_create:\n");
    return (status);
}


/*
 *  ======== App_exec ========
 *  HRTF spatial audio processing with dynamic azimuth trajectory.
 *
 *  1. Transfer shared region pointer to DSP (existing mechanism)
 *  2. Load HRIR table from file into SharedRegion, notify DSP
 *  3. Open input audio file and output file
 *  4. For each frame:
 *     a. Read CONV_BLOCK_SIZE mono samples from input
 *     b. Advance trajectory to get current azimuth index
 *     c. Send PROCESSING command with azimuth index to DSP
 *     d. Wait for DSP to complete HRTF convolution
 *     e. Write stereo output to output file
 *  5. Shutdown
 */
Int App_exec()
{
    Int                 status          = 0;
    UInt32              command         = 0;
    SharedRegion_SRPtr  sharedBufferPtr = 0;
    UInt32              event           = 0;
    struct timespec start_time, end_time;

    printf("--> App_exec:\n");

    int fd = -1, out_fd = -1;
    Int num_azimuths = 0;
    TrajectoryCtx traj;
    IHeap_Handle heap = NULL;
    long long total_us = 0;
    Int frame_idx = 0;

    printf("App_exec: Audio input=%s, output=%s\n", AUDIO_IN_PATH, AUDIO_OUT_PATH);
    printf("App_exec: HRIR table=%s\n", HRIR_TABLE_PATH);
    printf("App_exec: Writing sound signal to shared region 1 buffer\n");

    /* 1. convert local address space pointer to generic shared region pointer */
    sharedBufferPtr = SharedRegion_getSRPtr(Module.bufferPtr, SHARED_REGION_1);

    /* store only the lower two bytes of the shared region address pointer */
    command = (sharedBufferPtr & 0xFFFF);

    /* add shared region pointer address low command to payload */
    command = APP_SPTR_LADDR | command;

    /* 2. send shared region pointer address lower two bytes */
    status = Notify_sendEvent(Module.remoteProcId, Module.lineId,
            Module.eventId, command, TRUE);

    if (status < 0) {
        printf("App_exec: Error sending shared region pointer address lower "
                "two bytes\n");
        goto leave;
    }

    /* wait for shared region address acknowledge command */
    event = App_waitForEvent(&Module.eventQueue);

    if (event >= APP_E_FAILURE) {
        printf("App_exec: Received queue error: %d\n", event);
        status = -1;
        goto leave;
    }

    /* store only the upper two bytes of the shared region address pointer */
    command = ((sharedBufferPtr >> 16) & 0xFFFF);

    /* add shared region pointer address high command to payload */
    command = APP_SPTR_HADDR | command;

    /* 3. send shared region pointer address upper two bytes */
    status = Notify_sendEvent(Module.remoteProcId, Module.lineId,
            Module.eventId, command, TRUE);

    if (status < 0) {
        printf("App_exec: Error sending shared region pointer address upper "
                "two bytes\n");
        goto leave;
    }

    /* wait for shared region address acknowledge command */
    event = App_waitForEvent(&Module.eventQueue);

    if (event >= APP_E_FAILURE) {
        printf("App_exec: Received queue error: %d\n", event);
        status = -1;
        goto leave;
    }
    printf("App_exec: shared address transfer finished\n");

    /* 4. Load HRIR table into SharedRegion */
    heap = (IHeap_Handle) SharedRegion_getHeap(SHARED_REGION_1);
    if (heap == NULL) {
        printf("App_exec: Shared region heap not available\n");
        status = -1;
        goto leave;
    }

    num_azimuths = sofa_load_hrir_table(HRIR_TABLE_PATH, heap, Module.bufferPtr);
    if (num_azimuths <= 0) {
        printf("App_exec: Failed to load HRIR table\n");
        status = -1;
        goto leave;
    }

    /* 5. Notify DSP that HRIR table is loaded, with num_azimuths in payload */
    command = APP_CMD_LOAD_HRIR | (UInt32)(num_azimuths & 0xFFFF);
    status = Notify_sendEvent(Module.remoteProcId, Module.lineId,
            Module.eventId, command, TRUE);
    if (status < 0) {
        printf("App_exec: Error sending LOAD_HRIR command\n");
        goto leave;
    }

    /* Wait for DSP to acknowledge HRIR load */
    event = App_waitForEvent(&Module.eventQueue);
    if (event >= APP_E_FAILURE) {
        printf("App_exec: DSP HRIR load error: %d\n", event);
        status = -1;
        goto leave;
    }
    printf("App_exec: DSP confirmed HRIR loaded (%d azimuths)\n", num_azimuths);

    /* 6. Open input audio file (mono 16-bit PCM) */
    fd = open(AUDIO_IN_PATH, O_RDONLY);
    if (fd < 0) {
        perror("App_exec: Failed to open input audio file");
        status = -1;
        goto leave;
    }

    /* 7. Open output audio file (stereo 16-bit PCM) */
    out_fd = open(AUDIO_OUT_PATH, O_WRONLY | O_CREAT | O_TRUNC,
                  S_IRUSR | S_IWUSR);
    if (out_fd < 0) {
        perror("App_exec: Failed to open output audio file");
        status = -1;
        goto leave;
    }

    /* 8. Determine total frames from file size */
    {
        struct stat st;
        off_t file_size;
        Int total_frames;

        if (fstat(fd, &st) == 0) {
            file_size = st.st_size;
            total_frames = (Int)(file_size / AUDIO_INPUT_SIZE);
            printf("App_exec: Input file size=%lld bytes, %d frames\n",
                   (long long)file_size, total_frames);

            /* Initialize circular trajectory: 2 full rotations over file */
            trajectory_init(&traj, num_azimuths, total_frames, 2.0f);
        } else {
            perror("App_exec: fstat failed");
            trajectory_init(&traj, num_azimuths, 1000, 2.0f);
        }
    }

    /* 9. Main processing loop */
    printf("App_exec: Starting HRTF processing loop...\n");

    while (1)
    {
        Int total_read = 0;
        ssize_t n_read;
        ssize_t written;

        /* Read CONV_BLOCK_SIZE mono samples (AUDIO_INPUT_SIZE bytes) */
        while (total_read < AUDIO_INPUT_SIZE) {
            n_read = read(fd, Module.bufferPtr + total_read,
                          AUDIO_INPUT_SIZE - total_read);
            if (n_read > 0) {
                total_read += n_read;
            } else if (n_read == 0) {
                /* End of file */
                printf("\n[INFO] Reached end of input file after %d frames\n",
                       frame_idx);
                goto processing_done;
            } else {
                if (errno == EINTR) continue;
                perror("App_exec: read error from input file");
                goto leave;
            }
        }

        /* Advance trajectory to get current azimuth */
        Int az_idx = trajectory_step(&traj);

        /* Build PROCESSING command: [cmd][azimuth_idx][buf_size] */
        command = APP_CMD_PROCESSING
                | ((UInt32)(az_idx & 0xFF) << APP_AZIMUTH_SHIFT)
                | (UInt32)(AUDIO_INPUT_SIZE & 0xFFFF);

        if (frame_idx % 50 == 0) {
            printf("frame %d: azimuth=%d (angle=%.1f deg)\n",
                   frame_idx, az_idx, traj.current_angle);
        }

        clock_gettime(CLOCK_MONOTONIC, &start_time);

        /* Send processing command to DSP */
        status = Notify_sendEvent(Module.remoteProcId, Module.lineId,
                Module.eventId, command, TRUE);
        if (status < 0) {
            printf("App_exec: Error sending processing command\n");
            goto leave;
        }

        /* Wait for DSP to complete HRTF convolution */
        event = App_waitForEvent(&Module.eventQueue);
        if (event >= APP_E_FAILURE) {
            printf("App_exec: DSP processing error: %d\n", event);
            status = -1;
            goto leave;
        }

        clock_gettime(CLOCK_MONOTONIC, &end_time);
        total_us += get_duration_us(start_time, end_time);

        /* Write stereo output (AUDIO_OUTPUT_SIZE bytes) from buffer
         * Output is at offset AUDIO_INPUT_SIZE in the shared buffer */
        written = write(out_fd, Module.bufferPtr + AUDIO_INPUT_SIZE,
                        AUDIO_OUTPUT_SIZE);
        if (written < 0) {
            perror("App_exec: Error writing to output file");
            status = -1;
            goto leave;
        } else if (written < AUDIO_OUTPUT_SIZE) {
            printf("App_exec: Warning, partial write (%ld/%d bytes)\n",
                   (long)written, AUDIO_OUTPUT_SIZE);
        }

        frame_idx++;
    }

processing_done:
    if (frame_idx > 0) {
        double avg_latency = (double)total_us / frame_idx;
        printf("[PERF] Processed %d frames, total DSP time: %lld us, "
               "avg latency: %.2f us\n", frame_idx, total_us, avg_latency);
    }

leave:
    /* Send shutdown command to DSP */
    printf("App_exec: Sending shutdown to DSP...\n");
    command = APP_CMD_SHUTDOWN;
    status = Notify_sendEvent(Module.remoteProcId, Module.lineId,
            Module.eventId, command, TRUE);
    if (status < 0) {
        printf("App_exec: Error sending shutdown command\n");
    }
    event = App_waitForEvent(&Module.eventQueue);
    if (event >= APP_E_FAILURE) {
        printf("App_exec: DSP shutdown error: %d\n", event);
        status = -1;
    }

    printf("<-- App_exec:\n");
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
    if (out_fd >= 0) {
        close(out_fd);
        out_fd = -1;
    }
    return (status);
}

/*
 *  ======== App_delete ========
 *  1. sending shutdown command
 *  2. free buffer memory
 *  3. unregister notify callback
 *  4. delete sync object
 */
Int App_delete() 
{
    Int             status      = 0;
    UInt32          event       = 0;
    IHeap_Handle    heap; 

    printf("--> App_delete:\n");
   
    /* 1. sending shutdown command */
    status = Notify_sendEvent(Module.remoteProcId, Module.lineId, 
        Module.eventId, APP_CMD_SHUTDOWN, TRUE);

    if (status < 0 )  {
        printf("App_delete: Error sending shutdown command\n");
        goto leave;
    }    
    
    /* wait for shutdown acknowledge command */                               
    event = App_waitForEvent(&Module.eventQueue);

    if (event >= APP_E_FAILURE) {
        printf("App_delete: Received queue error: %d\n",event); 
        status = -1;       
        goto leave;
    }   
    
    /* 2. free buffer memory */
    heap = (IHeap_Handle) SharedRegion_getHeap(SHARED_REGION_1);
    
    Memory_free(heap,Module.bufferPtr,BUFFER_SIZE);

    /* 3. unregister notify callback */
    status = Notify_unregisterEvent(Module.remoteProcId, Module.lineId, 
            Module.eventId, App_notifyCB, (UArg) &Module.eventQueue);

    if (status < 0) {
        printf("App_delete: Unregistering notify callback failed\n"); 
        goto leave;
    }

    /* 4. delete sync object */
    sem_destroy(&Module.eventQueue.semH);
    
    printf("App_delete: Cleanup complete\n");

leave:
    printf("<-- App_delete:\n");
    return (status);
}

/*
 *  ======== App_notifyCB ========
 */
Void App_notifyCB(UInt16 procId, UInt16 lineId, UInt32 eventId, UArg arg,
        UInt32 payload)
{
    UInt            next;
    Event_Queue*    eventQueue = (Event_Queue *) arg;

    /* ignore no-op events */
    if (payload == APP_CMD_NOP) {
        return;
    }

    /* compute next slot in queue */
    next = (eventQueue->head + 1) % QUEUESIZE;

    if (next == eventQueue->tail) {
        /* queue is full, drop event and set error flag */
        eventQueue->error = APP_E_OVERFLOW;
    }
    else {
        /* queue head is only written to within this function */
        eventQueue->queue[eventQueue->head] = payload;
        eventQueue->head = next;
        
        /* signal semaphore (counting) that new event is in queue */
        sem_post(&eventQueue->semH);
    }

    return;
}

/*
 *  ======== App_waitForEvent ========
 */
static UInt32 App_waitForEvent(Event_Queue* eventQueue)
{
    UInt32 event;

    if (eventQueue->error >= APP_E_FAILURE) {
        event = eventQueue->error;
    }
    else {
        /* use counting semaphore to wait for next event */
        sem_wait(&eventQueue->semH);

        /* remove next command from queue */
        event = eventQueue->queue[eventQueue->tail];
        
        /* queue tail is only written to within this function */
        eventQueue->tail = (eventQueue->tail + 1) % QUEUESIZE;
    }

    return (event);
}
