/*
 * fsarchiver: Filesystem Archiver
 *
 * Copyright (C) 2008-2010 Francois Dupoux.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * Homepage: http://www.fsarchiver.org
 */

#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "fsarchiver.h"
#include "archreader.h"
#include "archwriter.h"
#include "common.h"
#include "error.h"
#include "syncthread.h"

void *thread_writer_fct(void *args)
{
    struct s_headinfo headinfo;
    struct s_blockinfo blkinfo;
    carchwriter *ai=NULL;
    s64 blknum;
    int type;
    
    // init
    inc_secthreads();
    
    if ((ai=(carchwriter *)args)==NULL)
    {   errprintf("ai is NULL\n");
        goto thread_writer_fct_error;
    }
    if (archwriter_volpath(ai)!=0)
    {   msgprintf(MSG_STACK, "archwriter_volpath() failed\n");
        goto thread_writer_fct_error;
    }
    if (archwriter_create(ai)!=0)
    {   msgprintf(MSG_STACK, "archwriter_create(%s) failed\n", ai->basepath);
        goto thread_writer_fct_error;
    }
    if (archwriter_write_volheader(ai)!=0)
    {   msgprintf(MSG_STACK, "cannot write volume header: archwriter_write_volheader() failed\n");
        goto thread_writer_fct_error;
    }
    
    while (queue_get_end_of_queue(&g_queue)==false)
    {
        if ((blknum=queue_dequeue_first(&g_queue, &type, &headinfo, &blkinfo))<0 && blknum!=QERR_ENDOFQUEUE) // error
        {   msgprintf(MSG_STACK, "queue_dequeue_first()=%ld=%s failed\n", (long)blknum, qerr(blknum));
            goto thread_writer_fct_error;
        }
        else if (blknum>0) // block or header found
        {
            switch (type)
            {
                case QITEM_TYPE_BLOCK:
                    if (archwriter_dowrite_block(ai, &blkinfo)!=0)
                    {   msgprintf(MSG_STACK, "archive_dowrite_block() failed\n");
                        goto thread_writer_fct_error;
                    }
                    free(blkinfo.blkdata);
                    break;
                case QITEM_TYPE_HEADER:
                    if (archwriter_dowrite_header(ai, &headinfo)!=0)
                    {   msgprintf(MSG_STACK, "archive_write_header() failed\n");
                        goto thread_writer_fct_error;
                    }
                    dico_destroy(headinfo.dico);
                    break;
                default:
                    errprintf("unexpected item type from queue: type=%d\n", type);
                    break;
            }
        }
    }
    
    // write last volume footer
    if (archwriter_write_volfooter(ai, true)!=0)
    {   msgprintf(MSG_STACK, "cannot write volume footer: archio_write_volfooter() failed\n");
        goto thread_writer_fct_error;
    }
    archwriter_close(ai);
    msgprintf(MSG_DEBUG1, "THREAD-WRITER: exit success\n");
    dec_secthreads();
    return NULL;
    
thread_writer_fct_error:
    msgprintf(MSG_DEBUG1, "THREAD-WRITER: exit remove\n");
    set_stopfillqueue(); // say to the create.c thread that it must stop
    while (queue_get_end_of_queue(&g_queue)==false) // wait until all the compression threads exit
        queue_destroy_first_item(&g_queue); // empty queue
    archwriter_close(ai);
    dec_secthreads();
    return NULL;
}

// ================================================================================================================
// ================================================================================================================
// ================================================================================================================

