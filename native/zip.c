//------------------------------------------------------------------------
// zip.c -- the native part of the zip brick: Clay glue for miniz
//
//  It registers one prototype, ZipCore. src/main.clay wraps it in a Clay
//  API: Zip is a clone of ZipCore, so zip archives have these methods, and
//  Zlib calls the stateless ones (compression, checksums) on ZipCore. The
//  methods named with a leading _ are internal; the Clay wrapper checks
//  arguments' defaults and calls them.
//
//  Every Zip object owns a ZipData: a miniz archive opened for reading (a
//  file, or a copy of a String's bytes) or for writing (a file, or memory).
//
//  The C API is in clay_plugin.h (see "Native plugins" in the Clay guide):
//  values cross as opaque handles, arguments are borrowed, and what a method
//  returns is a new reference (or its own receiver). api->raise longjmps
//  past these C frames, so every function here frees what it owns *before*
//  raising. The entry point must be named <plugin>_plugin_init.
//------------------------------------------------------------------------

#include "clay_plugin.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// brick.json defines MINIZ_NO_ZLIB_COMPATIBLE_NAMES for miniz.c as well, so
// nothing here or there #defines zlib's names (deflate, crc32, ...)
#include "miniz.h"

//------------------------------------------------------------------------
// the Clay plugin API instance
//------------------------------------------------------------------------
static const ClayPluginAPI *api;

// make_bytes/to_bytes (4) and dicts (5)
#define MIN_API_VERSION 5

#define ZIP_ERROR "ZipError"
#define MSG_BUF   512

// the stream formats the Zlib methods speak
enum { FMT_ZLIB = 0, FMT_RAW = 1, FMT_GZIP = 2 };

// what a Zip object is doing
enum { MODE_NONE = 0, MODE_READ, MODE_WRITE, MODE_CLOSED };

//------------------------------------------------------------------------
// what each Zip object owns
//------------------------------------------------------------------------
typedef struct {
    int mode;               // MODE_*
    int in_memory;          // writer: building the archive in memory
    void *mem;              // reader: our copy of the archive's bytes
    mz_zip_archive zip;
} ZipData;

//------------------------------------------------------------------------
// raise helpers (they do not return)
//------------------------------------------------------------------------
static void raise_zip_error(mz_zip_archive *zip, const char *what) {
    char msg[MSG_BUF];

    snprintf(msg, sizeof msg, "Zip: %s: %s", what,
             mz_zip_get_error_string(mz_zip_get_last_error(zip)));
    api->raise(msg, ZIP_ERROR);
}

//------------------------------------------------------------------------
// end whatever the archive is doing and free what it owns. A writer that
// was never closed is finalized first, so dropping the last reference to a
// Zip still leaves a complete archive on disk.
//------------------------------------------------------------------------
static void zipdata_end(ZipData *z) {
    if (z->mode == MODE_READ) {
        mz_zip_reader_end(&z->zip);
    } else if (z->mode == MODE_WRITE) {
        if (!z->in_memory)
            mz_zip_writer_finalize_archive(&z->zip);
            
        mz_zip_writer_end(&z->zip);
    }

    free(z->mem);

    z->mem = NULL;
    z->mode = MODE_CLOSED;
}

//------------------------------------------------------------------------
// runs when a Zip object is freed
//------------------------------------------------------------------------
static void zip_free(void *data) {
    ZipData *z = (ZipData *)data;

    if (z) {
        zipdata_end(z);
        free(z);
    }
}

//------------------------------------------------------------------------
// the ZipData of self; raises unless self is a Zip in the wanted mode
// (MODE_NONE: any mode)
//------------------------------------------------------------------------
static ZipData *zip_of(ClayPluginObject self, int want) {
    ZipData *z = (ZipData *)api->get_data(self);

    if (!z) {
        api->raise("Zip: not an archive (make one with Zip open or Zip create)", "RuntimeError");
        return NULL;
    }

    if (want != MODE_NONE && z->mode != want) {
        if (z->mode == MODE_CLOSED)
            api->raise("Zip: the archive is closed", ZIP_ERROR);
        else if (want == MODE_READ)
            api->raise("Zip: the archive was opened for writing, not reading", ZIP_ERROR);
        else
            api->raise("Zip: the archive was opened for reading, not writing", ZIP_ERROR);
        return NULL;
    }

    return z;
}

//------------------------------------------------------------------------
// argument helpers
//------------------------------------------------------------------------

