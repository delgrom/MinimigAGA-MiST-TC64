/*
Copyright 2005, 2006, 2007 Dennis van Weeren
Copyright 2008, 2009 Jakub Bednarski

This file is part of Minimig

Minimig is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3 of the License, or
(at your option) any later version.

Minimig is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

This is a simple FAT16 handler. It works on a sector basis to allow fastest acces on disk
images.

11-12-2005 - first version, ported from FAT1618.C

JB:
2008-10-11  - added SeekFile() and cluster_mask
            - limited file create and write support added
2009-05-01  - modified LoadDirectory() and GetDirEntry() to support sub-directories (with limitation of 511 files/subdirs per directory)
            - added GetFATLink() function
            - code cleanup
2009-05-03  - modified sorting algorithm in LoadDirectory() to display sub-directories above files
2009-08-23  - modified ScanDirectory() to support page scrolling and parent dir selection
2009-11-22  - modified FileSeek()
            - added FileReadEx()
2009-12-15  - all entries are now sorted by name with extension
            - directory short names are displayed with extensions

*/

#include <stdio.h>
#include <string.h>
#include "errors.h"
//#include <ctype.h>
#include "boot.h"
#include "mmc.h"
#include "fat.h"
#include "swap.h"
#include "hexdump.h"
// #include "small_printf.h"


#define DEBUG 0

int tolower(int c);

unsigned short directory_cluster;       // first cluster of directory (0 if root)
unsigned short entries_per_cluster;     // number of directory entries per cluster

// internal global variables
unsigned char fattype;              	// volume format 
unsigned char fat32;                // volume format is FAT32
unsigned long boot_sector;              // partition boot sector
unsigned long fat_start;                // start LBA of first FAT table
unsigned long data_start;               // start LBA of data field
unsigned long root_directory_cluster;   // root directory cluster (used in FAT32)
unsigned long root_directory_start;     // start LBA of directory table
unsigned long root_directory_size;      // size of directory region in sectors
unsigned char fat_number;               // number of FAT tables
unsigned long cluster_size;             // size of a cluster in sectors
unsigned long cluster_mask;             // binary mask of cluster number
unsigned short dir_entries;             // number of entry's in directory table
unsigned long fat_size;                 // size of fat

unsigned char sector_buffer[1024];       // sector buffer - room for two consecutive sectors...

static unsigned char exfat_buffer[512];
#define EXFAT_READ MMC_Read
#define EXFAT_BUFFER exfat_buffer
#include "../exfat.h"
static int exfat;
int _strnicmp(const char *s1, const char *s2, size_t n);

struct PartitionEntry partitions[4];	// lbastart and sectors will be byteswapped as necessary
int partitioncount;

FATBUFFER fat_buffer;                   // buffer for caching fat entries
unsigned long buffered_fat_index;       // index of buffered FAT sector

char DirEntryLFN[MAXDIRENTRIES][261];
DIRENTRY DirEntry[MAXDIRENTRIES];
unsigned char sort_table[MAXDIRENTRIES];
unsigned char nDirEntries;          // entries in DirEntry table
unsigned char iSelectedEntry;       // selected entry index
unsigned long iCurrentDirectory;    // cluster number of current directory, 0 for root
unsigned long iPreviousDirectory;   // cluster number of previous directory

// temporary storage buffers
char t_DirEntryLFN[MAXDIRENTRIES][261];
DIRENTRY t_DirEntry[MAXDIRENTRIES];
unsigned char t_sort_table[MAXDIRENTRIES];

// external functions
extern unsigned long GetTimer(unsigned long);

extern char BootPrint(const char *s);
void bprintfl(const char *fmt,unsigned long l)
{
	char s[64];
	sprintf(s,fmt,l);
	if(DEBUG)
		BootPrint(s);
}


// FindDrive() checks if a card is present and contains FAT formatted primary partition
unsigned int FindDrive(void)
{
    buffered_fat_index = -1;
    fattype = fat32 = exfat = 0;

	int i;

    if (!MMC_Read(0, sector_buffer)) // read MBR
        return(0);

	boot_sector=0;
	partitioncount=1;

	// If we can identify a filesystem on block 0 we don't look for partitions
    if (ExfatSignature(sector_buffer))
        partitioncount=0;
    if (strncmp((const char*)&sector_buffer[0x36], "FAT16   ", 8)==0) // check for FAT16
		partitioncount=0;
    if (strncmp((const char*)&sector_buffer[0x52], "FAT32   ", 8)==0) // check for FAT32
		partitioncount=0;

	if(partitioncount)
	{
        if (sector_buffer[510] != 0x55 || sector_buffer[511] != 0xaa)
        {
            SetError(ERROR_FILESYSTEM, "No partition signature found", 0, 0);
            return 0;
        }

        for (i = 0; i < 4; ++i)
        {
            const unsigned char *entry = sector_buffer + 446 + i * 16;
            memcpy(partitions[i].geometry, entry, sizeof(partitions[i].geometry));
            partitions[i].startlba = ExfatLE32(entry + 8);
            partitions[i].sectors = ExfatLE32(entry + 12);
        }
        boot_sector = partitions[0].startlba;
        for (partitioncount = 4; partitioncount > 1 && !partitions[partitioncount - 1].sectors;
             --partitioncount)
            ;
        if (!MMC_Read(boot_sector, sector_buffer))
            return 0;
    }

    if (ExfatSignature(sector_buffer))
    {
        memcpy(exfat_buffer, sector_buffer, 512);
        if (!ExfatMount(boot_sector) ||
            (partitioncount && ExfatLE32(exfat_buffer + 72) > partitions[0].sectors))
        {
            FatalError(ERROR_FILESYSTEM, "Unsupported exFAT volume", 0, 0);
            return 0;
        }
        exfat = 1;
        cluster_size = exfat_cluster_sectors;
        cluster_mask = ~(cluster_size - 1);
        return 1;
    }

    if (strncmp((const char*)&sector_buffer[0x36], "FAT16   ", 8)==0) // check for FAT16
		fattype = 16;

    if (strncmp((const char*)&sector_buffer[0x52], "FAT32   ", 8)==0) // check for FAT32
		fattype = 32;
	
    printf("partition type: 0x%02X (", sector_buffer[450]);
    switch (fattype)
    {
		case 0:
		    printf("NONE");
		    break;
		case 12:
		    printf("FAT12");
		    break;
		case 16:
		    printf("FAT16");
		    break;
		case 32:
		    printf("FAT32");
		    fat32 = 1;
		    break;
		default:
		    printf("UNKNOWN");
		    break;
    }
    printf(")\r");

    if (fattype != 32 && fattype != 16) // first partition filesystem type: FAT16 or FAT32
    {
		FatalError(ERROR_FILESYSTEM,"Unsupported partition type",fattype,0);
//        BootPrintEx("Unsupported partition type!");
        return(0);
    }

    if (sector_buffer[510] != 0x55 || sector_buffer[511] != 0xaa)  // check signature
    {
		FatalError(ERROR_FILESYSTEM,"Bad signature",sector_buffer[510],sector_buffer[511]);
        return(0);
	}

//    if (!MMC_Read(boot_sector, sector_buffer)) // read boot sector
//        return(0);

    // check for near-jump or short-jump opcode
    if (sector_buffer[0] != 0xe9 && sector_buffer[0] != 0xeb)
    {
		FatalError(ERROR_FILESYSTEM,"Boot sector not found",sector_buffer[0],0);
        return(0);
	}

    // check if blocksize is really 512 bytes
    if (sector_buffer[11] != 0x00 || sector_buffer[12] != 0x02)
    {
		FatalError(ERROR_FILESYSTEM,"Bad blocksize",sector_buffer[11],sector_buffer[12]);
        return(0);
	}

    // check medium descriptor byte, must be 0xf8 for hard drive
    if (sector_buffer[21] != 0xf8)
    {
		FatalError(ERROR_FILESYSTEM,"Bad media descriptor",sector_buffer[21],0);
        return(0);
	}

    if (fat32)
    {
        if (strncmp((const char*)&sector_buffer[0x52], "FAT32   ", 8) != 0) // check file system type
	    {
			FatalError(ERROR_FILESYSTEM,"Filesystem mismatch",sector_buffer[55],sector_buffer[56]);
            return(0);
		}

        cluster_size = sector_buffer[0x0D]; // get cluster_size in sectors
        cluster_mask = ~(cluster_size - 1); // calculate cluster mask
        dir_entries = cluster_size << 4; // total number of dir entries (16 entries per sector)
        root_directory_size = cluster_size; // root directory size in sectors
        fat_start = boot_sector + sector_buffer[0x0E] + (sector_buffer[0x0F] << 8); // reserved sector count before FAT table (usually 32 for FAT32)
        fat_number = sector_buffer[0x10];
        fat_size = sector_buffer[0x24] + (sector_buffer[0x25] << 8) + (sector_buffer[0x26] << 16) + (sector_buffer[0x27] << 24);
        data_start = fat_start + (fat_number * fat_size);
        root_directory_cluster = sector_buffer[0x2C] + (sector_buffer[0x2D] << 8) + (sector_buffer[0x2E] << 16) + ((sector_buffer[0x2F] & 0x0F) << 24);
        root_directory_start = (root_directory_cluster - 2) * cluster_size + data_start;
    }
    else
    {
        // calculate drive's parameters from bootsector, first up is size of directory
        dir_entries = sector_buffer[17] + (sector_buffer[18] << 8);
        root_directory_size = ((dir_entries << 5) + 511) >> 9;

        // calculate start of FAT,size of FAT and number of FAT's
        fat_start = boot_sector + sector_buffer[14] + (sector_buffer[15] << 8);
        fat_size = sector_buffer[22] + (sector_buffer[23] << 8);
        fat_number = sector_buffer[16];

        // calculate start of directory
        root_directory_start = fat_start + (fat_number * fat_size);
        root_directory_cluster = 0; // unused

        // get cluster_size
        cluster_size = sector_buffer[13];

        // calculate cluster mask
        cluster_mask = ~(cluster_size - 1);

        // calculate start of data
        data_start = root_directory_start + root_directory_size;
    }


    // some debug output
	if(DEBUG)
	{
		printf("fat_size: %lu\r", fat_size);
		printf("fat_number: %u\r", fat_number);
		printf("fat_start: %lu\r", fat_start);
		printf("root_directory_start: %lu\r", root_directory_start);
		printf("dir_entries: %u\r", dir_entries);
		printf("data_start: %lu\r", data_start);
		printf("cluster_size: %u\r", cluster_size);
		printf("cluster_mask: %08lX\r", cluster_mask);
	}

    return(1);
}

