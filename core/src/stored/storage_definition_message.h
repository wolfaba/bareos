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

#ifndef BAREOS_STORED_STORAGE_DEFINITION_MESSAGE_H_
#define BAREOS_STORED_STORAGE_DEFINITION_MESSAGE_H_

#include <string>

class StorageDefinitionMessage
{
public:
   StorageDefinitionMessage();
   bool ParseMessage(const char *msg);

   const std::string &StoreName() const { return store_name_; }
   const std::string &MediaType() const { return media_type_; }
   const std::string &PoolName() const { return pool_name_; }
   const std::string &PoolType() const { return pool_type_; }
   bool Append() const { return append_; }
   bool Copy() const { return copy_; }
   bool Stripe() const { return stripe_; }

private:
   bool is_valid_;
   bool append_, copy_, stripe_;
   std::string store_name_, media_type_, pool_name_, pool_type_;
};

#endif /* BAREOS_STORED_STORAGE_DEFINITION_MESSAGE_H_ */