//------------------------------------------------------------------------
// argv[i] as bytes; raises TypeError unless it is a String
//------------------------------------------------------------------------
static const char *arg_bytes(int argc, ClayPluginValue *argv, int i, const char *what, size_t *len) {
    const char *p = (i < argc) ? api->to_bytes(argv[i], len) : NULL;

    if (!p) {
        char msg[MSG_BUF];
        snprintf(msg, sizeof msg, "%s must be a String", what);
        api->raise(msg, "TypeError");
    }

    return p;
}

//------------------------------------------------------------------------
// argv[i] as a C string with no NUL inside (a name or a path)
//------------------------------------------------------------------------
static const char *arg_cstring(int argc, ClayPluginValue *argv, int i, const char *what) {
    size_t len = 0;
    const char *p = arg_bytes(argc, argv, i, what, &len);

    if (strlen(p) != len) {
        char msg[MSG_BUF];
        snprintf(msg, sizeof msg, "%s must not contain a NUL byte", what);
        api->raise(msg, "ArgumentError");
    }

    return p;
}

//------------------------------------------------------------------------
// argv[i] as an integer in lo..hi
//------------------------------------------------------------------------
static intptr_t arg_int(int argc, ClayPluginValue *argv, int i, const char *what, intptr_t lo, intptr_t hi) {
    char msg[MSG_BUF];

    if (i >= argc || !api->is_int(argv[i])) {
        snprintf(msg, sizeof msg, "%s must be an integer", what);
        api->raise(msg, "TypeError");
    }

    intptr_t n = api->to_int(argv[i]);

    if (n < lo || n > hi) {
        snprintf(msg, sizeof msg, "%s must be from %lld to %lld", what, (long long)lo, (long long)hi);
        api->raise(msg, "ArgumentError");
    }

    return n;
}

//------------------------------------------------------------------------
// compression level: 0 (store) .. 9 (best), and miniz's 10 (slowest, smallest)
//------------------------------------------------------------------------
static int arg_level(int argc, ClayPluginValue *argv, int i) {
    return (int)arg_int(argc, argv, i, "level", 0, MZ_UBER_COMPRESSION);
}

//========================================================================
// Zlib: whole-buffer compression and checksums
//========================================================================

//------------------------------------------------------------------------
// a growable output buffer
//------------------------------------------------------------------------
typedef struct {
    unsigned char *p;
    size_t len, cap;
} Buf;

//------------------------------------------------------------------------
//
//------------------------------------------------------------------------
static int buf_reserve(Buf *b, size_t extra) {
    if (b->cap - b->len >= extra)
        return 1;

    size_t cap = b->cap ? b->cap : 4096;
    while (cap - b->len < extra) {
        if (cap > SIZE_MAX / 2)
            return 0;
        cap *= 2;
    }

    unsigned char *p = (unsigned char *)realloc(b->p, cap);
    if (!p)
        return 0;

    b->p = p;
    b->cap = cap;
    return 1;
}

//------------------------------------------------------------------------
//
//------------------------------------------------------------------------
static int buf_put(Buf *b, const void *data, size_t n) {
    if (!buf_reserve(b, n))
        return 0;
    memcpy(b->p + b->len, data, n);
    b->len += n;
    return 1;
}