static char fileLFN[261];
static int doLFN(DIRENTRY *pEntry,char *lfnbuf)
{
	/* FIXME take care of lfn checksum here */
	if (pEntry->Attributes == ATTR_LFN)	// Do we have a long filename entry?
	{
		unsigned char *p=&pEntry->Name[0];
		int seq=p[0];
		int offset=((seq&0x1f)-1)*13;
		char *o=lfnbuf+offset;
		*o++=p[1];
		*o++=p[3];
		*o++=p[5];
		*o++=p[7];
		*o++=p[9];

		*o++=p[0xe];
		*o++=p[0x10];
		*o++=p[0x12];
		*o++=p[0x14];
		*o++=p[0x16];
		*o++=p[0x18];

		*o++=p[0x1c];
		*o++=p[0x1e];
		return(1);
	}
	return(0);
}


/* exFAT has no on-disk ".." entries. Resolve persisted directory cluster IDs
 * from the root, keeping only the cursors for the current path. */
static int ExfatLocateDirectory(unsigned long id, unsigned long *size, unsigned long *parent)
{
    static struct {
        ExfatDirectory cursor;
        unsigned long id;
    } path[32];
    unsigned int depth = 0;
    ExfatEntry entry;
    char name[256];

    *size = *parent = 0;
    if (!id)
        return 1;
    path[0].id = 0;
    ExfatOpenDirectory(&path[0].cursor, 0, 0);
    for (;;)
    {
        if (!ExfatNextEntry(&path[depth].cursor, &entry, name))
        {
            if (!depth)
                return 0;
            --depth;
            continue;
        }
        if (!(entry.attributes & ATTR_DIRECTORY) || !entry.size || (entry.size & 511))
            continue;
        if (entry.cluster == id)
        {
            *size = entry.size;
            *parent = path[depth].id;
            return 1;
        }
        if (depth + 1 < sizeof(path) / sizeof(path[0]))
        {
            unsigned int ancestor;
            for (ancestor = 0; ancestor <= depth; ++ancestor)
                if (path[ancestor].cursor.cluster == entry.cluster || path[ancestor].id == entry.cluster)
                    break;
            if (ancestor <= depth || (entry.cluster & EXFAT_CLUSTER_MASK) == exfat_root)
                continue;
            ++depth;
            path[depth].id = entry.cluster;
            ExfatOpenDirectory(&path[depth].cursor, entry.cluster, entry.size);
        }
    }
}

static int ExfatDirectoryAt(ExfatDirectory *dir, unsigned long cluster, unsigned long *parent)
{
    unsigned long size;
    if (!ExfatLocateDirectory(cluster, &size, parent))
        return 0;
    ExfatOpenDirectory(dir, cluster, size);
    return 1;
}

/* Existing OSD configuration records store an 11-byte FAT-style name.
 * Preserve plain 8.3 names and use a cluster-based alias for longer names. */
static void ExfatAlias(unsigned char *alias, const char *name, unsigned long cluster)
{
    const char *ext = 0, *p;
    for (p = name; *p; ++p)
        if (*p == '.')
            ext = p;
    unsigned int base = ext ? ext - name : strlen(name);
    unsigned int i;
    int short_name = base > 0 && base <= 8 && name[0] != '~';

    memset(alias, ' ', 11);
    for (i = 0; i < base && short_name; ++i)
    {
        unsigned char c = ExfatUpper(name[i]);
        if (c <= ' ' || c >= 127 || c == '.' || c == '?')
            short_name = 0;
        alias[i] = c;
    }
    if (!short_name || (ext && strlen(ext + 1) > 3))
    {
        alias[0] = '~';
        cluster &= EXFAT_CLUSTER_MASK;
        for (i = 8; i > 1;)
        {
            unsigned int digit = cluster & 31;
            alias[--i] = digit < 10 ? '0' + digit : 'A' + digit - 10;
            cluster >>= 5;
        }
    }
    if (ext && strlen(ext + 1) <= 3)
        for (i = 0; i < 3 && ext[i + 1]; ++i)
            alias[8 + i] = ExfatUpper(ext[i + 1]);
}

unsigned long DirectoryEntryCluster(const DIRENTRY *entry)
{
    unsigned long high = exfat ? entry->HighCluster : (fat32 ? entry->HighCluster & 0x0fff : 0);
    return (high << 16) | entry->StartCluster;
}

void FileSetDirectorySize(fileTYPE *file, const DIRENTRY *entry)
{
    file->size_high = exfat ? entry->CreateTime : 0;
    file->size = entry->FileSize;
}

static void ExfatMenuEntry(DIRENTRY *result, const ExfatEntry *entry, const char *name)
{
    memset(result, 0, sizeof(*result));
    ExfatAlias((unsigned char *)result, name, entry->cluster);
    result->HighCluster = entry->cluster >> 16;
    result->StartCluster = entry->cluster;
    result->Attributes = entry->attributes;
    /* The OSD's synthesized entries can reuse the FAT creation time field.
     * Actual FAT directory entries keep their standard 32-byte layout. */
    result->CreateTime = entry->size_high;
    result->FileSize = entry->size;
}

static unsigned int ExfatFileOpen(fileTYPE *file, const char *name)
{
    ExfatDirectory dir;
    ExfatEntry entry;
    unsigned long parent;
    char long_name[256];
    unsigned char alias[11];

    if (ExfatDirectoryAt(&dir, iCurrentDirectory, &parent))
        while (ExfatNextEntry(&dir, &entry, long_name))
        {
            if (entry.attributes & ATTR_DIRECTORY)
                continue;
            ExfatAlias(alias, long_name, entry.cluster);
            if (ExfatNameEqual(name, long_name) ||
                (strlen(name) == 11 && !_strnicmp(name, (const char *)alias, 11)))
            {
                memset(file, 0, sizeof(*file));
                memcpy(file->name, alias, sizeof(file->name));
                strcpy(file->long_name, long_name);
                file->attributes = entry.attributes;
                file->size = entry.size;
                file->size_high = entry.size_high;
                file->cluster = file->start_cluster = entry.cluster;
                return 1;
            }
        }
    memset(file, 0, sizeof(*file));
    SetError(ERROR_FILESYSTEM, "File not found", 0, 0);
    return 0;
}

unsigned int FileOpen(fileTYPE *file, const char *name)
{
//    unsigned long  iDirectory = 0;       // only root directory is supported
    DIRENTRY      *pEntry = NULL;        // pointer to current entry in sector buffer
    unsigned long  iDirectorySector;     // current sector of directory entries table
    unsigned long  iDirectoryCluster;    // start cluster of subdirectory or FAT32 root directory
    unsigned long  iEntry;               // entry index in directory cluster or FAT16 root directory
    unsigned long  nEntries;             // number of entries per cluster or FAT16 root directory size

    if (exfat)
        return ExfatFileOpen(file, name);

    if (iCurrentDirectory) // subdirectory
    {
        iDirectoryCluster = iCurrentDirectory;
        iDirectorySector = data_start + cluster_size * (iDirectoryCluster - 2);
        nEntries = cluster_size << 4; // 16 entries per sector
    }
    else // root directory
    {
        iDirectoryCluster = root_directory_cluster;
        iDirectorySector = root_directory_start;
        nEntries = fat32 ?  cluster_size << 4 : root_directory_size << 4; // 16 entries per sector
    }

    while (1)
    {
        for (iEntry = 0; iEntry < nEntries; iEntry++)
        {
            if ((iEntry & 0x0F) == 0) // first entry in sector, load the sector
            {
                if(!MMC_Read(iDirectorySector++, sector_buffer)) // root directory is linear
					return(0);
                pEntry = (DIRENTRY*)sector_buffer;
            }
            else
                pEntry++;


            if (pEntry->Name[0] != SLOT_EMPTY && pEntry->Name[0] != SLOT_DELETED) // valid entry??
            {
				if(doLFN(pEntry,fileLFN))
				{
//					printf("Have long filename entry\n");
				}
                else if (!(pEntry->Attributes & (ATTR_VOLUME | ATTR_DIRECTORY))) // not a volume nor directory
                {
                    if (strncmp(fileLFN,name,strlen(name))==0 || strncmp((const char*)pEntry->Name, name, sizeof(file->name)) == 0)
                    {
                        strncpy(file->name, (const char*)pEntry->Name, sizeof(file->name));
                        file->attributes = pEntry->Attributes;
//                        file->size = pEntry->FileSize; 																		// it only works when using little endian long representation
                        file->size_high = 0;
                        file->size = SwapBBBB(pEntry->FileSize); 		// for 68000
//                        file->size = ((file->size>>24)&0xFF)|((file->size>>8)&0xFF00)|((file->size<<8)&0xFF0000)|((file->size<<24)&0xFF000000); // for 68000 
//                        file->start_cluster = pEntry->StartCluster + (fat32 ? (pEntry->HighCluster & 0x0FFF) << 16 : 0); 	// it only works when using little endian long representation
                        file->start_cluster = SwapBB(pEntry->StartCluster) + (fat32 ? (SwapBB(pEntry->HighCluster) & 0x0FFF) << 16 : 0); 	// it only works when using little endian long representation for 68000 
//                        file->start_cluster = (((pEntry->StartCluster>>8)&0xFF)|((pEntry->StartCluster<<8)&0xFF00)) | (fat32 ? ((pEntry->HighCluster & 0x0F) << 24)|((pEntry->HighCluster & 0xFF00) << 8)  : 0);  // for 68000 
                        file->cluster =  file->start_cluster;
                        file->sector = 0;
                        file->entry.sector = iDirectorySector - 1;
                        file->entry.index = iEntry & 0x0F;

                        printf("file \"%s\" found\r", name);

                        return(1);
                    }
                }
				else
					fileLFN[0]=0; /* Invalidate LFN if we encounter anything between LFN and its file */
            }
        }

        if (iCurrentDirectory || fat32) // subdirectory is a linked cluster chain
        {
            iDirectoryCluster = GetFATLink(iDirectoryCluster); // get next cluster in chain

            if (fat32 ? (iDirectoryCluster & 0x0FFFFFF8) == 0x0FFFFFF8 : (iDirectoryCluster & 0xFFF8) == 0xFFF8) // check if end of cluster chain
                break; // no more clusters in chain

            iDirectorySector = data_start + cluster_size * (iDirectoryCluster - 2); // calculate first sector address of the new cluster
        }
        else
            break;
    }

	SetError(ERROR_FILESYSTEM,"File not found",0,0);

    printf("file \"%s\" not found\r", name);
    memset(file, 0, sizeof(fileTYPE));
    return(0);
}


