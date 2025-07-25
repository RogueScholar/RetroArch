/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *  Copyright (C) 2016-2019 - Brad Parker
 *  Copyright (C) 2016-2019 - Andrés Suárez
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include <compat/strl.h>
#include <file/file_path.h>
#include <lists/string_list.h>
#include <string/stdstring.h>
#include <file/config_file.h>

#include "../configuration.h"
#include "../file_path_special.h"
#include "../list_special.h"
#include "../verbosity.h"
#include "../input/input_driver.h"
#include "../input/input_remapping.h"

#include "tasks_internal.h"
#ifdef HAVE_BLISSBOX
#include "../input/include/blissbox.h"
#endif

#ifdef HAVE_MENU
#include "../menu/menu_driver.h"
#endif

#include "../runloop.h"

enum autoconfig_handle_flags
{
   AUTOCONF_FLAG_AUTOCONFIG_ENABLED     = (1 << 0),
   AUTOCONF_FLAG_SUPPRESS_NOTIFICATIONS = (1 << 1),
   AUTOCONF_FLAG_SUPPRESS_FAILURE_NOTIF = (1 << 2)
};

typedef struct
{
   char *dir_autoconfig;
   char *dir_driver_autoconfig;
   config_file_t *autoconfig_file;
   unsigned port;
   input_device_info_t device_info; /* unsigned alignment */
   uint8_t flags;
} autoconfig_handle_t;

/*********************/
/* Utility functions */
/*********************/

static void free_autoconfig_handle(autoconfig_handle_t *autoconfig_handle)
{
   if (!autoconfig_handle)
      return;

   if (autoconfig_handle->dir_autoconfig)
   {
      free(autoconfig_handle->dir_autoconfig);
      autoconfig_handle->dir_autoconfig = NULL;
   }

   if (autoconfig_handle->dir_driver_autoconfig)
   {
      free(autoconfig_handle->dir_driver_autoconfig);
      autoconfig_handle->dir_driver_autoconfig = NULL;
   }

   if (autoconfig_handle->autoconfig_file)
   {
      config_file_free(autoconfig_handle->autoconfig_file);
      autoconfig_handle->autoconfig_file = NULL;
   }

   free(autoconfig_handle);
   autoconfig_handle = NULL;
}

static void input_autoconfigure_free(retro_task_t *task)
{
   autoconfig_handle_t *autoconfig_handle = NULL;
   if (task && (autoconfig_handle = (autoconfig_handle_t*)task->state))
      free_autoconfig_handle(autoconfig_handle);
}

/******************************/
/* Autoconfig 'File' Handling */
/******************************/

/* Returns a value corresponding to the
 * 'affinity' between the connected input
 * device and the specified config file
 * > 0: No match
 * > 20-29: Device name matches
 * > 30-39: VID+PID match
 * > 50-59: Both device name and VID+PID match */
static unsigned input_autoconfigure_get_config_file_affinity(
      autoconfig_handle_t *autoconfig_handle,
      config_file_t *config)
{
   int i;
   char config_key[30];
   unsigned max_affinity           = 0;

   /* One main entry and up to 9 alternatives */
   for (i = 0; i < 10; i++)
   {
      size_t _len;
      char config_key_postfix[7];
      struct config_entry_list *entry = NULL;
      uint16_t config_vid = 0;
      uint16_t config_pid = 0;
      int tmp_int         = 0;
      unsigned affinity   = 0;

      if (i == 0)
         config_key_postfix[0] = '\0';
      else
         snprintf(config_key_postfix, sizeof(config_key_postfix),
                  "_alt%d",i);

      /* Parse config file */
      _len  = strlcpy(config_key, "input_vendor_id",
               sizeof(config_key));
      strlcpy(config_key  + _len, config_key_postfix,
            sizeof(config_key) - _len);
      if (config_get_int(config, config_key, &tmp_int))
         config_vid = (uint16_t)tmp_int;

      _len  = strlcpy(config_key, "input_product_id",
               sizeof(config_key));
      strlcpy(config_key  + _len, config_key_postfix,
               sizeof(config_key) - _len);
      if (config_get_int(config, config_key, &tmp_int))
         config_pid = (uint16_t)tmp_int;

      /* Check for matching VID+PID */
#ifdef HAVE_BLISSBOX
      /* > Bliss-Box shenanigans... */
      if (autoconfig_handle->device_info.vid == BLISSBOX_VID)
         config_pid = BLISSBOX_PID;

      if (     (autoconfig_handle->device_info.vid == config_vid)
            && (autoconfig_handle->device_info.pid == config_pid)
            && (config_vid != 0)
            && (config_pid != 0)
            && (autoconfig_handle->device_info.vid != BLISSBOX_VID)
            && (autoconfig_handle->device_info.pid != BLISSBOX_PID))
         affinity += 30;
#else
      if (     (autoconfig_handle->device_info.vid == config_vid)
            && (autoconfig_handle->device_info.pid == config_pid)
            && (config_vid != 0)
            && (config_pid != 0))
         affinity += 30;
#endif

      /* Check for matching device name */
      _len  = strlcpy(config_key, "input_device",
               sizeof(config_key));
      strlcpy(config_key  + _len, config_key_postfix,
            sizeof(config_key) - _len);
      if (     (entry  = config_get_entry(config, config_key))
            && !string_is_empty(entry->value)
            &&  string_is_equal(entry->value,
                autoconfig_handle->device_info.name))
         affinity += 20;

      /* Store the selected alternative as last digit of affinity. */
      if (affinity > 0)
         affinity += i;

      if (max_affinity < affinity)
         max_affinity = affinity;
   }

   return max_affinity;
}

