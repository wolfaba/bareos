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

#include "stored/use_device_message.h"

#include <memory>
#include <cstring>

#include "lib/util.h"

UseDeviceMessage::UseDeviceMessage()
   : is_valid_(false)
{
   return;
}

struct UseDeviceMessageTemp
{
   /* these c arrays are only used as a sscanf buffer
    * until sscanf can be replaced with std::regex/smatch,
    * currently older gcc 4.8.2 (Ubuntu 14) prevents this */

   char msg[200];
   char dn[128];
};

bool UseDeviceMessage::ParseMessage(const char *msg_in)
{
   is_valid_ = false;

   /* allocate big c-arrays on the heap (not stack) */
   std::unique_ptr<UseDeviceMessageTemp> temp(new UseDeviceMessageTemp);

   std::strncpy(temp->msg, msg_in, sizeof(temp->msg));
   UnbashSpaces(temp->msg); /* do not alter original message */

   constexpr char use_device[]  = "use device=%127s\n";
   bool conversion_ok = sscanf(temp->msg, use_device, temp->dn) == 1;

   if (conversion_ok) {
      dev_name_ = temp->dn;
   }

   is_valid_ = conversion_ok;
   return conversion_ok;
}
