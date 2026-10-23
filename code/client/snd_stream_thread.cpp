/*
===========================================================================
Copyright (C) 2026 the OpenMoHAA team

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

// snd_stream_thread.cpp -- the soundtrack's mp3 files decoded on a thread
//
// A song stream (openal_channel_two_d_stream) decodes 16384 samples on the
// main thread each time it queues a buffer: 0.4 to 0.9 ms a read, and opening
// it scans the whole file. A round restart starts the soundtrack again (2.7 ms
// in that frame with a 3 minute mp3), and a playing song refills 8 buffers in
// 8 frames in a row every few seconds.
//
// With s_streamThread 1, each song of a soundtrack (MUSIC_LoadSoundtrackFile)
// is read into memory when the soundtrack loads, and this thread scans it and
// decodes its first read (the head). A stream of it (S_CodecLoad(name, NULL))
// starts from the head, while the thread decodes the head again (to get there)
// and then the following reads ahead, into a ring of slots of one read each:
// the main thread copies PCM. The streams decode the same bytes in the same
// blocks as the codec's file streams, with a dithering sequence of their own
// (S_MP3_OpenMemoryStream), so the head and the thread's decode are the same
// PCM; a read that the thread hasn't decoded yet waits
// (openal_channel_two_d_stream::update asks S_CodecStreamReady first and
// queues it a frame later instead). Everything the thread uses is its own
// memory: no file system, zone or OpenAL calls.

#include "snd_local.h"
#include "snd_codec.h"

#ifdef USE_CODEC_MAD

#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

// one read of a stream channel: MAX_BUFFER_SAMPLES (omaha/snd_omaha.h)
// samples, at most the 8 KB of its buffer
#define ST_READ_SAMPLES 2048
#define ST_READ_MAX     8192
// reads decoded ahead per stream
#define ST_SLOTS        4
// soundtrack files kept in memory beyond those in use
#define ST_CACHE_BYTES  (64 << 20)

enum {
    ST_PREPARING,
    ST_READY,
    ST_FAILED
};

struct stFile_t {
    char             name[MAX_QPATH];
    byte            *data; // the compressed file
    int              length;
    std::atomic<int> state;
    // written by the thread before state turns ST_READY
    snd_info_t       info;
    int              readSize; // bytes of one read
    byte            *head;     // the first read
    int              headLength;
    // under st.lock: the preparation and the open streams
    int              refs;
    int              lastUse;
    stFile_t        *next;
};

struct stSlot_t {
    byte data[ST_READ_MAX];
    int  length;
};

struct stReader_t {
    snd_stream_t stream; // what the channel holds (stream.ptr: this)
    stFile_t    *file;
    // main thread
    int          reads;   // reads done
    bool         drained; // a short read was returned: the end
    // the thread's decoder
    snd_stream_t *dec;
    bool          decEnd;
    // the slots: read n is in slots[(n - 1) % ST_SLOTS] (read 0 is the head)
    stSlot_t            *slots;
    std::atomic<int>     decoded;  // reads decoded (the head counts)
    std::atomic<int>     consumed; // reads taken (the head counts): the slots in use are decoded - consumed
    std::atomic<bool>    closed;
    stReader_t          *next;
};

static struct {
    std::thread            *thread;
    std::mutex              lock;
    std::condition_variable wake; // the thread: a file to prepare, a slot freed, a stream closed, quit
    std::condition_variable done; // the main thread: a file prepared, a read decoded
    bool                    quit;
    stFile_t               *files;
    stReader_t             *readers;    // streams the thread decodes
    stSlot_t               *spareSlots; // the slots of the last stream freed
    int                     useCount;
} st;

static int ST_ReadStream(snd_stream_t *stream, int bytes, void *buffer);
static void ST_CloseStream(snd_stream_t *stream);

static snd_codec_t st_codec = {(char *)"mp3", NULL, NULL, ST_ReadStream, ST_CloseStream, NULL};

/*
=================
ST_Prepare

The thread: scans the file and decodes its head.
=================
*/
static void ST_Prepare(stFile_t *file)
{
    snd_stream_t *dec;

    dec = S_MP3_OpenMemoryStream(file->data, file->length);
    if (!dec) {
        file->state.store(ST_FAILED, std::memory_order_release);
        return;
    }

    file->info     = dec->info;
    file->readSize = ST_READ_SAMPLES * dec->info.width * dec->info.channels;
    if (file->readSize > ST_READ_MAX) {
        file->readSize = ST_READ_MAX;
    }

    file->head       = (byte *)malloc(file->readSize);
    file->headLength = file->head ? S_MP3_CodecReadStream(dec, file->readSize, file->head) : -1;
    S_MP3_CloseMemoryStream(dec);

    file->state.store(file->headLength >= 0 ? ST_READY : ST_FAILED, std::memory_order_release);
}