/* 'Attaches' specified autoconfig file to autoconfig
 * handle, parsing required device info metadata */
static void input_autoconfigure_set_config_file(
      autoconfig_handle_t *autoconfig_handle,
      config_file_t *config, unsigned alternative)
{
   size_t _len;
   char config_key[32];
   struct config_entry_list *entry    = NULL;

   /* Attach config file */
   autoconfig_handle->autoconfig_file = config;

   /* > Extract config file path + name */
   if (!string_is_empty(config->path))
   {
      const char *config_file_name = path_basename_nocompression(config->path);
      if (!string_is_empty(config_file_name))
         strlcpy(autoconfig_handle->device_info.config_name,
               config_file_name,
               sizeof(autoconfig_handle->device_info.config_name));
   }

   /* Parse config file */
   _len  = strlcpy(config_key, "input_device_display_name",
            sizeof(config_key));
   /* Read device display name */
   if (alternative > 0)
      snprintf(config_key + _len, sizeof(config_key) - _len,
               "_alt%d",alternative);

   if (  (entry = config_get_entry(config, config_key))
         && !string_is_empty(entry->value))
      strlcpy(autoconfig_handle->device_info.display_name,
            entry->value,
            sizeof(autoconfig_handle->device_info.display_name));

   /* Set auto-configured status to 'true' */
   autoconfig_handle->device_info.autoconfigured = true;
}

/* Attempts to find an 'external' autoconfig file
 * (in the autoconfig directory) matching the connected
 * input device
 * > Returns 'true' if successful */
static bool input_autoconfigure_scan_config_files_external(
      autoconfig_handle_t *autoconfig_handle)
{
   const char *dir_autoconfig           = autoconfig_handle->dir_autoconfig;
   const char *dir_driver_autoconfig    = autoconfig_handle->dir_driver_autoconfig;
   struct string_list *config_file_list = NULL;
   unsigned max_affinity                = 0;

   /* Attempt to fetch file listing from driver-specific
    * autoconfig directory */
   if (  !string_is_empty(dir_driver_autoconfig)
       && path_is_directory(dir_driver_autoconfig))
      config_file_list = dir_list_new_special(
            dir_driver_autoconfig, DIR_LIST_AUTOCONFIG,
            "cfg", false);

   if (!config_file_list || (config_file_list->size < 1))
   {
      /* No files found - attempt to fetch listing
       * from autoconfig base directory */
      if (config_file_list)
      {
         string_list_free(config_file_list);
         config_file_list = NULL;
      }

      if (  !string_is_empty(dir_autoconfig)
          && path_is_directory(dir_autoconfig))
         config_file_list = dir_list_new_special(
               dir_autoconfig, DIR_LIST_AUTOCONFIG,
               "cfg", false);
   }

   if (config_file_list)
   {
      bool match_found = false;
      if (config_file_list->size >= 1)
      {
         size_t i;
         config_file_t *best_config = NULL;

         /* Loop through external config files */
         for (i = 0; i < config_file_list->size; i++)
         {
            const char *config_file_path = config_file_list->elems[i].data;
            config_file_t *config        = NULL;
            unsigned affinity            = 0;

            if (string_is_empty(config_file_path))
               continue;

            /* Load autoconfig file */
            if (!(config = config_file_new_from_path_to_string(config_file_path)))
               continue;

            /* Check for a match */
            if (autoconfig_handle && config)
               affinity = input_autoconfigure_get_config_file_affinity(
                     autoconfig_handle, config);

            if (affinity > max_affinity)
            {
               if (best_config)
               {
                  config_file_free(best_config);
                  best_config = NULL;
               }

               /* 'Cache' config file for later processing */
               best_config  = config;
               config       = NULL;
               max_affinity = affinity;

               /* An affinity of 5x is a 'perfect' match,
                * and means we can return immediately */
               if (affinity >= 50)
                  break;
            }
            /* No match - just clean up config file */
            else
            {
               config_file_free(config);
               config = NULL;
            }
         }

         /* If we reach this point and a config file has
          * been cached, then we have a match */
         if (best_config)
         {
            if (autoconfig_handle && best_config)
               input_autoconfigure_set_config_file(
                     autoconfig_handle, best_config,
                     max_affinity % 10);
            match_found = true;
         }
      }
      string_list_free(config_file_list);
      config_file_list = NULL;
      if (match_found)
         return true;
   }

   return false;
}

