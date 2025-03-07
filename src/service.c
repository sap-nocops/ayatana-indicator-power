/*
 * Copyright 2013-2016 Canonical Ltd.
 * Copyright 2021-2023 Robert Tari
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
 *   Charles Kerr <charles.kerr@canonical.com>
 *   Ted Gould <ted@canonical.com>
 *   Robert Tari <robert@tari.in>
 */

#include <glib/gi18n.h>
#include <gio/gio.h>
#include <ayatana/common/utils.h>
#include "brightness.h"
#include "dbus-shared.h"
#include "device.h"
#include "device-provider.h"
#include "notifier.h"
#include "service.h"
#include "flashlight.h"
#include "utils.h"

#define BUS_NAME "org.ayatana.indicator.power"
#define BUS_PATH "/org/ayatana/indicator/power"

#define SETTINGS_SHOW_TIME_S "show-time"
#define SETTINGS_ICON_POLICY_S "icon-policy"
#define SETTINGS_SHOW_PERCENTAGE_S "show-percentage"

enum
{
  SIGNAL_NAME_LOST,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

enum
{
  PROP_0,
  PROP_BUS,
  PROP_DEVICE_PROVIDER,
  PROP_NOTIFIER,
  LAST_PROP
};

static GParamSpec * properties[LAST_PROP];

enum
{
  SECTION_HEADER    = (1<<0),
  SECTION_DEVICES   = (1<<1),
  SECTION_SETTINGS  = (1<<2),
};

enum
{
  PROFILE_PHONE,
  PROFILE_DESKTOP,
  PROFILE_DESKTOP_GREETER,
  N_PROFILES
};

static const char * const menu_names[N_PROFILES] =
{
  "phone",
  "desktop",
  "desktop_greeter"
};

enum
{
  POWER_INDICATOR_ICON_POLICY_PRESENT,
  POWER_INDICATOR_ICON_POLICY_CHARGE,
  POWER_INDICATOR_ICON_POLICY_NEVER
};

struct ProfileMenuInfo
{
  /* the root level -- the header is the only child of this */
  GMenu * menu;

  /* parent of the sections. This is the header's submenu */
  GMenu * submenu;

  guint export_id;
};

struct _IndicatorPowerServicePrivate
{
  GCancellable * cancellable;

  GSettings * settings;

  IndicatorPowerBrightness * brightness;

  guint own_id;
  guint actions_export_id;
  GDBusConnection * conn;

  gboolean menus_built;
  struct ProfileMenuInfo menus[N_PROFILES];

  GSimpleActionGroup * actions;
  GSimpleAction * header_action;
  GSimpleAction * battery_level_action;
  GSimpleAction * device_state_action;
  GSimpleAction * brightness_action;

  IndicatorPowerDevice * primary_device;
  GList * devices; /* IndicatorPowerDevice */

