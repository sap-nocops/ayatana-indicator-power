/*
 * Copyright 2014-2016 Canonical Ltd.
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
 *   Robert Tari <robert@tari.in>
 */

#ifndef LOMIRI_SOUNDSDIR
    #define LOMIRI_SOUNDSDIR ""
#endif

#include "glib-fixture.h"

#include "dbus-shared.h"
#include "device.h"
#include "notifier.h"

#include <gtest/gtest.h>

#include <libdbustest/dbus-test.h>

#include <libnotify/notify.h>

#include <glib.h>
#include <gio/gio.h>

/***
****
***/

class NotifyFixture: public GlibFixture
{
private:

  typedef GlibFixture super;

  static constexpr char const * NOTIFY_BUSNAME   {"org.freedesktop.Notifications"};
  static constexpr char const * NOTIFY_INTERFACE {"org.freedesktop.Notifications"};
  static constexpr char const * NOTIFY_PATH      {"/org/freedesktop/Notifications"};

protected:

  DbusTestService * service = nullptr;
  DbusTestDbusMock * mock = nullptr;
  DbusTestDbusMockObject * obj = nullptr;
  GDBusConnection * bus = nullptr;

  static constexpr int FIRST_NOTIFY_ID {1234};

  static constexpr int NOTIFICATION_CLOSED_EXPIRED   {1};
  static constexpr int NOTIFICATION_CLOSED_DISMISSED {2};
  static constexpr int NOTIFICATION_CLOSED_API       {3};
  static constexpr int NOTIFICATION_CLOSED_UNDEFINED {4};

  static constexpr char const * METHOD_CLOSE {"CloseNotification"};
  static constexpr char const * METHOD_NOTIFY {"Notify"};
  static constexpr char const * METHOD_GET_CAPS {"GetCapabilities"};
  static constexpr char const * METHOD_GET_INFO {"GetServerInformation"};
  static constexpr char const * SIGNAL_CLOSED {"NotificationClosed"};

  static constexpr char const * HINT_TIMEOUT {"x-lomiri-snap-decisions-timeout"};

protected:

  void SetUp()
  {
    super::SetUp();

    // init DBusMock / dbus-test-runner

    service = dbus_test_service_new(nullptr);

    GError * error = nullptr;
    mock = dbus_test_dbus_mock_new(NOTIFY_BUSNAME);
    obj = dbus_test_dbus_mock_get_object(mock,
                                         NOTIFY_PATH,
                                         NOTIFY_INTERFACE,
                                         &error);
    g_assert_no_error (error);

    // METHOD_GET_INFO
    dbus_test_dbus_mock_object_add_method(mock, obj, METHOD_GET_INFO,
                                          nullptr,
                                          G_VARIANT_TYPE("(ssss)"),
                                          "ret = ('mock-notify', 'test vendor', '1.0', '1.1')",
                                          &error);
    g_assert_no_error (error);

    // METHOD_NOTIFY
    auto str = g_strdup_printf("try:\n"
                               "  self.NextNotifyId\n"
                               "except AttributeError:\n"
                               "  self.NextNotifyId = %d\n"
                               "ret = self.NextNotifyId\n"
                               "self.NextNotifyId += 1\n",
                               FIRST_NOTIFY_ID);
    dbus_test_dbus_mock_object_add_method(mock, obj, METHOD_NOTIFY,
                                          G_VARIANT_TYPE("(susssasa{sv}i)"),
                                          G_VARIANT_TYPE_UINT32,
                                          str,
                                          &error);
    g_assert_no_error (error);
    g_free (str);

    // METHOD_CLOSE
    str = g_strdup_printf("self.EmitSignal('%s', '%s', 'uu', [ args[0], %d ])",
                          NOTIFY_INTERFACE,
                          SIGNAL_CLOSED,
                          NOTIFICATION_CLOSED_API);
    dbus_test_dbus_mock_object_add_method(mock, obj, METHOD_CLOSE,
                                          G_VARIANT_TYPE("(u)"),
                                          nullptr,
                                          str,
                                          &error);
    g_assert_no_error (error);
    g_free (str);

    dbus_test_service_add_task(service, DBUS_TEST_TASK(mock));
    dbus_test_service_start_tasks(service);

    bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    g_dbus_connection_set_exit_on_close(bus, FALSE);
    g_object_add_weak_pointer(G_OBJECT(bus), reinterpret_cast<gpointer*>(&bus));

    notify_init(SERVICE_EXEC);
  }