/* Attempts to find an internal autoconfig definition
 * matching the connected input device
 * > Returns 'true' if successful */
static bool input_autoconfigure_scan_config_files_internal(
      autoconfig_handle_t *autoconfig_handle)
{
   size_t i;

   /* Loop through internal autoconfig files
    * > input_builtin_autoconfs is a static const,
    *   and may be read safely in any thread  */
   for (i = 0; input_builtin_autoconfs[i]; i++)
   {
      char *autoconfig_str  = NULL;
      config_file_t *config = NULL;
      unsigned affinity     = 0;

      if (string_is_empty(input_builtin_autoconfs[i]))
         continue;

      /* Load autoconfig string */
      autoconfig_str = strdup(input_builtin_autoconfs[i]);
      config         = config_file_new_from_string(
            autoconfig_str, NULL);

      /* > String no longer required - clean up */
      free(autoconfig_str);
      autoconfig_str = NULL;

      /* Check for a match */
      if (autoconfig_handle && config)
         affinity = input_autoconfigure_get_config_file_affinity(
               autoconfig_handle, config);

      /* > In the case of internal autoconfigs, any kind
       *   of match is considered to be a success */
      if (affinity > 0)
      {
         if (autoconfig_handle && config)
            input_autoconfigure_set_config_file(
                  autoconfig_handle, config,
                  affinity % 10);
         return true;
      }

      /* No match - clean up */
      if (config)
      {
         config_file_free(config);
         config = NULL;
      }
   }

   return false;
}

/* Reallocate the automatically assigned player <-> port mapping if needed.
 * Objectives:
 * - if there is reservation for the device, assign it to the reserved player
 * - when assigning a new device to a reserved port, move the previous entry
 *      to first free slot if it was occupied
 * - use first free player port by default for new entries (overriding saved
 *      input_joypad_index, as it can
 *      get quite messy if reservations are done, due to the swaps above)
 * - do not consider "reserved" ports free
 * - if there is no reservation, do not change anything
 *      (not even the assignment to first free player port)
 */