// Verify that a directory cluster is valid by recursively tracing ".." entries back up to the root,
// then verifying that each directory entry exists within its parent.
// Returns 0 on failure
int ValidateDirectory(unsigned long directory)
{
    DIRENTRY      *pEntry = NULL;        // pointer to current entry in sector buffer
    unsigned long  iDirectorySector;     // current sector of directory entries table
    unsigned long  iDirectoryCluster;    // start cluster of subdirectory or FAT32 root directory
    unsigned long  iEntry;               // entry index in directory cluster or FAT16 root directory
    if (exfat)
    {
        unsigned long size, parent;
        return ExfatLocateDirectory(directory, &size, &parent);
    }

	if(!directory || (directory==root_directory_cluster))
	{
		return(1);
	}
    else // subdirectory
    {
        iDirectoryCluster = directory;
        iDirectorySector = data_start + cluster_size * (iDirectoryCluster - 2);
    }

    if(!MMC_Read(iDirectorySector++, sector_buffer)) // root directory is linear
		return(0);
    pEntry = (DIRENTRY*)sector_buffer;
    for (iEntry = 0; iEntry < 16; iEntry++)	// 16 entries in a single sector.  Assume ".." will be in the first sector.
    {
        if (pEntry->Name[0] != SLOT_EMPTY && pEntry->Name[0] != SLOT_DELETED) // valid entry??
        {
            if (pEntry->Attributes & ATTR_DIRECTORY) // is this a directory
            {
                if (strncmp((const char*)pEntry->Name, "..         ", sizeof(pEntry->Name)) == 0)
                {
					unsigned long parent=SwapBB(pEntry->StartCluster) + (fat32 ? (SwapBB(pEntry->HighCluster) & 0x0FFF) << 16 : 0);
                    printf("Parent directory is %ld \r",parent);
                    return(ValidateDirectory(parent) && FindDirectoryByCluster(parent,directory));
                }
            }
        }
        pEntry++;
    }
	return(0);
}


unsigned long FindDirectoryByCluster(unsigned long parent, unsigned long cluster)
{
    DIRENTRY      *pEntry = NULL;        // pointer to current entry in sector buffer
    unsigned long  iDirectorySector;     // current sector of directory entries table
    unsigned long  iDirectoryCluster;    // start cluster of subdirectory or FAT32 root directory
    unsigned long  iEntry;               // entry index in directory cluster or FAT16 root directory
    unsigned long  nEntries;             // number of entries per cluster or FAT16 root directory size
    if (exfat)
    {
        unsigned long size, actual_parent;
        return cluster && ExfatLocateDirectory(cluster, &size, &actual_parent) &&
               parent == actual_parent;
    }

    if (parent) // subdirectory
    {
        iDirectoryCluster = parent;
        iDirectorySector = data_start + cluster_size * (iDirectoryCluster - 2);
        nEntries = cluster_size << 4; // 16 entries per sector
    }
    else // root directory
    {
        iDirectoryCluster = root_directory_cluster;
        iDirectorySector = root_directory_start;
        nEntries = fat32 ?  cluster_size << 4 : root_directory_size << 4; // 16 entries per sector
    }

    while (1)
    {
        for (iEntry = 0; iEntry < nEntries; iEntry++)
        {
            if ((iEntry & 0x0F) == 0) // first entry in sector, load the sector
            {
                if(!MMC_Read(iDirectorySector++, sector_buffer)) // root directory is linear
					return(0);
                pEntry = (DIRENTRY*)sector_buffer;
            }
            else
                pEntry++;


            if (pEntry->Name[0] != SLOT_EMPTY && pEntry->Name[0] != SLOT_DELETED) // valid entry??
            {
                if (pEntry->Attributes & ATTR_DIRECTORY) // is this a directory
                {
					unsigned long dircluster=SwapBB(pEntry->StartCluster) + (fat32 ? (SwapBB(pEntry->HighCluster) & 0x0FFF) << 16 : 0);
					if(dircluster==cluster)
					{
                        printf("Found directory at %ld \r",dircluster);
                        return(1);
                    }
                }
            }
        }

        if (parent || fat32) // subdirectory is a linked cluster chain
        {
            iDirectoryCluster = GetFATLink(iDirectoryCluster); // get next cluster in chain

            if (fat32 ? (iDirectoryCluster & 0x0FFFFFF8) == 0x0FFFFFF8 : (iDirectoryCluster & 0xFFF8) == 0xFFF8) // check if end of cluster chain
                break; // no more clusters in chain

            iDirectorySector = data_start + cluster_size * (iDirectoryCluster - 2); // calculate first sector address of the new cluster
        }
        else
            break;
    }

    printf("Directory not found at %ld\r",cluster);
    return(0);
}


unsigned long FindDirectory(unsigned long parent, const char *name)
{
    DIRENTRY      *pEntry = NULL;        // pointer to current entry in sector buffer
    unsigned long  iDirectorySector;     // current sector of directory entries table
    unsigned long  iDirectoryCluster;    // start cluster of subdirectory or FAT32 root directory
    unsigned long  iEntry;               // entry index in directory cluster or FAT16 root directory
    unsigned long  nEntries;             // number of entries per cluster or FAT16 root directory size
    if (exfat)
    {
        ExfatDirectory dir;
        ExfatEntry entry;
        unsigned long grandparent;
        char long_name[256];
        if (ExfatDirectoryAt(&dir, parent, &grandparent))
            while (ExfatNextEntry(&dir, &entry, long_name))
                if ((entry.attributes & ATTR_DIRECTORY) && ExfatNameEqual(name, long_name))
                    return entry.cluster;
        return 0;
    }

    if (parent) // subdirectory
    {
        iDirectoryCluster = parent;
        iDirectorySector = data_start + cluster_size * (iDirectoryCluster - 2);
        nEntries = cluster_size << 4; // 16 entries per sector
    }
    else // root directory
    {
        iDirectoryCluster = root_directory_cluster;
        iDirectorySector = root_directory_start;
        nEntries = fat32 ?  cluster_size << 4 : root_directory_size << 4; // 16 entries per sector
    }

    while (1)
    {
        for (iEntry = 0; iEntry < nEntries; iEntry++)

        {
            if ((iEntry & 0x0F) == 0) // first entry in sector, load the sector
            {
                if(!MMC_Read(iDirectorySector++, sector_buffer)) // root directory is linear
					return(0);
                pEntry = (DIRENTRY*)sector_buffer;
            }
            else
                pEntry++;


            if (pEntry->Name[0] != SLOT_EMPTY && pEntry->Name[0] != SLOT_DELETED) // valid entry??
            {
                if (pEntry->Attributes & ATTR_DIRECTORY) // is this a directory
                {
                    if (strncmp((const char*)pEntry->Name, name, sizeof(pEntry->Name)) == 0)
                    {
						unsigned long dircluster=SwapBB(pEntry->StartCluster) + (fat32 ? (SwapBB(pEntry->HighCluster) & 0x0FFF) << 16 : 0);
                        printf("Found directory \"%s\" at %ld \r",name,dircluster);

                        return(dircluster);
                    }
                }
            }
        }

        if (parent || fat32) // subdirectory is a linked cluster chain
        {
            iDirectoryCluster = GetFATLink(iDirectoryCluster); // get next cluster in chain

            if (fat32 ? (iDirectoryCluster & 0x0FFFFFF8) == 0x0FFFFFF8 : (iDirectoryCluster & 0xFFF8) == 0xFFF8) // check if end of cluster chain
                break; // no more clusters in chain

            iDirectorySector = data_start + cluster_size * (iDirectoryCluster - 2); // calculate first sector address of the new cluster
        }
        else
            break;
    }

    printf("Directory \"%s\" not found\r", name);
    return(0);
}


