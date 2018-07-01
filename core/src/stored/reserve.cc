/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2012 Free Software Foundation Europe e.V.
   Copyright (C) 2016-2016 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/
/*
 * Kern Sibbald, MM
 * Split from job.c and acquire.c June 2005
 */
/**
 * @file
 * Drive reservation functions for Storage Daemon
 */

#include "include/bareos.h"
#include "stored/stored.h"
#include "stored/acquire.h"
#include "stored/autochanger.h"
#include "stored/wait.h"
#include "lib/util.h"
#include "include/jcr.h"

const int debuglevel = 150;

#ifdef SD_DEBUG_LOCK
int reservations_lock_count = 0;
#else
static int reservations_lock_count = 0;
#endif

static brwlock_t reservation_lock;

static int CanReserveDrive(DeviceControlRecord *dcr, ReserveContext &reserve_context);
static int ReserveDevice(ReserveContext &reserve_context);
static bool ReserveDeviceForRead(DeviceControlRecord *dcr);
static bool ReserveDeviceForAppend(DeviceControlRecord *dcr, ReserveContext &reserve_context);
static bool UseDeviceCmd(JobControlRecord *jcr);
static void QueueReserveMessage(JobControlRecord *jcr);
static void PopReserveMessages(JobControlRecord *jcr);
static bool UseDeviceReserve(JobControlRecord *jcr);

/* Responses sent to Director daemon */
static char OK_device[] =
   "3000 OK use device device=%s\n";
static char NO_device[] =
   "3924 Device \"%s\" not in SD Device"
   " resources or no matching Media Type.\n";
static char BAD_use[] =
   "3913 Bad use command: %s\n";

static void *dir_heartbeat_thread(void *arg)
{
   JobControlRecord *jcr = static_cast<JobControlRecord *>(arg);
   while (jcr->dir_heartbeat) {
      Bmicrosleep(1,0);
      if (jcr->dir_bsock) {
         jcr->dir_bsock->signal(BNET_HEARTBEAT);
      }
   }
   return nullptr;
}

static bool UseCommandDelegator(JobControlRecord *jcr, bool late_device_reservation)
{
   bool ok;

   if (!late_device_reservation) {
      ok = UseDeviceCmd(jcr);
      jcr->dir_heartbeat = true;
      pthread_create (&jcr->dir_heartbeat_thread_id, NULL, dir_heartbeat_thread, (void*)jcr);
   } else {
      jcr->dir_heartbeat = false;
      pthread_join(jcr->dir_heartbeat_thread_id, NULL);
      ok = UseDeviceReserve(jcr);
   }
   if (!ok) {
      jcr->setJobStatus(JS_ErrorTerminated);
      memset(jcr->sd_auth_key, 0, strlen(jcr->sd_auth_key));
   }
   return ok;
}

bool use_cmd(JobControlRecord *jcr)
{
   return UseCommandDelegator(jcr, false);
}

bool ReserveDevicesFiledStart(JobControlRecord *jcr)
{
   return UseCommandDelegator(jcr, true);
}

/**
 * This allows a given thread to recursively call LockReservations.
 * It must, of course, call unlock_... the same number of times.
 */
void InitReservationsLock()
{
   int errstat;
   if ((errstat=RwlInit(&reservation_lock)) != 0) {
      BErrNo be;
      Emsg1(M_ABORT, 0, _("Unable to initialize reservation lock. ERR=%s\n"),
            be.bstrerror(errstat));
   }

   InitVolListLock();
}

void TermReservationsLock()
{
   RwlDestroy(&reservation_lock);
   TermVolListLock();
}

/**
 * This applies to a drive and to Volumes
 */
void _lockReservations(const char *file, int line)
{
   int errstat;
   reservations_lock_count++;
   if ((errstat=RwlWritelock_p(&reservation_lock, file, line)) != 0) {
      BErrNo be;
      Emsg2(M_ABORT, 0, "RwlWritelock failure. stat=%d: ERR=%s\n",
           errstat, be.bstrerror(errstat));
   }
}

void _unLockReservations()
{
   int errstat;
   reservations_lock_count--;
   if ((errstat=RwlWriteunlock(&reservation_lock)) != 0) {
      BErrNo be;
      Emsg2(M_ABORT, 0, "RwlWriteunlock failure. stat=%d: ERR=%s\n",
           errstat, be.bstrerror(errstat));
   }
}

void DeviceControlRecord::SetReserved()
{
   reserved_ = true;
   Dmsg2(debuglevel, "Inc reserve=%d dev=%s\n", dev->NumReserved(), dev->print_name());
   dev->IncReserved();
}

void DeviceControlRecord::ClearReserved()
{
   if (reserved_) {
      reserved_ = false;
      dev->DecReserved();
      Dmsg2(debuglevel, "Dec reserve=%d dev=%s\n", dev->NumReserved(), dev->print_name());
   }
}

void DeviceControlRecord::UnreserveDevice()
{
   dev->Lock();
   if (IsReserved()) {
      ClearReserved();
      reserved_volume = false;

      if (dev->CanRead()) {
         dev->ClearRead();
      }

      if (dev->num_writers < 0) {
         Jmsg1(jcr, M_ERROR, 0, _("Hey! num_writers=%d!!!!\n"), dev->num_writers);
         dev->num_writers = 0;
      }

      if (dev->NumReserved() == 0 && dev->num_writers == 0) {
         VolumeUnused(this);
      }
   }
   dev->Unlock();
}

