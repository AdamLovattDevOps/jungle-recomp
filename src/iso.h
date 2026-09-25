#ifndef JUNGLE_ISO_H
#define JUNGLE_ISO_H

/* Copy one directory out of an ISO 9660 disc image: the game's JUNGLE
 * directory, from an image of the CD. Plain ISO 9660 is all the 1995 disc
 * uses (8.3 names, one level deep), so this reads the primary volume
 * descriptor and nothing else. Returns the number of files written, or -1. */
int iso_extract_dir(const char *image, const char *dir, const char *dest);

#endif
