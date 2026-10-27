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
 *  ======== Server.c ========
 *
 */

/* this define must precede inclusion of any xdc header file */
#define Registry_CURDESC Test__Desc
#define MODULE_NAME "Server"

#include <stdio.h>    /* by Guanqing 20210517  */
#include <string.h>

/* xdctools header files */
#include <xdc/std.h>
#include <xdc/runtime/Assert.h>
#include <xdc/runtime/Diags.h>
#include <xdc/runtime/Log.h>
#include <xdc/runtime/Memory.h>
#include <xdc/runtime/Registry.h>
#include <xdc/runtime/System.h>

/* package header files */
#include <ti/ipc/Notify.h>
#include <ti/ipc/SharedRegion.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Semaphore.h>
#include <ti/sysbios/BIOS.h>

/* local header files */
#include "../shared/SystemCfg.h"
#include "../shared/AppCommon.h"

/* module header file */
#include "Server.h"
#include "fft_conv.h"          /* HRTF FFT convolution engine */

#include "dsplib/sqrtsp.h"   /* using mathlib for C674X to test, by Guanqing 20210517  */

/* max number of outstanding commands minus one */
#define QUEUESIZE   8

int errno;

/* queue structure */
typedef struct
{
    UInt32              queue[QUEUESIZE];   /* command queue */
    UInt                head;               /* queue head pointer */
    UInt                tail;               /* queue tail pointer */
    UInt32              error;              /* error flag */
    Semaphore_Struct    semObj;             /* semaphore object */
    Semaphore_Handle    semH;               /* handle to object above */
} Event_Queue;

/* module structure */
typedef struct {
    Event_Queue     eventQueue;
    UInt16          remoteProcId;
    UInt16          lineId;             /* notify line id */
    UInt32          eventId;            /* notify event id */
} Server_Module;

Registry_Desc               Registry_CURDESC;
static Int                  Module_curInit = 0;
static Server_Module        Module;

/* private functions */
static UInt32 Server_waitForEvent(Event_Queue* eventQueue);
static Void Server_notifyCB(UInt16 procId, UInt16 lineId, UInt32 eventId,
        UArg arg, UInt32 payload);

/* ------------------------------------------------------------------ */
/*  HRTF processing state                                             */
/* ------------------------------------------------------------------ */

/* Number of azimuths loaded by Host (read from HRIR table header) */
static Int g_num_azimuths = 0;

/* Pointer to HRIR time-domain table in SharedRegion (set by LOAD_HRIR) */
static float *g_hrir_table = NULL;

/* Frequency-domain HRIR table: pre-computed for all azimuths
 * Layout: [num_azimuths][2 ears][FFT_N cplx_t] */
static cplx_t *g_hrir_freq_table = NULL;

/* Convolution state buffer (history for overlap-save) */
static float g_conv_state[CONV_HISTORY_LEN];

/* Current azimuth index (set by SET_AZIMUTH or from PROCESSING payload) */
static Int g_current_azimuth = 0;

/* Flag: HRIR frequency table initialized */
static Int g_hrir_ready = 0;

