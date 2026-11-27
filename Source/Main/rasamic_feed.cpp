/*++
Copyright (c) Rasa Contributors. MIT License.

Module Name:

    rasamic_feed.cpp

Abstract:

    User-mode PCM feed for the Rasa Mic capture endpoint.

    A WDF control device (interface GUID_DEVINTERFACE_RASAMIC_CTL) accepts
    IOCTL_RASA_WRITE_PCM and copies the payload into a global non-paged SPSC
    byte ring. CMiniportWaveRTStream::WriteBytes drains the ring into the DMA
    buffer; underrun produces silence. Single producer (one user-mode feed
    thread), single consumer (the capture stream's position updates).

    The audio miniport uses WdfDriverInitNoDispatchOverride, so portcls owns
    IRP dispatch for the audio FDO; this control device is a separate WDF
    device with its own queue and does not conflict.
--*/

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include "rasamic_feed.h"

#define RASA_FEED_POOLTAG 'aRsR'

typedef struct _RASA_RING {
    /* Producer index: bytes written, monotonically increasing. */
    DECLSPEC_ALIGN(64) volatile LONG64 Head;
    /* Consumer index: bytes read, monotonically increasing. */
    DECLSPEC_ALIGN(64) volatile LONG64 Tail;
    DECLSPEC_ALIGN(64) UCHAR Data[RASA_RING_CAPACITY];
} RASA_RING;

static RASA_RING*  g_Ring = NULL;
static WDFDEVICE   g_ControlDevice = NULL;

EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL RasaFeedEvtIoDeviceControl;
EVT_WDF_OBJECT_CONTEXT_CLEANUP    RasaFeedEvtQueueCleanup;

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS RasaFeedInit(_In_ WDFDRIVER Driver)
{
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFDEVICE_INIT* deviceInit = NULL;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFQUEUE queue;
    DECLARE_CONST_UNICODE_STRING(interfaceName, L"RasaMicCtl");
    DECLARE_CONST_UNICODE_STRING(controlSddl, SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RWX_RES_RWX);

    PAGED_CODE();

    if (g_Ring != NULL) {
        return STATUS_SUCCESS;  /* already initialized (one adapter) */
    }

    g_Ring = (RASA_RING*)ExAllocatePool2(
        POOL_FLAG_NON_PAGED | POOL_FLAG_UNINITIALIZED, sizeof(RASA_RING), RASA_FEED_POOLTAG);
    if (g_Ring == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    g_Ring->Head = 0;
    g_Ring->Tail = 0;

    deviceInit = WdfControlDeviceInitAllocate(Driver, &controlSddl);
    if (deviceInit == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }

    WdfDeviceInitSetDeviceType(deviceInit, RASA_CTL_DEVICE_TYPE);
    WdfDeviceInitSetCharacteristics(deviceInit, FILE_DEVICE_SECURE_OPEN, FALSE);
    WdfDeviceInitAssignName(deviceInit, &interfaceName);

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    attributes.SynchronizationScope = WdfSynchronizationScopeNone;

    status = WdfDeviceCreate(&deviceInit, &attributes, &g_ControlDevice);
    if (!NT_SUCCESS(status)) {
        goto Done;
    }

    status = WdfDeviceCreateDeviceInterface(g_ControlDevice, &GUID_DEVINTERFACE_RASAMIC_CTL, NULL);
    if (!NT_SUCCESS(status)) {
        goto Done;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = RasaFeedEvtIoDeviceControl;
    queueConfig.PowerManaged = WdfFalse;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = RasaFeedEvtQueueCleanup;
    status = WdfIoQueueCreate(g_ControlDevice, &queueConfig, &attributes, &queue);
    if (!NT_SUCCESS(status)) {
        goto Done;
    }

    WdfControlFinishInitializing(g_ControlDevice);

Done:
    if (!NT_SUCCESS(status)) {
        if (deviceInit != NULL) {
            WdfDeviceInitFree(deviceInit);
        }
        if (g_Ring != NULL) {
            ExFreePoolWithTag(g_Ring, RASA_FEED_POOLTAG);
            g_Ring = NULL;
        }
        g_ControlDevice = NULL;
    }
    return status;
}

#pragma code_seg("PAGE")
VOID RasaFeedCleanup(VOID)
{
    PAGED_CODE();
    if (g_Ring != NULL) {
        ExFreePoolWithTag(g_Ring, RASA_FEED_POOLTAG);
        g_Ring = NULL;
    }
    g_ControlDevice = NULL;
}

//=============================================================================
#pragma code_seg()
ULONG RasaFeedRead(_Out_writes_bytes_(cb) PVOID Dst, _In_ ULONG cb)
{
    if (g_Ring == NULL || cb == 0) {
        if (cb > 0) RtlZeroMemory(Dst, cb);
        return 0;
    }

    /* Acquire on Head pairs with the producer's release store; the consumer
     * owns Tail, so its own reads need no ordering beyond program order. */
    LONG64 head = ReadAcquire64(&g_Ring->Head);
    LONG64 tail = g_Ring->Tail;
    ULONG available = (ULONG)(head - tail);
    ULONG take = min(cb, available);
    ULONG done = 0;

    while (done < take) {
        ULONG offset = (ULONG)((tail + done) & (RASA_RING_CAPACITY - 1));
        ULONG run = min(take - done, RASA_RING_CAPACITY - offset);
        RtlCopyMemory((PUCHAR)Dst + done, g_Ring->Data + offset, run);
        done += run;
    }

    if (done < cb) {
        RtlZeroMemory((PUCHAR)Dst + done, cb - done);
    }

    WriteRelease64(&g_Ring->Tail, tail + done);
    return done;
}

#pragma code_seg()
VOID RasaFeedFlush(VOID)
{
    if (g_Ring == NULL) return;
    /* Single consumer; resetting both indices to the current head drops all
     * pending audio without racing the producer's next write. */
    LONG64 head = ReadAcquire64(&g_Ring->Head);
    WriteRelease64(&g_Ring->Tail, head);
}

//=============================================================================
#pragma code_seg()
VOID RasaFeedEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    NTSTATUS status;
    size_t length = 0;
    ULONG_PTR information = 0;

    UNREFERENCED_PARAMETER(Queue);

    switch (IoControlCode) {
    case IOCTL_RASA_WRITE_PCM: {
        PVOID inBuffer = NULL;
        RASA_WRITE_RESULT* result = NULL;

        if (g_Ring == NULL) {
            status = STATUS_DEVICE_NOT_READY;
            break;
        }
        if (InputBufferLength == 0 || OutputBufferLength < sizeof(RASA_WRITE_RESULT)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        status = WdfRequestRetrieveInputBuffer(Request, InputBufferLength, &inBuffer, &length);
        if (!NT_SUCCESS(status)) break;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(RASA_WRITE_RESULT), (PVOID*)&result, &length);
        if (!NT_SUCCESS(status)) break;

        ULONG cb = (ULONG)InputBufferLength;
        LONG64 head = g_Ring->Head;   /* producer-owned */
        LONG64 tail = ReadAcquire64(&g_Ring->Tail);
        ULONG space = (ULONG)(RASA_RING_CAPACITY - (head - tail));
        ULONG take = min(cb, space);
        ULONG done = 0;

        while (done < take) {
            ULONG offset = (ULONG)((head + done) & (RASA_RING_CAPACITY - 1));
            ULONG run = min(take - done, RASA_RING_CAPACITY - offset);
            RtlCopyMemory(g_Ring->Data + offset, (PUCHAR)inBuffer + done, run);
            done += run;
        }

        WriteRelease64(&g_Ring->Head, head + done);
        result->BytesAccepted = done;
        result->BytesDropped = cb - done;
        result->RingFillBytes = (ULONG)(head + done - ReadAcquire64(&g_Ring->Tail));
        information = sizeof(RASA_WRITE_RESULT);
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_RASA_FLUSH:
        RasaFeedFlush();
        status = STATUS_SUCCESS;
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, information);
}

#pragma code_seg()
VOID RasaFeedEvtQueueCleanup(_In_ WDFOBJECT Object)
{
    UNREFERENCED_PARAMETER(Object);
    RasaFeedFlush();
}