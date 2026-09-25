/* ISO 9660 extraction: see iso.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include "iso.h"
#ifdef _WIN32
#include <direct.h>
#define mkdir(p, m) _mkdir(p)
#define strcasecmp _stricmp
#endif

#define SECTOR 2048

static unsigned le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

static int read_at(FILE *f, unsigned lba, unsigned char *buf, size_t n)
{
    return fseek(f, (long)lba * SECTOR, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}

/* A directory record's name, without the ";1" version or a bare trailing dot. */
static void rec_name(const unsigned char *r, char *out, size_t cap)
{
    size_t n = r[32], k = 0;
    for (size_t i = 0; i < n && k + 1 < cap && r[33 + i] != ';'; i++) out[k++] = (char)r[33 + i];
    if (k && out[k - 1] == '.') k--;
    out[k] = 0;
}

/* Walk the records of the directory at lba; call fn for each real entry. */
typedef int (*RecFn)(FILE *f, const unsigned char *rec, void *ctx);
static int walk(FILE *f, unsigned lba, unsigned size, RecFn fn, void *ctx)
{
    unsigned char *d = malloc(size ? size : 1);
    if (!d || !read_at(f, lba, d, size)) { free(d); return -1; }
    for (unsigned off = 0; off < size; ) {
        unsigned len = d[off];
        if (len == 0) { off = (off / SECTOR + 1) * SECTOR; continue; }   /* records never straddle sectors */
        if (off + len > size || len < 34) break;
        const unsigned char *r = d + off;
        if (!(r[32] == 1 && (r[33] == 0 || r[33] == 1))) {             /* skip "." and ".." */
            int rc = fn(f, r, ctx);
            if (rc) { free(d); return rc; }
        }
        off += len;
    }
    free(d);
    return 0;
}

typedef struct { const char *want; unsigned lba, size; } Find;
static int find_dir(FILE *f, const unsigned char *r, void *ctx)
{
    Find *fd = ctx; char nm[64]; rec_name(r, nm, sizeof nm);
    if ((r[25] & 2) && !strcasecmp(nm, fd->want)) { fd->lba = le32(r + 2); fd->size = le32(r + 10); return 1; }
    return 0;
}

typedef struct { const char *dest; int n; } Copy;
static int copy_file(FILE *f, const unsigned char *r, void *ctx)
{
    Copy *c = ctx;
    if (r[25] & 2) return 0;                         /* the game's directory is flat */
    char nm[64], path[1200]; rec_name(r, nm, sizeof nm);
    for (char *p = nm; *p; p++) *p = (char)toupper((unsigned char)*p);
    if (!*nm || strchr(nm, '/')) return 0;
    snprintf(path, sizeof path, "%s/%s", c->dest, nm);
    unsigned lba = le32(r + 2), size = le32(r + 10);
    FILE *o = fopen(path, "wb");
    if (!o) { fprintf(stderr, "iso: cannot write %s\n", path); return -1; }
    unsigned char buf[64 * 1024];
    if (fseek(f, (long)lba * SECTOR, SEEK_SET) != 0) { fclose(o); return -1; }
    for (unsigned left = size; left; ) {
        size_t n = left < sizeof buf ? left : sizeof buf;
        if (fread(buf, 1, n, f) != n || fwrite(buf, 1, n, o) != n) { fclose(o); remove(path); return -1; }
        left -= (unsigned)n;
    }
    if (fclose(o) != 0) return -1;
    c->n++;
    return 0;
}

int iso_extract_dir(const char *image, const char *dir, const char *dest)
{
    FILE *f = fopen(image, "rb");
    if (!f) { fprintf(stderr, "iso: cannot open %s\n", image); return -1; }
    unsigned char pvd[SECTOR];
    if (!read_at(f, 16, pvd, SECTOR) || pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5)) {
        fprintf(stderr, "iso: %s is not an ISO 9660 image\n", image); fclose(f); return -1;
    }
    const unsigned char *root = pvd + 156;
    Find fd = { dir, 0, 0 };
    if (walk(f, le32(root + 2), le32(root + 10), find_dir, &fd) != 1) {
        fprintf(stderr, "iso: no %s directory in %s\n", dir, image); fclose(f); return -1;
    }
    mkdir(dest, 0755);
    Copy c = { dest, 0 };
    int rc = walk(f, fd.lba, fd.size, copy_file, &c);
    fclose(f);
    return rc < 0 ? -1 : c.n;
}