/*
=================
ST_DecodeRead

The thread: decodes the reader's next read into its slot.
=================
*/
static void ST_DecodeRead(stReader_t *r)
{
    stFile_t *file = r->file;
    stSlot_t *slot;
    int       n;

    if (!r->dec) {
        // decode the head again, to get past it
        r->dec = S_MP3_OpenMemoryStream(file->data, file->length);
        if (r->dec) {
            n = S_MP3_CodecReadStream(r->dec, file->readSize, r->slots[0].data);
            if (n < file->readSize) {
                r->decEnd = true;
            }
        } else {
            r->decEnd = true;
        }
    }

    n    = r->decoded.load(std::memory_order_relaxed);
    slot = &r->slots[(n - 1) % ST_SLOTS];
    if (r->decEnd) {
        slot->length = 0;
    } else {
        slot->length = S_MP3_CodecReadStream(r->dec, file->readSize, slot->data);
        if (slot->length < file->readSize) {
            r->decEnd = true;
        }
    }

    r->decoded.store(n + 1, std::memory_order_release);
}

/*
=================
ST_FreeReader
=================
*/
static void ST_FreeReader(stReader_t *r)
{
    if (r->dec) {
        S_MP3_CloseMemoryStream(r->dec);
    }
    if (st.spareSlots) {
        free(r->slots);
    } else {
        // the next stream takes them (256 KB: a new mapping each time)
        st.spareSlots = r->slots;
    }
    r->file->refs--;
    delete r;
}

/*
=================
ST_ThreadMain
=================
*/
static void ST_ThreadMain()
{
    std::unique_lock<std::mutex> lk(st.lock);
    stFile_t                    *file;
    stReader_t                 **prev, *r;

    while (!st.quit) {
        for (file = st.files; file; file = file->next) {
            if (file->state.load(std::memory_order_relaxed) == ST_PREPARING) {
                break;
            }
        }
        if (file) {
            lk.unlock();
            ST_Prepare(file);
            lk.lock();
            file->refs--;
            st.done.notify_all();
            continue;
        }

        // the closed streams, then one with a free slot
        for (prev = &st.readers; (r = *prev);) {
            if (r->closed.load(std::memory_order_acquire)) {
                *prev = r->next;
                ST_FreeReader(r);
                st.done.notify_all();
            } else {
                prev = &r->next;
            }
        }
        for (r = st.readers; r; r = r->next) {
            if (!r->decEnd
                && r->decoded.load(std::memory_order_relaxed) - r->consumed.load(std::memory_order_acquire)
                       < ST_SLOTS) {
                break;
            }
        }
        if (r) {
            lk.unlock();
            ST_DecodeRead(r);
            lk.lock();
            st.done.notify_all();
            continue;
        }

        st.wake.wait(lk);
    }
}

/*
=================
ST_FreeFiles

Main thread, under st.lock: the files no stream uses, all of them or until the
cache is below its size.
=================
*/
static void ST_FreeFiles(bool all)
{
    stFile_t **prev, *file, *oldest;
    int        total;

    for (;;) {
        total  = 0;
        oldest = NULL;
        for (file = st.files; file; file = file->next) {
            total += file->length;
            if (!file->refs && (!oldest || file->lastUse < oldest->lastUse)) {
                oldest = file;
            }
        }
        if (!oldest || (!all && total <= ST_CACHE_BYTES)) {
            return;
        }

        for (prev = &st.files; *prev != oldest; prev = &(*prev)->next) {}
        *prev = oldest->next;
        free(oldest->data);
        free(oldest->head);
        delete oldest;
    }
}