bool UseDeviceReserve(JobControlRecord *jcr)
{
   ReserveContext reserve_context;
   memset(&reserve_context, 0, sizeof(ReserveContext));

   reserve_context.jcr = jcr;
   reserve_context.append = jcr->append;

   int wait_for_device_retries = 0;
   int repeat = 0;
   bool fail = false;
   reserve_context.notify_dir = true;

   if (reserve_context.append) {
      reserve_context.jcr->dcr = jcr->dcr;
   } else {
      reserve_context.jcr->read_dcr = jcr->dcr;
   }

   bool ok = true;
   LockReservations();
   for ( ; !fail && !JobCanceled(jcr); ) {
      PopReserveMessages(jcr);
      reserve_context.suitable_device = false;
      reserve_context.have_volume = false;
      reserve_context.VolumeName[0] = 0;
      reserve_context.any_drive = false;
      if (!jcr->PreferMountedVols) {
         /*
          * Here we try to find a drive that is not used.
          * This will maximize the use of available drives.
          */
         reserve_context.num_writers = 20000000;   /* start with impossible number */
         reserve_context.low_use_drive = NULL;
         reserve_context.PreferMountedVols = false;
         reserve_context.exact_match = false;
         reserve_context.autochanger_only = true;
         if ((ok = FindSuitableDeviceForJob(jcr, reserve_context))) {
            break;
         }

         if (reserve_context.low_use_drive) {
            reserve_context.try_low_use_drive = true;
            if ((ok = FindSuitableDeviceForJob(jcr, reserve_context))) {
               break;
            }
            reserve_context.try_low_use_drive = false;
         }
         reserve_context.autochanger_only = false;
         if ((ok = FindSuitableDeviceForJob(jcr, reserve_context))) {
            break;
         }
      }

      /*
       * Now we look for a drive that may or may not be in use.
       * Look for an exact Volume match all drives
       */
      reserve_context.PreferMountedVols = true;
      reserve_context.exact_match = true;
      reserve_context.autochanger_only = false;
      if ((ok = FindSuitableDeviceForJob(jcr, reserve_context))) {
         break;
      }

      /*
       * Look for any mounted drive
       */
      reserve_context.exact_match = false;
      if ((ok = FindSuitableDeviceForJob(jcr, reserve_context))) {
         break;
      }

      /*
       * Try any drive
       */
      reserve_context.any_drive = true;
      if ((ok = FindSuitableDeviceForJob(jcr, reserve_context))) {
         break;
      }

      /*
       * Keep reservations locked *except* during WaitForDevice()
       */
      UnlockReservations();

      /*
       * The idea of looping on repeat a few times it to ensure
       * that if there is some subtle timing problem between two
       * jobs, we will simply try again, and most likely succeed.
       * This can happen if one job reserves a drive or finishes using
       * a drive at the same time a second job wants it.
       */
      if (repeat++ > 1) {              /* try algorithm 3 times */
         Bmicrosleep(30, 0);           /* wait a bit */
         Dmsg0(debuglevel, "repeat reserve algorithm\n");
      } else if (!reserve_context.suitable_device || !WaitForDevice(jcr, wait_for_device_retries)) {
         Dmsg0(debuglevel, "Fail. !suitable_device || !WaitForDevice\n");
         fail = true;
      }
      LockReservations();
      jcr->dir_bsock->signal(BNET_HEARTBEAT);  /* Inform Dir that we are alive */
   }

   UnlockReservations();

   if (!ok) {
      /*
       * If we get here, there are no suitable devices available, which
       * means nothing configured.  If a device is suitable but busy
       * with another Volume, we will not come here.
       */
      UnbashSpaces(jcr->dir_bsock->msg);
      PmStrcpy(jcr->errmsg, jcr->dir_bsock->msg);
      Jmsg(jcr, M_FATAL, 0, _("Device reservation failed for JobId=%d: %s\n"),
           jcr->JobId, jcr->errmsg);
      jcr->dir_bsock->fsend(NO_device, jcr->dev_name);

      Dmsg1(debuglevel, ">dird: %s", jcr->dir_bsock->msg);
   }
   ReleaseReserveMessages(jcr);
   return ok;
}

StorageDefinitionMessage::StorageDefinitionMessage()
   : is_valid(false)
   , regex("^use storage=(.{1,127}) media_type=(.{1,127}) "
           "pool_name=(.{1,127}) pool_type=(.{1,127})"
           "append=([0-9]) copy=([0-9]) stripe=([0-9])\n")
{
   return;
}

bool StorageDefinitionMessage::ParseMessage(std::string unbashed_message)
{
   is_valid = false;
   std::smatch sm;
   std::string input(unbashed_message);
   if (std::regex_match(input, sm, regex)) {
      if (sm.size() == 8) {
         StoreName = sm[1];
         media_type = sm[2];
         pool_name = sm[3];
         pool_type = sm[4];
         append = std::stoi(sm[5]);
         Copy = std::stoi(sm[6]);
         Stripe = std::stoi(sm[7]);
         is_valid = true;
      }
   }
   return is_valid;
}

UseDeviceMessage::UseDeviceMessage()
   : is_valid(false)
   , regex("use device=(.{1,127})\n")
{
   return;
}

bool UseDeviceMessage::ParseMessage(std::string unbashed_message)
{
   is_valid = false;
   std::smatch sm;
   std::string input(unbashed_message);
   if (std::regex_match(input, sm, regex)) {
      if (sm.size() == 2) {
         dev_name = sm[1];
         is_valid = true;
      }
   }
   return is_valid;
}


/**
 * We get the following type of information:
 *
 * use storage=xxx media_type=yyy pool_name=xxx pool_type=yyy append=1 copy=0 strip=0
 * use device=zzz
 * use device=aaa
 * use device=bbb
 * use storage=xxx media_type=yyy pool_name=xxx pool_type=yyy append=0 copy=0 strip=0
 * use device=bbb
 */
