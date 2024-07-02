#include <stdio.h>
#include "rafile.h"
#include "errors.h"

int RARead(RAFile *file,unsigned char *pBuffer, unsigned long bytes)
{
	int result=1;
	// Since we can only read from the SD card on 512-byte aligned boundaries,
	// we need to copy in multiple pieces.
	unsigned long blockoffset=file->ptr&511;	// Offset within the current 512 block at which the previous read finished
												// Bytes blockoffset to 512 will be drained first, before reading new data.

	if(blockoffset)	// If blockoffset is zero we'll just use aligned reads and don't need to drain the buffer.
	{
		int i;
		int l=blockoffset+bytes;
		if(l>512)
			l=512;
		for(i=blockoffset;i<l;++i)
		{
			*pBuffer++=file->buffer[i];
		}
		file->ptr+=l-blockoffset;
		bytes-=l-blockoffset;
	}

	// We've now read any bytes left over from a previous read.  If any data remains to be read we can read it
	// in 512-byte aligned chunks, until the last block.
	while(bytes>511)
	{
		if(file->ptr>=file->file.size)
		{
			SetError(ERROR_FILESYSTEM,"Read past file end",file->ptr,file->file.size);
			return(0);
		}
		result&=FileRead(&file->file,pBuffer);	// Read direct to pBuffer
		file->ptr+=512;
		if(file->ptr<file->file.size)
			FileNextSector(&file->file);
		bytes-=512;
		pBuffer+=512;
	}

	if(bytes)	// Do we have any bytes left to read?
	{
		int i;
		if(file->ptr>=file->file.size)
		{
			SetError(ERROR_FILESYSTEM,"Read past file end",file->ptr,file->file.size);
			return(0);
		}
		result&=FileRead(&file->file,file->buffer);	// Read to temporary buffer, allowing us to preserve any leftover for the next read.
		for(i=0;i<bytes;++i)
		{
			*pBuffer++=file->buffer[i];
		}
		file->ptr+=bytes;
		if(file->ptr<file->file.size)
			FileNextSector(&file->file);
	}
	return(result);
}


int RAReadLine(RAFile *file,unsigned char *pBuffer, unsigned long bytes)
{
	int result=1;
	// Since we can only read from the SD card on 512-byte aligned boundaries,
	// we need to copy in multiple pieces.
	while(bytes)
	{
		unsigned long blockoffset=file->ptr&511;	// Offset within the current 512 block at which the previous read finished
													// Bytes blockoffset to 512 will be drained first, before reading new data.
		int i;
		int l=blockoffset+bytes;
		if(!blockoffset)
		{
			if(file->ptr>=file->file.size)
			{
				SetError(ERROR_FILESYSTEM,"Read past file end",file->ptr,file->file.size);
				return(0);
			}
			result&=FileRead(&file->file,file->buffer);	// Read to temporary buffer, allowing us to preserve any leftover for the next read.
			FileNextSector(&file->file);
		}
		if(l>512)
			l=512;
		for(i=blockoffset;i<l;++i)
		{
			*pBuffer++=file->buffer[i];
			++file->ptr;
			--bytes;
			if(file->buffer[i]=='\n')
			{
				*pBuffer++=0;
				return(1);
			}
		}
	}
	return(0);
}


int RASeek(RAFile *file,unsigned long offset,unsigned long origin)
{
	int result=1;
	unsigned long blockoffset;
	unsigned long blockaddress;
	if(origin==SEEK_CUR)
		offset+=file->ptr;
	blockoffset=offset&511;
	blockaddress=(offset-blockoffset)>>9;	// address in 512-byte sectors...
	result&=FileSeek(&file->file,blockaddress,SEEK_SET);
	if(result && blockoffset)	// If we're seeking into the middle of a block, we need to buffer it...
	{
		result&=FileRead(&file->file,file->buffer);
		FileNextSector(&file->file);
	}
	file->ptr=offset;
	return(result);
}


int RAOpen(RAFile *file,const char *filename)
{
	int result=1;
	if(!file)
		return(0);
	result=FileOpen(&file->file,filename);
	file->size=file->file.size;
	file->ptr=0;
	return(result);
}

