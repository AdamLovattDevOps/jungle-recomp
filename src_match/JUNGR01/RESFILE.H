/* Layout of the resource-file control block, recovered from the field offsets
 * the exported accessors read and write. Only the fields reached so far are
 * named; the gaps are padding until something is known to live there. */
#ifndef RESFILE_H
#define RESFILE_H

typedef unsigned short WORD;
typedef unsigned char  BYTE;

typedef struct RESFILE {
    char  pad000[2];
    WORD  notify;                   /* 0x002  RESSETNOTIFY */
    char  pad004[0x10a - 0x004];
    WORD  maxFadeColors;            /* 0x10a */
    WORD  maxTransColors;           /* 0x10c */
    WORD  pad10e;                   /* 0x10e */
    WORD  stringCount;              /* 0x110 */
    WORD  variableCount;            /* 0x112 */
    BYTE  buildType;                /* 0x114 */
    char  pad115[0x19c - 0x115];
    WORD  dirty;                    /* 0x19c  set by every setter */
} RESFILE;

#endif
