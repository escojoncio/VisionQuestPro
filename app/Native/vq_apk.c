// VisionQuestPro: see vq_apk.h.
#include "vq_apk.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

static void seterr(char *err, size_t n, const char *fmt, ...) {
    if (!err || !n) return;
    va_list a; va_start(a, fmt); vsnprintf(err, n, fmt, a); va_end(a);
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static int pread_all(int fd, void *buf, size_t n, uint64_t off) {
    uint8_t *p = buf;
    while (n) {
        ssize_t r = pread(fd, p, n, (off_t)off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r; n -= (size_t)r; off += (uint64_t)r;
    }
    return 0;
}

typedef struct {
    char *name;
    uint64_t csize, usize, local_off;
    uint32_t crc;
    uint16_t method;
} VqEntry;

struct VqZip {
    int fd;
    int n;
    VqEntry *e;
};

VqZip *vq_zip_open(const char *path, char *err, size_t err_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { seterr(err, err_len, "open %s: %s", path, strerror(errno)); return NULL; }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 22) { close(fd); seterr(err, err_len, "%s is not a ZIP", path); return NULL; }
    uint64_t size = (uint64_t)st.st_size;
    // The end of central directory record is in the last 64 KiB + 22 bytes.
    size_t tail = size < 65557 ? (size_t)size : 65557;
    uint8_t *t = malloc(tail);
    if (!t || pread_all(fd, t, tail, size - tail)) { free(t); close(fd); seterr(err, err_len, "cannot read %s", path); return NULL; }
    long eocd = -1;
    for (long i = (long)tail - 22; i >= 0; i--)
        if (rd32(t + i) == 0x06054b50u) { eocd = i; break; }
    if (eocd < 0) { free(t); close(fd); seterr(err, err_len, "%s: no end of central directory", path); return NULL; }
    uint64_t count = rd16(t + eocd + 10), cd_size = rd32(t + eocd + 12), cd_off = rd32(t + eocd + 16);
    // ZIP64: the locator sits just before the EOCD.
    if (eocd >= 20 && rd32(t + eocd - 20) == 0x07064b50u) {
        uint64_t z64 = rd64(t + eocd - 20 + 8);
        uint8_t r[56];
        if (!pread_all(fd, r, sizeof r, z64) && rd32(r) == 0x06064b50u) {
            count = rd64(r + 32); cd_size = rd64(r + 40); cd_off = rd64(r + 48);
        }
    }
    free(t);
    if (cd_off + cd_size > size || count > 1000000 || cd_size > (256u << 20)) {
        close(fd); seterr(err, err_len, "%s: bad central directory", path); return NULL;
    }
    uint8_t *cd = malloc((size_t)cd_size + 1);
    VqZip *z = calloc(1, sizeof *z);
    if (z) z->e = calloc((size_t)count + 1, sizeof *z->e);
    if (!cd || !z || !z->e || pread_all(fd, cd, (size_t)cd_size, cd_off)) {
        free(cd); if (z) free(z->e); free(z); close(fd);
        seterr(err, err_len, "cannot read the central directory of %s", path); return NULL;
    }
    z->fd = fd;
    uint64_t p = 0;
    for (uint64_t i = 0; i < count; i++) {
        if (p + 46 > cd_size || rd32(cd + p) != 0x02014b50u) break;
        const uint8_t *h = cd + p;
        uint16_t nlen = rd16(h + 28), xlen = rd16(h + 30), clen = rd16(h + 32);
        if (p + 46 + nlen + xlen + clen > cd_size) break;
        VqEntry *e = &z->e[z->n];
        e->method = rd16(h + 10);
        e->crc = rd32(h + 16);
        e->csize = rd32(h + 20); e->usize = rd32(h + 24); e->local_off = rd32(h + 42);
        // ZIP64 extra field: the 0xffffffff fields, in order.
        const uint8_t *x = h + 46 + nlen, *xe = x + xlen;
        while (x + 4 <= xe) {
            uint16_t id = rd16(x), sz = rd16(x + 2);
            if (x + 4 + sz > xe) break;
            if (id == 1) {
                const uint8_t *q = x + 4, *qe = q + sz;
                if (e->usize == 0xffffffffu && q + 8 <= qe) { e->usize = rd64(q); q += 8; }
                if (e->csize == 0xffffffffu && q + 8 <= qe) { e->csize = rd64(q); q += 8; }
                if (e->local_off == 0xffffffffu && q + 8 <= qe) { e->local_off = rd64(q); q += 8; }
            }
            x += 4 + sz;
        }
        e->name = strndup((const char *)h + 46, nlen);
        if (!e->name) break;
        z->n++;
        p += 46 + nlen + xlen + clen;
    }
    free(cd);
    return z;
}

void vq_zip_close(VqZip *z) {
    if (!z) return;
    for (int i = 0; i < z->n; i++) free(z->e[i].name);
    free(z->e);
    close(z->fd);
    free(z);
}

int vq_zip_count(const VqZip *z) { return z ? z->n : 0; }
const char *vq_zip_name(const VqZip *z, int i) { return z && i >= 0 && i < z->n ? z->e[i].name : NULL; }
uint64_t vq_zip_size(const VqZip *z, int i) { return z && i >= 0 && i < z->n ? z->e[i].usize : 0; }
int vq_zip_find(const VqZip *z, const char *name) {
    if (!z || !name) return -1;
    for (int i = 0; i < z->n; i++) if (!strcmp(z->e[i].name, name)) return i;
    return -1;
}

// Streams entry `i` to `sink` in chunks. 0 on success.
typedef int (*vq_sink)(void *ctx, const uint8_t *p, size_t n);
static int zip_stream(VqZip *z, int i, vq_sink sink, void *ctx, char *err, size_t err_len) {
    if (!z || i < 0 || i >= z->n) { seterr(err, err_len, "no such entry"); return -1; }
    const VqEntry *e = &z->e[i];
    uint8_t lh[30];
    if (pread_all(z->fd, lh, 30, e->local_off) || rd32(lh) != 0x04034b50u) {
        seterr(err, err_len, "%s: bad local header", e->name); return -1;
    }
    uint64_t data = e->local_off + 30 + rd16(lh + 26) + rd16(lh + 28);
    enum { CH = 1 << 18 };
    uint8_t *in = malloc(CH), *out = malloc(CH);
    if (!in || !out) { free(in); free(out); seterr(err, err_len, "out of memory"); return -1; }
    uLong crc = crc32(0, Z_NULL, 0);
    uint64_t left = e->csize, produced = 0;
    int rc = 0;
    if (e->method == 0) {
        while (left && !rc) {
            size_t n = left < CH ? (size_t)left : CH;
            if (pread_all(z->fd, in, n, data)) { seterr(err, err_len, "%s: read error", e->name); rc = -1; break; }
            crc = crc32(crc, in, (uInt)n);
            if (sink(ctx, in, n)) { seterr(err, err_len, "%s: write error: %s", e->name, strerror(errno)); rc = -1; }
            data += n; left -= n; produced += n;
        }
    } else if (e->method == 8) {
        z_stream s; memset(&s, 0, sizeof s);
        if (inflateInit2(&s, -MAX_WBITS) != Z_OK) { free(in); free(out); seterr(err, err_len, "inflateInit"); return -1; }
        int zr = Z_OK;
        while (zr != Z_STREAM_END && !rc) {
            if (s.avail_in == 0) {
                if (!left) { seterr(err, err_len, "%s: truncated deflate data", e->name); rc = -1; break; }
                size_t n = left < CH ? (size_t)left : CH;
                if (pread_all(z->fd, in, n, data)) { seterr(err, err_len, "%s: read error", e->name); rc = -1; break; }
                data += n; left -= n;
                s.next_in = in; s.avail_in = (uInt)n;
            }
            s.next_out = out; s.avail_out = CH;
            zr = inflate(&s, Z_NO_FLUSH);
            if (zr != Z_OK && zr != Z_STREAM_END) { seterr(err, err_len, "%s: inflate %d", e->name, zr); rc = -1; break; }
            size_t got = CH - s.avail_out;
            crc = crc32(crc, out, (uInt)got);
            produced += got;
            if (got && sink(ctx, out, got)) { seterr(err, err_len, "%s: write error: %s", e->name, strerror(errno)); rc = -1; }
        }
        inflateEnd(&s);
    } else {
        seterr(err, err_len, "%s: compression method %u not supported", e->name, e->method); rc = -1;
    }
    free(in); free(out);
    if (!rc && (produced != e->usize || (uint32_t)crc != e->crc)) {
        seterr(err, err_len, "%s: CRC/size mismatch", e->name); rc = -1;
    }
    return rc;
}

static int sink_fd(void *ctx, const uint8_t *p, size_t n) {
    int fd = *(int *)ctx;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}

int vq_zip_extract(VqZip *z, int i, const char *out, char *err, size_t err_len) {
    int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { seterr(err, err_len, "create %s: %s", out, strerror(errno)); return -1; }
    int rc = zip_stream(z, i, sink_fd, &fd, err, err_len);
    if (close(fd) != 0 && !rc) { seterr(err, err_len, "close %s: %s", out, strerror(errno)); rc = -1; }
    if (rc) unlink(out);
    return rc;
}

typedef struct { uint8_t *p; size_t n, cap; } VqBuf;
static int sink_buf(void *ctx, const uint8_t *p, size_t n) {
    VqBuf *b = ctx;
    if (b->n + n > b->cap) return -1;
    memcpy(b->p + b->n, p, n); b->n += n;
    return 0;
}

uint8_t *vq_zip_read(VqZip *z, int i, size_t max, size_t *len, char *err, size_t err_len) {
    if (!z || i < 0 || i >= z->n) { seterr(err, err_len, "no such entry"); return NULL; }
    if (z->e[i].usize > max) { seterr(err, err_len, "%s is too large", z->e[i].name); return NULL; }
    VqBuf b = { malloc((size_t)z->e[i].usize + 1), 0, (size_t)z->e[i].usize };
    if (!b.p) { seterr(err, err_len, "out of memory"); return NULL; }
    if (zip_stream(z, i, sink_buf, &b, err, err_len)) { free(b.p); return NULL; }
    b.p[b.n] = 0;
    if (len) *len = b.n;
    return b.p;
}

// ---- Android binary XML ----

typedef struct { char *p; size_t n, cap; int oom; } VqText;
static void tx_put(VqText *t, const char *s, size_t n) {
    if (t->oom) return;
    if (t->n + n + 1 > t->cap) {
        size_t cap = t->cap ? t->cap : 4096;
        while (t->n + n + 1 > cap) cap *= 2;
        char *q = realloc(t->p, cap);
        if (!q) { t->oom = 1; return; }
        t->p = q; t->cap = cap;
    }
    memcpy(t->p + t->n, s, n); t->n += n; t->p[t->n] = 0;
}
static void tx_str(VqText *t, const char *s) { tx_put(t, s, strlen(s)); }
static void tx_esc(VqText *t, const char *s) {
    for (; s && *s; s++) {
        switch (*s) {
        case '&': tx_str(t, "&amp;"); break;
        case '<': tx_str(t, "&lt;"); break;
        case '>': tx_str(t, "&gt;"); break;
        case '"': tx_str(t, "&quot;"); break;
        default: tx_put(t, s, 1);
        }
    }
}

typedef struct { char **s; uint32_t n; } VqPool;

// One string of the pool, as UTF-8 (malloc'd).
static char *pool_string(const uint8_t *chunk, size_t clen, uint32_t idx, int utf8, uint32_t strings_start, const uint8_t *offs) {
    uint32_t o = rd32(offs + idx * 4);
    size_t at = (size_t)strings_start + o;
    if (at >= clen) return strdup("");
    const uint8_t *p = chunk + at, *end = chunk + clen;
    if (utf8) {
        // UTF-16 length (1 or 2 bytes), then UTF-8 length (1 or 2 bytes), then bytes.
        if (p < end && (*p & 0x80)) p += 2; else p += 1;
        if (p >= end) return strdup("");
        size_t n = *p;
        if (*p & 0x80) { if (p + 1 >= end) return strdup(""); n = ((size_t)(p[0] & 0x7f) << 8) | p[1]; p += 2; } else p += 1;
        if (p + n > end) n = (size_t)(end - p);
        return strndup((const char *)p, n);
    }
    if (p + 2 > end) return strdup("");
    size_t n = rd16(p); p += 2;
    if (n & 0x8000) { if (p + 2 > end) return strdup(""); n = ((n & 0x7fff) << 16) | rd16(p); p += 2; }
    if (p + n * 2 > end) n = (size_t)(end - p) / 2;
    char *out = malloc(n * 3 + 1);
    if (!out) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = rd16(p + i * 2);
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n) {
            uint32_t d = rd16(p + (i + 1) * 2);
            if (d >= 0xdc00 && d < 0xe000) { c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00); i++; }
        }
        if (c < 0x80) out[k++] = (char)c;
        else if (c < 0x800) { out[k++] = (char)(0xc0 | c >> 6); out[k++] = (char)(0x80 | (c & 0x3f)); }
        else if (c < 0x10000) { out[k++] = (char)(0xe0 | c >> 12); out[k++] = (char)(0x80 | ((c >> 6) & 0x3f)); out[k++] = (char)(0x80 | (c & 0x3f)); }
        else {
            char *o2 = realloc(out, n * 3 + k + 5); if (!o2) { free(out); return NULL; } out = o2;
            out[k++] = (char)(0xf0 | c >> 18); out[k++] = (char)(0x80 | ((c >> 12) & 0x3f));
            out[k++] = (char)(0x80 | ((c >> 6) & 0x3f)); out[k++] = (char)(0x80 | (c & 0x3f));
        }
    }
    out[k] = 0;
    return out;
}