static void reallocate_port_if_needed(unsigned detected_port, int vendor_id,
      int product_id, const char *device_name, const char *device_display_name)
{
   int player;
   char settings_value[NAME_MAX_LENGTH];
   char settings_value_device_name[NAME_MAX_LENGTH];
   unsigned prev_assigned_player_slots[MAX_USERS];
   int  settings_value_vendor_id;
   int  settings_value_product_id;
   unsigned first_free_player_slot = MAX_USERS + 1;
   bool device_has_reserved_slot   = false;
   bool no_reservation_at_all      = true;
   settings_t *settings            = config_get_ptr();

   for (player = 0; player < MAX_USERS; player++)
   {
      if (     first_free_player_slot > MAX_USERS
            && (detected_port == settings->uints.input_joypad_index[player]
            || !input_config_get_device_name(settings->uints.input_joypad_index[player]))
            && settings->uints.input_device_reservation_type[player]
            != INPUT_DEVICE_RESERVATION_RESERVED)
      {
         first_free_player_slot = player;
         RARCH_DBG("[Autoconf] First unconfigured / unreserved player is %d.\n",
                   player+1);
      }
      prev_assigned_player_slots[settings->uints.input_joypad_index[player]] = player;
      if (settings->uints.input_device_reservation_type[player] != INPUT_DEVICE_RESERVATION_NONE)
         no_reservation_at_all = false;
   }
   if (first_free_player_slot > settings->uints.input_max_users)
   {
      RARCH_ERR("[Autoconf] No free and unreserved player slots found for adding new device"
            " \"%s\"! Detected port %d, max_users: %d, first free slot %d.\n",
            device_name, detected_port,
            settings->uints.input_max_users,
            first_free_player_slot+1);
      RARCH_WARN("[Autoconf] Leaving detected player slot in place: %d.\n",
            prev_assigned_player_slots[detected_port]);
      return;
   }

   for (player = 0; player < MAX_USERS; player++)
   {
      if (settings->uints.input_device_reservation_type[player] != INPUT_DEVICE_RESERVATION_NONE)
         strlcpy(settings_value, settings->arrays.input_reserved_devices[player],
                 sizeof(settings_value));
      else
         settings_value[0] = '\0';

      if (!string_is_empty(settings_value))
      {
         RARCH_DBG("[Autoconf] Examining reserved device for player %d "
                   "type %d: %s against %04x:%04x.\n",
                   player+1,
                   settings->uints.input_device_reservation_type[player],
                   settings_value, vendor_id, product_id);

         if (sscanf(settings_value, "%04x:%04x ",
             &settings_value_vendor_id,
             &settings_value_product_id) != 2)
         {
            strlcpy(settings_value_device_name, settings_value,
                    sizeof(settings_value_device_name));
            device_has_reserved_slot =
                  string_is_equal(device_name, settings_value_device_name)
               || string_is_equal(device_display_name, settings_value_device_name);
         }
         else
            device_has_reserved_slot = (  vendor_id  == settings_value_vendor_id
                                       && product_id == settings_value_product_id);

         if (device_has_reserved_slot)
         {
            unsigned prev_assigned_port = settings->uints.input_joypad_index[player];
            if (     detected_port != prev_assigned_port
                 && !string_is_empty(input_config_get_device_name(prev_assigned_port))
                 && (( settings_value_vendor_id  == input_config_get_device_vid(prev_assigned_port)
                 && settings_value_product_id == input_config_get_device_pid(prev_assigned_port))
                 || strcmp(input_config_get_device_name(prev_assigned_port), settings_value_device_name) == 0))
            {
               RARCH_DBG("[Autoconf] Same type of device already took this slot, continuing search...\n");
               device_has_reserved_slot = false;
            }
            else
            {
               RARCH_DBG("[Autoconf] Reserved device matched.\n");
               break;
            }
         }
      }
   }

   if (device_has_reserved_slot)
   {
      unsigned prev_assigned_port = settings->uints.input_joypad_index[player];
      if (detected_port != prev_assigned_port)
      {
         RARCH_LOG("[Autoconf] Device \"%s\" (%x:%x) is reserved "
                   "for player %d, updating.\n",
                   device_name, vendor_id, product_id, player+1);

         /* todo: fix the pushed info message */
         settings->uints.input_joypad_index[player] = detected_port;

         RARCH_LOG("[Autoconf] Preferred slot was taken earlier by "
                   "\"%s\", reassigning that to %d.\n",
                    input_config_get_device_name(prev_assigned_port),
                    prev_assigned_player_slots[detected_port]+1);
         settings->uints.input_joypad_index[prev_assigned_player_slots[detected_port]] = prev_assigned_port;
         if (input_config_get_device_name(prev_assigned_port))
         {
            unsigned prev_assigned_port_l2 = settings->uints.input_joypad_index[first_free_player_slot];

            RARCH_LOG("[Autoconf] 2nd level reassignment, moving "
                      "previously assigned port %d to first free player %d.\n",
                      prev_assigned_port_l2, first_free_player_slot+1);
            settings->uints.input_joypad_index[prev_assigned_player_slots[detected_port]] = prev_assigned_port_l2;
            settings->uints.input_joypad_index[first_free_player_slot]                    = prev_assigned_port;
         }
      }
      else
      {
         RARCH_DBG("[Autoconf] Device \"%s\" (%x:%x) is reserved for "
                   "player %d, same as default assignment.\n",
                   device_name, vendor_id, product_id, player+1);
      }
   }
   else
   {
      unsigned prev_assigned_port;

      RARCH_DBG("[Autoconf] Device \"%s\" (%x:%x) is not reserved for "
            "any player slot.\n",
            device_name, vendor_id, product_id);
      /* Fallback in case no reservation is set up at all - to preserve
       * any previous setup where input_joypad_index may have been
       * customized. */
      if (   no_reservation_at_all
            || prev_assigned_player_slots[detected_port] == first_free_player_slot)
         return;

      prev_assigned_port = settings->uints.input_joypad_index[first_free_player_slot];
      settings->uints.input_joypad_index[first_free_player_slot] = detected_port;
      settings->uints.input_joypad_index[prev_assigned_player_slots[detected_port]] =
         prev_assigned_port;
      RARCH_DBG("[Autoconf] Earlier free player slot found, "
            "reassigning to player %d.\n",
            first_free_player_slot+1);
   }
}

/*************************/
/* Autoconfigure Connect */
/*************************/