//------------------------------------------------------------------------
//
//------------------------------------------------------------------------
static int buf_put_le32(Buf *b, mz_uint32 v) {
    unsigned char le[4] = { (unsigned char)v, (unsigned char)(v >> 8),
                            (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
    return buf_put(b, le, 4);
}

//------------------------------------------------------------------------
// the most miniz's stream API takes or gives in one go (avail_in/out 
// are 32-bit)
//------------------------------------------------------------------------
#define STREAM_CHUNK ((size_t)1 << 30)

//------------------------------------------------------------------------
// deflate src into b (appending), as zlib (window_bits 15) or raw (-15)
// Returns NULL on success, else a message.
//------------------------------------------------------------------------
static const char *deflate_into(Buf *b, const unsigned char *src, size_t n, int level, int window_bits) {
    mz_stream s;
    memset(&s, 0, sizeof s);

    if (mz_deflateInit2(&s, level, MZ_DEFLATED, window_bits, 9, MZ_DEFAULT_STRATEGY) != MZ_OK)
        return "could not start compressing";

    const char *err = NULL;
    size_t pos = 0;

    for (;;) {
        size_t in = n - pos < STREAM_CHUNK ? n - pos : STREAM_CHUNK;
        int last = (pos + in == n);

        s.next_in = src + pos;
        s.avail_in = (unsigned int)in;

        // run until this chunk is consumed (and, for the last one, the stream ends)
        int status;
        do {
            size_t room = (size_t)mz_deflateBound(&s, s.avail_in) + 64;
            if (room > STREAM_CHUNK)
                room = STREAM_CHUNK;
            if (!buf_reserve(b, room)) {
                err = "out of memory";
                goto done;
            }

            s.next_out = b->p + b->len;
            s.avail_out = (unsigned int)room;

            status = mz_deflate(&s, last ? MZ_FINISH : MZ_NO_FLUSH);
            b->len += room - s.avail_out;

            if (status != MZ_OK && status != MZ_STREAM_END && status != MZ_BUF_ERROR) {
                err = "compression failed";
                goto done;
            }
        } while (last ? status != MZ_STREAM_END : s.avail_in > 0);

        pos += in;
        if (last)
            break;
    }

done:
    mz_deflateEnd(&s);
    return err;
}

//------------------------------------------------------------------------
// inflate one stream from src[0..n) into b (appending), zlib (15) or raw
// (-15). *used is set to the bytes the stream took, so a caller can find
// what follows it. max > 0 caps the output b holds in total.
// Returns NULL on success, else a message.
//------------------------------------------------------------------------
static const char *inflate_into(Buf *b, const unsigned char *src, size_t n, int window_bits,
                                size_t max, size_t *used) {
    mz_stream s;
    memset(&s, 0, sizeof s);

    if (mz_inflateInit2(&s, window_bits) != MZ_OK)
        return "could not start decompressing";

    const char *err = NULL;
    size_t pos = 0;

    for (;;) {
        // feed input a chunk at a time
        if (s.avail_in == 0 && pos < n) {
            size_t in = n - pos < STREAM_CHUNK ? n - pos : STREAM_CHUNK;
            s.next_in = src + pos;
            s.avail_in = (unsigned int)in;
            pos += in;
        }

        if (max && b->len >= max) {
            // full: one more byte of output means it is too big
            unsigned char probe;
            s.next_out = &probe;
            s.avail_out = 1;
            int status = mz_inflate(&s, MZ_NO_FLUSH);
            if (status == MZ_STREAM_END && s.avail_out == 1)
                break;
            if (s.avail_out == 0) {
                err = "the decompressed data is larger than the limit";
                break;
            }
            if (status == MZ_DATA_ERROR) {
                err = "the data is corrupt";
                break;
            }
            if (s.avail_in == 0 && pos == n) {
                err = "the data is truncated";
                break;
            }
            continue;
        }

        size_t room = b->cap - b->len;
        if (room < 4096) {
            if (!buf_reserve(b, b->len < 4096 ? 4096 : b->len)) {
                err = "out of memory";
                break;
            }
            room = b->cap - b->len;
        }
        if (room > STREAM_CHUNK)
            room = STREAM_CHUNK;
        if (max && room > max - b->len)
            room = max - b->len;

        s.next_out = b->p + b->len;
        s.avail_out = (unsigned int)room;

        int status = mz_inflate(&s, MZ_NO_FLUSH);
        b->len += room - s.avail_out;

        if (status == MZ_STREAM_END)
            break;

        if (status == MZ_DATA_ERROR) {
            err = "the data is corrupt";
            break;
        }

        if (status != MZ_OK && status != MZ_BUF_ERROR) {
            err = "decompression failed";
            break;
        }

        // no progress possible: all input given and output had room
        if (s.avail_in == 0 && pos == n && s.avail_out > 0) {
            err = "the data is truncated";
            break;
        }
    }

    *used = (size_t)(s.next_in - src);

    mz_inflateEnd(&s);
    
    return err;
}

//------------------------------------------------------------------------
// gzip (RFC 1952): a header, raw deflate, then crc32 and size
//------------------------------------------------------------------------
static const char *gzip_into(Buf *b, const unsigned char *src, size_t n, int level) {
    // magic, deflate, no flags, no mtime, xfl (2: slowest, 4: fastest), OS unknown
    unsigned char hdr[10] = { 0x1f, 0x8b, 8, 0, 0, 0, 0, 0,
                              (unsigned char)(level >= 9 ? 2 : level == 1 ? 4 : 0), 255 };

    if (!buf_put(b, hdr, sizeof hdr))
        return "out of memory";

    const char *err = deflate_into(b, src, n, level, -MZ_DEFAULT_WINDOW_BITS);

    if (err)
        return err;

    mz_ulong crc = mz_crc32(MZ_CRC32_INIT, src, 0);

    for (size_t pos = 0; pos < n;) {
        size_t in = n - pos < STREAM_CHUNK ? n - pos : STREAM_CHUNK;
        crc = mz_crc32(crc, src + pos, in);
        pos += in;
    }

    if (!buf_put_le32(b, (mz_uint32)crc) || !buf_put_le32(b, (mz_uint32)n))
        return "out of memory";

    return NULL;
}

//------------------------------------------------------------------------
//
//------------------------------------------------------------------------
static mz_uint32 get_le32(const unsigned char *p) {
    return (mz_uint32)p[0] | ((mz_uint32)p[1] << 8) | ((mz_uint32)p[2] << 16) | ((mz_uint32)p[3] << 24);
}

//------------------------------------------------------------------------
//
//------------------------------------------------------------------------
static const char *gunzip_into(Buf *b, const unsigned char *src, size_t n, size_t max) {
    size_t pos = 0;
    int members = 0;

    // one member after another (`cat a.gz b.gz` is a valid gzip file); what
    // follows the last one is ignored if it isn't another member, as gzip does
    while (n - pos >= 2 && src[pos] == 0x1f && src[pos + 1] == 0x8b) {
        const unsigned char *m = src + pos;
        size_t left = n - pos;

        if (left < 18)
            return "the gzip data is truncated";

        if (m[2] != 8)
            return "the gzip data uses an unknown compression method";

        int flags = m[3];

        if (flags & 0xe0)
            return "the gzip header has reserved flags set";

        size_t h = 10;

        if (flags & 4) {                        // FEXTRA
            if (left < h + 2)
                return "the gzip data is truncated";

            h += 2 + (size_t)(m[h] | (m[h + 1] << 8));
        }

        if (flags & 8) {                        // FNAME
            while (h < left && m[h]) 
                h++;
            h++;
        }

        if (flags & 16) {                       // FCOMMENT
            while (h < left && m[h]) 
                h++;
            h++;
        }

        if (flags & 2)                          // FHCRC
            h += 2;
        
            if (h >= left)
            return "the gzip data is truncated";

        size_t start = b->len, used = 0;
        const char *err = inflate_into(b, m + h, left - h, -MZ_DEFAULT_WINDOW_BITS, max, &used);

        if (err)
            return err;

        size_t t = h + used;

        if (left - t < 8)
            return "the gzip data is truncated";

        mz_ulong crc = MZ_CRC32_INIT;

        for (size_t p = start; p < b->len;) {
            size_t in = b->len - p < STREAM_CHUNK ? b->len - p : STREAM_CHUNK;
            crc = mz_crc32(crc, b->p + p, in);
            p += in;
        }

        if (get_le32(m + t) != (mz_uint32)crc)
            return "the gzip data is corrupt (CRC mismatch)";

            if (get_le32(m + t + 4) != (mz_uint32)(b->len - start))
            return "the gzip data is corrupt (length mismatch)";

        pos += t + 8;
        members++;
    }

    if (members == 0)
        return "not gzip data";

    return NULL;
}

//------------------------------------------------------------------------
// ZipCore _compress(data, level, format) -> compressed String
//------------------------------------------------------------------------
static ClayPluginValue zlib_compress(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(self);

    size_t n = 0;
    const unsigned char *src = (const unsigned char *)arg_bytes(argc, argv, 0, "data", &n);
    int level = arg_level(argc, argv, 1);
    int format = (int)arg_int(argc, argv, 2, "format", FMT_ZLIB, FMT_GZIP);

    Buf b = { 0 };
    const char *err;

    if (format == FMT_GZIP)
        err = gzip_into(&b, src, n, level);
    else
        err = deflate_into(&b, src, n, level, format == FMT_RAW ? -MZ_DEFAULT_WINDOW_BITS : MZ_DEFAULT_WINDOW_BITS);

    if (err) {
        char msg[MSG_BUF];
        free(b.p);
        snprintf(msg, sizeof msg, "Zlib: %s", err);
    
        api->raise(msg, ZIP_ERROR);
    
        return CLAY_NIL;
    }

    ClayPluginValue out = api->make_bytes(b.p, b.len);
    
    free(b.p);

    return out;
}

//------------------------------------------------------------------------
// ZipCore _decompress(data, format, max) -> decompressed String
// max 0: no limit
//------------------------------------------------------------------------
static ClayPluginValue zlib_decompress(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(self);

    size_t n = 0;
    const unsigned char *src = (const unsigned char *)arg_bytes(argc, argv, 0, "data", &n);
    int format = (int)arg_int(argc, argv, 1, "format", FMT_ZLIB, FMT_GZIP);
    size_t max = (size_t)arg_int(argc, argv, 2, "max", 0, INTPTR_MAX);

    Buf b = { 0 };
    const char *err;

    if (format == FMT_GZIP) {
        err = gunzip_into(&b, src, n, max);
    } else {
        size_t used = 0;
        err = inflate_into(&b, src, n, format == FMT_RAW ? -MZ_DEFAULT_WINDOW_BITS : MZ_DEFAULT_WINDOW_BITS,
                           max, &used);
        if (!err && used != n)
            err = "there is data after the end of the compressed stream";
    }

    if (err) {
        char msg[MSG_BUF];
        free(b.p);
        snprintf(msg, sizeof msg, "Zlib: %s", err);
        api->raise(msg, ZIP_ERROR);
        return CLAY_NIL;
    }

    ClayPluginValue out = api->make_bytes(b.p, b.len);

    free(b.p);
    
    return out;
}

//------------------------------------------------------------------------
// ZipCore _crc32(data, start) / _adler32(data, start) -> integer
//------------------------------------------------------------------------
static ClayPluginValue zlib_checksum(int argc, ClayPluginValue *argv, int adler) {
    size_t n = 0;
    const unsigned char *src = (const unsigned char *)arg_bytes(argc, argv, 0, "data", &n);
    mz_ulong sum = (mz_ulong)arg_int(argc, argv, 1, adler ? "adler" : "crc", 0, (intptr_t)0xffffffffu);

    for (size_t pos = 0; pos < n;) {
        size_t in = n - pos < STREAM_CHUNK ? n - pos : STREAM_CHUNK;
        sum = adler ? mz_adler32(sum, src + pos, in) : mz_crc32(sum, src + pos, in);
        pos += in;
    }

    return api->make_int((intptr_t)(sum & 0xffffffffu));
}

//------------------------------------------------------------------------
//
//------------------------------------------------------------------------
static ClayPluginValue zlib_crc32(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(self);

    return zlib_checksum(argc, argv, 0);
}

//------------------------------------------------------------------------
//
//------------------------------------------------------------------------
static ClayPluginValue zlib_adler32(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(self);

    return zlib_checksum(argc, argv, 1);
}

//========================================================================
// Zip archives
//========================================================================

//------------------------------------------------------------------------
// ZipCore _new -> a new Zip, not yet open; self is the prototype
//------------------------------------------------------------------------
static ClayPluginValue zip_new(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(argc); CLAY_UNUSED(argv);

    ZipData *z = (ZipData *)calloc(1, sizeof(ZipData));

    if (!z) {
        api->raise("Zip: out of memory", "RuntimeError");
        return CLAY_NIL;
    }

    ClayPluginObject obj = api->clone(self);

    api->set_data(obj, z);

    return (ClayPluginValue)obj;
}

//------------------------------------------------------------------------
// a fresh Zip (from _new) about to be opened
//------------------------------------------------------------------------
static ZipData *zip_fresh(ClayPluginObject self) {
    ZipData *z = zip_of(self, MODE_NONE);

    if (z->mode != MODE_NONE)
        api->raise("Zip: the archive is already open", ZIP_ERROR);

    mz_zip_zero_struct(&z->zip);

    return z;
}

//------------------------------------------------------------------------
// zip _openFile(path) -> self, reading the archive at path
//------------------------------------------------------------------------
static ClayPluginValue zip_open_file(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    ZipData *z = zip_fresh(self);
    const char *path = arg_cstring(argc, argv, 0, "path");

    if (!mz_zip_reader_init_file(&z->zip, path, 0)) {
        char what[MSG_BUF];
        snprintf(what, sizeof what, "cannot open %s", path);
        z->mode = MODE_CLOSED;
        raise_zip_error(&z->zip, what);
    }

    z->mode = MODE_READ;

    return (ClayPluginValue)self;
}

//------------------------------------------------------------------------
// zip _openBytes(data) -> self, reading an archive held in a String
// (copied, so the archive does not depend on the String)
//------------------------------------------------------------------------
static ClayPluginValue zip_open_bytes(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    ZipData *z = zip_fresh(self);
    size_t n = 0;
    const char *src = arg_bytes(argc, argv, 0, "data", &n);

    z->mem = malloc(n ? n : 1);
    if (!z->mem) {
        api->raise("Zip: out of memory", "RuntimeError");
        return CLAY_NIL;
    }
    memcpy(z->mem, src, n);

    if (!mz_zip_reader_init_mem(&z->zip, z->mem, n, 0)) {
        free(z->mem);
        z->mem = NULL;
        z->mode = MODE_CLOSED;

        raise_zip_error(&z->zip, "cannot read the archive");
    }

    z->mode = MODE_READ;

    return (ClayPluginValue)self;
}

//------------------------------------------------------------------------
// zip _createFile(path) -> self, writing a new archive at path
//------------------------------------------------------------------------
static ClayPluginValue zip_create_file(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    ZipData *z = zip_fresh(self);
    const char *path = arg_cstring(argc, argv, 0, "path");

    if (!mz_zip_writer_init_file(&z->zip, path, 0)) {
        char what[MSG_BUF];
        snprintf(what, sizeof what, "cannot create %s", path);
        z->mode = MODE_CLOSED;

        raise_zip_error(&z->zip, what);
    }

    z->mode = MODE_WRITE;

    return (ClayPluginValue)self;
}

//------------------------------------------------------------------------
// zip _createBytes -> self, writing a new archive in memory
//------------------------------------------------------------------------
static ClayPluginValue zip_create_bytes(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(argc); CLAY_UNUSED(argv);

    ZipData *z = zip_fresh(self);

    if (!mz_zip_writer_init_heap(&z->zip, 0, 0)) {
        z->mode = MODE_CLOSED;
        raise_zip_error(&z->zip, "cannot create the archive");
    }

    z->mode = MODE_WRITE;
    z->in_memory = 1;
    return (ClayPluginValue)self;
}

//------------------------------------------------------------------------
// zip _mode -> "reading", "writing", "closed" (or nil: not opened)
//------------------------------------------------------------------------
static ClayPluginValue zip_mode(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(argc); CLAY_UNUSED(argv);

    ZipData *z = (ZipData *)api->get_data(self);

    if (!z || z->mode == MODE_NONE)
        return CLAY_NIL;

    return api->make_string(z->mode == MODE_READ ? "reading" : z->mode == MODE_WRITE ? "writing" : "closed");
}

//------------------------------------------------------------------------
// zip _inMemory -> true if the archive is being written to memory
//------------------------------------------------------------------------
static ClayPluginValue zip_in_memory(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(argc); CLAY_UNUSED(argv);

    ZipData *z = (ZipData *)api->get_data(self);
    
    return api->make_bool(z && z->in_memory);
}

//------------------------------------------------------------------------
// reading
//------------------------------------------------------------------------

// argv[i] as an entry index of the archive
static mz_uint arg_index(ZipData *z, int argc, ClayPluginValue *argv, int i) {
    mz_uint count = mz_zip_reader_get_num_files(&z->zip);

    if (count == 0)
        api->raise("Zip: the archive has no entries", ZIP_ERROR);

    return (mz_uint)arg_int(argc, argv, i, "index", 0, (intptr_t)count - 1);
}

//------------------------------------------------------------------------
// zip count -> how many entries the archive has
//------------------------------------------------------------------------
static ClayPluginValue zip_count(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(argc); CLAY_UNUSED(argv);

    ZipData *z = zip_of(self, MODE_READ);
    
    return api->make_int((intptr_t)mz_zip_reader_get_num_files(&z->zip));
}

//------------------------------------------------------------------------
// zip _find(name) -> the index of the entry called name (case-sensitive), or -1
//------------------------------------------------------------------------
static ClayPluginValue zip_find(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    ZipData *z = zip_of(self, MODE_READ);
    const char *name = arg_cstring(argc, argv, 0, "name");

    int i = mz_zip_reader_locate_file(&z->zip, name, NULL, MZ_ZIP_FLAG_CASE_SENSITIVE);

    return api->make_int(i);
}

//------------------------------------------------------------------------
// dict_set, and release the value (dict_set retains it)
//------------------------------------------------------------------------
static void dict_put(ClayPluginValue d, const char *key, ClayPluginValue v) {
    api->dict_set(d, key, v);
    api->release(v);
}

//------------------------------------------------------------------------
// zip _entry(index) -> #{name:, size:, compressedSize:, isDir:, modified:,
//                        crc32:, encrypted:, comment:, index:}
//------------------------------------------------------------------------
static ClayPluginValue zip_entry(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    ZipData *z = zip_of(self, MODE_READ);
    mz_uint i = arg_index(z, argc, argv, 0);
    mz_zip_archive_file_stat st;

    if (!mz_zip_reader_file_stat(&z->zip, i, &st))
        raise_zip_error(&z->zip, "cannot read an entry");

    ClayPluginValue d = api->dict_new();
    dict_put(d, "name", api->make_string(st.m_filename));
    dict_put(d, "size", api->make_int((intptr_t)st.m_uncomp_size));
    dict_put(d, "compressedSize", api->make_int((intptr_t)st.m_comp_size));
    dict_put(d, "isDir", api->make_bool(st.m_is_directory));

#ifndef MINIZ_NO_TIME
    dict_put(d, "modified", api->make_int((intptr_t)st.m_time));
#endif
    
dict_put(d, "crc32", api->make_int((intptr_t)st.m_crc32));
    dict_put(d, "encrypted", api->make_bool(st.m_is_encrypted));
    dict_put(d, "comment", api->make_bytes(st.m_comment, st.m_comment_size));
    dict_put(d, "index", api->make_int((intptr_t)i));

    return d;
}

//------------------------------------------------------------------------
// zip _read(index) -> the entry's contents, as a String
//------------------------------------------------------------------------
static ClayPluginValue zip_read(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    ZipData *z = zip_of(self, MODE_READ);

    mz_uint i = arg_index(z, argc, argv, 0);
    size_t n = 0;

    void *p = mz_zip_reader_extract_to_heap(&z->zip, i, &n, 0);
    
    if (!p) {
        char what[MSG_BUF], name[MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE];
        
        mz_zip_reader_get_filename(&z->zip, i, name, sizeof name);
        
        snprintf(what, sizeof what, "cannot read %s", name);

        raise_zip_error(&z->zip, what);
    }

    ClayPluginValue out = api->make_bytes(p, n);
    z->zip.m_pFree(z->zip.m_pAlloc_opaque, p);

    return out;
}

//------------------------------------------------------------------------
// zip _extract(index, path) -> path: writes the entry to a file
//------------------------------------------------------------------------
static ClayPluginValue zip_extract(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    ZipData *z = zip_of(self, MODE_READ);

    mz_uint i = arg_index(z, argc, argv, 0);
    const char *path = arg_cstring(argc, argv, 1, "path");

    if (!mz_zip_reader_extract_to_file(&z->zip, i, path, 0)) {
        char what[MSG_BUF];
        snprintf(what, sizeof what, "cannot extract to %s", path);

        raise_zip_error(&z->zip, what);
    }

    api->retain(argv[1]);

    return argv[1];
}

//------------------------------------------------------------------------
// writing
//------------------------------------------------------------------------

//------------------------------------------------------------------------
// zip _add(name, data, level) -> self: adds an entry holding data. A name
// ending in "/" (with empty data) is a directory.
//------------------------------------------------------------------------
static ClayPluginValue zip_add(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    ZipData *z = zip_of(self, MODE_WRITE);

    const char *name = arg_cstring(argc, argv, 0, "name");
    size_t n = 0;
    const char *data = arg_bytes(argc, argv, 1, "data", &n);
    int level = arg_level(argc, argv, 2);

    if (!mz_zip_writer_add_mem(&z->zip, name, data, n, (mz_uint)level)) {
        char what[MSG_BUF];
        snprintf(what, sizeof what, "cannot add %s", name);

        raise_zip_error(&z->zip, what);
    }

    return (ClayPluginValue)self;
}

//------------------------------------------------------------------------
// zip _addFile(name, path, level) -> self: adds the file at path as name,
// streaming it from disk and keeping its modification time
//------------------------------------------------------------------------
static ClayPluginValue zip_add_file(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    ZipData *z = zip_of(self, MODE_WRITE);

    const char *name = arg_cstring(argc, argv, 0, "name");
    const char *path = arg_cstring(argc, argv, 1, "path");
    int level = arg_level(argc, argv, 2);

    if (!mz_zip_writer_add_file(&z->zip, name, path, NULL, 0, (mz_uint)level)) {
        char what[MSG_BUF];
        snprintf(what, sizeof what, "cannot add %s", path);
        raise_zip_error(&z->zip, what);
    }

    return (ClayPluginValue)self;
}

//------------------------------------------------------------------------
// zip _finish -> finishes the archive and closes it. Returns the archive's
// bytes when it was written to memory, else nil.
//------------------------------------------------------------------------
static ClayPluginValue zip_finish(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(argc); CLAY_UNUSED(argv);

    ZipData *z = zip_of(self, MODE_WRITE);

    if (z->in_memory) {
        void *p = NULL;
        size_t n = 0;

        if (!mz_zip_writer_finalize_heap_archive(&z->zip, &p, &n)) {
            char msg[MSG_BUF];
            snprintf(msg, sizeof msg, "Zip: cannot finish the archive: %s",
                     mz_zip_get_error_string(mz_zip_get_last_error(&z->zip)));

            zipdata_end(z);

            api->raise(msg, ZIP_ERROR);
        }

        ClayPluginValue out = api->make_bytes(p, n);
        z->zip.m_pFree(z->zip.m_pAlloc_opaque, p);

        zipdata_end(z);

        return out;
    }

    if (!mz_zip_writer_finalize_archive(&z->zip)) {
        char msg[MSG_BUF];
        snprintf(msg, sizeof msg, "Zip: cannot finish the archive: %s",
                 mz_zip_get_error_string(mz_zip_get_last_error(&z->zip)));
 
        mz_zip_writer_end(&z->zip);
        z->mode = MODE_CLOSED;
 
        api->raise(msg, ZIP_ERROR);
    }

    // end it (finalize again in zipdata_end would be a harmless error)
    mz_zip_writer_end(&z->zip);

    z->mode = MODE_CLOSED;

    return CLAY_NIL;
}

//------------------------------------------------------------------------
// zip _close -> nil: closes a reader (or abandons nothing: a writer is
// finished by _finish). Closing twice is harmless.
//------------------------------------------------------------------------
static ClayPluginValue zip_close(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(argc); CLAY_UNUSED(argv);

    ZipData *z = zip_of(self, MODE_NONE);

    if (z->mode == MODE_READ)
        zipdata_end(z);

    return CLAY_NIL;
}

//------------------------------------------------------------------------
// ZipCore _raise(message, type): lets the Clay wrapper raise a typed error
// (ZipError, ArgumentError), which Clay's raise cannot
//------------------------------------------------------------------------
static ClayPluginValue zip_raise(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(self);

    const char *msg = arg_cstring(argc, argv, 0, "message");
    const char *type = arg_cstring(argc, argv, 1, "type");

    api->raise(msg, type);
    
    return CLAY_NIL;
}

//------------------------------------------------------------------------
// ZipCore methods
//------------------------------------------------------------------------
static ClayPluginMethod methods[] = {
    // Zlib (stateless)
    { "_compress",    zlib_compress },
    { "_decompress",  zlib_decompress },
    { "_crc32",       zlib_crc32 },
    { "_adler32",     zlib_adler32 },

    // archives
    { "_new",         zip_new },
    { "_openFile",    zip_open_file },
    { "_openBytes",   zip_open_bytes },
    { "_createFile",  zip_create_file },
    { "_createBytes", zip_create_bytes },
    { "_mode",        zip_mode },
    { "_inMemory",    zip_in_memory },
    { "count",        zip_count },
    { "_find",        zip_find },
    { "_entry",       zip_entry },
    { "_read",        zip_read },
    { "_extract",     zip_extract },
    { "_add",         zip_add },
    { "_addFile",     zip_add_file },
    { "_finish",      zip_finish },
    { "_close",       zip_close },
    { "_raise",       zip_raise },
    { NULL, NULL }
};

//------------------------------------------------------------------------
// Register our prototype
//------------------------------------------------------------------------
CLAY_PLUGIN_EXPORT void zip_plugin_init(const ClayPluginAPI *clay_api) {
    api = clay_api;

    if (api->api_version < MIN_API_VERSION) {
        api->raise("zip: this Clay is too old for the zip brick (plugin API 5 or newer is needed)", "RuntimeError");
        return;
    }

    ClayPluginType def = { "ZipCore", methods, zip_free, 0 };
    api->register_type(&def);
}
