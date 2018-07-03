/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2018-2018 Bareos GmbH & Co. KG

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

#include "stored/storage_definition_message.h"

#include <memory>
#include <cstring>

#include "lib/util.h"

StorageDefinitionMessage::StorageDefinitionMessage()
   : is_valid_(false)
{
   return;
}

struct StorageDefinitionMessageTemp
{
   /* these c arrays are only used as a sscanf buffer
    * until sscanf can be replaced with std::regex/smatch,
    * currently older gcc 4.8.2 (Ubuntu 14) prevents this */

   char msg[600];
   char sn[128], mt[128], pn[128], pt[128];
};

bool StorageDefinitionMessage::ParseMessage(const char *msg_in)
{
   is_valid_ = false;

   /* allocate big c-arrays on the heap (not stack) */
   std::unique_ptr<StorageDefinitionMessageTemp> temp(new StorageDefinitionMessageTemp);

   strncpy(temp->msg, msg_in, sizeof(temp->msg));
   UnbashSpaces(temp->msg); /* do not alter original message */

   constexpr char use_storage[] =
         "use storage=%127s media_type=%127s "
         "pool_name=%127s pool_type=%127s append=%d copy=%d stripe=%d\n";

   int ap, cp, st;
   bool conversion_ok = sscanf(temp->msg, use_storage,
               temp->sn, temp->mt, temp->pn, temp->pt, &ap, &cp, &st) == 7;

   if (conversion_ok) {
      store_name_ = temp->sn;
      media_type_ = temp->mt;
      pool_name_ = temp->pn;
      pool_type_ = temp->pt;
      append_ = ap == 0 ? false : true;
      copy_ = cp == 0 ? false : true;
      stripe_ = st == 0 ? false : true;
   }

   is_valid_ = conversion_ok;
   return conversion_ok;
}