/* ------------------------------------------------------------------ */
/*  process_hrtf_audio: replace old channel-swap with HRTF convolution*/
/*                                                                   */
/*  Reads mono Int16 input from buffer (AUDIO_INPUT_SIZE bytes),      */
/*  performs overlap-save HRTF convolution for both ears,            */
/*  writes interleaved stereo Int16 output back to buffer.           */
/* ------------------------------------------------------------------ */
static void process_hrtf_audio(Int16 *src, UInt16 byte_count, Int azimuth_idx)
{
    /* Input: first AUDIO_INPUT_SIZE bytes = CONV_BLOCK_SIZE mono Int16 samples */
    Int16 *input_mono = (Int16 *)src;

    /* Output: placed after input, AUDIO_OUTPUT_SIZE bytes */
    Int16 *output_stereo = (Int16 *)((Char *)src + AUDIO_INPUT_SIZE);

    cplx_t *hrir_freq_L;
    cplx_t *hrir_freq_R;

    /* Safety checks */
    if (!g_hrir_ready || azimuth_idx < 0 || azimuth_idx >= g_num_azimuths) {
        /* Fallback: just copy input to both channels (no spatialization) */
        int i;
        for (i = 0; i < CONV_BLOCK_SIZE; i++) {
            Int16 s = input_mono[i];
            output_stereo[2*i]     = s;
            output_stereo[2*i + 1] = s;
        }
        return;
    }

    /* Get frequency-domain HRIR for current azimuth */
    /* Layout: [azimuth][ear][FFT_N cplx_t] */
    hrir_freq_L = &g_hrir_freq_table[azimuth_idx * 2 * FFT_N + 0 * FFT_N];
    hrir_freq_R = &g_hrir_freq_table[azimuth_idx * 2 * FFT_N + 1 * FFT_N];

    /* Perform overlap-save convolution */
    overlap_save_convolve(
        input_mono,
        hrir_freq_L,
        hrir_freq_R,
        output_stereo,
        g_conv_state
    );
}

/* ------------------------------------------------------------------ */
/*  init_hrir_freq_table: pre-compute frequency-domain HRIR from     */
/*  time-domain table in SharedRegion. Called once after LOAD_HRIR.  */
/* ------------------------------------------------------------------ */
static void init_hrir_freq_table(void)
{
    Int az, ear, i;
    float hrir_time[HRIR_LEN];

    if (g_num_azimuths <= 0 || g_hrir_table == NULL)
        return;

    /* Allocate frequency-domain table in DSP heap */
    /* Size = num_azimuths * 2 * FFT_N * sizeof(cplx_t) */
    /* This uses the BIOS default heap (configured in Dsp.cfg) */
    /* We use static allocation for reliability */
    /* Total = 710 * 2 * 1024 * 8 = ~11.6MB - may be too large */
    /* For typical horizontal ring (~72 azimuths) = 72*2*1024*8 = ~1.2MB */

    /* Initialize FFT engine first */
    fft_engine_init();

    /* Clear convolution state */
    memset(g_conv_state, 0, sizeof(g_conv_state));

    Log_print1(Diags_INFO, "init_hrir_freq_table: pre-computing %d azimuths",
               (IArg)g_num_azimuths);

    for (az = 0; az < g_num_azimuths; az++) {
        for (ear = 0; ear < 2; ear++) {
            /* Extract time-domain HRIR from SharedRegion table */
            /* Table layout: [azimuth][ear][HRIR_LEN float] */
            float *src = &g_hrir_table[(az * 2 + ear) * HRIR_LEN];
            cplx_t *dst = &g_hrir_freq_table[(az * 2 + ear) * FFT_N];

            memcpy(hrir_time, src, HRIR_LEN * sizeof(float));
            hrir_time_to_freq(hrir_time, dst);
        }
    }

    g_hrir_ready = 1;
    Log_print0(Diags_INFO, "init_hrir_freq_table: ready");
}

/*
 *  ======== Server_init ========
 */
Void Server_init(Void)
{
    Registry_Result result;

    if (Module_curInit++ != 0) {
        return;  /* already initialized */
    }

    /* register with xdc.runtime to get a diags mask */
    result = Registry_addModule(&Registry_CURDESC, MODULE_NAME);
    Assert_isTrue(result == Registry_SUCCESS, (Assert_Id)NULL);
}


/*
 *  ======== Server_create ========
 *
 *  1. create sync object
 *  2. register notify callback
 *  3. wait until remote core has also registered notify callback
 */
