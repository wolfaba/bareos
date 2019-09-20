/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2019-2019 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation, which is
   listed in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/

#define STORAGE_DAEMON 1
#include "gtest/gtest.h"
#include "include/bareos.h"
#include "include/jcr.h"

static bool callback_called_from_destructor = false;
static void callback(JobControlRecord* jcr)
{
  callback_called_from_destructor = true;
}

TEST(job_control_record, constructor_destructor)
{
  std::shared_ptr<JobControlRecord> jcr(std::make_shared<JobControlRecord>());
  InitJcr(jcr, callback);

  // my_thread_id is set in the constructor
  EXPECT_EQ(jcr->my_thread_id, pthread_self());

  jcr.reset();
  EXPECT_TRUE(callback_called_from_destructor);
}
