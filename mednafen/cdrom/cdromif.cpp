/* Mednafen - Multi-system Emulator
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "../mednafen.h"
#include <string.h>
#include <sys/types.h>
#include "cdromif.h"
#include "CDAccess.h"
#include "../general.h"

#include <algorithm>

#include <boolean.h>
#include <rthreads/rthreads.h>
#include <retro_miscellaneous.h>

enum
{
   /* Status/Error messages */
   CDIF_MSG_DONE = 0,		   /* Read -> emu. args: No args. */
   CDIF_MSG_INFO,			      /* Read -> emu. args: str_message */
   CDIF_MSG_FATAL_ERROR,		/* Read -> emu. args: *TODO ARGS* */

   /* Command messages. */
   CDIF_MSG_DIEDIEDIE,		   /* Emu -> read */

   CDIF_MSG_READ_SECTOR		   /* Emu -> read
                              args[0] = lba
                              */
};

class CDIF_Message
{
   public:

      CDIF_Message();
      CDIF_Message(unsigned int message_, uint32_t arg0 = 0, uint32_t arg1 = 0, uint32_t arg2 = 0, uint32_t arg3 = 0);
      CDIF_Message(unsigned int message_, const std::string &str);
      ~CDIF_Message();

      unsigned int message;
      uint32_t args[4];
      void *parg;
      std::string str_message;
};

class CDIF_Queue
{
   public:

      CDIF_Queue();
      ~CDIF_Queue();

      bool Read(CDIF_Message *message, bool blocking = true);

      void Write(const CDIF_Message &message);

   private:
      std::queue<CDIF_Message> ze_queue;
      slock_t *ze_mutex;
      scond_t *ze_cond;
};


typedef struct
{
   bool valid;
   bool error;
   int32_t lba;
   uint8_t data[2352 + 96];
} CDIF_Sector_Buffer;

// TODO: prohibit copy constructor
class CDIF_MT : public CDIF
{
   public:

      CDIF_MT(CDAccess *cda);
      virtual ~CDIF_MT();

      virtual void HintReadSector(int32_t lba);
      virtual bool ReadRawSector(uint8_t *buf, int32_t lba);
      virtual bool ReadRawSectorPWOnly(uint8_t* pwbuf, int32_t lba, bool hint_fullread);

      // FIXME: Semi-private:
      int ReadThreadStart(void);

   private:

      CDAccess *disc_cdaccess;

      sthread_t *CDReadThread;

      // Queue for messages to the read thread.
      CDIF_Queue ReadThreadQueue;

      // Queue for messages to the emu thread.
      CDIF_Queue EmuThreadQueue;


      enum { SBSize = 256 };
      CDIF_Sector_Buffer SectorBuffers[SBSize];

      uint32_t SBWritePos;

      slock_t *SBMutex;
      scond_t *SBCond;


      /* Read-thread-only: */
      int32_t ra_lba;
      int32_t ra_count;
      int32_t last_read_lba;
};


// TODO: prohibit copy constructor
class CDIF_ST : public CDIF
{
   public:

      CDIF_ST(CDAccess *cda);
      virtual ~CDIF_ST();

      virtual void HintReadSector(int32_t lba);
      virtual bool ReadRawSector(uint8_t *buf, int32_t lba);
      virtual bool ReadRawSectorPWOnly(uint8_t* pwbuf, int32_t lba, bool hint_fullread);

   private:
      CDAccess *disc_cdaccess;
};

CDIF::CDIF() : UnrecoverableError(false)
{

}

CDIF::~CDIF()
{

}


CDIF_Message::CDIF_Message()
{
   message = 0;

   memset(args, 0, sizeof(args));
}

CDIF_Message::CDIF_Message(unsigned int message_, uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3)
{
   message = message_;
   args[0] = arg0;
   args[1] = arg1;
   args[2] = arg2;
   args[3] = arg3;
}

CDIF_Message::CDIF_Message(unsigned int message_, const std::string &str)
{
   message = message_;
   str_message = str;
}

