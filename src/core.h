/* core.h — types and asset-layer functions shared by the tools and the engine. */
#ifndef CORE_H
#define CORE_H
#include <stddef.h>

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef signed short   s16;

#define MAGIC        0x4C37
#define OFF_VERSION  0x54
#define OFF_TABLES   0xBE
#define OFF_PALETTE  0xEE
#define ENTRY_SIZE   10
#define BMP_HDR      20
#define FLAG_RLE     0x8000
#define RESIDENT_LO  8
#define RESIDENT_HI  16

static inline u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static inline u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) |
                                      ((u32)p[2] << 16) | ((u32)p[3] << 24); }

typedef struct { u16 type; u32 offset, size; } Entry;

typedef struct {
    u8    *data;
    size_t len;
    Entry *dir;
    int    ndir;
    const u8 *blob;          /* resident blob, table slot 5 */
    u32    blob_len;
    u8     pal[256][3];
} Container;

int  container_open(Container *c, const char *path);
const u8 *resource_bytes(const Container *c, const Entry *e);
int  bitmap_decode(const u8 *src, size_t size, int *w_out, int *h_out, u8 **px);
int  record_len(const u8 *b, size_t p, size_t n, int *targets, int *ntarget);
int  frame_resolve(const Container *c, int handle, int *ox, int *oy);
size_t adpcm_decode(const u8 *in, size_t n, s16 *out);
int  adpcm_load_tables(const char *dll_path);

#endif