#include <sstream>
static bool UseDeviceCmd(JobControlRecord *jcr)
{
   bool ok;
   DirectorStorage *store;
   alist *dirstore;

   /*
    * If there are multiple devices, the director sends us
    * use_device for each device that it wants to use.
    */
   dirstore = New(alist(10, not_owned_by_alist));
   jcr->reserve_msgs = New(alist(10, not_owned_by_alist));
   StorageDefinitionMessage storage_definition_message;
   UseDeviceMessage use_device_message;
   do {
      Dmsg1(debuglevel, "<dird: %s", jcr->dir_bsock->msg);
      UnbashSpaces(jcr->dir_bsock->msg);

      ok = storage_definition_message.ParseMessage(std::string(jcr->dir_bsock->msg));
      if (!ok) {
         break;
      }
      if (storage_definition_message.append) {
         jcr->write_store = dirstore;
      } else {
         jcr->read_store = dirstore;
      }
      store = new DirectorStorage;
      dirstore->append(store);
      memset(store, 0, sizeof(DirectorStorage));
      store->device = New(alist(10));
      bstrncpy(store->name, storage_definition_message.StoreName.c_str(), sizeof(store->name));
      bstrncpy(store->media_type, storage_definition_message.media_type.c_str(), sizeof(store->media_type));
      bstrncpy(store->pool_name, storage_definition_message.pool_name.c_str(), sizeof(store->pool_name));
      bstrncpy(store->pool_type, storage_definition_message.pool_type.c_str(), sizeof(store->pool_type));
      store->append = storage_definition_message.append;

      /*
       * Now get all devices
       */
      while (jcr->dir_bsock->recv() >= 0) {
         Dmsg1(debuglevel, "<dird device: %s", jcr->dir_bsock->msg);
         UnbashSpaces(jcr->dir_bsock->msg);
         ok = use_device_message.ParseMessage(jcr->dir_bsock->msg);
         if (!ok) {
            break;
         }
         store->device->append(bstrdup(use_device_message.dev_name.c_str()));
      }
   }  while (ok && jcr->dir_bsock->recv() >= 0);

   InitJcrDeviceWaitTimers(jcr);
   jcr->dcr = New(StorageDaemonDeviceControlRecord);
   SetupNewDcrDevice(jcr, jcr->dcr, NULL, NULL);
   if (storage_definition_message.append) {
      jcr->dcr->SetWillWrite();
   }

   if (!jcr->dcr) {
      BareosSocket *dir = jcr->dir_bsock;
      dir->fsend(_("3939 Could not get dcr\n"));
      Dmsg1(debuglevel, ">dird: %s", dir->msg);
      ok = false;
   }

   if (ok) {
      jcr->append = storage_definition_message.append;
      strcpy(jcr->dev_name, use_device_message.dev_name.c_str());
      return true;
   } else {
      UnbashSpaces(jcr->dir_bsock->msg);
      PmStrcpy(jcr->errmsg, jcr->dir_bsock->msg);
      Jmsg(jcr, M_FATAL, 0, _("Failed command: %s\n"), jcr->errmsg);
      jcr->dir_bsock->fsend(BAD_use, jcr->errmsg);
      Dmsg1(debuglevel, ">dird: %s", jcr->dir_bsock->msg);
   }

   ReleaseReserveMessages(jcr);
   return ok;
}

/**
 * Walk through the autochanger resources and check if the volume is in one of them.
 *
 * Returns:  true  if volume is in device
 *           false otherwise
 */
static bool IsVolInAutochanger(ReserveContext &rctx, VolumeReservationItem *vol)
{
   AutochangerResource *changer = vol->dev->device->changer_res;

   if (!changer) {
      return false;
   }

   if (bstrcmp(rctx.device_name, changer->name())) {
      Dmsg1(debuglevel, "Found changer device %s\n", vol->dev->device->name());
      return true;
   }
   Dmsg1(debuglevel, "Incorrect changer device %s\n", changer->name());

   return false;
}

/**
 * Search for a device suitable for this job.
 *
 * Note, this routine sets sets reserve_context.suitable_device if any
 * device exists within the SD. The device may not be actually useable.
 * It also returns if it finds a useable device.
 */