CDIF_Message::~CDIF_Message()
{

}

CDIF_Queue::CDIF_Queue()
{
   ze_mutex = slock_new();
   ze_cond  = scond_new();
}

CDIF_Queue::~CDIF_Queue()
{
   slock_free(ze_mutex);
   scond_free(ze_cond);
}

/* Returns false if message not read, true if it was read.  Will always return true if "blocking" is set.
 * Will throw MDFN_Error if the read message code is CDIF_MSG_FATAL_ERROR */
bool CDIF_Queue::Read(CDIF_Message *message, bool blocking)
{
   bool ret = false;

   slock_lock(ze_mutex);

   if(blocking)
   {
      while(ze_queue.size() == 0)
         scond_wait(ze_cond, ze_mutex);
   }

   if(ze_queue.size() != 0)
   {
      ret = true;
      *message = ze_queue.front();
      ze_queue.pop();
   }  

   slock_unlock(ze_mutex);

   if(ret)
   {
      if (message->message != CDIF_MSG_FATAL_ERROR)
         return true;

      log_cb(RETRO_LOG_ERROR, "%s", message->str_message.c_str());
   }

   return false;
}

void CDIF_Queue::Write(const CDIF_Message &message)
{
   slock_lock(ze_mutex);

   ze_queue.push(message);

   scond_signal(ze_cond); /* Signal while the mutex is held to prevent icky race conditions */

   slock_unlock(ze_mutex);
}

struct RTS_Args
{
   CDIF_MT *cdif_ptr;
};

static int ReadThreadStart_C(void *v_arg)
{
   RTS_Args *args = (RTS_Args *)v_arg;

   return args->cdif_ptr->ReadThreadStart();
}

int CDIF_MT::ReadThreadStart()
{
   bool Running = true;

   SBWritePos = 0;
   ra_lba = 0;
   ra_count = 0;
   last_read_lba = LBA_Read_Maximum + 1;

   disc_cdaccess->Read_TOC(&disc_toc);

   if(disc_toc.first_track < 1 || disc_toc.last_track > 99 || disc_toc.first_track > disc_toc.last_track)
   {
      log_cb(RETRO_LOG_ERROR, "TOC first(%d)/last(%d) track numbers bad.\n", disc_toc.first_track, disc_toc.last_track);
   }

   SBWritePos = 0;
   ra_lba = 0;
   ra_count = 0;
   last_read_lba = LBA_Read_Maximum + 1;
   memset(SectorBuffers, 0, SBSize * sizeof(CDIF_Sector_Buffer));

   EmuThreadQueue.Write(CDIF_Message(CDIF_MSG_DONE));

   while(Running)
   {
      CDIF_Message msg;

      // Only do a blocking-wait for a message if we don't have any sectors to read-ahead.
      // MDFN_DispMessage("%d %d %d\n", last_read_lba, ra_lba, ra_count);
      if(ReadThreadQueue.Read(&msg, ra_count ? false : true))
      {
         switch(msg.message)
         {
            case CDIF_MSG_DIEDIEDIE:
               Running = false;
               break;

            case CDIF_MSG_READ_SECTOR:
               {
                  static const int max_ra = 16;
                  static const int initial_ra = 1;
                  static const int speedmult_ra = 2;
                  int32_t new_lba = msg.args[0];

                  assert((unsigned int)max_ra < (SBSize / 4));

                  if(new_lba == (last_read_lba + 1))
                  {
                     int how_far_ahead = ra_lba - new_lba;

                     if(how_far_ahead <= max_ra)
                        ra_count = std::min(speedmult_ra, 1 + max_ra - how_far_ahead);
                     else
                        ra_count++;
                  }
                  else if(new_lba != last_read_lba)
                  {
                     ra_lba = new_lba;
                     ra_count = initial_ra;
                  }

                  last_read_lba = new_lba;
               }
               break;
         }
      }

      /* Don't read beyond what the disc (image) readers can handle sanely. */
      if(ra_count && ra_lba == LBA_Read_Maximum)
      {
         ra_count = 0;
         //printf("Ephemeral scarabs: %d!\n", ra_lba);
      }

      if(ra_count)
      {
         uint8_t tmpbuf[2352 + 96];
         bool error_condition = false;

         disc_cdaccess->Read_Raw_Sector(tmpbuf, ra_lba);

         slock_lock(SBMutex);

         SectorBuffers[SBWritePos].lba = ra_lba;
         memcpy(SectorBuffers[SBWritePos].data, tmpbuf, 2352 + 96);
         SectorBuffers[SBWritePos].valid = true;
         SectorBuffers[SBWritePos].error = error_condition;
         SBWritePos = (SBWritePos + 1) % SBSize;

         scond_signal(SBCond);
         slock_unlock(SBMutex);

         ra_lba++;
         ra_count--;
      }
   }

   return(1);
}

