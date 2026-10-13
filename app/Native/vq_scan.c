// VisionQuestPro: see vq_scan.h.
#include "vq_scan.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { uint8_t e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
                 uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags;
                 uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } Ehdr;
typedef struct { uint32_t sh_name, sh_type; uint64_t sh_flags, sh_addr, sh_offset, sh_size;
                 uint32_t sh_link, sh_info; uint64_t sh_addralign, sh_entsize; } Shdr;
typedef struct { uint32_t st_name; uint8_t st_info, st_other; uint16_t st_shndx;
                 uint64_t st_value, st_size; } Sym;

#define SHT_DYNSYM 11

typedef struct { char *buf; size_t len, cap; } Out;

static void out_add(Out *o, const char *s, size_t n) {
    if (o->len + n + 2 > o->cap) {
        size_t c = o->cap ? o->cap * 2 : 4096;
        while (c < o->len + n + 2) c *= 2;
        char *b = realloc(o->buf, c);
        if (!b) return;
        o->buf = b; o->cap = c;
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len++] = '\n';
    o->buf[o->len] = '\0';
}

static const uint8_t *map_file(const char *path, size_t *size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return NULL; }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return NULL;
    *size = (size_t)st.st_size;
    return p;
}

char *vq_elf_symbols(const char *path, int defined) {
    size_t size = 0;
    const uint8_t *m = map_file(path, &size);
    if (!m) return NULL;
    Out o = {0};
    const Ehdr *eh = (const Ehdr *)m;
    if (size < sizeof *eh || memcmp(eh->e_ident, "\x7f" "ELF", 4) || eh->e_ident[4] != 2 ||
        eh->e_shoff == 0 || eh->e_shentsize != sizeof(Shdr) ||
        eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(Shdr) > size) {
        munmap((void *)m, size);
        return NULL;
    }
    const Shdr *sh = (const Shdr *)(m + eh->e_shoff);
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_DYNSYM || sh[i].sh_link >= eh->e_shnum) continue;
        const Shdr *strs = &sh[sh[i].sh_link];
        if (sh[i].sh_offset + sh[i].sh_size > size || strs->sh_offset + strs->sh_size > size) continue;
        const Sym *syms = (const Sym *)(m + sh[i].sh_offset);
        size_t n = sh[i].sh_size / sizeof(Sym);
        const char *str = (const char *)(m + strs->sh_offset);
        for (size_t k = 1; k < n; k++) {
            if (syms[k].st_name >= strs->sh_size) continue;
            const char *nm = str + syms[k].st_name;
            if (!*nm) continue;
            int bind = syms[k].st_info >> 4;           // 1 GLOBAL, 2 WEAK
            if (bind != 1 && bind != 2) continue;
            int undef = syms[k].st_shndx == 0;
            if (defined ? undef : !undef) continue;
            size_t len = strnlen(nm, strs->sh_size - syms[k].st_name);
            if (!defined && bind == 2) {
                char tmp[512];
                int w = snprintf(tmp, sizeof tmp, "w %.*s", (int)(len < 500 ? len : 500), nm);
                out_add(&o, tmp, (size_t)w);
            } else {
                out_add(&o, nm, len);
            }
        }
    }
    munmap((void *)m, size);
    if (!o.buf) o.buf = calloc(1, 1);
    return o.buf;
}

static int class_char(uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '/' || c == '.' || c == '$';
}

static const char *const k_prefixes[] = {
    "android/", "java/", "javax/", "dalvik/", "com/oculus/", "com/meta/", "com/android/",
    "com/google/android/", NULL,
};

// Simple open-addressing set of seen names.
typedef struct { char **slot; size_t cap, n; } Set;

static uint64_t hash(const char *s) {
    uint64_t h = 1469598103934665603ull;
    while (*s) { h ^= (uint8_t)*s++; h *= 1099511628211ull; }
    return h;
}

static int set_add(Set *s, const char *v) {
    if ((s->n + 1) * 2 > s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 1024;
        char **ns = calloc(nc, sizeof *ns);
        if (!ns) return 0;
        for (size_t i = 0; i < s->cap; i++) {
            if (!s->slot[i]) continue;
            size_t j = hash(s->slot[i]) & (nc - 1);
            while (ns[j]) j = (j + 1) & (nc - 1);
            ns[j] = s->slot[i];
        }
        free(s->slot);
        s->slot = ns; s->cap = nc;
    }
    size_t j = hash(v) & (s->cap - 1);
    while (s->slot[j]) {
        if (!strcmp(s->slot[j], v)) return 0;
        j = (j + 1) & (s->cap - 1);
    }
    s->slot[j] = strdup(v);
    s->n++;
    return 1;
}

static void set_free(Set *s) {
    for (size_t i = 0; i < s->cap; i++) free(s->slot[i]);
    free(s->slot);
}

char *vq_java_classes(const char *path) {
    size_t size = 0;
    const uint8_t *m = map_file(path, &size);
    if (!m) return NULL;
    Out o = {0};
    Set seen = {0};
    char name[256];
    size_t i = 0;
    while (i < size) {
        if (!class_char(m[i])) { i++; continue; }
        size_t start = i;
        while (i < size && class_char(m[i])) i++;
        size_t len = i - start;
        if (len < 8 || len >= sizeof name) continue;
        memcpy(name, m + start, len);
        name[len] = '\0';
        // JNI type descriptors embed class names as "Landroid/os/Bundle;".
        if (name[0] == 'L' && strchr(name, '/')) memmove(name, name + 1, len--);
        // Dotted (Java source / AndroidJavaClass) form -> JNI form.
        int dotted = strchr(name, '.') != NULL;
        if (dotted) {
            if (strchr(name, '/')) continue;           // mixed: not a class name
            for (char *p = name; *p; p++) if (*p == '.') *p = '/';
        }
        int ok = 0;
        for (const char *const *p = k_prefixes; *p; p++)
            if (!strncmp(name, *p, strlen(*p))) { ok = 1; break; }
        if (!ok) continue;
        // Shape: package segments in lower case, then a class segment starting upper case;
        // drop a trailing method/field part ("android/os/Build.MODEL" was dotted: keep the class).
        char *last = strrchr(name, '/');
        if (!last || !last[1]) continue;
        if (!(last[1] >= 'A' && last[1] <= 'Z')) {
            // "android/os/Build/MODEL"-like from a dotted constant: back off one segment.
            *last = '\0';
            last = strrchr(name, '/');
            if (!last || !(last[1] >= 'A' && last[1] <= 'Z')) continue;
        }
        // A dotted constant ("android.os.Build.MODEL"): an all-caps last segment after a class.
        if (dotted) {
            int caps = 1;
            for (const char *p = last + 1; *p; p++)
                if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')) { caps = 0; break; }
            if (caps) {
                *last = '\0';
                char *prev = strrchr(name, '/');
                if (!prev || !(prev[1] >= 'A' && prev[1] <= 'Z')) continue;
            }
        }
        if (set_add(&seen, name)) out_add(&o, name, strlen(name));
    }
    set_free(&seen);
    munmap((void *)m, size);
    if (!o.buf) o.buf = calloc(1, 1);
    return o.buf;
}