bool FindSuitableDeviceForJob(JobControlRecord *jcr, ReserveContext &reserve_context)
{
   bool ok = false;
   DirectorStorage *store;
   char *device_name;
   alist *dirstore;
   DeviceControlRecord *dcr = jcr->dcr;

   if (reserve_context.append) {
      dirstore = jcr->write_store;
   } else {
      dirstore = jcr->read_store;
   }
   Dmsg5(debuglevel, "Start find_suit_dev PrefMnt=%d exact=%d suitable=%d chgronly=%d any=%d\n",
         reserve_context.PreferMountedVols, reserve_context.exact_match, reserve_context.suitable_device,
         reserve_context.autochanger_only, reserve_context.any_drive);

   /*
    * If the appropriate conditions of this if are met, namely that
    * we are appending and the user wants mounted drive (or we
    * force try a mounted drive because they are all busy), we
    * start by looking at all the Volumes in the volume list.
    */
   if (!IsVolListEmpty() && reserve_context.append && reserve_context.PreferMountedVols) {
      dlist *temp_vol_list;
      VolumeReservationItem *vol = NULL;
      temp_vol_list = dup_vol_list(jcr);

      Dmsg0(debuglevel, "look for vol in vol list\n");
      foreach_dlist(vol, temp_vol_list) {
         if (!vol->dev) {
            Dmsg1(debuglevel, "vol=%s no dev\n", vol->vol_name);
            continue;
         }

         /*
          * Check with Director if this Volume is OK
          */
         bstrncpy(dcr->VolumeName, vol->vol_name, sizeof(dcr->VolumeName));
         if (!dcr->DirGetVolumeInfo(GET_VOL_INFO_FOR_WRITE)) {
            continue;
         }

         Dmsg1(debuglevel, "vol=%s OK for this job\n", vol->vol_name);
         foreach_alist(store, dirstore) {
            int status;
            reserve_context.store = store;
            foreach_alist(device_name, store->device) {
               /*
                * Found a device, try to use it
                */
               reserve_context.device_name = device_name;
               reserve_context.device = vol->dev->device;

               if (vol->dev->IsAutochanger()) {
                  Dmsg1(debuglevel, "vol=%s is in changer\n", vol->vol_name);
                  if (!IsVolInAutochanger(reserve_context, vol) || !vol->dev->autoselect) {
                     continue;
                  }
               } else if (!bstrcmp(device_name, vol->dev->device->name())) {
                  Dmsg2(debuglevel, "device=%s not suitable want %s\n",
                        vol->dev->device->name(), device_name);
                  continue;
               }

               bstrncpy(reserve_context.VolumeName, vol->vol_name, sizeof(reserve_context.VolumeName));
               reserve_context.have_volume = true;

               Dmsg2(debuglevel, "try vol=%s on device=%s\n", reserve_context.VolumeName, device_name);
               status = ReserveDevice(reserve_context);
               if (status == 1) {             /* found available device */
                  Dmsg1(debuglevel, "Suitable device found=%s\n", device_name);
                  ok = true;
                  break;
               } else if (status == 0) {      /* device busy */
                  Dmsg1(debuglevel, "Suitable device=%s, busy: not use\n", device_name);
               } else {
                  Dmsg0(debuglevel, "No suitable device found.\n");
               }
               reserve_context.have_volume = false;
               reserve_context.VolumeName[0] = 0;
            } /* foreach_alist(device_name, store->device) */
            if (ok) {
               break;
            }
         } /* foreach_alist(store, dirstore) */
         if (ok) {
            break;
         }
      } /* foreach_dlist(vol, temp_vol_list) -- end for loop over reserved volumes */

      Dmsg0(debuglevel, "lock volumes\n");
      FreeTempVolList(temp_vol_list);
      temp_vol_list = NULL;
   }

   if (ok) {
      Dmsg1(debuglevel, "OK dev found. Vol=%s from in-use vols list\n", reserve_context.VolumeName);
      return true;
   }

   /*
    * No reserved volume we can use, so now search for an available device.
    *
    * For each storage device that the user specified, we
    * search and see if there is a resource for that device.
    */
   foreach_alist(store, dirstore) {
      reserve_context.store = store;
      foreach_alist(device_name, store->device) {
         int status;
         reserve_context.device_name = device_name;
         status = SearchResForDevice(reserve_context);
         if (status == 1) {                   /* found available device */
            Dmsg1(debuglevel, "available device found=%s\n", device_name);
            ok = true;
            break;
         } else if (status == 0) {            /* device busy */
            Dmsg1(debuglevel, "No usable device=%s, busy: not use\n", device_name);
         } else {
            Dmsg0(debuglevel, "No usable device found.\n");
         }
      }
      if (ok) {
         break;
      }
   }
   if (ok) {
      Dmsg1(debuglevel, "OK dev found. Vol=%s\n", reserve_context.VolumeName);
   } else {
      Dmsg0(debuglevel, "Leave find_suit_dev: no dev found.\n");
   }
   return ok;
}

/**
 * Search for a particular storage device with particular storage characteristics (MediaType).
 */
int SearchResForDevice(ReserveContext &reserve_context)
{
   int status;
   AutochangerResource *changer;

   foreach_res(changer, R_AUTOCHANGER) {
      Dmsg2(debuglevel, "Try match changer res=%s, wanted %s\n", changer->name(), reserve_context.device_name);
      if (bstrcmp(reserve_context.device_name, changer->name())) {
         foreach_alist(reserve_context.device, changer->device) {
            Dmsg1(debuglevel, "Try changer device %s\n", reserve_context.device->name());
            if (!reserve_context.device->autoselect) {
               Dmsg1(100, "Device %s not autoselect skipped.\n", reserve_context.device->name());
               continue;                      /* Device is not available */
            }
            status = ReserveDevice(reserve_context);
            if (status != 1) {                /* Try another device */
               continue;
            }

            /*
             * Debug code
             */
            if (reserve_context.store->append == SD_APPEND) {
               Dmsg2(debuglevel, "Device %s reserved=%d for append.\n",
                     reserve_context.device->name(), reserve_context.jcr->dcr->dev->NumReserved());
            } else {
               Dmsg2(debuglevel, "Device %s reserved=%d for read.\n",
                     reserve_context.device->name(), reserve_context.jcr->read_dcr->dev->NumReserved());
            }
            return status;
         }
      }
   }

   if (!reserve_context.autochanger_only) {
      foreach_res(reserve_context.device, R_DEVICE) {
         Dmsg2(debuglevel, "Try match res=%s wanted %s\n", reserve_context.device->name(), reserve_context.device_name);
         if (bstrcmp(reserve_context.device_name, reserve_context.device->name())) {
            status = ReserveDevice(reserve_context);
            if (status != 1) {                /* Try another device */
               continue;
            }
            /*
             * Debug code
             */
            if (reserve_context.store->append == SD_APPEND) {
               Dmsg2(debuglevel, "Device %s reserved=%d for append.\n",
                     reserve_context.device->name(), reserve_context.jcr->dcr->dev->NumReserved());
            } else {
               Dmsg2(debuglevel, "Device %s reserved=%d for read.\n",
                     reserve_context.device->name(), reserve_context.jcr->read_dcr->dev->NumReserved());
            }
            return status;
         }
      }

      /*
       * If we haven't found a available device and the devicereservebymediatype option
       * is set we try one more time where we allow any device with a matching mediatype.
       */
      if (me->device_reserve_by_mediatype) {
         foreach_res(reserve_context.device, R_DEVICE) {
            Dmsg3(debuglevel, "Try match res=%s, mediatype=%s wanted mediatype=%s\n",
                  reserve_context.device->name(), reserve_context.store->media_type, reserve_context.store->media_type);

            if (bstrcmp(reserve_context.store->media_type, reserve_context.device->media_type)) {
               status = ReserveDevice(reserve_context);
               if (status != 1) {                /* Try another device */
                  continue;
               }

               /*
                * Debug code
                */
               if (reserve_context.store->append == SD_APPEND) {
                  Dmsg2(debuglevel, "Device %s reserved=%d for append.\n",
                        reserve_context.device->name(), reserve_context.jcr->dcr->dev->NumReserved());
               } else {
                  Dmsg2(debuglevel, "Device %s reserved=%d for read.\n",
                        reserve_context.device->name(), reserve_context.jcr->read_dcr->dev->NumReserved());
               }
               return status;
            }
         }
      }
   }

   return -1;                                 /* Nothing found */
}