unsigned char lfn_checksum(unsigned char *pName)
{
    unsigned char i = 11;
    unsigned char checksum = 0;

    while (i--)
        checksum = ((checksum & 1) << 7) + (checksum >> 1) + *pName++;

    return checksum;
}

int _strnicmp(const char *s1, const char *s2, size_t n)
{
    char c1, c2;
    int v;

    do
    {
        c1 = *s1++;
        c2 = *s2++;
        v = (unsigned int)tolower(c1) - (unsigned int)tolower(c2);
    }
    while (v == 0 && c1 != '\0' && --n > 0);

    return v;
}

int CompareDirEntries(DIRENTRY *pDirEntry1, char *pLFN1, DIRENTRY *pDirEntry2, char *pLFN2)
{
    const char *pStr1, *pStr2;
    int len;
    int rc;

    if (exfat)
    {
        int parent1 = !memcmp(pDirEntry1->Name, "..      ", 8);
        int parent2 = !memcmp(pDirEntry2->Name, "..      ", 8);
        if (parent1 != parent2)
            return parent2 - parent1;
    }

    if ((pDirEntry1->Attributes & ATTR_DIRECTORY) && !(pDirEntry2->Attributes & ATTR_DIRECTORY)) // directories first
       return -1;

    if (!(pDirEntry1->Attributes & ATTR_DIRECTORY) && (pDirEntry2->Attributes & ATTR_DIRECTORY)) // directories first
       return 1;

    len = 260;
    if (*pLFN1)
       pStr1 = pLFN1;
    else
    {
        pStr1 = (const char*)pDirEntry1->Name;
        len = 11;
    }

    if (*pLFN2)
       pStr2 = pLFN2;
    else
    {
        pStr2 = (const char*)pDirEntry2->Name;
        len = 11;
    }

    rc = _strnicmp(pStr1, pStr2, len);

    if (rc == 0) // it might happen that both strings are equal when one is a long name and other not
    {
        if (*pLFN1)
        {
            if (!*pLFN2) // first string long, second short
                rc = 1;
        }
        else
        {
            if (*pLFN2) // first string short, second long
                rc = -1;
        }
    }
    if (exfat && !rc)
        rc = memcmp(pDirEntry1, pDirEntry2, 11);
    return(rc);
}