/*
=================
ST_FindFile
=================
*/
static stFile_t *ST_FindFile(const char *filename)
{
    stFile_t *file;

    for (file = st.files; file; file = file->next) {
        if (!Q_stricmp(file->name, filename)) {
            return file;
        }
    }
    return NULL;
}

/*
=================
S_StreamThread_Prefetch

A song of the soundtrack that loads: read into memory, prepared on the thread.
=================
*/
void S_StreamThread_Prefetch(const char *filename)
{
    stFile_t *file;
    void     *data;
    long      length;

    if (!s_streamThread || !s_streamThread->integer || strlen(filename) >= MAX_QPATH
        || Q_stricmp(COM_GetExtension(filename), "mp3")) {
        return;
    }

    {
        std::lock_guard<std::mutex> g(st.lock);

        file = ST_FindFile(filename);
        if (file) {
            file->lastUse = ++st.useCount;
            return;
        }
    }

    length = FS_ReadFile(filename, &data);
    if (!data) {
        // the codec's streams try the other extensions
        return;
    }

    file         = new stFile_t();
    file->data   = (byte *)malloc(length);
    file->length = length;
    if (file->data) {
        memcpy(file->data, data, length);
    }
    FS_FreeFile(data);
    if (!file->data) {
        delete file;
        return;
    }

    Q_strncpyz(file->name, filename, sizeof(file->name));
    file->state.store(ST_PREPARING, std::memory_order_relaxed);
    file->refs = 1;

    {
        std::lock_guard<std::mutex> g(st.lock);

        file->lastUse = ++st.useCount;
        file->next    = st.files;
        st.files      = file;
        ST_FreeFiles(false);

        if (!st.thread) {
            st.quit   = false;
            st.thread = new std::thread(ST_ThreadMain);
        }
    }
    st.wake.notify_one();
}

/*
=================
S_StreamThread_Open

A stream of a prepared soundtrack file, NULL when it isn't one (the codec's
file streams).
=================
*/
snd_stream_t *S_StreamThread_Open(const char *filename)
{
    stFile_t   *file;
    stReader_t *r;

    if (!s_streamThread || !s_streamThread->integer || !st.files) {
        return NULL;
    }

    std::unique_lock<std::mutex> lk(st.lock);

    file = ST_FindFile(filename);
    if (!file) {
        return NULL;
    }
    // the soundtrack loaded in this frame: what is left of its preparation
    while (file->state.load(std::memory_order_acquire) == ST_PREPARING) {
        st.done.wait(lk);
    }
    if (file->state.load(std::memory_order_relaxed) != ST_READY) {
        return NULL;
    }

    r                = new stReader_t();
    r->file          = file;
    r->stream.codec  = &st_codec;
    r->stream.info   = file->info;
    r->stream.length = file->length;
    r->stream.ptr    = r;
    r->decoded.store(1, std::memory_order_relaxed);
    r->consumed.store(1, std::memory_order_relaxed);
    r->closed.store(false, std::memory_order_relaxed);

    file->refs++;
    file->lastUse = ++st.useCount;

    if (file->headLength < file->readSize) {
        // the head is the whole file: nothing to decode
        r->decEnd = true;
        return &r->stream;
    }

    r->slots = st.spareSlots ? st.spareSlots : (stSlot_t *)malloc(sizeof(stSlot_t) * ST_SLOTS);
    st.spareSlots = NULL;
    if (!r->slots) {
        r->decEnd = true;
        return &r->stream;
    }

    r->next    = st.readers;
    st.readers = r;
    lk.unlock();
    st.wake.notify_one();

    return &r->stream;
}

/*
=================
S_StreamThread_Ready
=================
*/
qboolean S_StreamThread_Ready(snd_stream_t *stream, int bytes)
{
    stReader_t *r;

    if (stream->codec != &st_codec) {
        return qtrue;
    }

    r = (stReader_t *)stream->ptr;
    if (!r->reads || r->drained || !r->slots) {
        return qtrue;
    }
    return r->decoded.load(std::memory_order_acquire) > r->reads;
}