  virtual void TearDown()
  {
    notify_uninit();

    g_clear_object(&mock);
    g_clear_object(&service);
    g_object_unref(bus);

    // wait a little while for the scaffolding to shut down,
    // but don't block on it forever...
    unsigned int cleartry = 0;
    while ((bus != nullptr) && (cleartry < 50))
      {
        g_usleep(100000);
        while (g_main_context_pending(NULL))
          g_main_context_iteration(NULL, true);
        cleartry++;
      }

    super::TearDown();
  }

  /***
  ****
  ***/

  int get_notify_call_count() const
  {
    guint len {0u};
    GError* error {nullptr};
    dbus_test_dbus_mock_object_get_method_calls(mock, obj, METHOD_NOTIFY, &len, &error);
    g_assert_no_error(error);
    return len;
  }

  std::string get_notify_call_sound_file(int call_number)
  {
    std::string ret;

    guint len {0u};
    GError* error {nullptr};
    auto calls = dbus_test_dbus_mock_object_get_method_calls(mock, obj, METHOD_NOTIFY, &len, &error);
    g_return_val_if_fail(int(len) > call_number, ret);
    g_assert_no_error(error);

    constexpr int HINTS_PARAM_POSITION {6};
    const auto& call = calls[call_number];
    g_return_val_if_fail(g_variant_n_children(call.params) > HINTS_PARAM_POSITION, ret);
    auto hints = g_variant_get_child_value(call.params, HINTS_PARAM_POSITION);
    const gchar* sound_file = nullptr;
    auto success = g_variant_lookup(hints, "sound-file", "&s", &sound_file);
    g_return_val_if_fail(success, ret);
    if (sound_file != nullptr)
      ret = sound_file;
    g_clear_pointer(&hints, g_variant_unref);

    return ret;
  }

  void clear_method_calls()
  {
    GError* error{nullptr};
    ASSERT_TRUE(dbus_test_dbus_mock_object_clear_method_calls(mock, obj, &error));
    g_assert_no_error(error);
  }
};

/***
****
***/

// simple test to confirm the NotifyFixture plumbing all works
TEST_F(NotifyFixture, HelloWorld)
{
}

/***
****
***/


namespace
{
  static constexpr double percent_critical {2.0};
  static constexpr double percent_very_low {5.0};
  static constexpr double percent_low {10.0};

  void set_battery_percentage (IndicatorPowerDevice * battery, gdouble p)
  {
    g_object_set (battery, INDICATOR_POWER_DEVICE_PERCENTAGE, p, nullptr);
  }
}

TEST_F(NotifyFixture, PercentageToLevel)
{
  auto battery = indicator_power_device_new ("/object/path",
                                             UP_DEVICE_KIND_BATTERY,
                                             "Some Model",
                                             50.0,
                                             UP_DEVICE_STATE_DISCHARGING,
                                             30,
                                             TRUE);

  // confirm that the power levels trigger at the right percentages
  for (int i=100; i>=0; --i)
    {
      set_battery_percentage (battery, i);
      const auto level = indicator_power_notifier_get_power_level(battery);

       if (i <= percent_critical)
         EXPECT_STREQ (POWER_LEVEL_STR_CRITICAL, level);
       else if (i <= percent_very_low)
         EXPECT_STREQ (POWER_LEVEL_STR_VERY_LOW, level);
       else if (i <= percent_low)
         EXPECT_STREQ (POWER_LEVEL_STR_LOW, level);
       else
         EXPECT_STREQ (POWER_LEVEL_STR_OK, level);
     }

  g_object_unref (battery);
}

/***
****
***/

// scaffolding to monitor PropertyChanged signals
namespace
{
  enum
  {
    FIELD_POWER_LEVEL = (1<<0),
    FIELD_IS_WARNING  = (1<<1)
  };