/**
 * Try to reserve a specific device.
 *
 * Returns: 1 -- OK, have DeviceControlRecord
 *          0 -- must wait
 *         -1 -- fatal error
 */
static int ReserveDevice(ReserveContext &reserve_context)
{
   bool ok;
   DeviceControlRecord *dcr;
   const int name_len = MAX_NAME_LENGTH;

   Dmsg2(debuglevel, "chk MediaType device=%s request=%s\n",
         reserve_context.device->media_type, reserve_context.store->media_type);
   if (!bstrcmp(reserve_context.device->media_type, reserve_context.store->media_type)) {
      return -1;
   }

   if (!reserve_context.device->dev) {
      reserve_context.device->dev = InitDev(reserve_context.jcr, reserve_context.device);
   }
   if (!reserve_context.device->dev) {
      if (reserve_context.device->changer_res) {
        Jmsg(reserve_context.jcr, M_WARNING, 0, _("\n"
           "     Device \"%s\" in changer \"%s\" requested by DIR could not be opened or does not exist.\n"),
             reserve_context.device->name(), reserve_context.device_name);
      } else {
         Jmsg(reserve_context.jcr, M_WARNING, 0, _("\n"
            "     Device \"%s\" requested by DIR could not be opened or does not exist.\n"),
              reserve_context.device_name);
      }
      return -1;  /* no use waiting */
   }

   reserve_context.suitable_device = true;
   Dmsg1(debuglevel, "try reserve %s\n", reserve_context.device->name());

   if (reserve_context.store->append) {
      SetupNewDcrDevice(reserve_context.jcr, reserve_context.jcr->dcr, reserve_context.device->dev, NULL);
      dcr = reserve_context.jcr->dcr;
   } else {
      SetupNewDcrDevice(reserve_context.jcr, reserve_context.jcr->read_dcr, reserve_context.device->dev, NULL);
      dcr = reserve_context.jcr->read_dcr;
   }

   if (!dcr) {
      BareosSocket *dir = reserve_context.jcr->dir_bsock;

      dir->fsend(_("3926 Could not get dcr for device: %s\n"), reserve_context.device_name);
      Dmsg1(debuglevel, ">dird: %s", dir->msg);
      return -1;
   }

   if (reserve_context.store->append) {
      dcr->SetWillWrite();
   }

   bstrncpy(dcr->pool_name, reserve_context.store->pool_name, name_len);
   bstrncpy(dcr->pool_type, reserve_context.store->pool_type, name_len);
   bstrncpy(dcr->media_type, reserve_context.store->media_type, name_len);
   bstrncpy(dcr->dev_name, reserve_context.device_name, name_len);
   if (reserve_context.store->append == SD_APPEND) {
      Dmsg2(debuglevel, "call reserve for append: have_vol=%d vol=%s\n", reserve_context.have_volume, reserve_context.VolumeName);
      ok = ReserveDeviceForAppend(dcr, reserve_context);
      if (!ok) {
         goto bail_out;
      }

      reserve_context.jcr->dcr = dcr;
      Dmsg5(debuglevel, "Reserved=%d dev_name=%s mediatype=%s pool=%s ok=%d\n",
               dcr->dev->NumReserved(),
               dcr->dev_name, dcr->media_type, dcr->pool_name, ok);
      Dmsg3(debuglevel, "Vol=%s num_writers=%d, have_vol=%d\n",
         reserve_context.VolumeName, dcr->dev->num_writers, reserve_context.have_volume);
      if (reserve_context.have_volume) {
         Dmsg0(debuglevel, "Call reserve_volume for append.\n");
         if (reserve_volume(dcr, reserve_context.VolumeName)) {
            Dmsg1(debuglevel, "Reserved vol=%s\n", reserve_context.VolumeName);
         } else {
            Dmsg1(debuglevel, "Could not reserve vol=%s\n", reserve_context.VolumeName);
            goto bail_out;
         }
      } else {
         dcr->any_volume = true;
         Dmsg0(debuglevel, "no vol, call find_next_appendable_vol.\n");
         if (dcr->DirFindNextAppendableVolume()) {
            bstrncpy(reserve_context.VolumeName, dcr->VolumeName, sizeof(reserve_context.VolumeName));
            reserve_context.have_volume = true;
            Dmsg1(debuglevel, "looking for Volume=%s\n", reserve_context.VolumeName);
         } else {
            Dmsg0(debuglevel, "No next volume found\n");
            reserve_context.have_volume = false;
            reserve_context.VolumeName[0] = 0;

            /*
             * If there is at least one volume that is valid and in use,
             * but we get here, check if we are running with prefers
             * non-mounted drives.  In that case, we have selected a
             * non-used drive and our one and only volume is mounted
             * elsewhere, so we bail out and retry using that drive.
             */
            if (dcr->FoundInUse() && !reserve_context.PreferMountedVols) {
               reserve_context.PreferMountedVols = true;
               if (dcr->VolumeName[0]) {
                  dcr->UnreserveDevice();
               }
               goto bail_out;
            }

            /*
             * Note. Under some circumstances, the Director can hand us
             * a Volume name that is not the same as the one on the current
             * drive, and in that case, the call above to find the next
             * volume will fail because in attempting to reserve the Volume
             * the code will realize that we already have a tape mounted,
             * and it will fail.  This *should* only happen if there are
             * writers, thus the following test.  In that case, we simply
             * bail out, and continue waiting, rather than plunging on
             * and hoping that the operator can resolve the problem.
             */
            if (dcr->dev->num_writers != 0) {
               if (dcr->VolumeName[0]) {
                  dcr->UnreserveDevice();
               }
               goto bail_out;
            }
         }
      }
   } else {
      ok = ReserveDeviceForRead(dcr);
      if (ok) {
         reserve_context.jcr->read_dcr = dcr;
         Dmsg5(debuglevel, "Read reserved=%d dev_name=%s mediatype=%s pool=%s ok=%d\n",
               dcr->dev->NumReserved(),
               dcr->dev_name, dcr->media_type, dcr->pool_name, ok);
      }
   }
   if (!ok) {
      goto bail_out;
   }

   if (reserve_context.notify_dir) {
      PoolMem dev_name;
      BareosSocket *dir = reserve_context.jcr->dir_bsock;
      PmStrcpy(dev_name, reserve_context.device->name());
      BashSpaces(dev_name);
      ok = dir->fsend(OK_device, dev_name.c_str());  /* Return real device name */
      Dmsg1(debuglevel, ">dird: %s", dir->msg);
   } else {
      ok = true;
   }
   return ok ? 1 : -1;

bail_out:
   reserve_context.have_volume = false;
   reserve_context.VolumeName[0] = 0;
   Dmsg0(debuglevel, "Not OK.\n");
   return 0;
}