CDIF_MT::CDIF_MT(CDAccess *cda) : disc_cdaccess(cda), CDReadThread(NULL), SBMutex(NULL), SBCond(NULL)
{
   CDIF_Message msg;
   RTS_Args s;

   SBMutex            = slock_new();
   SBCond             = scond_new();

   UnrecoverableError = false;

   s.cdif_ptr = this;

   CDReadThread = sthread_create((void (*)(void*))ReadThreadStart_C, &s);
   EmuThreadQueue.Read(&msg);
}


CDIF_MT::~CDIF_MT()
{
   bool thread_deaded_failed = false;

   ReadThreadQueue.Write(CDIF_Message(CDIF_MSG_DIEDIEDIE));

   if(!thread_deaded_failed)
      sthread_join(CDReadThread);

   if(SBMutex)
   {
      slock_free(SBMutex);
      SBMutex = NULL;
   }

   if(SBCond)
   {
      scond_free(SBCond);
      SBCond = NULL;
   }
}

bool CDIF::ValidateRawSector(uint8_t *buf)
{
   int mode = buf[12 + 3];

   if(mode != 0x1 && mode != 0x2)
      return(false);

   if(!edc_lec_check_and_correct(buf, mode == 2))
      return(false);

   return(true);
}

bool CDIF_MT::ReadRawSector(uint8_t *buf, int32_t lba)
{
   bool found = false;
   bool error_condition = false;

   if(UnrecoverableError)
   {
      memset(buf, 0, 2352 + 96);
      return(false);
   }

   if(lba < LBA_Read_Minimum || lba > LBA_Read_Maximum)
   {
      printf("Attempt to read sector out of bounds; LBA=%d\n", lba);
      memset(buf, 0, 2352 + 96);
      return(false);
   }
   //fprintf(stderr, "%d\n", ra_lba - lba);

   ReadThreadQueue.Write(CDIF_Message(CDIF_MSG_READ_SECTOR, lba));

   slock_lock(SBMutex);

   do
   {
      for(int i = 0; i < SBSize; i++)
      {
         if(SectorBuffers[i].valid && SectorBuffers[i].lba == lba)
         {
            error_condition = SectorBuffers[i].error;
            memcpy(buf, SectorBuffers[i].data, 2352 + 96);
            found = true;
         }
      }

      if(!found)
         scond_wait((scond_t*)SBCond, (slock_t*)SBMutex);
   } while(!found);

   slock_unlock(SBMutex);

   return(!error_condition);
}

bool CDIF_MT::ReadRawSectorPWOnly(uint8_t* pwbuf, int32_t lba, bool hint_fullread)
{
   if(UnrecoverableError)
   {
      memset(pwbuf, 0, 96);
      return(false);
   }

   if(lba < LBA_Read_Minimum || lba > LBA_Read_Maximum)
   {
      printf("Attempt to read sector out of bounds; LBA=%d\n", lba);
      memset(pwbuf, 0, 96);
      return(false);
   }

   if(disc_cdaccess->Fast_Read_Raw_PW_TSRE(pwbuf, lba))
   {
      if(hint_fullread)
         ReadThreadQueue.Write(CDIF_Message(CDIF_MSG_READ_SECTOR, lba));

      return(true);
   }
   else
   {
      uint8_t tmpbuf[2352 + 96];
      bool ret;

      ret = ReadRawSector(tmpbuf, lba);
      memcpy(pwbuf, tmpbuf + 2352, 96);

      return ret;
   }
}