int ScanDirectory(unsigned long mode, char *extension, unsigned char options)
{
    ExfatDirectory exfat_dir;
    DIRENTRY exfat_entry;
    unsigned long parent = 0;
    int parent_pending = 0;
    DIRENTRY *pEntry = NULL;            // pointer to current entry in sector buffer
    unsigned long iDirectorySector;     // current sector of directory entries table
    unsigned long iDirectoryCluster;
    unsigned long iEntry;               // entry index in directory
    unsigned long nEntries;
             char i;
    unsigned char x;
    unsigned char nNewEntries = 0;      // indicates if a new entry has been found (used in scroll mode)
    char rc = 0; //return code
    char find_file = 0;
    char find_dir = 0;
    char is_file = 0;

    unsigned char sequence_number = 0;
    unsigned char name_checksum = 0;
    unsigned char prev_sequence_number = 0;
    unsigned char prev_name_checksum = 0;

    char *ptr;
    static char lfn[261];
    unsigned char lfn_error = 1;
    /*
    unsigned long time;
    time = GetTimer(0);
    */
    lfn[0] = 0;

    if (mode == SCAN_INIT)
    {
        nDirEntries = 0;
        iSelectedEntry = 0;
        for (i = 0; i < MAXDIRENTRIES; i++)
            sort_table[i] = i;
    }
    else
    {
        if (nDirEntries == 0) // directory is empty so there is no point in searching for any entry
            return 0;

        if (mode == SCAN_NEXT)
        {
            if (iSelectedEntry + 1 < nDirEntries) // scroll within visible items
            {
                iSelectedEntry++;
                return 0;
            }
            if (nDirEntries < MAXDIRENTRIES)
                return 0;
        }
        else if (mode == SCAN_PREV)
        {
            if (iSelectedEntry > 0) // scroll within visible items
            {
                iSelectedEntry--;
                return 0;
            }
        }
        else if (mode ==SCAN_NEXT_PAGE)
        {
            if (iSelectedEntry + 1 < nDirEntries)
            {
                iSelectedEntry = nDirEntries - 1;
                return 0;
            }
            if (nDirEntries < MAXDIRENTRIES)
                return 0;
        }
        else if (mode == SCAN_PREV_PAGE)
        {
            if (iSelectedEntry)
            {
                iSelectedEntry = 0;
                return 0;
            }
        }

        find_file = options & FIND_FILE;
        find_dir = options & FIND_DIR;
    }

    if (exfat)
    {
        if (!ExfatDirectoryAt(&exfat_dir, iCurrentDirectory, &parent))
            return 0;
        parent_pending = iCurrentDirectory != 0;
        nEntries = 0;
    }
    else if (iCurrentDirectory) // subdirectory
    {
		printf("Scanning directory %x\n",iCurrentDirectory);
        iDirectoryCluster = iCurrentDirectory;
        iDirectorySector = data_start + cluster_size * (iDirectoryCluster - 2);
        nEntries = cluster_size << 4; // 16 entries per sector
    }
    else // root directory
    {
		printf("Scanning root directory %x\n",root_directory_cluster);
        iDirectoryCluster = root_directory_cluster;
        iDirectorySector = root_directory_start;
        nEntries = fat32 ?  cluster_size << 4 : root_directory_size << 4; // 16 entries per sector
    }

    while (1)
    {
        for (iEntry = 0; exfat || iEntry < nEntries; iEntry++)
        {
            if (exfat)
            {
                ExfatEntry entry;
                if (parent_pending)
                {
                    parent_pending = 0;
                    memset(&exfat_entry, 0, sizeof(exfat_entry));
                    memcpy(exfat_entry.Name, "..         ", 11);
                    exfat_entry.Attributes = ATTR_DIRECTORY;
                    exfat_entry.StartCluster = parent;
                    exfat_entry.HighCluster = parent >> 16;
                }
                else
                {
                    if (!ExfatNextEntry(&exfat_dir, &entry, lfn))
                        break;
                    if (!entry.size && !entry.size_high)
                        continue;
                    ExfatMenuEntry(&exfat_entry, &entry, lfn);
                    name_checksum = lfn_checksum(exfat_entry.Name);
                }
                pEntry = &exfat_entry;
            }
            else if ((iEntry & 0xF) == 0) // first entry in sector, load the sector
            {
                if(!MMC_Read(iDirectorySector++, sector_buffer))
					return(0);
                pEntry = (DIRENTRY*)sector_buffer;
				for (i = 0; i < 16; i++) 
				{
					if (pEntry->Attributes != ATTR_LFN)
					{
						pEntry->StartCluster = SwapBB(pEntry->StartCluster);
						pEntry->HighCluster = SwapBB(pEntry->HighCluster);
						pEntry->FileSize = SwapBBBB(pEntry->FileSize);
					}
					pEntry++;
				}		
                pEntry = (DIRENTRY*)sector_buffer;
            }
            else
                pEntry++;

            if (pEntry->Name[0] != SLOT_EMPTY && pEntry->Name[0] != SLOT_DELETED) // valid entry??
            {
                if (pEntry->Attributes == ATTR_LFN) // long file name entry
                {
                    if (options & SCAN_LFN)
                    {
                        sequence_number = ((unsigned char*)pEntry)[0];
                        name_checksum = ((unsigned char*)pEntry)[13];
                        ptr  = &lfn[((sequence_number & 0x1F) - 1) * 13];

                        if (sequence_number & 0x40)
                            lfn_error = 0;
                        else
                            if ((sequence_number & 0x1F) != (prev_sequence_number & 0x1F) - 1
									|| name_checksum != prev_name_checksum || (sequence_number & 0x1F) > sizeof(lfn) / 13 - 1)
                                lfn_error = 1;

                        prev_sequence_number = sequence_number;
                        prev_name_checksum = name_checksum;

                        if (!lfn_error)
                        {
                            *ptr++ = ((unsigned char*)pEntry)[1];
                            *ptr++ = ((unsigned char*)pEntry)[3];
                            *ptr++ = ((unsigned char*)pEntry)[5];
                            *ptr++ = ((unsigned char*)pEntry)[7];
                            *ptr++ = ((unsigned char*)pEntry)[9]; // first 5 characters
                            *ptr++ = ((unsigned char*)pEntry)[14];
                            *ptr++ = ((unsigned char*)pEntry)[16];
                            *ptr++ = ((unsigned char*)pEntry)[18];
                            *ptr++ = ((unsigned char*)pEntry)[20];
                            *ptr++ = ((unsigned char*)pEntry)[22];
                            *ptr++ = ((unsigned char*)pEntry)[24]; // next 6 characters
                            *ptr++ = ((unsigned char*)pEntry)[28];
                            *ptr++ = ((unsigned char*)pEntry)[30]; // last 2 characters

                            if (sequence_number & 0x40) // last lfn part
                                *ptr++ = 0;
//							printf("Long filename: %s\n",&lfn[((sequence_number & 0x1F) - 1) * 13]);
                        }
                        else
						{
							SetError(ERROR_FILESYSTEM,"bad long filename",sequence_number,name_checksum);
                            printf("LFN error!\r");
						}
                    }
                }
                else // if not an LFN entry
                {
                    is_file = ~pEntry->Attributes & ATTR_DIRECTORY;

					 // if not VOLUME label (also filter current directory entry)
                    if (!(pEntry->Attributes & (ATTR_VOLUME | ATTR_HIDDEN)) && (pEntry->Name[0] != '.' || pEntry->Name[1] != ' '))
                    {
                        if ((extension[0] == '*') || (strncmp((const char*)&pEntry->Name[8], extension, 3) == 0)
								 || (options & SCAN_DIR && pEntry->Attributes & ATTR_DIRECTORY))
                        {
                            if (mode == SCAN_INIT)
                            { // scan the directory table and return first MAXDIRENTRIES alphabetically sorted entries
                                if (nDirEntries < MAXDIRENTRIES) // initial directory scan (first 8 entries)
                                {
                                    DirEntry[nDirEntries] = *pEntry; // add new entry at first empty slot in storage buffer
                                    DirEntryLFN[nDirEntries][0] = 0;
                                    if (lfn[0])
                                        if (lfn_checksum(pEntry->Name) == name_checksum)
                                            strncpy(DirEntryLFN[nDirEntries], lfn, sizeof(lfn));

                                    nDirEntries++;
                                }
                                else
                                {
									 // compare new entry with the last already found
                                    if (CompareDirEntries(pEntry, lfn, &DirEntry[sort_table[MAXDIRENTRIES-1]],
											DirEntryLFN[sort_table[MAXDIRENTRIES-1]]) < 0)
                                    {
										// replace the last entry with the new one if appropriate
                                        DirEntry[sort_table[MAXDIRENTRIES-1]] = *pEntry;
                                        DirEntryLFN[sort_table[MAXDIRENTRIES-1]][0] = 0;
                                        if (lfn[0])
                                            if (lfn_checksum(pEntry->Name) == name_checksum)
                                                strncpy(DirEntryLFN[sort_table[MAXDIRENTRIES-1]], lfn, sizeof(lfn));
                                    }
                                }

								// one pass bubble-sorting (table is already sorted, only the new item must be placed in order)
                                for (i = nDirEntries - 1; i > 0; i--)
                                {
									// compare items and swap if necessary
                                    if (CompareDirEntries(&DirEntry[sort_table[i]], DirEntryLFN[sort_table[i]],
											&DirEntry[sort_table[i-1]], DirEntryLFN[sort_table[i-1]])<0)
                                    {
                                        x = sort_table[i];
                                        sort_table[i] = sort_table[i-1];
                                        sort_table[i-1] = x;
                                    }
                                    else
                                        break; // don't check further entries as they are already sorted
                                }
                            }
                            else if (mode == SCAN_INIT_FIRST)
                            { // find a dir entry with given cluster number and store it in the buffer
                                if (DirectoryEntryCluster(pEntry) == iPreviousDirectory)
                                { // directory entry found
                                    for (i = 0; i< MAXDIRENTRIES; i++)
                                        sort_table[i] = i; // init sorting table

                                    nDirEntries = 1;
                                    iSelectedEntry = 0;

                                    DirEntry[0] = *pEntry; // add the entry at the top of the buffer
                                    DirEntryLFN[0][0] = 0;
                                    if (lfn[0])
                                        if (lfn_checksum(pEntry->Name) == name_checksum)
                                            strncpy(DirEntryLFN[0], lfn, sizeof(lfn));

                                    rc = 1; // indicate to the caller that the directory entry has been found
                                }
                            }
                            else if (mode == SCAN_INIT_NEXT)
                            {
								// scan the directory table and return next MAXDIRENTRIES-1
								// alphabetically sorted entries (first entry is in the buffer)

								// compare new entry with the first one
                                if (CompareDirEntries(pEntry, lfn, &DirEntry[sort_table[0]], DirEntryLFN[sort_table[0]]) > 0)
                                {
                                    if (nDirEntries < MAXDIRENTRIES) // initial directory scan (first 8 entries)
                                    {
                                        DirEntry[nDirEntries] = *pEntry; // add new entry at first empty slot in storage buffer
                                        DirEntryLFN[nDirEntries][0] = 0;
                                        if (lfn[0])
                                            if (lfn_checksum(pEntry->Name) == name_checksum)
                                                strncpy(DirEntryLFN[nDirEntries], lfn, sizeof(lfn));

                                        nDirEntries++;
                                    }
                                    else
                                    {
										// compare new entry with the last already found
                                        if (CompareDirEntries(pEntry, lfn, &DirEntry[sort_table[MAXDIRENTRIES-1]],
												DirEntryLFN[sort_table[MAXDIRENTRIES-1]]) < 0)
                                        {
											// replace the last entry with the new one if appropriate
                                            DirEntry[sort_table[MAXDIRENTRIES-1]] = *pEntry;
                                            DirEntryLFN[sort_table[MAXDIRENTRIES-1]][0] = 0;
                                            if (lfn[0])
                                                if (lfn_checksum(pEntry->Name) == name_checksum)
                                                    strncpy(DirEntryLFN[sort_table[MAXDIRENTRIES-1]], lfn, sizeof(lfn));
                                        }
                                    }

									// one pass bubble-sorting (table is already sorted, only the new item must be placed in order)
                                    for (i = nDirEntries - 1; i > 0; i--)
                                    {
										// compare items and swap if necessary
                                        if (CompareDirEntries(&DirEntry[sort_table[i]], DirEntryLFN[sort_table[i]],
												&DirEntry[sort_table[i-1]], DirEntryLFN[sort_table[i-1]])<0)
                                        {
                                            x = sort_table[i];
                                            sort_table[i] = sort_table[i-1];
                                            sort_table[i-1] = x;
                                        }
                                        else
                                            break; // don't check further entries as they are already sorted
                                    }
                                }
                            }
                            else if (mode == SCAN_NEXT) // replace the last dir entry with the new (higher) one
                            {
                                if (nNewEntries == 0) // no entry higher than the last one has been found yet
                                {
									// found entry higher than the last one
                                    if (CompareDirEntries(pEntry, lfn, &DirEntry[sort_table[MAXDIRENTRIES-1]],
											DirEntryLFN[sort_table[MAXDIRENTRIES-1]]) > 0)
                                    {
                                        nNewEntries++;
                                        DirEntry[sort_table[0]] = *pEntry; // replace the first entry with the found one
                                        DirEntryLFN[sort_table[0]][0] = 0;
                                        if (lfn[0])
                                            if (lfn_checksum(pEntry->Name) == name_checksum)
                                                strncpy(DirEntryLFN[sort_table[0]], lfn, sizeof(lfn));

                                        // scroll entries' indices
                                        x = sort_table[0];
                                        for (i = 0; i < MAXDIRENTRIES-1; i++)
                                            sort_table[i] = sort_table[i+1];

                                        sort_table[MAXDIRENTRIES-1] = x; // last entry is the found one
                                    }
                                }
                                else
                                {
									// higher entry already found but we need to check the remaining ones
									// if any of them is lower then the already found one
                                    // check if the found entry is lower than the last one and higher than the last but one,
									// if so then replace the last one with it
                                    if (CompareDirEntries(pEntry, lfn, &DirEntry[sort_table[MAXDIRENTRIES-1]],
												DirEntryLFN[sort_table[MAXDIRENTRIES-1]]) < 0)
                                        if (CompareDirEntries(pEntry, lfn, &DirEntry[sort_table[MAXDIRENTRIES-2]],
												DirEntryLFN[sort_table[MAXDIRENTRIES-2]]) > 0)
                                        {
                                            DirEntry[sort_table[MAXDIRENTRIES-1]] = *pEntry;
                                            DirEntryLFN[sort_table[MAXDIRENTRIES-1]][0] = 0;
                                            if (lfn[0])
                                                if (lfn_checksum(pEntry->Name) == name_checksum)
                                                    strncpy(DirEntryLFN[sort_table[MAXDIRENTRIES-1]], lfn, sizeof(lfn));
                                        }
                                }
                            }
                            else if (mode == SCAN_PREV) // replace the first dir entry with the new (lower) one
                            {
                                if (nNewEntries == 0) // no entry lower than the first one has been found yet
                                {
									// found entry lower than the first one
                                    if (CompareDirEntries(pEntry, lfn, &DirEntry[sort_table[0]], DirEntryLFN[sort_table[0]]) < 0)
                                    {
                                        nNewEntries++;
                                        if (nDirEntries < MAXDIRENTRIES)
                                        {
                                            //sort_table[nDirEntries] = nDirEntries; // init sorting table
                                            nDirEntries++;
                                        }
										// replace the last entry with the found one
                                        DirEntry[sort_table[MAXDIRENTRIES-1]] = *pEntry;
                                        DirEntryLFN[sort_table[MAXDIRENTRIES-1]][0] = 0;
                                        if (lfn[0])
                                            if (lfn_checksum(pEntry->Name) == name_checksum)
                                                strncpy(DirEntryLFN[sort_table[MAXDIRENTRIES-1]], lfn, sizeof(lfn));

                                        // scroll entries' indices
                                        x = sort_table[MAXDIRENTRIES-1];
                                        for (i = MAXDIRENTRIES - 1; i > 0; i--)
                                            sort_table[i] = sort_table[i-1];

                                        sort_table[0] = x; // the first entry is the found one
                                    }
                                }
                                else
                                {
									// lower entry already found but we need to check the remaining ones
									// if any of them is higher then the already found one
                                    // check if the found entry is higher than the first one and lower than the second one,
									// if so then replace the first one with it
                                    if (CompareDirEntries(pEntry, lfn, &DirEntry[sort_table[0]], DirEntryLFN[sort_table[0]]) > 0)
                                        if (CompareDirEntries(pEntry, lfn, &DirEntry[sort_table[1]], DirEntryLFN[sort_table[1]]) < 0)
                                        {
                                            DirEntry[sort_table[0]] = *pEntry;
                                            DirEntryLFN[sort_table[0]][0] = 0;
                                            if (lfn[0])
                                                if (lfn_checksum(pEntry->Name) == name_checksum)
                                                    strncpy(DirEntryLFN[sort_table[0]], lfn, sizeof(lfn));
                                        }
                                }
                            }
                            else if (mode == SCAN_NEXT_PAGE) // find next 8 entries
                            {
								// compare with the last visible entry
                                if (CompareDirEntries(pEntry, lfn, &DirEntry[sort_table[MAXDIRENTRIES-1]],
										DirEntryLFN[sort_table[MAXDIRENTRIES-1]]) > 0)
                                {
									// initial directory scan (first 8 entries)
                                    if (nNewEntries < MAXDIRENTRIES)
                                    {
										// add new entry at first empty slot in storage buffer
                                        t_DirEntry[nNewEntries] = *pEntry;
                                        t_DirEntryLFN[nNewEntries][0] = 0;
                                        if (lfn[0])
                                            if (lfn_checksum(pEntry->Name) == name_checksum)
                                                strncpy(t_DirEntryLFN[nNewEntries], lfn, sizeof(lfn));

                                        t_sort_table[nNewEntries] = nNewEntries; // init sorting table
                                        nNewEntries++;
                                    }
                                    else
                                    {
										// compare new entry with the last already found
                                        if (CompareDirEntries(pEntry, lfn, &t_DirEntry[t_sort_table[MAXDIRENTRIES-1]],
												t_DirEntryLFN[t_sort_table[MAXDIRENTRIES-1]]) < 0)
                                        {
 											// replace the last entry with the new one if appropriate
                                            t_DirEntry[t_sort_table[MAXDIRENTRIES-1]] = *pEntry;
                                            t_DirEntryLFN[t_sort_table[MAXDIRENTRIES-1]][0] = 0;
                                            if (lfn[0])
                                                if (lfn_checksum(pEntry->Name) == name_checksum)
                                                    strncpy(t_DirEntryLFN[t_sort_table[MAXDIRENTRIES-1]], lfn, sizeof(lfn));
                                        }
                                    }

									// one pass bubble-sorting (table is already sorted, only the new item must be placed in order)
                                    for (i = nNewEntries - 1; i > 0; i--)
                                    {
										// compare items and swap if necessary
                                        if (CompareDirEntries(&t_DirEntry[t_sort_table[i]], t_DirEntryLFN[t_sort_table[i]],
												&t_DirEntry[t_sort_table[i-1]], t_DirEntryLFN[t_sort_table[i-1]]) < 0)
                                        {
                                            x = t_sort_table[i];
                                            t_sort_table[i] = t_sort_table[i-1];
                                            t_sort_table[i-1] = x;
                                        }
                                        else
                                            break; // don't check further entries as they are already sorted
                                    }
                                }
                            }
                            else if (mode == SCAN_PREV_PAGE) // find next 8 entries
                            {
								// compare with the last visible entry
                                if (CompareDirEntries(pEntry, lfn, &DirEntry[sort_table[0]], DirEntryLFN[sort_table[0]]) < 0)
                                {
									// initial directory scan (first 8 entries)
                                    if (nNewEntries < MAXDIRENTRIES)
                                    {
										// add new entry at first empty slot in storage buffer
                                        t_DirEntry[nNewEntries] = *pEntry;
                                        t_DirEntryLFN[nNewEntries][0] = 0;
                                        if (lfn[0])
                                            if (lfn_checksum(pEntry->Name) == name_checksum)
                                                strncpy(t_DirEntryLFN[nNewEntries], lfn, sizeof(lfn));

                                        t_sort_table[nNewEntries] = nNewEntries; // init sorting table
                                        nNewEntries++;
                                    }
                                    else
                                    {
										// compare new entry with the last already found
                                        if (CompareDirEntries(pEntry, lfn, &t_DirEntry[t_sort_table[MAXDIRENTRIES-1]],
												t_DirEntryLFN[t_sort_table[MAXDIRENTRIES-1]]) > 0)
                                        {
											// replace the last entry with the new one if appropriate
                                            t_DirEntry[t_sort_table[MAXDIRENTRIES-1]] = *pEntry;
                                            t_DirEntryLFN[t_sort_table[MAXDIRENTRIES-1]][0] = 0;
                                            if (lfn[0])
                                                if (lfn_checksum(pEntry->Name) == name_checksum)
                                                    strncpy(t_DirEntryLFN[t_sort_table[MAXDIRENTRIES-1]], lfn, sizeof(lfn));
                                        }
                                    }
									// one pass bubble-sorting (table is already sorted, only the new item must be placed in order)
                                    for (i = nNewEntries - 1; i > 0; i--)
                                    {
										// compare items and swap if necessary
                                        if (CompareDirEntries(&t_DirEntry[t_sort_table[i]], t_DirEntryLFN[t_sort_table[i]],
													&t_DirEntry[t_sort_table[i-1]], t_DirEntryLFN[t_sort_table[i-1]]) > 0)
                                        {
                                            x = t_sort_table[i];
                                            t_sort_table[i] = t_sort_table[i-1];
                                            t_sort_table[i-1] = x;
                                        }
                                        else
                                            break; // don't check further entries as they are already sorted
                                    }
                                }
                            }
                            else if ((mode >= '0' && mode <= '9') || (mode >= 'A' && mode <= 'Z'))
                            {
								// find first entry beginning with given character
                                if (find_file)
                                    x = tolower(pEntry->Name[0]) >= tolower(mode) && is_file;
                                else if (find_dir)
                                    x = tolower(pEntry->Name[0]) >= tolower(mode) || is_file;
                                else
									// compare with the last visible entry
                                    x = (CompareDirEntries(pEntry, lfn, &DirEntry[sort_table[iSelectedEntry]],
											DirEntryLFN[sort_table[iSelectedEntry]]) > 0);

                                if (x)
                                {
                                    if (nNewEntries < MAXDIRENTRIES) // initial directory scan (first 8 entries)
                                    {
                                        t_DirEntry[nNewEntries] = *pEntry; // add new entry at first empty slot in storage buffer
                                        t_DirEntryLFN[nNewEntries][0] = 0;
                                        if (lfn[0])
                                            if (lfn_checksum(pEntry->Name) == name_checksum)
                                                strncpy(t_DirEntryLFN[nNewEntries], lfn, sizeof(lfn));

                                        t_sort_table[nNewEntries] = nNewEntries; // init sorting table
                                        nNewEntries++;
                                    }
                                    else
                                    {
										// compare new entry with the last already found
                                        if (CompareDirEntries(pEntry, lfn, &t_DirEntry[t_sort_table[MAXDIRENTRIES-1]],
												t_DirEntryLFN[t_sort_table[MAXDIRENTRIES-1]]) < 0)
                                        {
											 // replace the last entry with the new one if appropriate
                                            t_DirEntry[t_sort_table[MAXDIRENTRIES-1]] = *pEntry;
                                            t_DirEntryLFN[t_sort_table[MAXDIRENTRIES-1]][0] = 0;
                                            if (lfn[0])
                                                if (lfn_checksum(pEntry->Name) == name_checksum)
                                                    strncpy(t_DirEntryLFN[t_sort_table[MAXDIRENTRIES-1]], lfn, sizeof(lfn));
                                        }
                                    }

									// one pass bubble-sorting (table is already sorted, only the new item must be placed in order)
                                    for (i = nNewEntries - 1; i > 0; i--)
                                    {
										 // compare items and swap if necessary
                                        if (CompareDirEntries(&t_DirEntry[t_sort_table[i]],
												t_DirEntryLFN[t_sort_table[i]], &t_DirEntry[t_sort_table[i-1]],
												t_DirEntryLFN[t_sort_table[i-1]]) < 0)
                                        {
                                            x = t_sort_table[i];

                                            t_sort_table[i] = t_sort_table[i-1];
                                            t_sort_table[i-1] = x;
                                        }
                                        else
                                            break; // don't check further entries as they are already sorted
                                    }
                                }
                            }
                        }
                    }
                    lfn[0] = 0;
                }
            }
        }
        if (exfat)
            break;
        if (iCurrentDirectory || fat32) // subdirectory is a linked cluster chain
        {
            iDirectoryCluster = GetFATLink(iDirectoryCluster); // get next cluster in chain
//			printf("iDirectoryCluster: %x\n",iDirectoryCluster);
			// check if end of chain
            if (fat32 ? (iDirectoryCluster & 0x0FFFFFF8) == 0x0FFFFFF8 : (iDirectoryCluster & 0xFFF8) == 0xFFF8)
                break; // no more clusters in chain

			// calculate first sector address of the new cluster
            iDirectorySector = data_start + cluster_size * (iDirectoryCluster - 2);
//			printf("New directory sector: %x\n",iDirectorySector);
        }
        else
            break;
    }


    if (nNewEntries)
    {
        if (mode == SCAN_NEXT_PAGE)
        {
            unsigned char j = MAXDIRENTRIES - nNewEntries; // number of remaining old entries to scroll
            for (i = 0; i < j; i++)
            {
                x = sort_table[i];
                sort_table[i] = sort_table[i + nNewEntries];
                sort_table[i + nNewEntries] = x;
            }
            // copy temporary buffer to display
            for (i = 0; i < nNewEntries; i++)
            {
                DirEntry[sort_table[i + j]] = t_DirEntry[t_sort_table[i]];
                strcpy(DirEntryLFN[sort_table[i + j]], t_DirEntryLFN[t_sort_table[i]]);
            }
        }
        else if (mode == SCAN_PREV_PAGE)
        { // note: temporary buffer entries are in reverse order
            unsigned char j = nNewEntries - 1;
            for (i = 7; i > j; i--)
            {
                x = sort_table[i];
                sort_table[i] = sort_table[i - nNewEntries];
                sort_table[i - nNewEntries] = x;
            }
            // copy temporary buffer to display
            for (i = 0; i < nNewEntries; i++)
            {
                DirEntry[sort_table[j - i]] = t_DirEntry[t_sort_table[i]];
                strcpy(DirEntryLFN[sort_table[j - i]], t_DirEntryLFN[t_sort_table[i]]);
            }
            nDirEntries += nNewEntries;
            if (nDirEntries > MAXDIRENTRIES)
                nDirEntries = MAXDIRENTRIES;
        }
        else if ((mode >= '0' && mode <= '9') || (mode >= 'A' && mode <= 'Z'))
        {
            if (t_DirEntry[t_sort_table[0]].Name[0] == mode)
            {
                x = 1; // if we were looking for a file we couldn't find anything other
                if (find_dir)
                { // when looking for a directory we could find a file beginning with the same character as given one
                     x = t_DirEntry[t_sort_table[0]].Attributes & ATTR_DIRECTORY;
                }
                else if (!find_file) // find_next
                { // when looking for a directory we could find a file beginning with the same character as given one
                    x = (t_DirEntry[t_sort_table[0]].Attributes & ATTR_DIRECTORY) == (DirEntry[sort_table[iSelectedEntry]].Attributes & ATTR_DIRECTORY);
                }

                if (x)
                { // first entry is what we were searching for
                    for (i = 0; i < nNewEntries; i++)
                    {
                        DirEntry[sort_table[i]] = t_DirEntry[t_sort_table[i]];
                        strcpy(DirEntryLFN[sort_table[i]], t_DirEntryLFN[t_sort_table[i]]);
                    }
                    nDirEntries = nNewEntries;
                    iSelectedEntry = 0;
                    rc = 1; // inform the caller that the search succeeded
                }
            }
        }
    }
    /*
    time = GetTimer(0) - time;
    printf("ScanDirectory(): %lu ms\r", time >> 20);
    */
    return rc;
}