static void cb_input_autoconfigure_connect(
      retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   unsigned port;
   autoconfig_handle_t *autoconfig_handle = NULL;

   if (!task)
      return;

   if (!(autoconfig_handle = (autoconfig_handle_t*)task->state))
      return;

   /* Use local copy of port index for brevity... */
   port = autoconfig_handle->port;

   /* We perform the actual 'connect' in this
    * callback, to ensure it occurs on the main
    * thread */

   /* Copy task handle parameters into global
    * state objects:
    * > Name */
   if (!string_is_empty(autoconfig_handle->device_info.name))
      input_config_set_device_name(port,
            autoconfig_handle->device_info.name);
   else
      input_config_clear_device_name(port);

   /* > Display name */
   if (!string_is_empty(autoconfig_handle->device_info.display_name))
      input_config_set_device_display_name(port,
            autoconfig_handle->device_info.display_name);
   else if (!string_is_empty(autoconfig_handle->device_info.name))
      input_config_set_device_display_name(port,
            autoconfig_handle->device_info.name);
   else
      input_config_clear_device_display_name(port);

   /* > Driver */
   if (!string_is_empty(autoconfig_handle->device_info.joypad_driver))
      input_config_set_device_joypad_driver(port,
            autoconfig_handle->device_info.joypad_driver);
   else
      input_config_clear_device_joypad_driver(port);

   /* > VID/PID */
   input_config_set_device_vid(port, autoconfig_handle->device_info.vid);
   input_config_set_device_pid(port, autoconfig_handle->device_info.pid);

   if (!string_is_empty(autoconfig_handle->device_info.config_name))
      input_config_set_device_config_name(port,
            autoconfig_handle->device_info.config_name);
   else
      input_config_set_device_config_name(port,
            msg_hash_to_str(MENU_ENUM_LABEL_VALUE_NOT_AVAILABLE));

   /* > Auto-configured state */
   input_config_set_device_autoconfigured(port,
         autoconfig_handle->device_info.autoconfigured);

   /* Reset any existing binds */
   input_config_reset_autoconfig_binds(port);

   /* If an autoconfig file is available, load its
    * bind mappings */
   if (autoconfig_handle->device_info.autoconfigured)
      input_config_set_autoconfig_binds(port,
            autoconfig_handle->autoconfig_file);

   reallocate_port_if_needed(port,autoconfig_handle->device_info.vid,
         autoconfig_handle->device_info.pid,
         autoconfig_handle->device_info.name,
         autoconfig_handle->device_info.display_name);
}

static void input_autoconfigure_connect_handler(retro_task_t *task)
{
   char task_title[NAME_MAX_LENGTH + 16];
   autoconfig_handle_t *autoconfig_handle = NULL;
   bool match_found                       = false;
   const char *device_display_name        = NULL;

   task_title[0] = '\0';

   if (!task)
      return;

   autoconfig_handle = (autoconfig_handle_t*)task->state;

   if (   !autoconfig_handle
       || string_is_empty(autoconfig_handle->device_info.name)
       || !(autoconfig_handle->flags & AUTOCONF_FLAG_AUTOCONFIG_ENABLED))
   {
      task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
      return;
   }

   /* Annoyingly, we have to scan all the autoconfig
    * files (and in-built configs) in a single shot
    * > Would prefer to scan one config per iteration
    *   of the task, but this would render the gamepad
    *   unusable for multiple frames after loading
    *   content... */

   /* Scan in order of preference:
    * - External autoconfig files
    * - Internal autoconfig definitions */
   if (!(match_found = input_autoconfigure_scan_config_files_external(
         autoconfig_handle)))
      match_found = input_autoconfigure_scan_config_files_internal(
         autoconfig_handle);

   /* If no match was found, attempt to use
    * fallback mapping
    * > Only enabled for certain drivers */
   if (!match_found)
   {
      const char *fallback_device_name = NULL;

      /* Preset fallback device names - must match
       * those set in 'input_autodetect_builtin.c' */
      if (string_is_equal(autoconfig_handle->device_info.joypad_driver,
            "android"))
         fallback_device_name = "Android Gamepad";
      else if (string_is_equal(autoconfig_handle->device_info.joypad_driver,
            "xinput"))
         fallback_device_name = "XInput Controller";
      else if (string_is_equal(autoconfig_handle->device_info.joypad_driver,
            "sdl2"))
         fallback_device_name = "Standard Gamepad";
#ifdef HAVE_TEST_DRIVERS
      else if (string_is_equal(autoconfig_handle->device_info.joypad_driver,
            "test"))
         fallback_device_name = "Test Gamepad";
#endif
      if (   !string_is_empty(fallback_device_name)
          && !string_is_equal(autoconfig_handle->device_info.name,
               fallback_device_name))
      {
         char *name_backup = strdup(autoconfig_handle->device_info.name);

         strlcpy(autoconfig_handle->device_info.name,
               fallback_device_name,
               sizeof(autoconfig_handle->device_info.name));

         /* This is not a genuine match - leave
          * match_found set to 'false' regardless
          * of the outcome */
         input_autoconfigure_scan_config_files_internal(
               autoconfig_handle);

         strlcpy(autoconfig_handle->device_info.name,
               name_backup,
               sizeof(autoconfig_handle->device_info.name));

         free(name_backup);
         name_backup = NULL;
      }
   }

   /* Get display name for task status message */
   device_display_name = autoconfig_handle->device_info.display_name;
   if (string_is_empty(device_display_name))
      device_display_name = autoconfig_handle->device_info.name;
   if (string_is_empty(device_display_name))
      device_display_name = msg_hash_to_str(MENU_ENUM_LABEL_VALUE_NOT_AVAILABLE);

   /* Generate task status message
    * > Note that 'connection successful' messages
    *   may be suppressed, but error messages are
    *   always shown */
   task->style = TASK_STYLE_NEGATIVE;
   if (autoconfig_handle->device_info.autoconfigured)
   {
      /* Successful addition style */
      task->style = TASK_STYLE_POSITIVE;

      if (match_found)
      {
         /* A valid autoconfig was applied */
         if (!(autoconfig_handle->flags & AUTOCONF_FLAG_SUPPRESS_NOTIFICATIONS))
            snprintf(task_title, sizeof(task_title),
                  msg_hash_to_str(MSG_DEVICE_CONFIGURED_IN_PORT_NR),
                  device_display_name,
                  autoconfig_handle->port + 1);
      }
      /* Device is autoconfigured, but a (most likely
       * incorrect) fallback definition was used... */
      else if (!(autoconfig_handle->flags & AUTOCONF_FLAG_SUPPRESS_FAILURE_NOTIF))
         snprintf(task_title, sizeof(task_title),
                  msg_hash_to_str(MSG_DEVICE_NOT_CONFIGURED_FALLBACK_NR),
                  device_display_name,
                  autoconfig_handle->device_info.vid,
                  autoconfig_handle->device_info.pid);
   }
   /* Autoconfig failed */
   else if (!(autoconfig_handle->flags & AUTOCONF_FLAG_SUPPRESS_FAILURE_NOTIF))
         snprintf(task_title, sizeof(task_title),
                  msg_hash_to_str(MSG_DEVICE_NOT_CONFIGURED_NR),
                  device_display_name,
                  autoconfig_handle->device_info.vid,
                  autoconfig_handle->device_info.pid);

   /* Update task title */
   task_free_title(task);
   if (!string_is_empty(task_title))
   {
      task_set_title(task, strdup(task_title));
      RARCH_LOG("[Autoconf] %s.\n", task_title);
   }

   task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
}

