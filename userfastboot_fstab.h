/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef USERFASTBOOT_FSTAB_H_
#define USERFASTBOOT_FSTAB_H_

#include <stdbool.h>
#include <fs_mgr.h>

// Load and parse volume data from /etc/recovery.fstab.
void load_volume_table();

// Return the struct fstab_rec* record for this path (or NULL).
struct fstab_rec* volume_for_path(const char* path);

// Return struct fstab_rec* record for named entry (minus the leading '/')
struct fstab_rec *volume_for_name(const char *name);

// publish all the types and sizes of known partitions
// if wait is true, wait for device nodes to show up
void publish_all_part_data(bool wait);

// Get the name of the primary system disk. This is the largest
// non-removable disk on the device
char *get_primary_disk_name(void);

#endif