/*
=================
ST_ReadStream

A read of a stream: the head, then the slots (waits for the thread when it
hasn't decoded it yet).
=================
*/
static int ST_ReadStream(snd_stream_t *stream, int bytes, void *buffer)
{
    stReader_t *r    = (stReader_t *)stream->ptr;
    stFile_t   *file = r->file;
    stSlot_t   *slot;
    int         decoded, length;

    if (r->drained) {
        return 0;
    }

    // the channel reads file->readSize bytes each time (a smaller read would
    // lose the rest of the read)
    if (!r->reads) {
        decoded = file->headLength;
        length  = Q_min(decoded, bytes);
        memcpy(buffer, file->head, length);
    } else if (!r->slots) {
        decoded = length = 0;
    } else {
        if (r->decoded.load(std::memory_order_acquire) <= r->reads) {
            std::unique_lock<std::mutex> lk(st.lock);
            while (r->decoded.load(std::memory_order_acquire) <= r->reads) {
                st.done.wait(lk);
            }
        }

        slot    = &r->slots[(r->reads - 1) % ST_SLOTS];
        decoded = slot->length;
        length  = Q_min(decoded, bytes);
        memcpy(buffer, slot->data, length);

        r->consumed.store(r->reads + 1, std::memory_order_release);
        {
            // the thread may be waiting for this slot
            std::lock_guard<std::mutex> g(st.lock);
        }
        st.wake.notify_one();
    }

    r->reads++;
    if (decoded < file->readSize) {
        // the end of the file: the codec's next reads return 0 too
        r->drained = true;
    }
    return length;
}

/*
=================
ST_CloseStream
=================
*/
static void ST_CloseStream(snd_stream_t *stream)
{
    stReader_t *r = (stReader_t *)stream->ptr;

    {
        std::lock_guard<std::mutex> g(st.lock);

        if (!r->slots) {
            // never given to the thread
            r->file->refs--;
            delete r;
            return;
        }
        r->closed.store(true, std::memory_order_release);
    }
    st.wake.notify_one();
}

/*
=================
S_StreamThread_Flush

A map loads: the files no stream uses are forgotten (the next soundtrack reads
its files again).
=================
*/
void S_StreamThread_Flush(void)
{
    std::unique_lock<std::mutex> lk(st.lock);
    stReader_t                  *r;

    // the streams that S_StopAllSounds closed hold their files until the
    // thread frees them
    for (;;) {
        for (r = st.readers; r && !r->closed.load(std::memory_order_relaxed); r = r->next) {}
        if (!r || !st.thread) {
            break;
        }
        st.wake.notify_one();
        st.done.wait(lk);
    }

    ST_FreeFiles(true);
}

/*
=================
S_StreamThread_Shutdown

The sound system shuts down: its channels have closed their streams.
=================
*/
void S_StreamThread_Shutdown(void)
{
    stReader_t *r;
    stFile_t   *file;

    if (st.thread) {
        {
            std::lock_guard<std::mutex> g(st.lock);
            st.quit = true;
        }
        st.wake.notify_one();
        st.thread->join();
        delete st.thread;
        st.thread = NULL;
    }

    while ((r = st.readers)) {
        st.readers = r->next;
        if (!r->closed.load(std::memory_order_relaxed)) {
            Com_DPrintf("S_StreamThread_Shutdown: a stream of %s is still open\n", r->file->name);
        }
        ST_FreeReader(r);
    }
    while ((file = st.files)) {
        st.files = file->next;
        free(file->data);
        free(file->head);
        delete file;
    }
    free(st.spareSlots);
    st.spareSlots = NULL;
}

#else // USE_CODEC_MAD

// without mp3 support the codec's file streams play everything

void S_StreamThread_Prefetch(const char *filename) {}

snd_stream_t *S_StreamThread_Open(const char *filename)
{
    return NULL;
}

qboolean S_StreamThread_Ready(snd_stream_t *stream, int bytes)
{
    return qtrue;
}

void S_StreamThread_Flush(void) {}

void S_StreamThread_Shutdown(void) {}

#endif // USE_CODEC_MAD
