/*
 * Copyright 2011 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef USERFASTBOOT_PLUGIN_H
#define USERFASTBOOT_PLUGIN_H

#include <userfastboot_ui.h>
#include <cutils/hashmap.h>

typedef int (*flash_func)(Hashmap *params, int *fd, unsigned sz);

#define MAX_OEM_ARGS 64

typedef int (*oem_func)(int argc, char **argv);

int aboot_register_flash_cmd(char *key, flash_func callback);

int aboot_register_oem_cmd(char *key, oem_func callback);

/* Takes ownership of the value pointer, may be freed at any time. Do not
 * use a constant string! xstrdup() is your friend.
 * It uses a copy of the name pointer, can be a constant string or something
 * on the heap; free it after publishing if you need to */
void fastboot_publish(char *name, char *value);

/* If non-NULL, run this during provisioning checks, which are
 * performed before automatic update packages are applied */
void set_platform_provision_function(int (*fn)(void));

#endif