Int Server_create(UInt16 remoteProcId)
{
    Int                 status      = 0;
    Semaphore_Params    semParams;

    Log_print0(Diags_ENTRY, "--> Server_create:");
    
    /* setting default values */
    Module.eventQueue.head      = 0;               
    Module.eventQueue.tail      = 0;              
    Module.eventQueue.error     = 0;       
    Module.eventQueue.semH      = NULL;
    Module.lineId               = SystemCfg_LineId;
    Module.eventId              = SystemCfg_EventId;
    Module.remoteProcId         = remoteProcId;
    
    /* enable some log events */
    Diags_setMask(MODULE_NAME"+EXF");

    /* 1. create sync object */
    Semaphore_Params_init(&semParams);
    semParams.mode = Semaphore_Mode_COUNTING;
    
    Semaphore_construct(&Module.eventQueue.semObj, 0, &semParams);

    Module.eventQueue.semH = Semaphore_handle(&Module.eventQueue.semObj);

    /* 2. register notify callback */
    status = Notify_registerEvent(Module.remoteProcId, Module.lineId, 
            Module.eventId, Server_notifyCB,(UArg)  &Module.eventQueue);

    if (status < 0) {
        Log_error0("Server_create: Device failed to register notify event");
        goto leave;
    }

    /* 3. wait until remote core has also registered notify callback */
    do {
        status = Notify_sendEvent(Module.remoteProcId,Module.lineId,
                Module.eventId, APP_CMD_NOP, TRUE);

        if (status == Notify_E_EVTNOTREGISTERED) {
            Task_sleep(100);
        }
        
    } while (status == Notify_E_EVTNOTREGISTERED);

    if (status < 0 ) {
        Log_error0("Server_create: Host failed to register callback");
        goto leave;
    }
    
    Log_print0(Diags_INFO, "Server_create: Slave is ready");

leave:
    Log_print1(Diags_EXIT, "<-- Server_create: %d", (IArg)status);
    return (status);
}


/*
 *  ======== Server_exec ========
 *  1. wait for shared region pointer address lower two bytes 
 *  2. save only the payload which represents the shared region pointer
 *     address lower two bytes
 *  3. wait for shared region pointer address upper two bytes
 *  4. join shared region pointer address upper and lower two bytes
 *  5. translate shared region pointer to local address space pointer
 *  6. convert lowercase string to uppercase
 *  7. send operation complete command 
 */