/**
 * We "reserve" the drive by setting the ST_READREADY bit.
 * No one else should touch the drive until that is cleared.
 * This allows the DIR to "reserve" the device before actually starting the job.
 */
static bool ReserveDeviceForRead(DeviceControlRecord *dcr)
{
   Device *dev = dcr->dev;
   JobControlRecord *jcr = dcr->jcr;
   bool ok = false;

   ASSERT(dcr);
   if (JobCanceled(jcr)) {
      return false;
   }

   dev->Lock();

   if (dev->IsDeviceUnmounted()) {
      Dmsg1(debuglevel, "Device %s is BLOCKED due to user unmount.\n", dev->print_name());
      Mmsg(jcr->errmsg, _("3601 JobId=%u device %s is BLOCKED due to user unmount.\n"),
           jcr->JobId, dev->print_name());
      QueueReserveMessage(jcr);
      goto bail_out;
   }

   if (dev->IsBusy()) {
      Dmsg4(debuglevel, "Device %s is busy ST_READREADY=%d num_writers=%d reserved=%d.\n",
         dev->print_name(),
         BitIsSet(ST_READREADY, dev->state) ? 1 : 0, dev->num_writers, dev->NumReserved());
      Mmsg(jcr->errmsg, _("3602 JobId=%u device %s is busy (already reading/writing).\n"),
           jcr->JobId, dev->print_name());
      QueueReserveMessage(jcr);
      goto bail_out;
   }

   /*
    * Note: on failure this returns jcr->errmsg properly edited
    */
   if (GeneratePluginEvent(jcr, bsdEventDeviceReserve, dcr) != bRC_OK) {
      QueueReserveMessage(jcr);
      goto bail_out;
   }
   dev->ClearAppend();
   dev->SetRead();
   dcr->SetReserved();
   ok = true;

bail_out:
   dev->Unlock();
   return ok;
}

/**
 * We reserve the device for appending by incrementing
 * NumReserved(). We do virtually all the same work that
 * is done in acquire_device_for_append(), but we do
 * not attempt to mount the device. This routine allows
 * the DIR to reserve multiple devices before *really*
 * starting the job. It also permits the SD to refuse
 * certain devices (not up, ...).
 *
 * Note, in reserving a device, if the device is for the
 * same pool and the same pool type, then it is acceptable.
 * The Media Type has already been checked. If we are
 * the first to reserve the device, we put the pool
 * name and pool type in the device record.
 */
static bool ReserveDeviceForAppend(DeviceControlRecord *dcr, ReserveContext &reserve_context)
{
   JobControlRecord *jcr = dcr->jcr;
   Device *dev = dcr->dev;
   bool ok = false;

   ASSERT(dcr);
   if (JobCanceled(jcr)) {
      return false;
   }

   dev->Lock();

   if (dev->CanRead()) {
      Mmsg(jcr->errmsg, _("3603 JobId=%u device %s is busy reading.\n"),
           jcr->JobId, dev->print_name());
      Dmsg1(debuglevel, "Failed: %s", jcr->errmsg);
      QueueReserveMessage(jcr);
      goto bail_out;
   }

   if (dev->IsDeviceUnmounted()) {
      Mmsg(jcr->errmsg, _("3604 JobId=%u device %s is BLOCKED due to user unmount.\n"),
           jcr->JobId, dev->print_name());
      Dmsg1(debuglevel, "Failed: %s", jcr->errmsg);
      QueueReserveMessage(jcr);
      goto bail_out;
   }

   Dmsg1(debuglevel, "reserve_append device is %s\n", dev->print_name());

   if (CanReserveDrive(dcr, reserve_context) != 1) {
      Dmsg0(debuglevel, "CanReserveDrive!=1\n");
      goto bail_out;
   }

   /*
    * Note: on failure this returns jcr->errmsg properly edited
    */
   if (GeneratePluginEvent(jcr, bsdEventDeviceReserve, dcr) != bRC_OK) {
      QueueReserveMessage(jcr);
      goto bail_out;
   }
   dcr->SetReserved();
   ok = true;

bail_out:
   dev->Unlock();
   return ok;
}