void ChangeDirectory(unsigned long iStartCluster)
{
    iPreviousDirectory = iCurrentDirectory;
    iCurrentDirectory = iStartCluster;
}

unsigned long CurrentDirectory()
{
	return(iCurrentDirectory);
}

unsigned long GetFATLink(unsigned long cluster)
{
// this function returns linked cluster for the given one
// remember to check if the returned value indicates end of chain condition

    unsigned long fat_index;
    unsigned short buffer_index;

    if (fat32)
    {
        fat_index    = cluster >> 7;    // calculate sector number in the FAT32 that contains the desired link (256 links per sector)
        buffer_index = cluster & 0x7F;  // calculate offset in the buffered FAT32 sector containing the link
    }
    else
    {
        fat_index    = cluster >> 8;    // calculate sector number in the FAT16 that contains the desired link (256 links per sector)
        buffer_index = cluster & 0xFF;  // calculate offset in the buffered FAT16 sector containing the link
    }

    // read the desired FAT sector if not already in the buffer
    if (fat_index != buffered_fat_index)
    {
        if (!MMC_Read(fat_start+fat_index, (unsigned char*)&fat_buffer))
            return(0);

        // remember the index of buffered FAT sector
        buffered_fat_index = fat_index;
    }


//    return(fat32 ? fat_buffer.fat32[buffer_index] & 0x0FFFFFFF : fat_buffer.fat16[buffer_index]); // get FAT link
    return(fat32 ? SwapBBBB(fat_buffer.fat32[buffer_index]) & 0x0FFFFFFF : SwapBB(fat_buffer.fat16[buffer_index])); // get FAT link for 68000
}

