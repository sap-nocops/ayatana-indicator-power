/*
 * Copyright 2023 The UBports project
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranties of
 * MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *   Lorenzo Torracchi <lorenzotorracchi@mail.com>
 */

#include "keep-screen-on.h"

#include <gio/gio.h>
#include <ayatana/common/utils.h>

gboolean activated = 0;
guint oldValue = 0;
GSettings* g_settings;

gboolean
keep_screen_on_activated()
{
  return activated;
}

void
toggle_keep_screen_on_action(GAction *action,
                         GVariant *parameter G_GNUC_UNUSED,
                         gpointer data G_GNUC_UNUSED)
{
  GVariant *state;

  state = g_action_get_state(action);
  activated = g_variant_get_boolean(state);
  g_variant_unref(state);

  gchar* key = ayatana_common_utils_is_lomiri() ? "activity-timeout" : "idle-delay";

  guint newValue;
  guint curValue = g_settings_get_uint (g_settings_instance(), key);
  if (curValue == 0) {
      newValue = oldValue;
  } else {
      oldValue = curValue;
      newValue = 0;
  }

  gboolean is_set = g_settings_set(g_settings_instance(), key, "u", newValue)
  if(is_set) {
      g_simple_action_set_state (G_SIMPLE_ACTION (action), g_variant_new_boolean(!activated));
  } else {
      g_warning("cannot set %s", key);
  }
}

int
keep_screen_on_supported()
{
  return 1;
}

GSettings*
g_settings_instance()
{
    if (!g_settings) {
        g_settings = g_settings_new(ayatana_common_utils_is_lomiri() ? "com.lomiri.touch.system" : "org.gnome.desktop.session");
    }
    return g_settings;
}