static bool autoconfigure_connect_finder(retro_task_t *task, void *user_data)
{
   autoconfig_handle_t *autoconfig_handle = NULL;
   unsigned *port                         = NULL;

   if (!task || !user_data)
      return false;

   if (task->handler != input_autoconfigure_connect_handler)
      return false;

   autoconfig_handle = (autoconfig_handle_t*)task->state;
   if (!autoconfig_handle)
      return false;

   port = (unsigned*)user_data;
   return (*port == autoconfig_handle->port);
}

bool input_autoconfigure_connect(
      const char *name,
      const char *display_name,
      const char *driver,
      unsigned port,
      unsigned vid,
      unsigned pid)
{
   task_finder_data_t find_data;
   retro_task_t *task                     = NULL;
   autoconfig_handle_t *autoconfig_handle = NULL;
   bool driver_valid                      = false;
   settings_t *settings                   = config_get_ptr();
   bool autoconfig_enabled                = settings ?
         settings->bools.input_autodetect_enable : false;
   const char *dir_autoconfig             = settings ?
         settings->paths.directory_autoconfig : NULL;
   bool notification_show_autoconfig      = settings ?
         settings->bools.notification_show_autoconfig : true;
   bool notification_show_autoconfig_fails = settings ?
         settings->bools.notification_show_autoconfig_fails : true;

   if (port >= MAX_INPUT_DEVICES)
      return false;

   /* Cannot connect a device that is currently
    * being connected */
   find_data.func     = autoconfigure_connect_finder;
   find_data.userdata = (void*)&port;

   if (task_queue_find(&find_data))
      return false;

   /* Configure handle */
   if (!(autoconfig_handle = (autoconfig_handle_t*)
            calloc(1, sizeof(autoconfig_handle_t))))
      return false;

   autoconfig_handle->port                         = port;
   autoconfig_handle->device_info.vid              = vid;
   autoconfig_handle->device_info.pid              = pid;
   autoconfig_handle->device_info.name[0]          = '\0';
   autoconfig_handle->device_info.display_name[0]  = '\0';
   autoconfig_handle->device_info.config_name[0]   = '\0';
   autoconfig_handle->device_info.joypad_driver[0] = '\0';
   autoconfig_handle->device_info.autoconfigured   = false;
   autoconfig_handle->device_info.name_index       = 0;
   if (autoconfig_enabled)
      autoconfig_handle->flags |= AUTOCONF_FLAG_AUTOCONFIG_ENABLED;
   if (!notification_show_autoconfig)
      autoconfig_handle->flags |= AUTOCONF_FLAG_SUPPRESS_NOTIFICATIONS;
   if (!notification_show_autoconfig_fails)
      autoconfig_handle->flags |= AUTOCONF_FLAG_SUPPRESS_FAILURE_NOTIF;
   autoconfig_handle->dir_autoconfig               = NULL;
   autoconfig_handle->dir_driver_autoconfig        = NULL;
   autoconfig_handle->autoconfig_file              = NULL;

   if (!string_is_empty(name))
      strlcpy(autoconfig_handle->device_info.name, name,
            sizeof(autoconfig_handle->device_info.name));

   if (!string_is_empty(display_name))
      strlcpy(autoconfig_handle->device_info.display_name, display_name,
            sizeof(autoconfig_handle->device_info.display_name));

   if ((driver_valid = !string_is_empty(driver)))
      strlcpy(autoconfig_handle->device_info.joypad_driver,
            driver, sizeof(autoconfig_handle->device_info.joypad_driver));

   /* > Have to cache both the base autoconfig directory
    *   and the driver-specific autoconfig directory
    *   - Driver-specific directory is scanned by
    *     default, if available
    *   - If driver-specific directory is unavailable,
    *     we scan the base autoconfig directory as
    *     a fallback */
   if (!string_is_empty(dir_autoconfig))
   {
      autoconfig_handle->dir_autoconfig = strdup(dir_autoconfig);

      if (driver_valid)
      {
         char dir_driver_autoconfig[DIR_MAX_LENGTH];
         /* Generate driver-specific autoconfig directory */
         fill_pathname_join_special(dir_driver_autoconfig,
               dir_autoconfig,
               autoconfig_handle->device_info.joypad_driver,
               sizeof(dir_driver_autoconfig));

         if (!string_is_empty(dir_driver_autoconfig))
            autoconfig_handle->dir_driver_autoconfig =
                  strdup(dir_driver_autoconfig);
      }
   }

#ifdef HAVE_BLISSBOX
   /* Bliss-Box shenanigans... */
   if (autoconfig_handle->device_info.vid == BLISSBOX_VID)
      input_autoconfigure_blissbox_override_handler(
            (int)autoconfig_handle->device_info.vid,
            (int)autoconfig_handle->device_info.pid,
            autoconfig_handle->device_info.name,
            sizeof(autoconfig_handle->device_info.name));
#endif

   /* If we are reconnecting a device that is already
    * connected and autoconfigured, then there is no need
    * to generate additional 'connection successful'
    * task status messages
    * > Can skip this check if autoconfig notifications
    *   have been disabled by the user */
   if (   !(autoconfig_handle->flags & AUTOCONF_FLAG_SUPPRESS_NOTIFICATIONS)
       && !string_is_empty(autoconfig_handle->device_info.name))
   {
      const char *last_device_name = input_config_get_device_name(port);
      uint16_t last_vid            = input_config_get_device_vid(port);
      uint16_t last_pid            = input_config_get_device_pid(port);
      bool last_autoconfigured     = input_config_get_device_autoconfigured(port);

      if (  !string_is_empty(last_device_name)
          && string_is_equal(autoconfig_handle->device_info.name,
               last_device_name)
          && (autoconfig_handle->device_info.vid == last_vid)
          && (autoconfig_handle->device_info.pid == last_pid)
          && last_autoconfigured)
         autoconfig_handle->flags |= AUTOCONF_FLAG_SUPPRESS_NOTIFICATIONS;
   }

   /* Configure task */
   if (!(task = task_init()))
   {
      free_autoconfig_handle(autoconfig_handle);
      return false;
   }

   task->handler  = input_autoconfigure_connect_handler;
   task->state    = autoconfig_handle;
   task->title    = NULL;
   task->callback = cb_input_autoconfigure_connect;
   task->cleanup  = input_autoconfigure_free;
   task->flags   &= ~RETRO_TASK_FLG_MUTE;

   task_queue_push(task);

   return true;
}