  IndicatorPowerDeviceProvider * device_provider;
  IndicatorPowerNotifier * notifier;
};

typedef IndicatorPowerServicePrivate priv_t;

G_DEFINE_TYPE_WITH_PRIVATE(IndicatorPowerService, indicator_power_service, G_TYPE_OBJECT)

/***
****
****  DEVICES
****
***/

/* the higher the weight, the more interesting the device */
static int
get_device_kind_weight (const IndicatorPowerDevice * device)
{
  UpDeviceKind kind;
  static gboolean initialized = FALSE;
  static int weights[UP_DEVICE_KIND_LAST];

  kind = indicator_power_device_get_kind (device);
  g_return_val_if_fail (0<=kind && kind<UP_DEVICE_KIND_LAST, 0);

  if (G_UNLIKELY(!initialized))
    {
      int i;

      initialized = TRUE;

      for (i=0; i<UP_DEVICE_KIND_LAST; i++)
        weights[i] = 1;
      weights[UP_DEVICE_KIND_BATTERY] = 2;
      weights[UP_DEVICE_KIND_LINE_POWER] = 0;
    }

  return weights[kind];
}

/* sort devices from most interesting to least interesting on this criteria:
   1. device that supplied the power to the system
   2. discharging items from least time remaining until most time remaining
   3. charging items from most time left to charge to least time left to charge
   4. charging items with an unknown time remaining
   5. discharging items with an unknown time remaining
   6. batteries, then non-line power, then line-power */
static gint
device_compare_func (gconstpointer ga, gconstpointer gb)
{
  int ret;
  int state;
  const IndicatorPowerDevice * a = ga;
  const IndicatorPowerDevice * b = gb;
  const gboolean a_power_supply = indicator_power_device_get_power_supply (a);
  const gboolean b_power_supply = indicator_power_device_get_power_supply (b);
  const int a_state = indicator_power_device_get_state (a);
  const int b_state = indicator_power_device_get_state (b);
  const gdouble a_percentage = indicator_power_device_get_percentage (a);
  const gdouble b_percentage = indicator_power_device_get_percentage (b);
  const time_t a_time = indicator_power_device_get_time (a);
  const time_t b_time = indicator_power_device_get_time (b);

  ret = 0;

  if (!ret && (a_power_supply != b_power_supply))
    {
      if (a_power_supply) /* a provides power to the system */
        {
          ret = -1;
        }
      else /* b provides power to the system */
        {
          ret = 1;
        }
    }

  state = UP_DEVICE_STATE_DISCHARGING;
  if (!ret && (((a_state == state) && a_time) ||
               ((b_state == state) && b_time)))
    {
      if (a_state != state) /* b is discharging */
        {
          ret = 1;
        }
      else if (b_state != state) /* a is discharging */
        {
          ret = -1;
        }
      else /* both are discharging; least-time-left goes first */
        {
          if (!a_time || !b_time) /* known time always trumps unknown time */
            ret = a_time ? -1 : 1;
          else if (a_time != b_time)
            ret = a_time < b_time ? -1 : 1;
          else
            ret = a_percentage < b_percentage ? -1 : 1;
        }
    }

  state = UP_DEVICE_STATE_CHARGING;
  if (!ret && ((a_state == state) || (b_state == state)))
    {
      if (a_state != state) /* b is charging */
        {
          ret = 1;
        }
      else if (b_state != state) /* a is charging */
        {
          ret = -1;
        }
      else /* both are discharging; most-time-to-charge goes first */
        {
          if (!a_time || !b_time) /* known time always trumps unknown time */
            ret = a_time ? -1 : 1;
          else if (a_time != b_time)
            ret = a_time > b_time ? -1 : 1;
          else
            ret = a_percentage < b_percentage ? -1 : 1;
        }
    }

  state = UP_DEVICE_STATE_DISCHARGING;
  if (!ret && ((a_state == state) || (b_state == state)))
    {
      if (a_state != state) /* b is discharging */
        {
          ret = 1;
        }
      else if (b_state != state) /* a is discharging */
        {
          ret = -1;
        }
      else /* both are discharging; use percentage */
        {
            ret = a_percentage < b_percentage ? -1 : 1;
        }
    }

  /* neither device is charging nor discharging... */

  /* unless there's no other option,
     don't choose a device with an unknown state.
     https://bugs.launchpad.net/ubuntu/+source/indicator-power/+bug/1470080 */
  state = UP_DEVICE_STATE_UNKNOWN;
  if (!ret && ((a_state == state) || (b_state == state)))
    {
      if (a_state != state) /* b is unknown */
        {
          ret = -1;
        }
      else if (b_state != state) /* a is unknown */
        {
          ret = 1;
        }
    }

  if (!ret)
    {
      const int weight_a = get_device_kind_weight (a);
      const int weight_b = get_device_kind_weight (b);

      if (weight_a > weight_b)
        {
          ret = -1;
        }
      else if (weight_a < weight_b)
        {
          ret = 1;
        }
    }

  if (!ret)
    ret = a_state - b_state;

  return ret;
}

static const char*
device_state_to_string(UpDeviceState device_state)
{
  const char * str;

  switch (device_state)
    {
      case UP_DEVICE_STATE_CHARGING:
        str = "charging";
        break;

      case UP_DEVICE_STATE_DISCHARGING:
        str = "discharging";
        break;

      case UP_DEVICE_STATE_EMPTY:
        str = "empty";
        break;

      case UP_DEVICE_STATE_FULLY_CHARGED:
        str = "fully-charged";
        break;

      case UP_DEVICE_STATE_PENDING_CHARGE:
        str = "pending-charge";
        break;

      case UP_DEVICE_STATE_PENDING_DISCHARGE:
        str = "pending-discharge";
        break;

      default:
        str = "unknown";
        break;
    }

  return str;
}

static GVariant *
calculate_device_state_action_state (IndicatorPowerService * self)
{
  const priv_t * const p = self->priv;
  UpDeviceState device_state;

  if (p->primary_device != NULL)
    device_state = indicator_power_device_get_state(p->primary_device);
  else
    device_state = UP_DEVICE_STATE_UNKNOWN;

  return g_variant_new_string(device_state_to_string(device_state));
}

/***
****
****  HEADER SECTION
****
***/

static void
count_batteries (GList * devices, int *total, int *inuse)
{
  GList * l;

  for (l=devices; l!=NULL; l=l->next)
    {
      const IndicatorPowerDevice * device = INDICATOR_POWER_DEVICE(l->data);

      if (indicator_power_device_get_kind(device) == UP_DEVICE_KIND_BATTERY ||
          indicator_power_device_get_kind(device) == UP_DEVICE_KIND_UPS)
        {
          ++*total;

          const UpDeviceState state = indicator_power_device_get_state (device);
          if ((state == UP_DEVICE_STATE_CHARGING) ||
              (state == UP_DEVICE_STATE_DISCHARGING))
            ++*inuse;
        }
    }

  g_debug ("count_batteries found %d batteries (%d are charging/discharging)",
           *total, *inuse);
}

static gboolean
should_be_visible (IndicatorPowerService * self)
{
  if (!ayatana_common_utils_is_lomiri())
  {
      return TRUE;
  }

  gboolean visible = TRUE;
  priv_t * p = self->priv;

  const int policy = g_settings_get_enum (p->settings, SETTINGS_ICON_POLICY_S);
  g_debug ("policy is: %d (present==0, charge==1, never==2)", policy);

  if (policy == POWER_INDICATOR_ICON_POLICY_NEVER)
    {
      visible = FALSE;
    }
    else
    {
      int batteries=0, inuse=0;

      count_batteries (p->devices, &batteries, &inuse);

      if (policy == POWER_INDICATOR_ICON_POLICY_PRESENT)
        {
          visible = batteries > 0;
        }
      else if (policy == POWER_INDICATOR_ICON_POLICY_CHARGE)
        {
          visible = inuse > 0;
        }
    }

  g_debug ("should_be_visible: %s", visible?"yes":"no");
  return visible;
}

static GVariant *
create_header_state (IndicatorPowerService * self)
{
  GVariantBuilder b;
  const priv_t * const p = self->priv;

  g_variant_builder_init (&b, G_VARIANT_TYPE("a{sv}"));

  g_variant_builder_add (&b, "{sv}", "title", g_variant_new_string (_("Battery")));

  g_variant_builder_add (&b, "{sv}", "tooltip", g_variant_new_string (_("Battery status and power management")));

  g_variant_builder_add (&b, "{sv}", "visible",
                         g_variant_new_boolean (should_be_visible (self)));

  if (p->primary_device != NULL)
    {
      char * title;
      GIcon * icon;
      const gboolean want_time = g_settings_get_boolean (p->settings, SETTINGS_SHOW_TIME_S);
      const gboolean want_percent = g_settings_get_boolean (p->settings, SETTINGS_SHOW_PERCENTAGE_S);

      title = indicator_power_device_get_readable_title (p->primary_device,
                                                         want_time,
                                                         want_percent);
      if (title)
        {
          if (*title)
            g_variant_builder_add (&b, "{sv}", "label", g_variant_new_take_string (title));
          else
            g_free (title);
        }

      title = indicator_power_device_get_accessible_title (p->primary_device,
                                                           want_time,
                                                           want_percent);
      if (title)
        {
          if (*title)
            g_variant_builder_add (&b, "{sv}", "accessible-desc", g_variant_new_take_string (title));
          else
            g_free (title);
        }

      if ((icon = indicator_power_device_get_gicon (p->primary_device, TRUE, TRUE)))
        {
          GVariant * serialized_icon = g_icon_serialize (icon);

          if (serialized_icon != NULL)
            {
              g_variant_builder_add (&b, "{sv}", "icon", serialized_icon);
              g_variant_unref (serialized_icon);
            }

          g_object_unref (icon);
        }
    }
    else if (ayatana_common_utils_is_lomiri())
    {
        g_variant_builder_add (&b, "{sv}", "label", g_variant_new_string ("Fake battery"));
        g_variant_builder_add (&b, "{sv}", "accessible-desc", g_variant_new_string ("Fake battery"));
        GIcon *pIcon = g_themed_icon_new_with_default_fallbacks ("battery-missing-symbolic");
        GVariant *pIconSerialised = g_icon_serialize (pIcon);
        g_object_unref (pIcon);
        g_variant_builder_add (&b, "{sv}", "icon", pIconSerialised);
        g_variant_unref (pIconSerialised);
    }

  return g_variant_builder_end (&b);
}


/***
****
****  DEVICE SECTION
****
***/

static GMenuModel *
create_devices_section (IndicatorPowerService * self, int profile)
{
    GMenu * menu = g_menu_new ();
    GList * l;

    for (l=self->priv->devices; l!=NULL; l=l->next)
    {
        IndicatorPowerDevice *device = l->data;
        const UpDeviceKind kind = indicator_power_device_get_kind (device);

        if (kind != UP_DEVICE_KIND_LINE_POWER)
        {
            gchar *sLabel = NULL;

            if (kind == UP_DEVICE_KIND_BATTERY)
            {
                if (!ayatana_common_utils_is_lomiri())
                {
                    sLabel = indicator_power_device_get_readable_text (device, FALSE);
                }
                else
                {
                    sLabel = g_strdup (_("Charge level"));
                }
            }
            else
            {
                sLabel = indicator_power_device_get_readable_text (device, TRUE);
            }

            GMenuItem * item = g_menu_item_new (sLabel, NULL);
            g_free (sLabel);
            g_menu_item_set_attribute (item, "x-ayatana-type", "s", "org.ayatana.indicator.level");
            guint16 battery_level = (guint16)(indicator_power_device_get_percentage (device) + 0.5);
            g_menu_item_set_attribute (item, "x-ayatana-level", "q", battery_level);

            if (!ayatana_common_utils_is_lomiri())
            {
                GIcon * icon = indicator_power_device_get_gicon (device, FALSE, FALSE);

                if (icon)
                {
                    GVariant * serialized_icon = g_icon_serialize (icon);

                    if (serialized_icon != NULL)
                    {
                        g_menu_item_set_attribute_value (item, G_MENU_ATTRIBUTE_ICON, serialized_icon);
                        g_variant_unref (serialized_icon);
                    }

                    g_object_unref (icon);
                }

                g_menu_item_set_action_and_target(item, "indicator.activate-statistics", "s", indicator_power_device_get_object_path (device));
            }

            g_menu_append_item (menu, item);
            g_object_unref (item);
        }
    }

    return G_MENU_MODEL (menu);
}

/***
****
****  SETTINGS SECTION
****
***/

static GMenuItem *
create_brightness_menu_item(void)
{
  GIcon * icon;
  GMenuItem * item;

  item = g_menu_item_new(NULL, "indicator.brightness");
  g_menu_item_set_attribute(item, "x-ayatana-type", "s", "org.ayatana.indicator.slider");
  g_menu_item_set_attribute(item, "min-value", "d", 0.0);
  g_menu_item_set_attribute(item, "max-value", "d", 1.0);

  icon = g_themed_icon_new("display-brightness-min");
  g_menu_item_set_attribute_value(item, "min-icon", g_icon_serialize(icon));
  g_clear_object(&icon);

  icon = g_themed_icon_new("display-brightness-max");
  g_menu_item_set_attribute_value(item, "max-icon", g_icon_serialize(icon));
  g_clear_object(&icon);

  return item;
}

static GVariant *
action_state_for_brightness (IndicatorPowerService * self)
{
  IndicatorPowerBrightness * b = self->priv->brightness;
  return g_variant_new_double(indicator_power_brightness_get_percentage(b));
}

static void
update_brightness_action_state (IndicatorPowerService * self)
{
  g_simple_action_set_state (self->priv->brightness_action,
                             action_state_for_brightness (self));
}

static void
on_brightness_change_requested (GSimpleAction * action      G_GNUC_UNUSED,
                                GVariant      * parameter,
                                gpointer        gself)
{
  IndicatorPowerService * self = INDICATOR_POWER_SERVICE (gself);

  indicator_power_brightness_set_percentage(self->priv->brightness,
                                            g_variant_get_double (parameter));
}

static GMenuModel *
create_desktop_settings_section (IndicatorPowerService * self G_GNUC_UNUSED)
{
  GMenu * menu = g_menu_new ();

  g_menu_append (menu,
                 _("Power Settings…"),
                 "indicator.activate-settings");

  return G_MENU_MODEL (menu);
}

static GMenuModel *
create_phone_settings_section(IndicatorPowerService * self)
{
  GMenu * section;
  GMenuItem * item;
  gboolean ab_supported;

  section = g_menu_new();

  item = create_brightness_menu_item();
  g_menu_append_item(section, item);
  update_brightness_action_state(self);
  g_object_unref(item);

  g_object_get(self->priv->brightness,
               "auto-brightness-supported", &ab_supported,
               NULL);

  if (ab_supported)
    {
      item = g_menu_item_new(_("Adjust brightness automatically"), "indicator.auto-brightness(true)");
      g_menu_item_set_attribute(item, "x-ayatana-type", "s", "org.ayatana.indicator.switch");
      g_menu_append_item(section, item);
      g_object_unref(item);
    }

  if (flashlight_supported())
  {
    item = g_menu_item_new(_("Flashlight"), "indicator.flashlight(true)");
    g_menu_item_set_attribute(item, "x-ayatana-type", "s", "org.ayatana.indicator.switch");
    g_menu_append_item(section, item);
    g_object_unref(item);
    if (flashlight_activated())
    {
      item = g_menu_item_new(_("Warning: Heavy use can damage the LED!"), "indicator.flashlight(true)");
      g_menu_append_item(section, item);
      g_object_unref(item);
    }
  }

  if (keep_screen_on_supported())
  {
    item = g_menu_item_new(_("Keep screen on"), "indicator.keep-screen-on(true)");
    g_menu_item_set_attribute(item, "x-ayatana-type", "s", "org.ayatana.indicator.switch");
    g_menu_append_item(section, item);
    g_object_unref(item);
  }

  g_menu_append(section, _("Battery settings…"), "indicator.activate-phone-settings");

  return G_MENU_MODEL(section);
}

/***
****
****  SECTION REBUILDING
****
***/

/**
 * A small helper function for rebuild_now().
 * - removes the previous section
 * - adds and unrefs the new section
 */
static void
rebuild_section (GMenu * parent, int pos, GMenuModel * new_section)
{
  g_menu_remove (parent, pos);
  g_menu_insert_section (parent, pos, NULL, new_section);
  g_object_unref (new_section);
}

static void
rebuild_now (IndicatorPowerService * self, guint sections)
{
  priv_t * p = self->priv;
  struct ProfileMenuInfo * phone   = &p->menus[PROFILE_PHONE];
  struct ProfileMenuInfo * desktop = &p->menus[PROFILE_DESKTOP];
  struct ProfileMenuInfo * greeter = &p->menus[PROFILE_DESKTOP_GREETER];

  if (sections & SECTION_HEADER)
    {
      g_simple_action_set_state (p->header_action, create_header_state (self));
    }

  if (!p->menus_built)
    return;

  if (sections & SECTION_DEVICES)
    {
      rebuild_section (phone->submenu, 0, create_devices_section (self, PROFILE_PHONE));
      rebuild_section (desktop->submenu, 0, create_devices_section (self, PROFILE_DESKTOP));
      rebuild_section (greeter->submenu, 0, create_devices_section (self, PROFILE_DESKTOP_GREETER));
    }

  if (sections & SECTION_SETTINGS)
    {
      rebuild_section (desktop->submenu, 1, create_desktop_settings_section (self));
      rebuild_section (phone->submenu, 1, create_phone_settings_section (self));
    }
}

static inline void
rebuild_header_now (IndicatorPowerService * self)
{
  rebuild_now (self, SECTION_HEADER);
}

static void
create_menu (IndicatorPowerService * self, int profile)
{
  GMenu * menu;
  GMenu * submenu;
  GMenuItem * header;
  GMenuModel * sections[16];
  guint i;
  guint n = 0;

  g_assert (0<=profile && profile<N_PROFILES);
  g_assert (self->priv->menus[profile].menu == NULL);

  /* build the sections */

  switch (profile)
    {
      case PROFILE_PHONE:
        sections[n++] = create_devices_section (self, PROFILE_PHONE);
        sections[n++] = create_phone_settings_section (self);
        break;

      case PROFILE_DESKTOP:
        sections[n++] = create_devices_section (self, PROFILE_DESKTOP);
        sections[n++] = create_desktop_settings_section (self);
        break;

      case PROFILE_DESKTOP_GREETER:
        sections[n++] = create_devices_section (self, PROFILE_DESKTOP_GREETER);
        break;
    }

  /* add sections to the submenu */

  submenu = g_menu_new ();

  for (i=0; i<n; ++i)
    {
      g_menu_append_section (submenu, NULL, sections[i]);
      g_object_unref (sections[i]);
    }

  /* add submenu to the header */
  header = g_menu_item_new (NULL, "indicator._header");
  g_menu_item_set_attribute (header, "x-ayatana-type",
                             "s", "org.ayatana.indicator.root");
  g_menu_item_set_submenu (header, G_MENU_MODEL (submenu));
  g_object_unref (submenu);

  /* add header to the menu */
  menu = g_menu_new ();
  g_menu_append_item (menu, header);
  g_object_unref (header);

  self->priv->menus[profile].menu = menu;
  self->priv->menus[profile].submenu = submenu;
}

/***
****  GActions
***/

static void
on_settings_activated (GSimpleAction * a      G_GNUC_UNUSED,
                       GVariant      * param  G_GNUC_UNUSED,
                       gpointer        gself  G_GNUC_UNUSED)
{
  utils_handle_settings_request();
}

static void
on_statistics_activated (GSimpleAction * a      G_GNUC_UNUSED,
                         GVariant      * param,
                         gpointer        gself  G_GNUC_UNUSED)
{
  if (ayatana_common_utils_is_mate())
    {
      char *cmd = g_strconcat ("mate-power-statistics", " --device ",
                               g_variant_get_string (param, NULL), NULL);
      ayatana_common_utils_execute_command (cmd);
      g_free (cmd);
    }
  else if (ayatana_common_utils_is_budgie() || ayatana_common_utils_is_gnome() || ayatana_common_utils_is_unity())
    {
      char *cmd = g_strconcat ("gnome-power-statistics", " --device ",
                               g_variant_get_string (param, NULL), NULL);
      ayatana_common_utils_execute_command (cmd);
      g_free (cmd);
    }
  else
    {
      ayatana_common_utils_zenity_warning ("dialog-warning",
                      _("Warning"),
                      _("The Ayatana Power Indicator does not support evoking the\npower statistics application of your desktop environment, yet.\n\nPlease report this to the developers at:\nhttps://github.com/ArcticaProject/ayatana-indicator-power/issues"));
    }
}

static void
on_phone_settings_activated (GSimpleAction * a      G_GNUC_UNUSED,
                             GVariant      * param  G_GNUC_UNUSED,
                             gpointer        gself  G_GNUC_UNUSED)
{
    utils_handle_settings_request();
}

/***
****
***/

static gboolean
convert_auto_prop_to_state(GBinding     * binding     G_GNUC_UNUSED,
                           const GValue * from_value,
                           GValue       * to_value,
                           gpointer       user_data   G_GNUC_UNUSED)
{
  const gboolean b = g_value_get_boolean(from_value);
  g_value_set_variant(to_value, g_variant_new_boolean(b));
  return TRUE;
}

static gboolean
convert_auto_state_to_prop(GBinding     * binding     G_GNUC_UNUSED,
                           const GValue * from_value,
                           GValue       * to_value,
                           gpointer       user_data   G_GNUC_UNUSED)
{
  GVariant * v = g_value_get_variant(from_value);
  g_value_set_boolean(to_value, g_variant_get_boolean(v));
  return TRUE;
}

static void
init_gactions (IndicatorPowerService * self)
{
  GSimpleAction * a;
  GAction * show_time_action;
  GAction * show_percentage_action;
  priv_t * p = self->priv;

  GActionEntry entries[] = {
    { "activate-settings", on_settings_activated },
    { "activate-phone-settings", on_phone_settings_activated },
    { "activate-statistics", on_statistics_activated, "s" }
  };

  p->actions = g_simple_action_group_new ();

  g_action_map_add_action_entries (G_ACTION_MAP(p->actions),
                                   entries,
                                   G_N_ELEMENTS(entries),
                                   self);

  /* add the header action */
  a = g_simple_action_new_stateful ("_header", NULL, create_header_state (self));
  g_action_map_add_action (G_ACTION_MAP(p->actions), G_ACTION(a));
  p->header_action = a;

  /* add the charge state action */
  a = g_simple_action_new_stateful ("device-state", NULL, calculate_device_state_action_state(self));
  g_simple_action_set_enabled (a, FALSE);
  g_action_map_add_action (G_ACTION_MAP(p->actions), G_ACTION(a));
  p->device_state_action = a;

  /* add the auto-brightness action */
  GVariantType *pType = g_variant_type_new ("b");
  a = g_simple_action_new_stateful ("auto-brightness", pType, g_variant_new_boolean (FALSE));
  g_variant_type_free (pType);
  g_object_bind_property_full(p->brightness, "auto-brightness",
                              a,  "state",
                              G_BINDING_SYNC_CREATE|G_BINDING_BIDIRECTIONAL,
                              convert_auto_prop_to_state,
                              convert_auto_state_to_prop,
                              NULL, NULL);
  g_action_map_add_action(G_ACTION_MAP(p->actions), G_ACTION(a));

  /* add the flashlight action */
  pType = g_variant_type_new ("b");
  a = g_simple_action_new_stateful ("flashlight", pType, g_variant_new_boolean (FALSE));
  g_variant_type_free (pType);
  g_action_map_add_action (G_ACTION_MAP(p->actions), G_ACTION(a));
  g_signal_connect(a, "change-state", G_CALLBACK(toggle_flashlight_action), self);

  /* add the keep screen on action */
  pType = g_variant_type_new ("b");
  a = g_simple_action_new_stateful ("keep-screen-on", pType, g_variant_new_boolean (FALSE));
  g_variant_type_free (pType);
  g_action_map_add_action (G_ACTION_MAP(p->actions), G_ACTION(a));
  g_signal_connect(a, "change-state", G_CALLBACK(toggle_keep_screen_on_action), self);

  /* add the brightness action */
  a = g_simple_action_new_stateful ("brightness", NULL, action_state_for_brightness (self));
  g_action_map_add_action (G_ACTION_MAP(p->actions), G_ACTION(a));
  g_signal_connect (a, "change-state", G_CALLBACK(on_brightness_change_requested), self);
  p->brightness_action = a;

  /* add the show-time action */
  show_time_action = g_settings_create_action (p->settings, "show-time");
  g_action_map_add_action (G_ACTION_MAP(p->actions), show_time_action);

  /* add the show-percentage action */
  show_percentage_action = g_settings_create_action (p->settings, "show-percentage");
  g_action_map_add_action (G_ACTION_MAP(p->actions), show_percentage_action);

  rebuild_header_now (self);

  g_object_unref (show_time_action);
  g_object_unref (show_percentage_action);
}

/***
****  GDBus Name Ownership & Menu / Action Exporting
***/

static void
on_bus_acquired (GDBusConnection * connection,
                 const gchar     * name,
                 gpointer          gself)
{
  int i;
  guint id;
  GError * err = NULL;
  IndicatorPowerService * self = INDICATOR_POWER_SERVICE(gself);
  priv_t * p = self->priv;
  GString * path = g_string_new (NULL);

  g_debug ("bus acquired: %s", name);

  p->conn = (GDBusConnection*)g_object_ref(G_OBJECT (connection));
  g_object_notify_by_pspec (G_OBJECT(self), properties[PROP_BUS]);

  /* export the battery properties */
  if (p->notifier != NULL)
    indicator_power_notifier_set_bus (p->notifier, connection);

  /* export the actions */
  if ((id = g_dbus_connection_export_action_group (connection,
                                                   BUS_PATH,
                                                   G_ACTION_GROUP (p->actions),
                                                   &err)))
    {
      p->actions_export_id = id;
    }
  else
    {
      g_warning ("cannot export action group: %s", err->message);
      g_clear_error (&err);
    }

  /* export the menus */
  for (i=0; i<N_PROFILES; ++i)
    {
      struct ProfileMenuInfo * menu = &p->menus[i];

      g_string_printf (path, "%s/%s", BUS_PATH, menu_names[i]);

      if ((id = g_dbus_connection_export_menu_model (connection,
                                                     path->str,
                                                     G_MENU_MODEL (menu->menu),
                                                     &err)))
        {
          menu->export_id = id;
        }
      else
        {
          g_warning ("cannot export %s menu: %s", path->str, err->message);
          g_clear_error (&err);
        }
    }

  g_string_free (path, TRUE);
}

static void
unexport (IndicatorPowerService * self)
{
  int i;
  priv_t * p = self->priv;

  /* unexport the menus */
  for (i=0; i<N_PROFILES; ++i)
    {
      guint * id = &self->priv->menus[i].export_id;

      if (*id)
        {
          g_dbus_connection_unexport_menu_model (p->conn, *id);
          *id = 0;
        }
    }

  /* unexport the actions */
  if (p->actions_export_id)
    {
      g_dbus_connection_unexport_action_group (p->conn, p->actions_export_id);
      p->actions_export_id = 0;
    }
}

static void
on_name_lost (GDBusConnection * connection G_GNUC_UNUSED,
              const gchar     * name,
              gpointer          gself)
{
  IndicatorPowerService * self = INDICATOR_POWER_SERVICE (gself);

  g_debug ("%s %s name lost %s", G_STRLOC, G_STRFUNC, name);

  unexport (self);

  g_signal_emit (self, signals[SIGNAL_NAME_LOST], 0, NULL);
}

/***
****  Events
***/

static void
on_devices_changed (IndicatorPowerService * self)
{
  priv_t * p = self->priv;

  /* update the device list */
  g_list_free_full (p->devices, (GDestroyNotify)g_object_unref);
  p->devices = indicator_power_device_provider_get_devices (p->device_provider);

  /* update the primary device */
  g_clear_object (&p->primary_device);
  p->primary_device = indicator_power_service_choose_primary_device (p->devices);

  /* update the notifier's battery */
  if ((p->primary_device != NULL) && (indicator_power_device_get_kind(p->primary_device) == UP_DEVICE_KIND_BATTERY))
    indicator_power_notifier_set_battery (p->notifier, p->primary_device);
  else
    indicator_power_notifier_set_battery (p->notifier, NULL);

  /* update the device-state action's state */
  g_simple_action_set_state (p->device_state_action, calculate_device_state_action_state(self));

  rebuild_now (self, SECTION_HEADER | SECTION_DEVICES);
}

static void
on_auto_brightness_supported_changed(IndicatorPowerService * self)
{
  rebuild_now(self, SECTION_SETTINGS);
}


/***
****  GObject virtual functions
***/

static void
my_get_property (GObject     * o,
                  guint         property_id,
                  GValue      * value,
                  GParamSpec  * pspec)
{
  IndicatorPowerService * self = INDICATOR_POWER_SERVICE (o);
  priv_t * p = self->priv;

  switch (property_id)
    {
      case PROP_BUS:
        g_value_set_object (value, p->conn);
        break;

      case PROP_DEVICE_PROVIDER:
        g_value_set_object (value, p->device_provider);
        break;

      case PROP_NOTIFIER:
        g_value_set_object (value, p->notifier);
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (o, property_id, pspec);
    }
}

static void
my_set_property (GObject       * o,
                 guint           property_id,
                 const GValue  * value,
                 GParamSpec    * pspec)
{
  IndicatorPowerService * self = INDICATOR_POWER_SERVICE (o);

  switch (property_id)
    {
      case PROP_DEVICE_PROVIDER:
        indicator_power_service_set_device_provider (self, g_value_get_object (value));
        break;

      case PROP_NOTIFIER:
        indicator_power_service_set_notifier (self, g_value_get_object (value));
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (o, property_id, pspec);
    }
}

static void
my_dispose (GObject * o)
{
  IndicatorPowerService * self = INDICATOR_POWER_SERVICE(o);
  priv_t * p = self->priv;

  if (p->own_id)
    {
      g_bus_unown_name (p->own_id);
      p->own_id = 0;
    }

  unexport (self);

  if (p->cancellable != NULL)
    {
      g_cancellable_cancel (p->cancellable);
      g_clear_object (&p->cancellable);
    }

  if (p->settings != NULL)
    {
      g_signal_handlers_disconnect_by_data (p->settings, self);

      g_clear_object (&p->settings);
    }

  g_clear_object (&p->notifier);
  g_clear_object (&p->brightness_action);
  g_clear_object (&p->brightness);
  g_clear_object (&p->battery_level_action);
  g_clear_object (&p->header_action);
  g_clear_object (&p->actions);

  g_clear_object (&p->conn);

  indicator_power_service_set_device_provider (self, NULL);
  indicator_power_service_set_notifier (self, NULL);

  G_OBJECT_CLASS (indicator_power_service_parent_class)->dispose (o);
}

/***
****  Instantiation
***/

static void
indicator_power_service_init (IndicatorPowerService * self)
{
  priv_t * p;
  int i;

  p = indicator_power_service_get_instance_private(self);
  self->priv = p;

  p->cancellable = g_cancellable_new ();

  p->settings = g_settings_new ("org.ayatana.indicator.power");

  p->brightness = indicator_power_brightness_new();
  g_signal_connect_swapped(p->brightness, "notify::percentage",
                           G_CALLBACK(update_brightness_action_state), self);

  init_gactions (self);

  g_signal_connect_swapped (p->settings, "changed", G_CALLBACK(rebuild_header_now), self);

  for (i=0; i<N_PROFILES; ++i)
    create_menu(self, i);
  p->menus_built = TRUE;

  g_signal_connect_swapped(p->brightness, "notify::auto-brightness-supported",
                           G_CALLBACK(on_auto_brightness_supported_changed), self);

  p->own_id = g_bus_own_name(G_BUS_TYPE_SESSION,
                             BUS_NAME,
                             G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT,
                             on_bus_acquired,
                             NULL,
                             on_name_lost,
                             self,
                             NULL);
}

static void
indicator_power_service_class_init (IndicatorPowerServiceClass * klass)
{
  GObjectClass * object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = my_dispose;
  object_class->get_property = my_get_property;
  object_class->set_property = my_set_property;

  signals[SIGNAL_NAME_LOST] = g_signal_new (
    INDICATOR_POWER_SERVICE_SIGNAL_NAME_LOST,
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    G_STRUCT_OFFSET (IndicatorPowerServiceClass, name_lost),
    NULL, NULL,
    g_cclosure_marshal_VOID__VOID,
    G_TYPE_NONE, 0);

  properties[PROP_0] = NULL;

  properties[PROP_BUS] = g_param_spec_object (
    "bus",
    "Bus",
    "GDBusConnection for exporting menus/actions",
    G_TYPE_OBJECT,
    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  properties[PROP_DEVICE_PROVIDER] = g_param_spec_object (
    "device-provider",
    "Device Provider",
    "Source for power devices",
    G_TYPE_OBJECT,
    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_NOTIFIER] = g_param_spec_object (
    "notifier",
    "Notifier",
    "Notifies user of important battery changes",
    G_TYPE_OBJECT,
    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, LAST_PROP, properties);
}

/***
****  Public API
***/

IndicatorPowerService *
indicator_power_service_new (IndicatorPowerDeviceProvider * device_provider,
                             IndicatorPowerNotifier * notifier)
{
  GObject * o = g_object_new (INDICATOR_TYPE_POWER_SERVICE,
                              "device-provider", device_provider,
                              "notifier", notifier,
                              NULL);

  return INDICATOR_POWER_SERVICE (o);
}

void
indicator_power_service_set_device_provider (IndicatorPowerService * self,
                                             IndicatorPowerDeviceProvider * dp)
{
  priv_t * p;

  g_return_if_fail (INDICATOR_IS_POWER_SERVICE (self));
  g_return_if_fail (!dp || INDICATOR_IS_POWER_DEVICE_PROVIDER (dp));
  p = self->priv;

  if (p->device_provider != NULL)
    {
      g_signal_handlers_disconnect_by_data (p->device_provider, self);

      g_clear_object (&p->device_provider);

      g_clear_object (&p->primary_device);

      g_list_free_full (p->devices, g_object_unref);
      p->devices = NULL;
    }

  if (dp != NULL)
    {
      p->device_provider = g_object_ref (dp);

      g_signal_connect_swapped (p->device_provider, "devices-changed",
                                G_CALLBACK(on_devices_changed), self);

      on_devices_changed (self);
    }
}

void
indicator_power_service_set_notifier (IndicatorPowerService  * self,
                                      IndicatorPowerNotifier * notifier)
{
  priv_t * p;

  g_return_if_fail (INDICATOR_IS_POWER_SERVICE (self));
  g_return_if_fail (!notifier || INDICATOR_IS_POWER_NOTIFIER (notifier));
  p = self->priv;

  if (p->notifier == notifier)
    return;

  g_clear_object (&p->notifier);

  if (notifier != NULL)
    {
      p->notifier = g_object_ref (notifier);
      indicator_power_notifier_set_bus (p->notifier, p->conn);
    }
}


/* If a device has multiple batteries and uses only one of them at a time,
   they should be presented as separate items inside the battery menu,
   but everywhere else they should be aggregated (bug 880881).
   Their percentages should be averaged. If any are discharging,
   the aggregated time remaining should be the maximum of the times
   for all those that are discharging, plus the sum of the times
   for all those that are idle. Otherwise, the aggregated time remaining
   should be the the maximum of the times for all those that are charging. */
static IndicatorPowerDevice *
create_totalled_battery_device (const GList * devices)
{
  const GList * l;
  guint n_charged = 0;
  guint n_charging = 0;
  guint n_discharging = 0;
  guint n_batteries = 0;
  double sum_percent = 0;
  time_t max_discharge_time = 0;
  time_t max_charge_time = 0;
  time_t sum_charged_time = 0;
  IndicatorPowerDevice * device = NULL;

  for (l=devices; l!=NULL; l=l->next)
    {
      const IndicatorPowerDevice * walk = INDICATOR_POWER_DEVICE(l->data);

      if (indicator_power_device_get_kind(walk) == UP_DEVICE_KIND_BATTERY)
        {
          const double percent = indicator_power_device_get_percentage (walk);
          const time_t t = indicator_power_device_get_time (walk);
          const UpDeviceState state = indicator_power_device_get_state (walk);


          if (percent > 0.01)
            {
              sum_percent += percent;
              ++n_batteries;
            }

          if (state == UP_DEVICE_STATE_CHARGING)
            {
              ++n_charging;
              max_charge_time = MAX(max_charge_time, t);
            }
          else if (state == UP_DEVICE_STATE_DISCHARGING)
            {
              ++n_discharging;
              max_discharge_time = MAX(max_discharge_time, t);
            }
          else if (state == UP_DEVICE_STATE_FULLY_CHARGED)
            {
              ++n_charged;
              sum_charged_time += t;
            }
        }
    }

  if (n_batteries > 1)
    {
      const double percent = sum_percent / n_batteries;
      UpDeviceState state;
      time_t time_left;

      if (n_discharging > 0)
        {
          state = UP_DEVICE_STATE_DISCHARGING;
          time_left = max_discharge_time + sum_charged_time;
        }
      else if (n_charging > 0)
        {
          state = UP_DEVICE_STATE_CHARGING;
          time_left = max_charge_time;
        }
      else if (n_charged > 0)
        {
          state = UP_DEVICE_STATE_FULLY_CHARGED;
          time_left = 0;
        }
      else
        {
          state = UP_DEVICE_STATE_UNKNOWN;
          time_left = 0;
        }

      device = indicator_power_device_new (NULL,
                                           UP_DEVICE_KIND_BATTERY,
                                           NULL,
                                           percent,
                                           state,
                                           time_left,
                                           TRUE);
    }

  return device;
}

/**
 * If there are multiple UP_DEVICE_KIND_BATTERY devices in the list,
 * they're merged into a new 'totalled' device representing the sum of them.
 *
 * Returns: (element-type IndicatorPowerDevice)(transfer full): a list of devices
 */
static GList*
merge_batteries_together (GList * devices)
{
  GList * ret;
  IndicatorPowerDevice * merged_device;

  if ((merged_device = create_totalled_battery_device (devices)))
    {
      GList * l;

      ret = g_list_append (NULL, merged_device);

      for (l=devices; l!=NULL; l=l->next)
        if (indicator_power_device_get_kind(INDICATOR_POWER_DEVICE(l->data)) != UP_DEVICE_KIND_BATTERY)
          ret = g_list_append (ret, g_object_ref(l->data));
    }
  else /* not enough batteries to merge */
    {
      ret = g_list_copy (devices);
      g_list_foreach (ret, (GFunc)g_object_ref, NULL);
    }

  return ret;
}

IndicatorPowerDevice *
indicator_power_service_choose_primary_device (GList * devices)
{
  IndicatorPowerDevice * primary = NULL;

  if (devices != NULL)
    {
      GList * tmp = merge_batteries_together (devices);
      tmp = g_list_sort (tmp, device_compare_func);
      primary = g_object_ref (tmp->data);
      g_list_free_full (tmp, (GDestroyNotify)g_object_unref);
    }

  return primary;
}