Int Server_exec()
{
    Int                 status              = 0;
    SharedRegion_SRPtr  sharedBufferPtr     = 0;
    UInt32              event;
    Char*               bufferPtr;
    UInt16		arm_command,command_bufsize;

    Log_print0(Diags_ENTRY | Diags_INFO, "--> Server_exec:");

    /* 1. wait for shared region pointer address lower two bytes*/
    event = Server_waitForEvent(&Module.eventQueue);
    
    if (event >= APP_E_FAILURE) {
        Log_error1("Server_exec: Received queue error: %d",event); 
        status = -1;       
        goto leave;
    } 
   
    /* 2. save only the payload which represents the shared region pointer 
     *    address lower two bytes 
     */ 
    sharedBufferPtr = event & APP_SPTR_MASK;
    
    /* send shared region pointer address acknowledge */
    status = Notify_sendEvent(Module.remoteProcId, Module.lineId, 
            Module.eventId, APP_SPTR_ADDR_ACK, TRUE);

    if (status < 0 )  {
        Log_print0(Diags_INFO,"Server_exec: Error sending shared region pointer"
                "address acknowledge");
        goto leave;
    }    
    
    /* 3. wait for shared region pointer address upper two bytes*/
    event = Server_waitForEvent(&Module.eventQueue);
    
    if (event >= APP_E_FAILURE) {
        Log_error1("Server_exec: Received queue error: %d",event); 
        status = -1;       
        goto leave;
    } 
    
    /* 4. join shared region pointer address upper and lower two bytes */
    sharedBufferPtr = ((event & APP_SPTR_MASK) << 16) | sharedBufferPtr;
    
    /* send shared region pointer address acknowledge */
    status = Notify_sendEvent(Module.remoteProcId, Module.lineId, 
            Module.eventId, APP_SPTR_ADDR_ACK, TRUE);

    if (status < 0 )  {
        Log_print0(Diags_INFO,"Server_exec: Error sending shared region pointer"
                "address acknowledge");
        goto leave;
    }        

    arm_command = 1;
    do{
        event = Server_waitForEvent(&Module.eventQueue);
        if (event >= APP_E_FAILURE) {
            Log_error1("Server_exec: Received queue error by ARM Core processing command: %d",event);
            status = -1;
            goto leave;
        }
        switch(event & APP_CMD_MASK)        /*  notify commands 00 - FF : FF000000 */
        {
            case APP_CMD_LOAD_HRIR:
            {
                /* Host has loaded HRIR table into SharedRegion.
                 * The sharedBufferPtr points to the HRIR table.
                 * Low 16 bits of payload = num_azimuths.
                 */
                UInt16 num_az = event & APP_SPTR_MASK;
                Char *hrir_ptr = SharedRegion_getPtr(sharedBufferPtr);

                /* Skip 16-byte binary header (magic+version+num_az+hrir_len+ears+sr)
                 * then the float data starts.
                 * Actually the Host loads the raw float table directly;
                 * num_az comes in the payload.
                 */
                g_hrir_table = (float *)(hrir_ptr + SR1_HRIR_TABLE_OFFSET);
                g_num_azimuths = (Int)num_az;

                /* Allocate frequency-domain HRIR table on DSP heap.
                 * Size = num_az * 2 * FFT_N * sizeof(cplx_t)
                 */
                {
                    UInt tbl_size = (UInt)g_num_azimuths * 2 * FFT_N * sizeof(cplx_t);

                    /* Use BIOS default heap via xdc.runtime.Memory */
                    g_hrir_freq_table = (cplx_t *)Memory_calloc(NULL, tbl_size, 0, NULL);

                    if (g_hrir_freq_table == NULL) {
                        Log_error0("Server_exec: Failed to allocate HRIR freq table");
                        status = -1;
                        goto leave;
                    }
                }

                /* Pre-compute frequency-domain HRIR */
                init_hrir_freq_table();

                Log_print1(Diags_INFO, "Server_exec: HRIR loaded, %d azimuths",
                           (IArg)g_num_azimuths);

                /* Acknowledge HRIR load */
                status = Notify_sendEvent(Module.remoteProcId, Module.lineId,
                        Module.eventId, APP_CMD_HRIR_LOADED_ACK, TRUE);
                if (status < 0) {
                    Log_error0("Server_exec: Error sending HRIR loaded ack");
                    goto leave;
                }
            }
            break;

            case APP_CMD_SET_AZIMUTH:
            {
                /* Set current azimuth index from payload low 16 bits */
                UInt16 az_idx = event & APP_SPTR_MASK;
                g_current_azimuth = (Int)az_idx;
                Log_print1(Diags_INFO, "Server_exec: azimuth set to %d",
                           (IArg)g_current_azimuth);
            }
            break;

            case APP_CMD_PROCESSING:
            {
                /* Extract azimuth index from payload bits [23:16] */
                UInt16 az_idx = (event & APP_AZIMUTH_MASK) >> APP_AZIMUTH_SHIFT;
                command_bufsize = event & APP_SPTR_MASK;

                /* Use azimuth from payload if valid, else fall back to global */
                if (az_idx < (UInt16)g_num_azimuths)
                    g_current_azimuth = (Int)az_idx;

                /* Translate shared region pointer to local address */
                bufferPtr = SharedRegion_getPtr(sharedBufferPtr);

                /* Perform HRTF convolution */
                process_hrtf_audio((Int16*)bufferPtr, command_bufsize,
                                   g_current_azimuth);

                /* Send operation complete command */
                status = Notify_sendEvent(Module.remoteProcId, Module.lineId,
                        Module.eventId, APP_CMD_OP_COMPLETE, TRUE);
                if (status < 0) {
                    Log_print0(Diags_INFO,"Server_exec: Error sending operation complete "
                        "command");
                    goto leave;
                }
            }
            break;

            case APP_CMD_SHUTDOWN:
            /* Send shutdown acknowledge */
            status = Notify_sendEvent(Module.remoteProcId, Module.lineId,Module.eventId, APP_CMD_SHUTDOWN_ACK,TRUE);
            if (status < 0 )  {
                Log_print0(Diags_INFO,"Server_exec: Error sending shutdown complete "
                    "command");
                goto leave;
            }
            arm_command = 0;
            break;
        }

    }while(arm_command);
    
    Log_print0(Diags_INFO, "Server_exec: Completed lowercase to uppercase "
            "string operation");    

leave:
    Log_print1(Diags_EXIT, "<-- Server_exec: %d", (IArg)status);
    return (status);
}