static const char *ps(const VqPool *pool, uint32_t i) { return i < pool->n && pool->s[i] ? pool->s[i] : ""; }

char *vq_axml_to_text(const uint8_t *d, size_t len) {
    if (!d || len < 8 || rd16(d) != 0x0003) return NULL;
    VqPool pool = {0};
    VqText t = {0};
    // Namespace prefixes by URI string index.
    uint32_t ns_uri[16], ns_pre[16]; int nns = 0;
    int pending_ns = 0;
    tx_str(&t, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
    size_t p = rd16(d + 2);
    int depth = 0;
    while (p + 8 <= len) {
        uint16_t type = rd16(d + p), hsize = rd16(d + p + 2);
        uint32_t csize = rd32(d + p + 4);
        if (csize < 8 || p + csize > len) break;
        const uint8_t *c = d + p;
        if (type == 0x0001 && !pool.s && csize >= 28) {             // string pool
            uint32_t count = rd32(c + 8), flags = rd32(c + 16), sstart = rd32(c + 20);
            if (count < 1000000 && 28 + (size_t)count * 4 <= csize) {
                pool.s = calloc(count ? count : 1, sizeof *pool.s);
                if (pool.s) {
                    pool.n = count;
                    for (uint32_t i = 0; i < count; i++)
                        pool.s[i] = pool_string(c, csize, i, (flags & 0x100) != 0, sstart, c + hsize);
                }
            }
        } else if (type == 0x0100 && csize >= 24) {                  // start namespace
            if (nns < 16) { ns_pre[nns] = rd32(c + 16); ns_uri[nns] = rd32(c + 20); nns++; pending_ns = 1; }
        } else if (type == 0x0102 && csize >= 36) {                  // start element
            uint32_t name = rd32(c + 20);
            uint16_t astart = rd16(c + 24), asize = rd16(c + 26), acount = rd16(c + 28);
            for (int k = 0; k < depth; k++) tx_str(&t, "  ");
            tx_str(&t, "<"); tx_str(&t, ps(&pool, name));
            if (pending_ns) {
                for (int k = 0; k < nns; k++) {
                    tx_str(&t, " xmlns:"); tx_str(&t, ps(&pool, ns_pre[k]));
                    tx_str(&t, "=\""); tx_esc(&t, ps(&pool, ns_uri[k])); tx_str(&t, "\"");
                }
                pending_ns = 0;
            }
            const uint8_t *a = c + 16 + astart;
            if (asize < 20) asize = 20;
            for (uint16_t k = 0; k < acount; k++, a += asize) {
                if (a + 20 > c + csize) break;
                uint32_t ans = rd32(a), aname = rd32(a + 4), araw = rd32(a + 8);
                uint8_t dtype = a[15]; uint32_t data = rd32(a + 16);
                tx_str(&t, " ");
                if (ans != 0xffffffffu) {
                    const char *pre = NULL;
                    for (int q = 0; q < nns; q++) if (ns_uri[q] == ans) pre = ps(&pool, ns_pre[q]);
                    if (pre) { tx_str(&t, pre); tx_str(&t, ":"); }
                }
                tx_str(&t, ps(&pool, aname));
                tx_str(&t, "=\"");
                char num[64];
                if (araw != 0xffffffffu) tx_esc(&t, ps(&pool, araw));
                else if (dtype == 0x03) tx_esc(&t, ps(&pool, data));
                else if (dtype == 0x12) tx_str(&t, data ? "true" : "false");
                else if (dtype == 0x01) { snprintf(num, sizeof num, "@0x%08x", data); tx_str(&t, num); }
                else if (dtype == 0x11) { snprintf(num, sizeof num, "0x%x", data); tx_str(&t, num); }
                else if (dtype == 0x04) { float f; memcpy(&f, &data, 4); snprintf(num, sizeof num, "%g", f); tx_str(&t, num); }
                else { snprintf(num, sizeof num, "%d", (int32_t)data); tx_str(&t, num); }
                tx_str(&t, "\"");
            }
            tx_str(&t, ">\n");
            depth++;
        } else if (type == 0x0103 && csize >= 24) {                  // end element
            if (depth > 0) depth--;
            for (int k = 0; k < depth; k++) tx_str(&t, "  ");
            tx_str(&t, "</"); tx_str(&t, ps(&pool, rd32(c + 20))); tx_str(&t, ">\n");
        }
        p += csize;
    }
    for (uint32_t i = 0; i < pool.n; i++) free(pool.s[i]);
    free(pool.s);
    if (t.oom) { free(t.p); return NULL; }
    return t.p;
}