/****************************/
/* Autoconfigure Disconnect */
/****************************/

static void cb_input_autoconfigure_disconnect(
      retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   unsigned port;
   autoconfig_handle_t *autoconfig_handle = NULL;

   if (!task)
      return;

   if (!(autoconfig_handle = (autoconfig_handle_t*)task->state))
      return;

   /* Use local copy of port index for brevity... */
   port = autoconfig_handle->port;

   /* We perform the actual 'disconnect' in this
    * callback, to ensure it occurs on the main thread */
   input_config_clear_device_name(port);
   input_config_clear_device_display_name(port);
   input_config_clear_device_config_name(port);
   input_config_clear_device_joypad_driver(port);
   input_config_set_device_vid(port, 0);
   input_config_set_device_pid(port, 0);
   input_config_set_device_autoconfigured(port, false);
   input_config_reset_autoconfig_binds(port);
}

static void input_autoconfigure_disconnect_handler(retro_task_t *task)
{
   autoconfig_handle_t *autoconfig_handle = NULL;

   if (!task)
      return;

   if ((autoconfig_handle = (autoconfig_handle_t*)task->state))
   {
      char task_title[NAME_MAX_LENGTH + 16];
      const char *device_display_name = NULL;
      /* Removal style */
      task->style = TASK_STYLE_NEGATIVE;

      /* Get display name for task status message */
      device_display_name = autoconfig_handle->device_info.display_name;
      if (string_is_empty(device_display_name))
         device_display_name = autoconfig_handle->device_info.name;
      if (string_is_empty(device_display_name))
         device_display_name = msg_hash_to_str(MENU_ENUM_LABEL_VALUE_NOT_AVAILABLE);

      /* Set task title */
      snprintf(task_title, sizeof(task_title),
            msg_hash_to_str(MSG_DEVICE_DISCONNECTED_FROM_PORT_NR),
            device_display_name,
            autoconfig_handle->port + 1);

      task_free_title(task);
      if (!(autoconfig_handle->flags & AUTOCONF_FLAG_SUPPRESS_NOTIFICATIONS))
         task_set_title(task, strdup(task_title));
      if (!string_is_empty(task_title))
         RARCH_LOG("[Autoconf] %s.\n", task_title);
   }

   task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
}