static int IsPoolOk(DeviceControlRecord *dcr)
{
   Device *dev = dcr->dev;
   JobControlRecord *jcr = dcr->jcr;

   if (bstrcmp(dev->pool_name, dcr->pool_name) &&
       bstrcmp(dev->pool_type, dcr->pool_type)) {
      /*
       * OK, compatible device
       */
      Dmsg1(debuglevel, "OK dev: %s num_writers=0, reserved, pool matches\n", dev->print_name());
      return 1;
   } else {
      /* Drive Pool not suitable for us */
      Mmsg(jcr->errmsg, _(
            "3608 JobId=%u wants Pool=\"%s\" but have Pool=\"%s\" nreserve=%d on drive %s.\n"),
            (uint32_t)jcr->JobId, dcr->pool_name, dev->pool_name,
            dev->NumReserved(), dev->print_name());
      Dmsg1(debuglevel, "Failed: %s", jcr->errmsg);
      QueueReserveMessage(jcr);
   }
   return 0;
}

static bool IsMaxJobsOk(DeviceControlRecord *dcr)
{
   Device *dev = dcr->dev;
   JobControlRecord *jcr = dcr->jcr;

   Dmsg5(debuglevel, "MaxJobs=%d Jobs=%d reserves=%d Status=%s Vol=%s\n",
         dcr->VolCatInfo.VolCatMaxJobs,
         dcr->VolCatInfo.VolCatJobs, dev->NumReserved(),
         dcr->VolCatInfo.VolCatStatus,
         dcr->VolumeName);

   /*
    * Limit max concurrent jobs on this drive
    */
   if (dev->max_concurrent_jobs > 0 && dev->max_concurrent_jobs <=
              (uint32_t)(dev->num_writers + dev->NumReserved())) {
      Mmsg(jcr->errmsg, _("3609 JobId=%u Max concurrent jobs exceeded on drive %s.\n"),
           (uint32_t)jcr->JobId, dev->print_name());
      Dmsg1(debuglevel, "Failed: %s", jcr->errmsg);
      QueueReserveMessage(jcr);
      return false;
   }
   if (bstrcmp(dcr->VolCatInfo.VolCatStatus, "Recycle")) {
      return true;
   }
   if (dcr->VolCatInfo.VolCatMaxJobs > 0 && dcr->VolCatInfo.VolCatMaxJobs <=
       (dcr->VolCatInfo.VolCatJobs + dev->NumReserved())) {
      Mmsg(jcr->errmsg, _("3610 JobId=%u Volume max jobs exceeded on drive %s.\n"),
           (uint32_t)jcr->JobId, dev->print_name());
      Dmsg1(debuglevel, "reserve dev failed: %s", jcr->errmsg);
      QueueReserveMessage(jcr);
      return false;                /* wait */
   }
   return true;
}

/**
 * Returns: 1 if drive can be reserved
 *          0 if we should wait
 *         -1 on error or impossibility
 */