  struct ChangedParams
  {
    std::string power_level = POWER_LEVEL_STR_OK;
    bool is_warning = false;
    uint32_t fields = 0;
  };

  void on_battery_property_changed (GDBusConnection *connection G_GNUC_UNUSED,
                                    const gchar *sender_name G_GNUC_UNUSED,
                                    const gchar *object_path G_GNUC_UNUSED,
                                    const gchar *interface_name G_GNUC_UNUSED,
                                    const gchar *signal_name G_GNUC_UNUSED,
                                    GVariant *parameters,
                                    gpointer gchanged_params)
  {
    g_return_if_fail (g_variant_n_children (parameters) == 3);
    auto dict = g_variant_get_child_value (parameters, 1);
    g_return_if_fail (g_variant_is_of_type (dict, G_VARIANT_TYPE_DICTIONARY));
    auto changed_params = static_cast<ChangedParams*>(gchanged_params);

    const char * power_level;
    if (g_variant_lookup (dict, "PowerLevel", "&s", &power_level, nullptr))
    {
      changed_params->power_level = power_level;
      changed_params->fields |= FIELD_POWER_LEVEL;
    }

    gboolean is_warning;
    if (g_variant_lookup (dict, "IsWarning", "b", &is_warning, nullptr))
    {
      changed_params->is_warning = is_warning;
      changed_params->fields |= FIELD_IS_WARNING;
    }

    g_variant_unref (dict);
  }
}

TEST_F(NotifyFixture, LevelsDuringBatteryDrain)
{
  auto battery = indicator_power_device_new ("/object/path",
                                             UP_DEVICE_KIND_BATTERY,
                                             "Some Model",
                                             50.0,
                                             UP_DEVICE_STATE_DISCHARGING,
                                             30,
                                             TRUE);

  // set up a notifier and give it the battery so changing the battery's
  // charge should show up on the bus.
  auto notifier = indicator_power_notifier_new ();
  indicator_power_notifier_set_battery (notifier, battery);
  indicator_power_notifier_set_bus (notifier, bus);
  wait_msec();

  ChangedParams changed_params;
  auto sub_tag = g_dbus_connection_signal_subscribe (bus,
                                                     nullptr,
                                                     "org.freedesktop.DBus.Properties",
                                                     "PropertiesChanged",
                                                     BUS_PATH"/Battery",
                                                     nullptr,
                                                     G_DBUS_SIGNAL_FLAGS_NONE,
                                                     on_battery_property_changed,
                                                     &changed_params,
                                                     nullptr);

  // confirm that draining the battery puts
  // the power_level change through its paces
  for (int i=100; i>=0; --i)
    {
      changed_params = ChangedParams();
      EXPECT_TRUE (changed_params.fields == 0);

      const auto old_level = indicator_power_notifier_get_power_level(battery);
      set_battery_percentage (battery, i);
      const auto new_level = indicator_power_notifier_get_power_level(battery);
      wait_msec();

      if (old_level == new_level)
        {
          EXPECT_EQ (0, (changed_params.fields & FIELD_POWER_LEVEL));
        }
      else
        {
          EXPECT_EQ (FIELD_POWER_LEVEL, (changed_params.fields & FIELD_POWER_LEVEL));
          EXPECT_EQ (new_level, changed_params.power_level);
        }
    }

  // cleanup
  g_dbus_connection_signal_unsubscribe (bus, sub_tag);
  g_object_unref (battery);
  g_object_unref (notifier);
}

/***
****
***/

