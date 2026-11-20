/*++
Copyright (c) Rasa Contributors. MIT License.

Module Name:

    rasamic_feed.h

Abstract:

    Shared contract between the RasaMic kernel capture feed and the user-mode
    server (WindowsDriverBackend). One global SPSC byte ring: the user-mode
    feed thread produces via IOCTL_RASA_WRITE_PCM, the WaveRT capture stream
    consumes in WriteBytes at DISPATCH_LEVEL. Indices are monotonic byte
    counters on separate cache lines; capacity is a power of two.

    Feed format (v1): 48 kHz, 2 channels, 32-bit int PCM, interleaved.
    Conversion from mono float happens in user mode; the kernel stays dumb.
--*/

#ifndef _RASAMIC_FEED_H_
#define _RASAMIC_FEED_H_

/* Device interface exposed by the WDF control device (open via SetupDi
 * enumeration of this GUID). */
/* {8B4E9D2A-3C61-4F7E-9A0D-2E5B6C8F1A47} */
DEFINE_GUID(GUID_DEVINTERFACE_RASAMIC_CTL,
    0x8b4e9d2a, 0x3c61, 0x4f7e, 0x9a, 0x0d, 0x2e, 0x5b, 0x6c, 0x8f, 0x1a, 0x47);

#define RASA_CTL_DEVICE_TYPE 0x8000

/* Input buffer = interleaved stereo int32 PCM @48k. Output = RASA_WRITE_RESULT. */
#define IOCTL_RASA_WRITE_PCM \
    CTL_CODE(RASA_CTL_DEVICE_TYPE, 0x800, METHOD_IN_DIRECT, FILE_ANY_ACCESS)

/* No input/output. Drops all pending audio (used on stream stop/pause). */
#define IOCTL_RASA_FLUSH \
    CTL_CODE(RASA_CTL_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _RASA_WRITE_RESULT {
    ULONG BytesAccepted;
    ULONG BytesDropped;   /* ring was full */
    ULONG RingFillBytes;  /* head - tail after the write; user-mode drift compensation */
} RASA_WRITE_RESULT;

#define RASA_RING_CAPACITY (256 * 1024)  /* ~682 ms of 48k stereo int32 */

#if defined(_KERNEL_MODE)

NTSTATUS RasaFeedInit(_In_ WDFDRIVER Driver);
VOID     RasaFeedCleanup(VOID);
/* Consume up to cb bytes into Dst; zero-fills the remainder on underrun.
 * Returns bytes copied from the ring. Safe at DISPATCH_LEVEL. */
ULONG    RasaFeedRead(_Out_writes_bytes_(cb) PVOID Dst, _In_ ULONG cb);
VOID     RasaFeedFlush(VOID);

#endif /* _KERNEL_MODE */

#endif /* _RASAMIC_FEED_H_ */