static unsigned int ExfatSeek(fileTYPE *file, unsigned long offset)
{
    unsigned long cluster = file->cluster;
    unsigned long target = offset & cluster_mask;
    unsigned long current = file->sector & cluster_mask;

    unsigned long sectors = FileSectorCount(file);

    if (offset > sectors)
        return 0;
    /* An aligned EOF has no sector to map. Keep the last cluster there. */
    if (sectors && !(file->size & 511))
    {
        if (offset == sectors)
            target = (offset - 1) & cluster_mask;
        if (file->sector == sectors)
            current = (file->sector - 1) & cluster_mask;
    }
    if (cluster & EXFAT_CONTIGUOUS)
    {
        cluster = target >= current ? cluster + (target - current) / cluster_size :
                                       cluster - (current - target) / cluster_size;
        if (!ExfatValidCluster(cluster))
            return 0;
    }
    else
    {
        if (current > target)
        {
            cluster = file->start_cluster;
            current = 0;
        }
        while (current < target)
        {
            cluster = ExfatNextCluster(cluster);
            if (!cluster)
                return 0;
            current += cluster_size;
        }
    }
    file->cluster = cluster;
    file->sector = offset;
    return 1;
}

#pragma section_code_init
RAMFUNC unsigned int FileNextSector(fileTYPE *file)
{
    unsigned long sb;
    unsigned short i;

    if (exfat)
        return ExfatSeek(file, file->sector + 1);

    // increment sector index
    file->sector++;

	if(file->sector > FileSectorCount(file))	// Attempt to seek beyond the end of the file
	{
		FatalError(ERROR_FILESYSTEM,"NextSector() beyond end of file",file->sector,FileSectorCount(file));
		return(0);
	}

    if (file->sector == FileSectorCount(file) && !(file->size & 511))
        return 1;

    // cluster's boundary crossed?
    if ((file->sector&~cluster_mask) == 0)
    {
        if (fat32)
        {
            sb = file->cluster >> 7; // calculate sector number containing FAT-link
            i = file->cluster & 0x7F; // calculate link offsset within sector
        }
        else
        {
            sb = file->cluster >> 8; // calculate sector number containing FAT-link
            i = file->cluster & 0xFF; // calculate link offsset within sector
        }

        // read sector of FAT if not already in the buffer
        if (sb != buffered_fat_index)
        {
            if (!MMC_Read(fat_start + sb, (unsigned char*)&fat_buffer))
                return(0);

            // remember current buffer index
            buffered_fat_index = sb;
        }

        file->cluster = fat32 ? SwapBBBB(fat_buffer.fat32[i]) & 0x0FFFFFFF : SwapBB(fat_buffer.fat16[i]); // get FAT link for 68000 
    }

    return(1);
}
#pragma section_no_code_init

unsigned int FileSeek(fileTYPE *file, unsigned long offset, unsigned long origin)
{
// offset in sectors (512 bytes)
// origin can be set to SEEK_SET or SEEK_CUR

    unsigned long sectors = FileSectorCount(file);
    unsigned long cluster = file->cluster;
    unsigned long current = file->sector;
    unsigned long target;

    if (origin == SEEK_CUR)
    {
        if (current > sectors || offset > sectors - current)
            return 0;
        offset += current;
    }
    if (exfat)
        return ExfatSeek(file, offset);
    if (offset > sectors)
    {
        FatalError(ERROR_FILESYSTEM, "Seek() beyond end of file", offset, sectors);
        return 0;
    }

    target = offset;
    if (sectors && !(file->size & 511))
    {
        if (current == sectors)
            --current;
        if (target == sectors)
            --target;
    }
    current &= cluster_mask;
    target &= cluster_mask;
    if (current > target)
    {
        current = 0;
        cluster = file->start_cluster;
    }
    while (current < target)
    {
        cluster = GetFATLink(cluster);
        if (cluster < 2 || cluster >= (fat32 ? 0x0ffffff7UL : 0xfff7UL))
            return 0;
        current += cluster_size;
    }
    file->sector = offset;
    file->cluster = cluster;
    return 1;
}

#pragma section_code_init
RAMFUNC unsigned int FileRead(fileTYPE *file, unsigned char *pBuffer) 
{
    unsigned long sb;

    if (exfat)
    {
        if (file->sector >= FileSectorCount(file) + ((file->size & 511) != 0) ||
            !ExfatValidCluster(file->cluster))
            return 0;
        return MMC_Read(ExfatSector(file->cluster) + (file->sector & ~cluster_mask), pBuffer);
    }

    sb = data_start;                         // start of data in partition
    sb += cluster_size * (file->cluster-2);  // cluster offset
    sb += file->sector & ~cluster_mask;      // sector offset in cluster

    if (!MMC_Read(sb, pBuffer)) // read sector from drive
        return(0);
    else
        return(1);
}
#pragma section_no_code_init