TEST_F(NotifyFixture, EventsThatChangeNotifications)
{
  // GetCapabilities returns an array containing 'actions', so that we'll
  // get snap decisions and the 'IsWarning' property
  GError * error = nullptr;
  dbus_test_dbus_mock_object_add_method (mock,
                                         obj,
                                         METHOD_GET_CAPS,
                                         nullptr,
                                         G_VARIANT_TYPE_STRING_ARRAY,
                                         "ret = ['actions', 'body']",
                                         &error);
  g_assert_no_error (error);

  auto battery = indicator_power_device_new ("/object/path",
                                             UP_DEVICE_KIND_BATTERY,
                                             "Some Model",
                                             percent_low + 1.0,
                                             UP_DEVICE_STATE_DISCHARGING,
                                             30,
                                             TRUE);

  // the file we expect to play on a low battery notification...
  const char* expected_file = LOMIRI_SOUNDSDIR "/notifications/" LOW_BATTERY_SOUND;
  char* tmp = g_filename_to_uri(expected_file, nullptr, nullptr);
  const std::string low_power_uri {tmp};
  g_clear_pointer(&tmp, g_free);

  // set up a notifier and give it the battery so changing the battery's
  // charge should show up on the bus.
  auto notifier = indicator_power_notifier_new ();
  indicator_power_notifier_set_battery (notifier, battery);
  indicator_power_notifier_set_bus (notifier, bus);
  ChangedParams changed_params;
  auto sub_tag = g_dbus_connection_signal_subscribe (bus,
                                                     nullptr,
                                                     "org.freedesktop.DBus.Properties",
                                                     "PropertiesChanged",
                                                     BUS_PATH"/Battery",
                                                     nullptr,
                                                     G_DBUS_SIGNAL_FLAGS_NONE,
                                                     on_battery_property_changed,
                                                     &changed_params,
                                                     nullptr);

  // test setup case
  wait_msec();
  EXPECT_STREQ (POWER_LEVEL_STR_OK, changed_params.power_level.c_str());

  // change the percent past the 'low' threshold and confirm that
  // a) the power level changes
  // b) we get a notification
  changed_params = ChangedParams();
  set_battery_percentage (battery, percent_low);
  wait_msec();
  EXPECT_EQ (FIELD_POWER_LEVEL|FIELD_IS_WARNING, changed_params.fields);
  EXPECT_EQ (indicator_power_notifier_get_power_level(battery), changed_params.power_level);
  EXPECT_TRUE (changed_params.is_warning);
  EXPECT_EQ (1, get_notify_call_count());
  EXPECT_EQ (low_power_uri, get_notify_call_sound_file(0));
  clear_method_calls();

  // now test that the warning changes if the level goes down even lower...
  changed_params = ChangedParams();
  set_battery_percentage (battery, percent_very_low);
  wait_msec();
  EXPECT_EQ (FIELD_POWER_LEVEL, changed_params.fields);
  EXPECT_STREQ (POWER_LEVEL_STR_VERY_LOW, changed_params.power_level.c_str());
  EXPECT_EQ (1, get_notify_call_count());
  EXPECT_EQ (low_power_uri, get_notify_call_sound_file(0));
  clear_method_calls();

  // ...and that the warning is taken down if the battery is plugged back in...
  changed_params = ChangedParams();
  g_object_set (battery, INDICATOR_POWER_DEVICE_STATE, UP_DEVICE_STATE_CHARGING, nullptr);
  wait_msec();
  EXPECT_EQ (FIELD_IS_WARNING, changed_params.fields);
  EXPECT_FALSE (changed_params.is_warning);
  EXPECT_EQ (0, get_notify_call_count());

  // ...and that it comes back if we unplug again...
  changed_params = ChangedParams();
  g_object_set (battery, INDICATOR_POWER_DEVICE_STATE, UP_DEVICE_STATE_DISCHARGING, nullptr);
  wait_msec();
  EXPECT_EQ (FIELD_IS_WARNING, changed_params.fields);
  EXPECT_TRUE (changed_params.is_warning);
  EXPECT_EQ (1, get_notify_call_count());
  EXPECT_EQ (low_power_uri, get_notify_call_sound_file(0));
  clear_method_calls();

  // ...and that it's taken down if the power level is OK
  changed_params = ChangedParams();
  set_battery_percentage (battery, percent_low+1);
  wait_msec();
  EXPECT_EQ (FIELD_POWER_LEVEL|FIELD_IS_WARNING, changed_params.fields);
  EXPECT_STREQ (POWER_LEVEL_STR_OK, changed_params.power_level.c_str());
  EXPECT_FALSE (changed_params.is_warning);
  EXPECT_EQ (0, get_notify_call_count());

  // cleanup
  g_dbus_connection_signal_unsubscribe (bus, sub_tag);
  g_object_unref (notifier);
  g_object_unref (battery);
}