void CDIF_MT::HintReadSector(int32_t lba)
{
   if(UnrecoverableError)
      return;

   ReadThreadQueue.Write(CDIF_Message(CDIF_MSG_READ_SECTOR, lba));
}

int CDIF::ReadSector(uint8_t* buf, int32_t lba, uint32_t sector_count, bool suppress_uncorrectable_message)
{
   int ret = 0;

   if(UnrecoverableError)
      return(false);

   while(sector_count--)
   {
      uint8_t tmpbuf[2352 + 96];

      if(!ReadRawSector(tmpbuf, lba))
      {
         puts("CDIF Raw Read error");
         return(false);
      }

      if(!ValidateRawSector(tmpbuf))
      {
         if(!suppress_uncorrectable_message)
         {
            MDFN_DispMessage("Uncorrectable data at sector %d", lba);
            log_cb(RETRO_LOG_ERROR, "Uncorrectable data at sector %d", lba);
         }

         return(false);
      }

      const int mode = tmpbuf[12 + 3];

      if(!ret)
         ret = mode;

      if(mode == 1)
      {
         memcpy(buf, &tmpbuf[12 + 4], 2048);
      }
      else if(mode == 2)
      {
         memcpy(buf, &tmpbuf[12 + 4 + 8], 2048);
      }
      else
      {
         printf("CDIF_ReadSector() invalid sector type at LBA=%u\n", (unsigned int)lba);
         return(false);
      }

      buf += 2048;
      lba++;
   }

   return(ret);
}

//
//
// Single-threaded implementation follows.
//
//

CDIF_ST::CDIF_ST(CDAccess *cda) : disc_cdaccess(cda)
{
   //puts("***WARNING USING SINGLE-THREADED CD READER***");

   UnrecoverableError = false;

   disc_cdaccess->Read_TOC(&disc_toc);

   if(disc_toc.first_track < 1 || disc_toc.last_track > 99 || disc_toc.first_track > disc_toc.last_track)
   {
      throw(MDFN_Error(0, "TOC first(%d)/last(%d) track numbers bad.", disc_toc.first_track, disc_toc.last_track));
   }
}

CDIF_ST::~CDIF_ST()
{

}

void CDIF_ST::HintReadSector(int32_t lba)
{
   // TODO: disc_cdaccess seek hint? (probably not, would require asynchronousitycamel)
}

bool CDIF_ST::ReadRawSector(uint8_t *buf, int32_t lba)
{
   if(UnrecoverableError)
   {
      memset(buf, 0, 2352 + 96);
      return(false);
   }

   if(lba < LBA_Read_Minimum || lba > LBA_Read_Maximum)
   {
      printf("Attempt to read sector out of bounds; LBA=%d\n", lba);
      memset(buf, 0, 2352 + 96);
      return(false);
   }

   disc_cdaccess->Read_Raw_Sector(buf, lba);

   return(true);
}

bool CDIF_ST::ReadRawSectorPWOnly(uint8_t* pwbuf, int32_t lba, bool hint_fullread)
{
   if(UnrecoverableError)
   {
      memset(pwbuf, 0, 96);
      return(false);
   }

   if(lba < LBA_Read_Minimum || lba > LBA_Read_Maximum)
   {
      printf("Attempt to read sector out of bounds; LBA=%d\n", lba);
      memset(pwbuf, 0, 96);
      return(false);
   }

   if(disc_cdaccess->Fast_Read_Raw_PW_TSRE(pwbuf, lba))
      return(true);
   else
   {
      uint8_t tmpbuf[2352 + 96];
      bool ret;

      ret = ReadRawSector(tmpbuf, lba);
      memcpy(pwbuf, tmpbuf + 2352, 96);

      return ret;
   }
}

