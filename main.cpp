#include <xcb/xcb.h>
#include <xcb/randr.h>

#include <gio/gio.h>
#include <glib-unix.h>

#include <algorithm>
#include <string>

using namespace std;

static GDBusConnection *g_dbus = nullptr;

static xcb_connection_t *g_conn = nullptr;
static xcb_window_t g_root = 0;

static GSettings *g_setsMatePower = nullptr;
static GSettings *g_setsMateScreenSaver = nullptr;

static bool isLidClosedOnBattery(bool &hasLidOut)
{
    bool onBattery = false;
    bool hasLid = false;
    bool isLidClosed = false;
    auto ret = g_dbus_connection_call_sync(
        g_dbus,
        "org.freedesktop.UPower",
        "/org/freedesktop/UPower",
        "org.freedesktop.DBus.Properties",
        "GetAll",
        g_variant_new("(s)", "org.freedesktop.UPower"),
        G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        nullptr
    );
    if (ret)
    {
        GVariant *arr = nullptr;
        g_variant_get(ret, "(@a{sv})", &arr);
        if (arr)
        {
            GVariantIter iter;
            gchar *key = nullptr;
            GVariant *value = nullptr;
            g_variant_iter_init(&iter, arr);
            while (g_variant_iter_loop(&iter, "{sv}", &key, &value))
            {
                if (g_strcmp0(key, "OnBattery") == 0)
                {
                    onBattery = g_variant_get_boolean(value);
                }
                else if (g_strcmp0(key, "LidIsPresent") == 0)
                {
                    hasLid = g_variant_get_boolean(value);
                }
                else if (g_strcmp0(key, "LidIsClosed") == 0)
                {
                    isLidClosed = g_variant_get_boolean(value);
                }
            }
            g_variant_unref(arr);
        }
        g_variant_unref(ret);
    }
    hasLidOut = hasLid;
    return onBattery && hasLid && isLidClosed;
}

static void processDisplays(bool *hasLidOut = nullptr)
{
    bool hasLid = false;
    const bool lidClosedOnBattery = isLidClosedOnBattery(hasLid);
    if (hasLidOut)
        *hasLidOut = hasLid;
    if (!hasLid)
        return;

    if (auto reply = xcb_randr_get_screen_resources_reply(g_conn, xcb_randr_get_screen_resources(g_conn, g_root), nullptr))
    {
        bool hasLaptopDisplay = false;
        uint32_t nExternalDisplays = 0;
        const int nOutputs = xcb_randr_get_screen_resources_outputs_length(reply);
        const auto outputs = xcb_randr_get_screen_resources_outputs(reply);
        for (int i = 0; i < nOutputs; ++i)
        {
            if (auto reply2 = xcb_randr_get_output_info_reply(g_conn, xcb_randr_get_output_info(g_conn, outputs[i], reply->timestamp), nullptr))
            {
                if (reply2->connection == 0)
                {
                    const int nameLen = xcb_randr_get_output_info_name_length(reply2);
                    const auto nameRaw = reinterpret_cast<const char *>(xcb_randr_get_output_info_name(reply2));
                    string name(nameRaw, nameLen);
                    transform(name.begin(), name.end(), name.begin(), [](char c) {
                        return tolower(c);
                    });
                    if (name.find("edp") != string::npos || name.find("lvds") != string::npos)
                        hasLaptopDisplay = true;
                    else
                        ++nExternalDisplays;
                }
                free(reply2);
            }
        }
        free(reply);

        if (hasLaptopDisplay)
        {
            const auto newValue = (nExternalDisplays > 0)
                ? "nothing"
                : "suspend"
            ;
            if (auto value = g_settings_get_string(g_setsMatePower, "button-lid-battery"))
            {
                if (g_strcmp0(value, newValue) != 0)
                {
                    g_settings_set_string(g_setsMatePower, "button-lid-battery", newValue);
                    if (lidClosedOnBattery && nExternalDisplays == 0)
                    {
                        if (g_settings_get_boolean(g_setsMateScreenSaver, "lock-enabled"))
                        {
                            g_variant_unref(g_dbus_connection_call_sync(
                                g_dbus,
                                "org.freedesktop.login1",
                                "/org/freedesktop/login1",
                                "org.freedesktop.login1.Manager",
                                "LockSession",
                                g_variant_new("(s)", ""),
                                nullptr,
                                G_DBUS_CALL_FLAGS_NONE,
                                -1,
                                nullptr,
                                nullptr
                            ));
                        }
                        g_variant_unref(g_dbus_connection_call_sync(
                            g_dbus,
                            "org.freedesktop.login1",
                            "/org/freedesktop/login1",
                            "org.freedesktop.login1.Manager",
                            "Suspend",
                            g_variant_new("(b)", false),
                            nullptr,
                            G_DBUS_CALL_FLAGS_NONE,
                            -1,
                            nullptr,
                            nullptr
                        ));
                    }
                }
                free(value);
            }
        }
    }
}

static gboolean processXcbEvents(gint fd, GIOCondition condition, gpointer)
{
    auto e = xcb_poll_for_event(g_conn);

    if (!e)
    {
        if (xcb_connection_has_error(g_conn))
            return false;
        return true;
    }

    processDisplays();

    free(e);

    return true;
}

int main()
{
    g_dbus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
    if (!g_dbus)
        return -1;

    g_conn = xcb_connect(nullptr, nullptr);
    if (!g_conn)
    {
        g_dbus_connection_close_sync(g_dbus, nullptr, nullptr);
        return -1;
    }

    auto xcbSource = g_unix_fd_add(xcb_get_file_descriptor(g_conn), G_IO_IN, processXcbEvents, nullptr);

    if (auto reply = xcb_get_extension_data(g_conn, &xcb_randr_id); reply && reply->present)
    {
        g_root = xcb_setup_roots_iterator(xcb_get_setup(g_conn)).data->root;

        xcb_randr_query_version_unchecked(g_conn, XCB_RANDR_MAJOR_VERSION, XCB_RANDR_MINOR_VERSION);
        xcb_randr_select_input(
            g_conn,
            g_root,
            XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE | XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE | XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE | XCB_RANDR_NOTIFY_MASK_OUTPUT_PROPERTY
        );
    }

    g_setsMatePower = g_settings_new_with_path("org.mate.power-manager", "/org/mate/power-manager/");
    g_setsMateScreenSaver = g_settings_new_with_path("org.mate.screensaver", "/org/mate/screensaver/");

    if (g_root && g_setsMatePower && g_setsMateScreenSaver)
    {
        bool hasLid = false;
        processDisplays(&hasLid);
        if (hasLid)
        {
            auto mainLoop = g_main_loop_new(nullptr, false);
            g_main_loop_run(mainLoop);
            g_object_unref(mainLoop);
        }
    }

    g_object_unref(g_setsMatePower);
    g_object_unref(g_setsMateScreenSaver);

    g_source_remove(xcbSource);
    xcb_disconnect(g_conn);

    g_dbus_connection_close_sync(g_dbus, nullptr, nullptr);

    return 0;
}