static int CanReserveDrive(DeviceControlRecord *dcr, ReserveContext &reserve_context)
{
   Device *dev = dcr->dev;
   JobControlRecord *jcr = dcr->jcr;

   Dmsg5(debuglevel, "PrefMnt=%d exact=%d suitable=%d chgronly=%d any=%d\n",
         reserve_context.PreferMountedVols, reserve_context.exact_match, reserve_context.suitable_device,
         reserve_context.autochanger_only, reserve_context.any_drive);

   if (!IsMaxJobsOk(dcr)) {
      return 0;
   }

   /*
    * Setting any_drive overrides PreferMountedVols flag
    */
   if (!reserve_context.any_drive) {
      /*
       * When PreferMountedVols is set, we keep track of the
       * drive in use that has the least number of writers, then if
       * no unmounted drive is found, we try that drive. This
       * helps spread the load to the least used drives.
       */
      if (reserve_context.try_low_use_drive && dev == reserve_context.low_use_drive) {
         Dmsg2(debuglevel, "OK dev=%s == low_drive=%s.\n",
            dev->print_name(), reserve_context.low_use_drive->print_name());
         return 1;
      }

      /*
       * If he wants a free drive, but this one is busy, no go
       */
      if (!reserve_context.PreferMountedVols && dev->IsBusy()) {
         /*
          * Save least used drive
          */
         if ((dev->num_writers + dev->NumReserved()) < reserve_context.num_writers) {
            reserve_context.num_writers = dev->num_writers + dev->NumReserved();
            reserve_context.low_use_drive = dev;
            Dmsg2(debuglevel, "set low use drive=%s num_writers=%d\n",
               dev->print_name(), reserve_context.num_writers);
         } else {
            Dmsg1(debuglevel, "not low use num_writers=%d\n", dev->num_writers+dev->NumReserved());
         }
         Mmsg(jcr->errmsg, _("3605 JobId=%u wants free drive but device %s is busy.\n"),
              jcr->JobId, dev->print_name());
         Dmsg1(debuglevel, "Failed: %s", jcr->errmsg);
         QueueReserveMessage(jcr);
         return 0;
      }

      if (reserve_context.PreferMountedVols && !dev->vol && dev->IsTape()) {
         Mmsg(jcr->errmsg, _("3606 JobId=%u prefers mounted drives, but drive %s has no Volume.\n"),
              jcr->JobId, dev->print_name());
         Dmsg1(debuglevel, "Failed: %s", jcr->errmsg);
         QueueReserveMessage(jcr);
         return 0;                 /* No volume mounted */
      }

      /*
       * Check for exact Volume name match
       * ***FIXME*** for Disk, we can accept any volume that goes with this drive.
       */
      if (reserve_context.exact_match && reserve_context.have_volume) {
         bool ok;

         Dmsg5(debuglevel, "PrefMnt=%d exact=%d suitable=%d chgronly=%d any=%d\n",
               reserve_context.PreferMountedVols, reserve_context.exact_match, reserve_context.suitable_device,
               reserve_context.autochanger_only, reserve_context.any_drive);
         Dmsg4(debuglevel, "have_vol=%d have=%s resvol=%s want=%s\n",
                  reserve_context.have_volume, dev->VolHdr.VolumeName,
                  dev->vol?dev->vol->vol_name:"*None*", reserve_context.VolumeName);
         ok = bstrcmp(dev->VolHdr.VolumeName, reserve_context.VolumeName) ||
                 (dev->vol && bstrcmp(dev->vol->vol_name, reserve_context.VolumeName));
         if (!ok) {
            Mmsg(jcr->errmsg, _("3607 JobId=%u wants Vol=\"%s\" drive has Vol=\"%s\" on drive %s.\n"),
                 jcr->JobId, reserve_context.VolumeName, dev->VolHdr.VolumeName, dev->print_name());
            QueueReserveMessage(jcr);
            Dmsg3(debuglevel, "not OK: dev have=%s resvol=%s want=%s\n",
                  dev->VolHdr.VolumeName, dev->vol?dev->vol->vol_name:"*None*", reserve_context.VolumeName);
            return 0;
         }
         if (!dcr->Can_i_use_volume()) {
            return 0;              /* fail if volume on another drive */
         }
      }
   }

   if (reserve_context.autochanger_only &&
       !dev->IsBusy() &&
       dev->VolHdr.VolumeName[0] == 0) {
      Dmsg1(debuglevel, "OK Res Unused autochanger %s.\n", dev->print_name());
      bstrncpy(dev->pool_name, dcr->pool_name, sizeof(dev->pool_name));
      bstrncpy(dev->pool_type, dcr->pool_type, sizeof(dev->pool_type));
      return 1;                       /* reserve drive */
   }

   if (dev->num_writers == 0) {
      if (dev->NumReserved()) {
         return IsPoolOk(dcr);
      } else if (dev->CanAppend()) {
         if (IsPoolOk(dcr)) {
            return 1;
         } else {
            /*
             * Changing pool, unload old tape if any in drive
             */
            Dmsg0(debuglevel, "OK dev: num_writers=0, not reserved, pool change, unload changer\n");
            /*
             * ***FIXME*** use SetUnload()
             */
            UnloadAutochanger(dcr, -1);
         }
      }

      Dmsg1(debuglevel, "OK Dev avail reserved %s\n", dev->print_name());
      bstrncpy(dev->pool_name, dcr->pool_name, sizeof(dev->pool_name));
      bstrncpy(dev->pool_type, dcr->pool_type, sizeof(dev->pool_type));
      return 1;                       /* reserve drive */
   }

   if (dev->CanAppend() || dev->num_writers > 0) {
      return IsPoolOk(dcr);
   } else {
      Pmsg1(000, _("Logic error!!!! JobId=%u Should not get here.\n"), (int)jcr->JobId);
      Mmsg(jcr->errmsg, _("3910 JobId=%u Logic error!!!! drive %s Should not get here.\n"),
           jcr->JobId, dev->print_name());
      QueueReserveMessage(jcr);
      Jmsg0(jcr, M_FATAL, 0, _("Logic error!!!! Should not get here.\n"));

      return -1;                      /* error, should not get here */
   }
}

/**
 * Queue a reservation error or failure message for this jcr
 */
static void QueueReserveMessage(JobControlRecord *jcr)
{
   int i;
   alist *msgs;
   char *msg;

   jcr->lock();

   msgs = jcr->reserve_msgs;
   if (!msgs) {
      goto bail_out;
   }
   /*
    * Look for duplicate message.  If found, do not insert
    */
   for (i=msgs->size()-1; i >= 0; i--) {
      msg = (char *)msgs->get(i);
      if (!msg) {
         goto bail_out;
      }

      /*
       * Comparison based on 4 digit message number
       */
      if (bstrncmp(msg, jcr->errmsg, 4)) {
         goto bail_out;
      }
   }

   /*
    * Message unique, so insert it.
    */
   jcr->reserve_msgs->push(bstrdup(jcr->errmsg));

bail_out:
   jcr->unlock();
}

/**
 * Pop and release any reservations messages
 */
static void PopReserveMessages(JobControlRecord *jcr)
{
   alist *msgs;
   char *msg;

   jcr->lock();
   msgs = jcr->reserve_msgs;
   if (!msgs) {
      goto bail_out;
   }
   while ((msg = (char *)msgs->pop())) {
      free(msg);
   }
bail_out:
   jcr->unlock();
}

/**
 * Also called from acquire.c
 */
void ReleaseReserveMessages(JobControlRecord *jcr)
{
   PopReserveMessages(jcr);
   jcr->lock();
   if (!jcr->reserve_msgs) {
      goto bail_out;
   }
   delete jcr->reserve_msgs;
   jcr->reserve_msgs = NULL;

bail_out:
   jcr->unlock();
}
