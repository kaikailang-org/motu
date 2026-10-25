"""A stand-in for org.freedesktop.FileManager1: logs each call to argv[1]."""
import json
import sys

import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

XML = """<node><interface name="org.freedesktop.FileManager1">
<method name="ShowItems"><arg type="as" direction="in"/><arg type="s" direction="in"/></method>
<method name="ShowFolders"><arg type="as" direction="in"/><arg type="s" direction="in"/></method>
</interface></node>"""
LOG = sys.argv[1]
IFACE = Gio.DBusNodeInfo.new_for_xml(XML).interfaces[0]


def on_call(conn, sender, path, iface, method, params, invocation):
    with open(LOG, "a") as f:
        f.write(f"{method} {json.dumps(params.unpack()[0])}\n")
    invocation.return_value(None)


def on_bus(conn, name):
    conn.register_object("/org/freedesktop/FileManager1", IFACE, on_call, None, None)


def on_name(conn, name):
    print("file manager ready", flush=True)


Gio.bus_own_name(Gio.BusType.SESSION, "org.freedesktop.FileManager1", Gio.BusNameOwnerFlags.NONE,
                 on_bus, on_name, None)
GLib.MainLoop().run()