class CDIF_Stream_Thing : public Stream
{
   public:

      CDIF_Stream_Thing(CDIF *cdintf_arg, uint32_t lba_arg, uint32_t sector_count_arg);
      ~CDIF_Stream_Thing();

      virtual uint64_t attributes(void);

      virtual uint64_t read(void *data, uint64_t count, bool error_on_eos = true);
      virtual void write(const void *data, uint64_t count);
      virtual void truncate(uint64_t length);

      virtual void seek(int64_t offset, int whence);
      virtual uint64_t tell(void);
      virtual uint64_t size(void);
      virtual void flush(void);
      virtual void close(void);

   private:
      CDIF *cdintf;
      const uint32_t start_lba;
      const uint32_t sector_count;
      int64_t position;
};

CDIF_Stream_Thing::CDIF_Stream_Thing(CDIF *cdintf_arg, uint32_t start_lba_arg, uint32_t sector_count_arg) : cdintf(cdintf_arg), start_lba(start_lba_arg), sector_count(sector_count_arg)
{

}

CDIF_Stream_Thing::~CDIF_Stream_Thing()
{

}

uint64_t CDIF_Stream_Thing::attributes(void)
{
   return(ATTRIBUTE_READABLE | ATTRIBUTE_SEEKABLE);
}

uint64_t CDIF_Stream_Thing::read(void *data, uint64_t count, bool error_on_eos)
{
   if(count > (((uint64_t)sector_count * 2048) - position))
   {
      if(error_on_eos)
         throw MDFN_Error(0, "EOF");

      count = ((uint64_t)sector_count * 2048) - position;
   }

   if(!count)
      return(0);

   for(uint64_t rp = position; rp < (position + count); rp = (rp &~ 2047) + 2048)
   {
      uint8_t buf[2048];  

      if(!cdintf->ReadSector(buf, start_lba + (rp / 2048), 1))
         throw MDFN_Error(ErrnoHolder(EIO));

      memcpy((uint8_t*)data + (rp - position), buf + (rp & 2047), std::min<uint64_t>(2048 - (rp & 2047), count - (rp - position)));
   }

   position += count;

   return count;
}

void CDIF_Stream_Thing::write(const void *data, uint64_t count)
{
   throw MDFN_Error(ErrnoHolder(EBADF));
}

void CDIF_Stream_Thing::truncate(uint64_t length)
{
   throw MDFN_Error(ErrnoHolder(EBADF));
}

void CDIF_Stream_Thing::seek(int64_t offset, int whence)
{
   int64_t new_position;

   switch(whence)
   {
      default:
         throw MDFN_Error(ErrnoHolder(EINVAL));
         break;

      case SEEK_SET:
         new_position = offset;
         break;

      case SEEK_CUR:
         new_position = position + offset;
         break;

      case SEEK_END:
         new_position = ((int64_t)sector_count * 2048) + offset;
         break;
   }

   if(new_position < 0 || new_position > ((int64_t)sector_count * 2048))
      throw MDFN_Error(ErrnoHolder(EINVAL));

   position = new_position;
}

uint64_t CDIF_Stream_Thing::tell(void)
{
   return position;
}

uint64_t CDIF_Stream_Thing::size(void)
{
   return(sector_count * 2048);
}

void CDIF_Stream_Thing::flush(void)
{

}

void CDIF_Stream_Thing::close(void)
{

}


Stream *CDIF::MakeStream(int32_t lba, uint32_t sector_count)
{
   return new CDIF_Stream_Thing(this, lba, sector_count);
}


CDIF *CDIF_Open(const std::string& path, bool image_memcache)
{
   CDAccess *cda = CDAccess_Open(path, image_memcache);

   if(!image_memcache)
      return new CDIF_MT(cda);
   return new CDIF_ST(cda);
}