static bool autoconfigure_disconnect_finder(retro_task_t *task, void *user_data)
{
   autoconfig_handle_t *autoconfig_handle = NULL;
   unsigned *port                         = NULL;

   if (!task || !user_data)
      return false;

   if (task->handler != input_autoconfigure_disconnect_handler)
      return false;

   if (!(autoconfig_handle = (autoconfig_handle_t*)task->state))
      return false;

   port = (unsigned*)user_data;
   return (*port == autoconfig_handle->port);
}

/* Note: There is no real need for autoconfigure
 * 'disconnect' to be a task - we are merely setting
 * a handful of variables. However:
 * - Making it a task means we can call
 *   input_autoconfigure_disconnect() on any thread
 *   thread, and defer the global state changes until
 *   the task queue is handled on the *main* thread
 * - By using a task for both 'connect' and 'disconnect',
 *   we ensure uniformity of OSD status messages */
bool input_autoconfigure_disconnect(unsigned port, const char *name)
{
   task_finder_data_t find_data;
   retro_task_t *task                     = NULL;
   autoconfig_handle_t *autoconfig_handle = NULL;
   settings_t *settings                   = config_get_ptr();
   input_driver_state_t *input_st         = input_state_get_ptr();
   bool notification_show_autoconfig      = settings ? settings->bools.notification_show_autoconfig : true;
   bool pause_on_disconnect               = settings ? settings->bools.pause_on_disconnect : true;
   bool menu_pause_libretro               = settings ? settings->bools.menu_pause_libretro : false;
   bool core_is_running                   = (runloop_state_get_ptr()->flags & RUNLOOP_FLAG_CORE_RUNNING) ? true : false;

   if (port >= MAX_INPUT_DEVICES)
      return false;

   /* Cannot disconnect a device that is currently
    * being disconnected */
   find_data.func     = autoconfigure_disconnect_finder;
   find_data.userdata = (void*)&port;

   if (task_queue_find(&find_data))
      return false;

   /* Configure handle */
   autoconfig_handle = (autoconfig_handle_t*)calloc(1, sizeof(autoconfig_handle_t));

   if (!autoconfig_handle)
      return false;

   autoconfig_handle->port      = port;
   if (!notification_show_autoconfig)
      autoconfig_handle->flags |= AUTOCONF_FLAG_SUPPRESS_NOTIFICATIONS;

   /* Use display_name as name instead since autoconfig display_name
    * is destroyed already, and real name does not matter at this point */
   if (input_st && !string_is_empty(input_st->input_device_info[port].display_name))
      strlcpy(autoconfig_handle->device_info.name,
            input_st->input_device_info[port].display_name,
            sizeof(autoconfig_handle->device_info.name));
   else if (!string_is_empty(name))
      strlcpy(autoconfig_handle->device_info.name,
            name, sizeof(autoconfig_handle->device_info.name));

   /* Configure task */
   if (!(task = task_init()))
   {
      free_autoconfig_handle(autoconfig_handle);
      return false;
   }

   task->handler  = input_autoconfigure_disconnect_handler;
   task->state    = autoconfig_handle;
   task->title    = NULL;
   task->callback = cb_input_autoconfigure_disconnect;
   task->cleanup  = input_autoconfigure_free;

   task_queue_push(task);

   if (pause_on_disconnect && core_is_running)
   {
#ifdef HAVE_MENU
      bool menu_is_alive = (menu_state_get_ptr()->flags & MENU_ST_FLAG_ALIVE)
         ? true : false;
      if (menu_pause_libretro)
      {
         if (!menu_is_alive)
            command_event(CMD_EVENT_MENU_TOGGLE, NULL);
      }
      else
         command_event(CMD_EVENT_PAUSE, NULL);
#else
      command_event(CMD_EVENT_PAUSE, NULL);
#endif
   }

   return true;
}