unsigned int FileReadEx(fileTYPE *file, unsigned char *pBuffer, unsigned long nSize)
{
    unsigned long sb;
    unsigned long bc; // block count of single multisector read operation

    if (exfat)
    {
        unsigned long sectors = FileSectorCount(file);
        /* Multi-sector transfers are used by HDFs and may go directly to the FPGA. */
        if (file->sector > sectors || nSize > sectors - file->sector)
            return 0;
        while (nSize)
        {
            if (!ExfatValidCluster(file->cluster))
                return 0;
            bc = cluster_size - (file->sector & ~cluster_mask);
            if (nSize < bc)
                bc = nSize;
            sb = ExfatSector(file->cluster) + (file->sector & ~cluster_mask);
            if (bc == 1 ? !MMC_Read(sb, pBuffer) : !MMC_ReadMultiple(sb, pBuffer, bc))
                return 0;
            if (!ExfatSeek(file, file->sector + bc))
                return 0;
            if (pBuffer)
                pBuffer += bc << 9;
            nSize -= bc;
        }
        return 1;
    }

    while (nSize)
    {
        sb = data_start;                         // start of data in partition
        sb += cluster_size * (file->cluster-2);  // cluster offset
        sb += file->sector & ~cluster_mask;      // sector offset in cluster
        bc = cluster_size - (file->sector & ~cluster_mask); // sector offset in the cluster
        if (nSize < bc)
            bc = nSize;

        if (!MMC_ReadMultiple(sb, pBuffer, bc))
            return 0;

        if (!FileSeek(file, bc, SEEK_CUR))
            return 0;

        nSize -= bc;
    }

    return 1;
}


unsigned int FileWrite(fileTYPE *file,unsigned char *pBuffer)
{
    unsigned long sector;

    if (exfat)
    {
        if ((file->attributes & ATTR_READONLY) ||
            file->sector >= FileSectorCount(file) + ((file->size & 511) != 0) || !ExfatValidCluster(file->cluster))
            return 0;
        return MMC_Write(ExfatSector(file->cluster) + (file->sector & ~cluster_mask), pBuffer);
    }

    sector = data_start;                       // start of data in partition
    sector += cluster_size * (file->cluster-2);  // cluster offset
    sector += file->sector & ~cluster_mask;    // sector offset in cluster

    if (!MMC_Write(sector, pBuffer)) // write sector from drive
        return(0);
    else
        return(1);
}


unsigned int FileCreate(unsigned long iDirectory, fileTYPE *file)
{
    if (exfat || file->size_high)
        return 0;

    // TODO: deleted entries are not empty, they have to be cleared first
    /*
    find empty dir entry
    find empty cluster in fat
    mark cluster in fat as used
    update fat copy
    update dir entry
    */

    DIRENTRY *pEntry = NULL;            // pointer to current entry in sector buffer
    unsigned long iDirectorySector;     // current sector of directory entries table
    unsigned long iDirectoryCluster;    // start cluster of subdirectory or FAT32 root directory
    unsigned long iEntry;               // entry index in directory cluster or FAT16 root directory
    unsigned long nEntries;             // number of entries per cluster or FAT16 root directory size

    if (iDirectory) // subdirectory
    {
        iDirectoryCluster = iDirectory;
        iDirectorySector = data_start + cluster_size * (iDirectoryCluster-2);
        nEntries = cluster_size << 4; // 16 entries per sector
    }
    else // root directory
    {
        iDirectoryCluster = root_directory_cluster;
        iDirectorySector = root_directory_start;
        nEntries = fat32 ?  cluster_size << 4 : root_directory_size << 4; // 16 entries per sector
    }

    while (1)
    {
        for (iEntry = 0; iEntry < nEntries; iEntry++)
        {
            if ((iEntry & 0x0F) == 0) // first entry in sector, load the sector
            {
                if(!MMC_Read(iDirectorySector++, sector_buffer)) // read directory sector
					return(0);
                pEntry = (DIRENTRY*)sector_buffer;
            }
            else
                pEntry++;


            if (pEntry->Name[0] == SLOT_EMPTY)
            {
                printf("Empty entry found in sector %lu at index %lu\r", iDirectorySector-1, iEntry&0x0F);

                // free cluster is marked as 0x0000
                // last cluster in chain is 0xFFFF
                unsigned long fat_index = 0; // first sector of FAT
                unsigned long buffer_index = 2;  // two first entries are reserved
                while (fat_index < fat_size)
                {
                    // read sector of FAT if not already in the buffer
                    if (fat_index != buffered_fat_index)
                    {
                        if (!MMC_Read(fat_start + fat_index, (unsigned char*)&fat_buffer))
                        {
                            printf("FileCreate(): FAT read failed!\r");
                            return(0);
                        }
                        // remember current buffer index
                        buffered_fat_index = fat_index;
                    }

                    unsigned long buffer_size = fat32 ? 128 : 256;
                    while (buffer_index < buffer_size)
                    { // search through all entries in current sector

                        if ((fat32 ? fat_buffer.fat32[buffer_index] : fat_buffer.fat16[buffer_index]) == 0)
                        {   // empty cluster found
                            unsigned long cluster = (fat_index << (fat32 ? 7 : 8)) + buffer_index;  // calculate cluster number

                            printf("Empty cluster: %lu\r", cluster);

                            // mark cluster as used
                            if (fat32)
//                                fat_buffer.fat32[buffer_index] = 0x0FFFFFFF; // FAT32 EOC change
                                fat_buffer.fat32[buffer_index] = 0xFFFFFF0F; // FAT32 EOC change for 68000!
                            else
                                fat_buffer.fat16[buffer_index] = 0xFFFF; // FAT16 EOC

                            // store FAT sector
                            if (!MMC_Write(fat_start + fat_index, (unsigned char*)&fat_buffer))
                            {
                                printf("FileCreate(): FAT write failed!\r");
                                return(0);
                            }

                            // update FAT copies
                            unsigned long i;
                            for (i = 1; i < fat_number; i++)
                            {
                                if (!MMC_Write(fat_start + (i * fat_size) + fat_index, (unsigned char*)&fat_buffer))
                                {
                                    printf("FileCreate(): FAT copy #%lu write failed!\r", i);
                                    return(0);
                                }
                            }

                            // initialize direntry
                            memset((void*)pEntry, 0, sizeof(DIRENTRY));
                            memcpy((void*)pEntry->Name, file->name, 11);
                            pEntry->Attributes = file->attributes;
                            pEntry->CreateDate = SwapBB(FILEDATE(2009, 9, 1));
                            pEntry->CreateTime = SwapBB(FILETIME(0, 0, 0));
                            pEntry->AccessDate = SwapBB(FILEDATE(2009, 9, 1));
                            pEntry->ModifyDate = SwapBB(FILEDATE(2009, 9, 1));
                            pEntry->ModifyTime = SwapBB(FILETIME(0, 0, 0));
                            pEntry->StartCluster = (unsigned short)SwapBB(cluster); // for 68000
                            pEntry->HighCluster = fat32 ? (unsigned short)SwapBB(cluster >> 16) : 0; // for 68000
							printf("FileCreate() filesize (before endian swap) is %lx\n",file->size);
                            pEntry->FileSize = SwapBBBB(file->size); // for 68000
							printf("FileCreate() filesize (after endian swap) is %lx\n",pEntry->FileSize);

                            // store dir entry
                            if (!MMC_Write(iDirectorySector - 1, sector_buffer))
                            {
                                printf("FileCreate(): directory write failed!\r");
                                return(0);
                            }

                            file->start_cluster = cluster;
                            file->cluster = cluster;
                            file->sector = 0;
                            file->entry.sector = iDirectorySector - 1;
                            file->entry.index = iEntry & 0x0F;

                            return(1);
                        }
                        buffer_index++;
                    }
                    buffer_index = 0; // go to the start of sector
                    fat_index++; // go to the next sector of FAT
                }

                return(1);
            }
        }

        if (iDirectory || fat32) // subdirectory is a linked cluster chain
        {
            iDirectoryCluster = GetFATLink(iDirectoryCluster); // get next cluster in chain

            if (fat32 ? (iDirectoryCluster & 0x0FFFFFF8) == 0x0FFFFFF8 : (iDirectoryCluster & 0xFFF8) == 0xFFF8) // check if end of chain
                break; // no more clusters in chain

            iDirectorySector = data_start + cluster_size * (iDirectoryCluster - 2); // calculate first sector address of the new cluster
        }
        else
            break;
    }

    SetError(ERROR_FILESYSTEM,"Can\'t create config file!",0,0);
    return(0);
}


// changing of allocated cluster number is not supported - new size must be within current cluster number
unsigned int UpdateEntry(fileTYPE *file)
{
    if (exfat || file->size_high)
        return 0;

    DIRENTRY *pEntry;

    if (!MMC_Read(file->entry.sector, sector_buffer))
    {
        printf("UpdateEntry(): directory read failed!\r");
        return(0);
    }

    pEntry = (DIRENTRY*)sector_buffer;
    pEntry += file->entry.index;
    memcpy((void*)pEntry->Name, file->name, 11);
    pEntry->Attributes = file->attributes;

    if ((SwapBBBB(pEntry->FileSize) + cluster_size - 1) / (cluster_size << 9) != (file->size + cluster_size - 1) / (cluster_size << 9))
    {
        printf("UpdateEntry(): different number of clusters!\r");
        printf("pEntry->FileSize = %lu\r", SwapBBBB(pEntry->FileSize));
        printf("file->size = %lu\r", file->size);
        printf("cluster_size = %u\r", cluster_size);
        return(0);
    }

      pEntry->FileSize = SwapBBBB(file->size); // for 68000

    if (!MMC_Write(file->entry.sector, sector_buffer))
    {
        printf("UpdateEntry(): directory write failed!\r");
        return(0);
    }

    return(1);
}


