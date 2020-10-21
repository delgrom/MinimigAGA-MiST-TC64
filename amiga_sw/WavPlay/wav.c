#include <stdio.h>
#include <dos/dos.h>

#include "wav.h"

void ErrorMessage(const char *msg);


int wav_read(struct Wav *wav,char *buf,int size)
{
    int result=0;
    if(wav->file)
    {
        result=Read(wav->file,buf,size);
    }
    return(result);
}

static unsigned char tmp[16];

struct Wav *wav_open(const char *filename)
{
    struct Wav *wav=(struct Wav *)malloc(sizeof(struct Wav));
    wav->file=0;
    wav->length=0;
    if(!wav)
        return(0);

    if(wav->file=Open(filename,MODE_OLDFILE))
    {
        int ok=1;
        int bytesread;
        bytesread=Read(wav->file,tmp,12);
        if(bytesread==12)
        {
            if(strncmp("RIFF",tmp,4)==0 && strncmp("WAVE",&tmp[8],4)==0)
            {
                printf("Found WAVE header\n");
                while(ok && !wav->length)
                {
                    int l;
                    bytesread=Read(wav->file,tmp,8);
                    if(bytesread!=8)
                        ok=0;
                    l=(tmp[7]<<24)|(tmp[6]<<16)|(tmp[5]<<8)|tmp[4];
                    if(strncmp("fmt ",tmp,4)==0)
                    {
                        printf("Found fmt chunk\n");
                        if(Read(wav->file,tmp,16)!=16)
                            ok=0;
                        l-=16;
                        if(tmp[0]!=1 || tmp[1]!=0) /* PCM? */
                            ok=0;
                        if(tmp[2]!=2 || tmp[3]!=0) /* Stereo */
                            ok=0;
                        if(tmp[4]!=0x44 || tmp[5]!=0xac || tmp[6]!=0 || tmp[7]!=0) /* 44100Hz */
                            ok=0;
                        if(tmp[14]!=16 || tmp[15]!=0) /* 16 bits */
                            ok=0;
                        if(!ok)
                            ErrorMessage("Format must be 44100Hz 16bit stereo.");
                    }

                    else if(strncmp("data",tmp,4)==0)
                    {
                        printf("Found data chunk with length %d\n",l);
                        wav->length=l;
                        l=0;
                    }
                    else
                        printf("Skipping unknown chunk %x with length %d\n",*(int *)tmp,l);
                    if(l)
                        Seek(wav->file,l,OFFSET_CURRENT);
                }
            }
            else
                ErrorMessage("Not a WAV file");
        }
        else
            ErrorMessage("Can't read header");
        if(!ok)
        {
            Close(wav->file);
            free(wav);
            wav=0;
        }
    }
    return(wav);
}


void wav_close(struct Wav *wav)
{
    if(wav)
    {
        if(wav->file)
            Close(wav->file);
        free(wav);
    }
}