int read_and_copy_block(carchreader *ai, cdico *blkdico, int *sumok, bool skip)
{
    struct s_blockinfo blkinfo;
    u32 arblockcsumorig;
    u32 arblockcsumcalc;
    u32 curblocksize; // data size
    u64 blockoffset; // offset of the block in the file
    u16 compalgo; // compression algo used
    u16 cryptalgo; // encryption algo used
    u32 finalsize; // compressed  block size
    u32 compsize;
    u8 *buffer;
    s64 lres;
    
    // init
    *sumok=-1;
    
    if (dico_get_u64(blkdico, 0, BLOCKHEADITEMKEY_BLOCKOFFSET, &blockoffset)!=0)
    {   msgprintf(3, "cannot get blockoffset from block-header\n");
        return -1;
    }
    
    if (dico_get_u32(blkdico, 0, BLOCKHEADITEMKEY_REALSIZE, &curblocksize)!=0 || curblocksize>FSA_MAX_BLKSIZE)
    {   msgprintf(3, "cannot get blocksize from block-header\n");
        return -1;
    }
    
    if (dico_get_u16(blkdico, 0, BLOCKHEADITEMKEY_COMPRESSALGO, &compalgo)!=0)
    {   msgprintf(3, "cannot get BLOCKHEADITEMKEY_COMPRESSALGO from block-header\n");
        return -1;
    }

    if (dico_get_u16(blkdico, 0, BLOCKHEADITEMKEY_ENCRYPTALGO, &cryptalgo)!=0)
    {   msgprintf(3, "cannot get BLOCKHEADITEMKEY_ENCRYPTALGO from block-header\n");
        return -1;
    }

    if (dico_get_u32(blkdico, 0, BLOCKHEADITEMKEY_ARSIZE, &finalsize)!=0)
    {   msgprintf(3, "cannot get BLOCKHEADITEMKEY_ARSIZE from block-header\n");
        return -1;
    }
    
    if (dico_get_u32(blkdico, 0, BLOCKHEADITEMKEY_COMPSIZE, &compsize)!=0)
    {   msgprintf(3, "cannot get BLOCKHEADITEMKEY_COMPSIZE from block-header\n");
        return -1;
    }
    
    if (dico_get_u32(blkdico, 0, BLOCKHEADITEMKEY_ARCSUM, &arblockcsumorig)!=0)
    {   msgprintf(3, "cannot get BLOCKHEADITEMKEY_ARCSUM from block-header\n");
        return -1;
    }
    
    if (skip==true) // the main thread does not need that block (block belongs to a filesys we want to skip)
    {
        if (lseek64(ai->archfd, (long)finalsize, SEEK_CUR)<0)
        {   sysprintf("cannot skip block (finalsize=%ld) failed\n", (long)finalsize);
            return -1;
        }
        return 0;
    }
    
    // ---- allocate memory
    if ((buffer=malloc(finalsize))==NULL)
    {   errprintf("cannot allocate block: malloc(%d) failed\n", finalsize);
        return -1;
    }
    
    if (read(ai->archfd, buffer, (long)finalsize)!=(long)finalsize)
    {   sysprintf("cannot read block (finalsize=%ld) failed\n", (long)finalsize);
        free(buffer);
        return -1;
    }
    
    // prepare blkinfo for the queue
    memset(&blkinfo, 0, sizeof(blkinfo));
    blkinfo.blkdata=(char*)buffer;
    blkinfo.blkrealsize=curblocksize;
    blkinfo.blkoffset=blockoffset;
    blkinfo.blkarcsum=arblockcsumorig;
    blkinfo.blkcompalgo=compalgo;
    blkinfo.blkcryptalgo=cryptalgo;
    blkinfo.blkarsize=finalsize;
    blkinfo.blkcompsize=compsize;
    
    // ---- checksum
    arblockcsumcalc=fletcher32(buffer, finalsize);
    if (arblockcsumcalc!=arblockcsumorig) // bad checksum
    {
        errprintf("block is corrupt at offset=%ld, blksize=%ld\n", (long)blockoffset, (long)curblocksize);
        free(blkinfo.blkdata);
        if ((blkinfo.blkdata=malloc(curblocksize))==NULL)
        {   errprintf("cannot allocate block: malloc(%d) failed\n", curblocksize);
            return -1;
        }
        memset(blkinfo.blkdata, 0, curblocksize);
        *sumok=false;
        if ((lres=queue_add_block(&g_queue, &blkinfo, QITEM_STATUS_DONE))!=QERR_SUCCESS)
        {   errprintf("queue_add_block()=%ld=%s failed\n", (long)lres, qerr(lres));
            return -1;
        }
        // go to the beginning of the corrupted contents so that the next header is searched here
        if (lseek64(ai->archfd, -(long long)finalsize, SEEK_CUR)<0)
        {   errprintf("lseek64() failed\n");
        }
    }
    else // no corruption detected
    {   *sumok=true;
        if ((lres=queue_add_block(&g_queue, &blkinfo, QITEM_STATUS_TODO))!=QERR_SUCCESS)
        {   if (lres!=QERR_CLOSED)
                errprintf("queue_add_block()=%ld=%s failed\n", (long)lres, qerr(lres));
            return -1;
        }
    }
    
    return 0;
}