/*
 *  ======== Server_delete ========
 *  1. wait for shutdown command
 *  2. unregister notify callback 
 *  3. delete sync object
 */
    
Int Server_delete()
{
    Int     status = 0; 
    UInt32  event;

    Log_print0(Diags_ENTRY, "--> Server_delete:");
    
    /* 1. wait for shutdown command */
    event = Server_waitForEvent(&Module.eventQueue);

    if (event >= APP_E_FAILURE) {
        Log_error1("Server_delete: Received queue error: %d",event); 
        status = -1;       
        goto leave;
    } 

    /* send shutdown command acknowledge */
    status = Notify_sendEvent(Module.remoteProcId, Module.lineId, 
            Module.eventId, APP_CMD_SHUTDOWN_ACK, TRUE);

    if (status < 0 )  {
        Log_error0("Server_delete: Error sending shutdown command acknowledge");
        goto leave;
    }

    /* 2. unregister notify callback */
    status = Notify_unregisterEvent(Module.remoteProcId, Module.lineId, 
            Module.eventId, Server_notifyCB,(UArg) &Module.eventQueue);

    if (status < 0) {
        Log_error0("Server_delete: Unregistering event has failed");
        goto leave;
    }

    /* 3. delete sync object */
    Semaphore_destruct(&Module.eventQueue.semObj);
    
    Log_print0(Diags_INFO,"Server_delete: Cleanup complete");

leave:
    Log_print1(Diags_EXIT, "<-- Server_delete: %d", (IArg)status);
    return (status);
}

/*
 *  ======== Server_exit ========
 */

Void Server_exit(Void)
{

    if (Module_curInit-- != 1) {
        return;  /* object still being used */
    }

    /*
     * Note that there isn't a Registry_removeModule() yet:
     *     https://bugs.eclipse.org/bugs/show_bug.cgi?id=315448
     *
     * ... but this is where we'd call it.
     */
}

/*
 *  ======== Server_notifyCB ========
 *
 *  This function is called from within an ISR
 */
Void Server_notifyCB( UInt16 procId, UInt16 lineId, UInt32  eventId, UArg arg,
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
        eventQueue->queue[eventQueue->head] = payload;
        
        /* queue head is only written to within this function */
        eventQueue->head = next;
        
        /* signal semaphore (counting) that new event is in queue */
        Semaphore_post(eventQueue->semH);
    }

    return;
}

/*
 *  ======== Server_waitForEvent ========
 * 
 */
static UInt32 Server_waitForEvent(Event_Queue* eventQueue)
{
    UInt32 event;

    if (eventQueue->error >= APP_E_FAILURE) {
        event = eventQueue->error;
    }
    else {
        /* use counting semaphore to wait for next event */
        Semaphore_pend(eventQueue->semH,  BIOS_WAIT_FOREVER);

        /* remove next command from queue */
        event = eventQueue->queue[eventQueue->tail];
        
        /* queue tail is only written to within this function */
        eventQueue->tail = (eventQueue->tail + 1) % QUEUESIZE;
    }

    return (event);
}

