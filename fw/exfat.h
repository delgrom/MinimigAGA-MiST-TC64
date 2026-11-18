/* Shared exFAT reader for the EightThirtyTwo boot ROM and OSD firmware.
 * EXFAT_READ and EXFAT_BUFFER refer to the caller's sector I/O and scratch space.
 * Disk fields follow https://learn.microsoft.com/en-us/windows/win32/fileio/exfat-specification
 */
#ifndef MINIMIG_EXFAT_H
#define MINIMIG_EXFAT_H

#define EXFAT_CONTIGUOUS 0x80000000UL
#define EXFAT_CLUSTER_MASK 0x7fffffffUL

static unsigned long exfat_fat, exfat_heap, exfat_root, exfat_clusters;
static unsigned long exfat_cluster_sectors;
static unsigned long exfat_cached_sector;

typedef struct {
    unsigned long cluster;
    unsigned long sectors_left;
    unsigned long offset;
} ExfatDirectory;

typedef struct {
    unsigned long cluster;
    unsigned long size;
    unsigned int size_high;
    unsigned char attributes;
} ExfatEntry;

static unsigned long ExfatLE32(const unsigned char *p)
{
    return p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static int ExfatSignature(const unsigned char *p)
{
    return p[3] == 'E' && p[4] == 'X' && p[5] == 'F' && p[6] == 'A' &&
           p[7] == 'T' && p[8] == ' ' && p[9] == ' ' && p[10] == ' ';
}

static int ExfatValidCluster(unsigned long cluster)
{
    cluster &= EXFAT_CLUSTER_MASK;
    return cluster >= 2 && cluster - 2 < exfat_clusters;
}

static int ExfatMount(unsigned long base)
{
    const unsigned char *p = EXFAT_BUFFER;
    unsigned long length = ExfatLE32(p + 72);
    unsigned long fat = ExfatLE32(p + 80);
    unsigned long fat_length = ExfatLE32(p + 84);
    unsigned long heap = ExfatLE32(p + 88);
    unsigned int shift = p[109];

    exfat_clusters = ExfatLE32(p + 92);
    exfat_root = ExfatLE32(p + 96);
    if (p[510] != 0x55 || p[511] != 0xaa || p[108] != 9 ||
        p[105] != 1 || shift > 16 || p[110] != 1 ||
        ExfatLE32(p + 76) || !length || base > 0xffffffffUL - length ||
        fat < 24 || heap >= length || fat >= heap ||
        fat_length > heap - fat || !exfat_clusters ||
        exfat_clusters >= EXFAT_CLUSTER_MASK ||
        exfat_clusters > ((length - heap) >> shift) ||
        ((exfat_clusters + 1) >> 7) >= fat_length ||
        exfat_root < 2 || exfat_root - 2 >= exfat_clusters)
        return 0;

    exfat_fat = base + fat;
    exfat_heap = base + heap;
    exfat_cluster_sectors = 1UL << shift;
    exfat_cached_sector = ~0UL;
    return 1;
}

static unsigned long ExfatSector(unsigned long cluster)
{
    return exfat_heap + ((cluster & EXFAT_CLUSTER_MASK) - 2) * exfat_cluster_sectors;
}

static int ExfatReadSector(unsigned long sector)
{
    if (sector != exfat_cached_sector)
    {
        exfat_cached_sector = ~0UL;
        if (!EXFAT_READ(sector, EXFAT_BUFFER))
            return 0;
        exfat_cached_sector = sector;
    }
    return 1;
}

static unsigned long ExfatNextCluster(unsigned long cluster)
{
    unsigned long next;
    if (cluster & EXFAT_CONTIGUOUS)
        next = cluster + 1;
    else
    {
        if (!ExfatReadSector(exfat_fat + (cluster >> 7)))
            return 0;
        next = ExfatLE32(EXFAT_BUFFER + ((cluster & 127) << 2));
        if (next & EXFAT_CONTIGUOUS)
            return 0;
    }
    return ExfatValidCluster(next) ? next : 0;
}

static void ExfatOpenDirectory(ExfatDirectory *dir, unsigned long cluster,
                               unsigned long size)
{
    dir->cluster = cluster ? cluster : exfat_root;
    /* Root has no length field. Bound traversal even if its chain loops. */
    dir->sectors_left = cluster ? size >> 9 : exfat_clusters * exfat_cluster_sectors;
    dir->offset = 0;
    exfat_cached_sector = ~0UL;
}

static const unsigned char *ExfatNextSlot(ExfatDirectory *dir)
{
    unsigned long offset = dir->offset;
    unsigned long sector;

    if (!dir->sectors_left || !dir->cluster)
        return 0;
    if (offset == (exfat_cluster_sectors << 9))
    {
        dir->cluster = ExfatNextCluster(dir->cluster);
        if (!dir->cluster)
            return 0;
        offset = 0;
    }
    sector = ExfatSector(dir->cluster) + (offset >> 9);
    if (!ExfatReadSector(sector))
        return 0;

    dir->offset = offset + 32;
    if (!(dir->offset & 511))
        --dir->sectors_left;
    return EXFAT_BUFFER + (offset & 511);
}

static int ExfatNextEntry(ExfatDirectory *dir, ExfatEntry *entry, char *name)
{
    const unsigned char *p;
    unsigned int remaining, length, count, i, checksum, expected, stream, size_high;
    unsigned long cluster, size;

    while ((p = ExfatNextSlot(dir)) && p[0])
    {
        if (p[0] != 0x85 || p[1] < 2)
            continue;
        remaining = p[1];
        expected = p[2] | (p[3] << 8);
        entry->attributes = p[4];
        checksum = 0;
        length = 0;
        count = 0;
        size = 0;
        size_high = 0;
        cluster = 0;
        stream = 1;
        for (i = 0; i < 32; ++i)
            if (i != 2 && i != 3)
                checksum = (((checksum >> 1) | ((checksum & 1) << 15)) + p[i]) & 0xffff;
        while (remaining--)
        {
            p = ExfatNextSlot(dir);
            if (!p || !p[0])
                return 0;
            for (i = 0; i < 32; ++i)
                checksum = (((checksum >> 1) | ((checksum & 1) << 15)) + p[i]) & 0xffff;

            if (stream)
            {
                stream = 0;
                if (p[0] != 0xc0 || ExfatLE32(p + 28) > 511 ||
                    ExfatLE32(p + 12) != ExfatLE32(p + 28) ||
                    ExfatLE32(p + 8) != ExfatLE32(p + 24))
                    break;
                size_high = ExfatLE32(p + 28);
                if ((entry->attributes & 0x10) && size_high)
                    break;
                length = p[3];
                size = ExfatLE32(p + 24);
                cluster = ExfatLE32(p + 20);
                if (cluster & EXFAT_CONTIGUOUS)
                    break;
                if (size || size_high)
                {
                    unsigned long sectors = ((unsigned long)size_high << 23) | (size >> 9);
                    unsigned long last = (sectors - !(size & 511)) / exfat_cluster_sectors;
                    if (!ExfatValidCluster(cluster) || last >= exfat_clusters)
                        break;
                    if (p[1] & 2)
                    {
                        if (last >= exfat_clusters - (cluster - 2))
                            break;
                        cluster |= EXFAT_CONTIGUOUS;
                    }
                }
                else if (cluster || (entry->attributes & 0x10))
                    break;
            }
            else if (count < length)
            {
                if (p[0] != 0xc1)
                    break;
                for (i = 2; i < 32 && count < length; i += 2)
                    name[count++] = p[i + 1] || p[i] < 32 || p[i] >= 127 ? '?' : p[i];
            }
            else if ((p[0] & 0xe0) != 0xe0)
                break; /* Unknown critical secondary entry. */

            if (!remaining && length && count == length && checksum == expected)
            {
                name[count] = 0;
                entry->size = size;
                entry->size_high = size_high;
                entry->cluster = cluster;
                return 1;
            }
        }
    }
    return 0;
}

static unsigned char ExfatUpper(unsigned char c)
{
    return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c;
}

static int ExfatNameEqual(const char *a, const char *b)
{
    while (ExfatUpper(*a) == ExfatUpper(*b))
    {
        if (!*a++)
            return 1;
        ++b;
    }
    return 0;
}

#endif
