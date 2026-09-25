#include "RESFILE.H"

int __far __pascal LibMain(unsigned hInstance, unsigned wDataSeg,
                           unsigned cbHeapSize, char __far *lpCmdLine)
{
    return 1;
}

WORD __far __pascal __loadds RESCOUNTSTRINGS(RESFILE __near *p)
{
    return p->stringCount;
}

WORD __far __pascal __loadds RESCOUNTVARIABLES(RESFILE __near *p)
{
    return p->variableCount;
}

WORD __far __pascal __loadds RESGETMAXFADECOLORS(RESFILE __near *p)
{
    if (p != 0)
        return p->maxFadeColors;
    return 0;
}

WORD __far __pascal __loadds RESGETMAXTRANSCOLORS(RESFILE __near *p)
{
    if (p != 0)
        return p->maxTransColors;
    return 0;
}

/* Returns a WORD, not a BYTE: the original zero-extends the byte field with
 * `sub ah,ah` and returns 0 as a full-word `xor ax,ax`, which is what a
 * word-sized return type does with a byte-sized field. */
WORD __far __pascal __loadds RESGETBUILDTYPE(RESFILE __near *p)
{
    if (p != 0)
        return p->buildType;
    return 0;
}

void __far __pascal __loadds RESSETSTRINGCOUNT(RESFILE __near *p, WORD n)
{
    p->stringCount = n;
    p->dirty = 1;
}

void __far __pascal __loadds RESSETVARIABLECOUNT(RESFILE __near *p, WORD n)
{
    p->variableCount = n;
    p->dirty = 1;
}

/* Four bytes short of matching: the original loads the value before the
 * pointer, this loads the pointer first. Same registers, same offsets, only the
 * order of the two loads differs. A handle-and-cast form was tried and emits
 * the same thing, so it is not the parameter type. */
void __far __pascal __loadds RESSETNOTIFY(RESFILE __near *p, WORD n)
{
    p->notify = n;
}
