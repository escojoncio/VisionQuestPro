// VisionQuestPro: SPIR-V harvest (see vq_harvest.h).
#include "vq_harvest.h"

#include "vq_apk.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define VQ_MAX_FILE ((size_t)512 << 20)  // larger single files are skipped (logged)

static uint32_t rd32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t rd32be(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t rd16be(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint64_t rd64be(const uint8_t *p) { return (uint64_t)rd32be(p) << 32 | rd32be(p + 4); }

// ---- LZ4 block -------------------------------------------------------------------------------
long vq_lz4_block(const uint8_t *src, size_t n, uint8_t *dst, size_t cap) {
  size_t i = 0, o = 0;
  while (i < n) {
    const uint8_t t = src[i++];
    size_t lit = t >> 4;
    if (lit == 15) {
      uint8_t b;
      do {
        if (i >= n) return -1;
        b = src[i++];
        lit += b;
      } while (b == 255);
    }
    if (lit > n - i || lit > cap - o) return -1;
    memcpy(dst + o, src + i, lit);
    i += lit, o += lit;
    if (i >= n) break;  // last sequence: literals only
    if (n - i < 2) return -1;
    const size_t off = src[i] | src[i + 1] << 8;
    i += 2;
    if (!off || off > o) return -1;
    size_t m = t & 15;
    if (m == 15) {
      uint8_t b;
      do {
        if (i >= n) return -1;
        b = src[i++];
        m += b;
      } while (b == 255);
    }
    m += 4;
    if (m > cap - o) return -1;
    for (size_t k = 0; k < m; ++k, ++o) dst[o] = dst[o - off];  // overlapping copy
  }
  return (long)o;
}

// ---- SPIR-V ----------------------------------------------------------------------------------
static int plausibleOp(uint32_t op) { return op < 450 || (op >= 4400 && op < 6500); }

size_t vq_spirv_length(const uint8_t *p, size_t avail) {
  if (avail < 40 || rd32le(p) != 0x07230203u) return 0;
  const uint32_t ver = rd32le(p + 4), bound = rd32le(p + 12), schema = rd32le(p + 16);
  if ((ver & 0xFF00FFFFu) || ver < 0x10000u || ver > 0x10600u || !bound || bound > 4000000u || schema) return 0;
  if ((rd32le(p + 20) & 0xFFFF) != 17) return 0;  // modules start with OpCapability
  size_t q = 20, end = 0;
  while (q + 4 <= avail) {
    const uint32_t w = rd32le(p + q), wc = w >> 16, op = w & 0xFFFF;
    if (!wc || q + (size_t)wc * 4 > avail || !plausibleOp(op)) break;
    q += (size_t)wc * 4;
    if (op == 56) {  // OpFunctionEnd: the module ends unless another OpFunction follows
      end = q;
      if (q + 4 > avail || (rd32le(p + q) & 0xFFFF) != 54) break;
    }
  }
  return end;
}

// ---- UnityFS ---------------------------------------------------------------------------------
static int cstrEnd(const uint8_t *b, size_t n, size_t *o) {
  while (*o < n && b[*o]) ++*o;
  if (*o >= n) return -1;
  ++*o;
  return 0;
}

uint8_t *vq_unityfs_unpack(const uint8_t *b, size_t n, size_t *out_len, uint32_t *lzma_skipped) {
  if (n < 64 || memcmp(b, "UnityFS\0", 8)) return NULL;
  size_t o = 8;
  const uint32_t ver = rd32be(b + o);
  o += 4;
  if (cstrEnd(b, n, &o) || cstrEnd(b, n, &o) || o + 20 > n) return NULL;
  const uint64_t size = rd64be(b + o);
  const uint32_t cs = rd32be(b + o + 8), us = rd32be(b + o + 12), fl = rd32be(b + o + 16);
  o += 20;
  if (ver >= 7) o = (o + 15) & ~(size_t)15;
  const uint8_t *info;
  size_t data = 0;
  if (fl & 0x80) {  // blocks info at the end
    if (size > n || cs > size) return NULL;
    info = b + size - cs;
    data = o;
  } else {
    if (o + cs > n) return NULL;
    info = b + o;
    data = o + cs;
    if (fl & 0x200) data = (data + 15) & ~(size_t)15;
  }
  if (us < 20 || us > (64u << 20)) return NULL;
  uint8_t *ib = malloc(us);
  if (!ib) return NULL;
  const uint32_t ct = fl & 0x3F;
  if (ct == 0 && cs == us) memcpy(ib, info, us);
  else if ((ct != 2 && ct != 3) || vq_lz4_block(info, cs, ib, us) != (long)us) {
    free(ib);
    return NULL;
  }
  const uint32_t nb = rd32be(ib + 16);
  if ((uint64_t)nb * 10 + 20 > us) {
    free(ib);
    return NULL;
  }
  size_t total = 0;
  for (uint32_t k = 0; k < nb; ++k) total += rd32be(ib + 20 + k * 10);
  if (total > ((size_t)768 << 20)) {
    free(ib);
    return NULL;
  }
  uint8_t *out = malloc(total ? total : 1);
  if (!out) {
    free(ib);
    return NULL;
  }
  size_t w = 0;
  for (uint32_t k = 0; k < nb; ++k) {
    const uint8_t *e = ib + 20 + k * 10;
    const uint32_t u = rd32be(e), c = rd32be(e + 4), f = rd16be(e + 8) & 0x3F;
    if (data + c > n) break;
    const uint8_t *blk = b + data;
    data += c;
    if (f == 0 && c == u) memcpy(out + w, blk, u), w += u;
    else if ((f == 2 || f == 3) && vq_lz4_block(blk, c, out + w, u) == (long)u) w += u;
    else if (f == 1) {
      if (lzma_skipped) ++*lzma_skipped;  // LZMA: not decoded here (rare for Quest builds)
    }
  }
  free(ib);
  *out_len = w;
  return out;
}

// ---- harvest ---------------------------------------------------------------------------------
typedef struct {
  const char *out;
  uint64_t *set;
  size_t cap, count;
  VqHarvestStats *st;
  void (*log)(const char *, void *);
  void *user;
} Ctx;

static void logf_(Ctx *c, const char *fmt, ...) {
  if (!c->log) return;
  char line[600];
  va_list a;
  va_start(a, fmt);
  vsnprintf(line, sizeof line, fmt, a);
  va_end(a);
  c->log(line, c->user);
}

static uint64_t fnv64(const uint8_t *p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h ? h : 1;
}

static int setInsert(Ctx *c, uint64_t h) {
  if ((c->count + 1) * 2 > c->cap) {
    const size_t nc = c->cap ? c->cap * 2 : 4096;
    uint64_t *ns = calloc(nc, 8);
    if (!ns) return 0;
    for (size_t i = 0; i < c->cap; ++i)
      if (c->set[i]) {
        size_t j = c->set[i] & (nc - 1);
        while (ns[j]) j = (j + 1) & (nc - 1);
        ns[j] = c->set[i];
      }
    free(c->set);
    c->set = ns, c->cap = nc;
  }
  size_t j = h & (c->cap - 1);
  while (c->set[j]) {
    if (c->set[j] == h) return 0;
    j = (j + 1) & (c->cap - 1);
  }
  c->set[j] = h;
  ++c->count;
  return 1;
}

static void scanBytes(Ctx *c, const uint8_t *b, size_t n) {
  static const uint8_t magic[4] = {0x03, 0x02, 0x23, 0x07};
  size_t i = 0;
  while (i + 40 <= n) {
    const uint8_t *m = memchr(b + i, 0x03, n - i - 3);
    if (!m) break;
    i = (size_t)(m - b);
    if (memcmp(m, magic, 4)) {
      ++i;
      continue;
    }
    const size_t len = vq_spirv_length(m, n - i);
    if (!len) {
      ++i;
      continue;
    }
    ++c->st->spirv;
    const uint64_t h = fnv64(m, len);
    if (setInsert(c, h)) {
      char path[1200];
      snprintf(path, sizeof path, "%s/%016llx.spv", c->out, (unsigned long long)h);
      FILE *f = fopen(path, "wb");
      if (f) {
        fwrite(m, 1, len, f);
        fclose(f);
        ++c->st->unique;
      }
    }
    i += len;
  }
}

static void scanBlob(Ctx *c, const uint8_t *b, size_t n) {
  ++c->st->files;
  size_t ul = 0;
  uint8_t *u = vq_unityfs_unpack(b, n, &ul, &c->st->lzma_skip);
  if (u) {
    ++c->st->bundles;
    scanBytes(c, u, ul);
    free(u);
  } else {
    scanBytes(c, b, n);
  }
}

static int hasSuffix(const char *s, const char *suf) {
  const size_t a = strlen(s), b = strlen(suf);
  return a >= b && !strcasecmp(s + a - b, suf);
}

static void scanZip(Ctx *c, const char *path) {
  char e[256];
  VqZip *z = vq_zip_open(path, e, sizeof e);
  if (!z) {
    logf_(c, "vq.harvest zip_fail file=\"%s\" error=\"%s\"", path, e);
    return;
  }
  for (int i = 0; i < vq_zip_count(z); ++i) {
    const char *nm = vq_zip_name(z, i);
    if (!strncmp(nm, "lib/", 4) || !strncmp(nm, "META-INF/", 9) || hasSuffix(nm, ".dex") ||
        hasSuffix(nm, ".dll") || hasSuffix(nm, "/") || hasSuffix(nm, ".mp4") || hasSuffix(nm, ".ogg") ||
        hasSuffix(nm, ".wem") || hasSuffix(nm, ".bnk") || strstr(nm, "global-metadata"))
      continue;
    size_t len = 0;
    uint8_t *b = vq_zip_read(z, i, VQ_MAX_FILE, &len, e, sizeof e);
    if (!b) continue;
    scanBlob(c, b, len);
    free(b);
  }
  vq_zip_close(z);
}

static void scanFile(Ctx *c, const char *path, uint64_t size) {
  if (hasSuffix(path, ".apk") || hasSuffix(path, ".obb") || hasSuffix(path, ".zip")) {
    scanZip(c, path);
    return;
  }
  if (hasSuffix(path, ".so") || hasSuffix(path, ".mp4") || hasSuffix(path, ".wem") || hasSuffix(path, ".bnk") ||
      hasSuffix(path, ".spv") || size < 64)
    return;
  if (size > VQ_MAX_FILE) {
    logf_(c, "vq.harvest skip_large file=\"%s\" bytes=%llu", path, (unsigned long long)size);
    return;
  }
  FILE *f = fopen(path, "rb");
  if (!f) return;
  uint8_t *b = malloc((size_t)size);
  if (b && fread(b, 1, (size_t)size, f) == size) scanBlob(c, b, (size_t)size);
  free(b);
  fclose(f);
}

typedef struct {
  char **v;
  size_t n, cap;
} List;

static void collect(const char *dir, const char *skip, List *l, int depth) {
  if (depth > 12) return;
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    if (skip && !strcmp(p, skip)) continue;
    struct stat s;
    if (stat(p, &s)) continue;
    if (S_ISDIR(s.st_mode)) collect(p, skip, l, depth + 1);
    else if (S_ISREG(s.st_mode)) {
      if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 256;
        l->v = realloc(l->v, l->cap * sizeof *l->v);
      }
      l->v[l->n++] = strdup(p);
    }
  }
  closedir(d);
}

int vq_harvest_spirv(const char *game_dir, const char *out_dir, VqHarvestStats *stats, vq_harvest_progress progress,
                     void *user, void (*log)(const char *, void *), char *err, size_t err_len) {
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  VqHarvestStats st;
  memset(&st, 0, sizeof st);
  if (mkdir(out_dir, 0755) && errno != EEXIST) {
    snprintf(err, err_len, "mkdir %s: %s", out_dir, strerror(errno));
    return -1;
  }
  Ctx c = {out_dir, NULL, 0, 0, &st, log, user};
  List l = {0};
  collect(game_dir, out_dir, &l, 0);
  for (size_t i = 0; i < l.n; ++i) {
    struct stat s;
    if (!stat(l.v[i], &s)) scanFile(&c, l.v[i], (uint64_t)s.st_size);
    if (progress) progress((uint32_t)(i + 1), (uint32_t)l.n, user);
    free(l.v[i]);
  }
  free(l.v);
  free(c.set);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  st.ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
  logf_(&c, "vq.harvest files=%u bundles=%u lzma_skipped=%u spirv=%u unique=%u ms=%.0f", st.files, st.bundles,
        st.lzma_skip, st.spirv, st.unique, st.ms);
  if (stats) *stats = st;
  return 0;
}