void *thread_reader_fct(void *args)
{
    char magic[FSA_SIZEOF_MAGIC];
    u32 endofarchive=false;
    carchreader *ai=NULL;
    cdico *dico=NULL;
    u16 fsid;
    int sumok;
    u64 errors;
    s64 lres;
    int res;
    
    // init
    errors=0;
    inc_secthreads();

    if ((ai=(carchreader *)args)==NULL)
    {   errprintf("ai is NULL\n");
        goto thread_reader_fct_error;
    }
    
    // open archive file
    if (archreader_volpath(ai)!=0)
    {   errprintf("archreader_volpath() failed\n");
        goto thread_reader_fct_error;
    }
    
    if (archreader_open(ai)!=0)
    {   errprintf("archreader_open(%s) failed\n", ai->basepath);
        goto thread_reader_fct_error;
    }
    
    // read volume header
    if (archreader_read_volheader(ai)!=0)
    {   errprintf("archio_read_volheader() failed\n");
        goto thread_reader_fct_error;
    }
    
    // ---- read main archive header
    if ((res=archreader_read_header(ai, magic, &dico, false, &fsid))!=ERR_SUCCESS)
    {   errprintf("archreader_read_header() failed to read the archive header\n");
        goto thread_reader_fct_error; // this header is required to continue
    }
    
    if (dico_get_u32(dico, 0, MAINHEADKEY_ARCHIVEID, &ai->archid)!=0)
    {   msgprintf(3, "cannot get archive-id from main header\n");
        goto thread_reader_fct_error;
    }
    
    if ((lres=queue_add_header(&g_queue, dico, magic, fsid))!=QERR_SUCCESS)
    {   errprintf("queue_add_header()=%ld=%s failed to add the archive header\n", (long)lres, qerr(lres));
        goto thread_reader_fct_error;
    }
    
    // read all other data from file (filesys-header, normal objects headers, ...)
    while (endofarchive==false && get_stopfillqueue()==false)
    {
        if ((res=archreader_read_header(ai, magic, &dico, true, &fsid))!=ERR_SUCCESS)
        {   dico_destroy(dico);
            msgprintf(MSG_STACK, "archreader_read_header() failed to read next header\n");
            if (res==ERR_MINOR) // header is corrupt or not what we expected
            {   errors++;
                msgprintf(MSG_DEBUG1, "ERR_MINOR\n");
                continue;
            }
            else // fatal error (eg: cannot read archive from disk)
            {
                msgprintf(MSG_DEBUG1, "!ERR_MINOR\n");
                goto thread_reader_fct_error;
            }
        }
        
        // read header and see if it's for archive management or higher level data
        if (strncmp(magic, FSA_MAGIC_VOLF, FSA_SIZEOF_MAGIC)==0) // header is "end of volume"
        {
            archreader_close(ai);
            
            // check the "end of archive" flag in header
            if (dico_get_u32(dico, 0, VOLUMEFOOTKEY_LASTVOL, &endofarchive)!=0)
            {   errprintf("cannot get compr from block-header\n");
                goto thread_reader_fct_error;
            }
            msgprintf(MSG_VERB2, "End of volume [%s]\n", ai->volpath);
            if (endofarchive!=true)
            {
                archreader_incvolume(ai, false);
                while (regfile_exists(ai->volpath)!=true)
                {
                    // wait until the queue is empty so that the main thread does not pollute the screen
                    while (queue_count(&g_queue)>0)
                        usleep(5000);
                    fflush(stdout);
                    fflush(stderr);
                    msgprintf(MSG_FORCE, "File [%s] is not found, please type the path to volume %ld:\n", ai->volpath, (long)ai->curvol);
                    fprintf(stdout, "New path:> ");
                    res=scanf("%s", ai->volpath);
                }
                
                msgprintf(MSG_VERB2, "New volume is [%s]\n", ai->volpath);
                if (archreader_open(ai)!=0)
                {   msgprintf(MSG_STACK, "archreader_open() failed\n");
                    goto thread_reader_fct_error;
                }
                if (archreader_read_volheader(ai)!=0)
                {      msgprintf(MSG_STACK, "archio_read_volheader() failed\n");
                    goto thread_reader_fct_error;
                }
            }
            dico_destroy(dico);
        }
        else // high-level archive (not involved in volume management)
        {
            if (strncmp(magic, FSA_MAGIC_BLKH, FSA_SIZEOF_MAGIC)==0) // header starts a data block
            {
                if (read_and_copy_block(ai, dico, &sumok, g_fsbitmap[fsid]==0)!=0)
                {   msgprintf(MSG_STACK, "read_and_copy_blocks() failed\n");
                    goto thread_reader_fct_error;
                }
                
                if (sumok==false) errors++;
                dico_destroy(dico);
            }
            else // another higher level header
            {
                // if it's a global header or a if this local header belongs to a filesystem that the main thread needs
                if (fsid==FSA_FILESYSID_NULL || g_fsbitmap[fsid]==1)
                {
                    if ((lres=queue_add_header(&g_queue, dico, magic, fsid))!=QERR_SUCCESS)
                    {   msgprintf(MSG_STACK, "queue_add_header()=%ld=%s failed\n", (long)lres, qerr(lres));
                        goto thread_reader_fct_error;
                    }
                }
                else // header not used: remove data strucutre in dynamic memory
                {
                    dico_destroy(dico);
                }
            }
        }
    }
    
thread_reader_fct_error:
    msgprintf(MSG_DEBUG1, "THREAD-READER: queue_set_end_of_queue(&g_queue, true)\n");
    queue_set_end_of_queue(&g_queue, true); // don't wait for more data from this thread
    dec_secthreads();
    msgprintf(MSG_DEBUG1, "THREAD-READER: exit\n");
    return NULL;
}
