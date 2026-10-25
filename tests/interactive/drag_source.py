"""A GTK 4 window whose label drags argv[1] as text/uri-list, as a file manager does."""
import sys

import gi
gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gdk, Gio, GLib, Gtk

URI = Gio.File.new_for_path(sys.argv[1]).get_uri()


def prepare(source, x, y):
    print(f"drag prepare {URI}", flush=True)
    return Gdk.ContentProvider.new_for_bytes("text/uri-list", GLib.Bytes.new(f"{URI}\r\n".encode()))


def ended(source, drag, delete):
    print("drag end", flush=True)


def cancelled(source, drag, reason):
    print(f"drag cancel {reason.value_nick}", flush=True)
    return False


Gtk.init()
win = Gtk.Window(title="drag source")
win.set_default_size(240, 160)
label = Gtk.Label(label="drag me")
label.set_hexpand(True)
label.set_vexpand(True)
src = Gtk.DragSource(actions=Gdk.DragAction.COPY)
src.connect("prepare", prepare)
src.connect("drag-end", ended)
src.connect("drag-cancel", cancelled)
label.add_controller(src)
win.set_child(label)
win.connect("close-request", lambda w: sys.exit(0))
win.present()
print("drag source ready", flush=True)
GLib.MainLoop().run()
