"""Drives tests/interactive/main.kai with real X input: run inside Xvfb and a
D-Bus session, as `drive.py <binary> <work dir>`. Prints one line per check
and exits 1 if any failed."""
import json
import os
import pty
import re
import subprocess
import sys
import threading
import time

import gi
gi.require_version("Atspi", "2.0")
from gi.repository import Atspi
from Xlib import X, display

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TITLE = "Motu input"


class App:
    """The program under test; its stdout comes through a pty, so it is line buffered."""

    def __init__(self, binary, env):
        master, slave = pty.openpty()
        self.proc = subprocess.Popen([binary], cwd=ROOT, env=env, stdout=slave, stderr=sys.stderr)
        os.close(slave)
        self.lines = []
        self.lock = threading.Lock()
        threading.Thread(target=self._read, args=(master,), daemon=True).start()

    def _read(self, fd):
        buf = b""
        while True:
            try:
                chunk = os.read(fd, 4096)
            except OSError:
                return
            if not chunk:
                return
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode(errors="replace").rstrip("\r")
                print(f"  app| {text}", flush=True)
                with self.lock:
                    self.lines.append(text)

    def mark(self):
        with self.lock:
            return len(self.lines)

    def expect(self, pattern, since, timeout=15):
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                for line in self.lines[since:]:
                    if re.fullmatch(pattern, line):
                        return line
            time.sleep(0.05)
        return None


FAILED = []


def check(name, ok, detail):
    print(f"{'ok  ' if ok else 'FAIL'} {name}: {detail}", flush=True)
    if not ok:
        FAILED.append(name)
    return ok


def expect_check(app, name, pattern, since, timeout=15):
    line = app.expect(pattern, since, timeout)
    check(name, line is not None, line if line is not None else f"no line matching {pattern!r}")
    return line


# --- X ---

XD = display.Display()
ROOTWIN = XD.screen().root


def xdo(*args):
    subprocess.run(["xdotool", *[str(a) for a in args]], check=True)
    time.sleep(0.15)


def type_path(text):
    """Pastes `text` over the focused entry: typed key by key, the file chooser's
    asynchronous completion inserts stale suffixes."""
    subprocess.run(["xclip", "-selection", "clipboard", "-i"], input=text.encode(), check=True)
    key("ctrl+a")
    key("ctrl+v")
    time.sleep(1)


def key(*keys):
    xdo("key", "--clearmodifiers", *keys)


def click_at(x, y):
    xdo("mousemove", int(x), int(y))
    xdo("click", 1)


def windows_of(pid):
    out = subprocess.run(["xdotool", "search", "--onlyvisible", "--pid", str(pid)],
                         capture_output=True, text=True).stdout
    return {int(w) for w in out.split()}


def window_named(name, timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        out = subprocess.run(["xdotool", "search", "--onlyvisible", "--name", f"^{name}$"],
                             capture_output=True, text=True).stdout.split()
        if out:
            return int(out[0])
        time.sleep(0.1)
    return None


def new_window(pid, before, timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        fresh = windows_of(pid) - before
        if fresh:
            return fresh.pop()
        time.sleep(0.1)
    return None


def origin(wid):
    w = XD.create_resource_object("window", wid)
    t = ROOTWIN.translate_coords(w, 0, 0)
    return t.x, t.y


def popups():
    out = {}
    for w in ROOTWIN.query_tree().children:
        a = w.get_attributes()
        if a.map_state == X.IsViewable and a.override_redirect:
            g = w.get_geometry()
            out[w.id] = (g.x, g.y, g.width, g.height)
    return out


def new_popup(before, timeout=5):
    deadline = time.time() + timeout
    while time.time() < deadline:
        fresh = {k: v for k, v in popups().items() if k not in before}
        if fresh:
            return list(fresh.values())[-1]
        time.sleep(0.05)
    return None


def screenshot(name):
    out = os.environ.get("MOTU_SHOTS")
    if out:
        subprocess.run(["import", "-window", "root", os.path.join(out, f"{name}.png")])


# --- accessibility ---

PRUNE = {"document web", "document frame"}


def walk(acc, depth=0):
    if acc is None or depth > 40:
        return
    yield acc
    try:
        if acc.get_role_name() in PRUNE:
            return
        n = acc.get_child_count()
    except Exception:
        return
    for i in range(n):
        try:
            child = acc.get_child_at_index(i)
        except Exception:
            continue
        yield from walk(child, depth + 1)


def app_acc():
    desktop = Atspi.get_desktop(0)
    for i in range(desktop.get_child_count()):
        app = desktop.get_child_at_index(i)
        if app is None:
            continue
        app.clear_cache()
        for j in range(app.get_child_count()):
            win = app.get_child_at_index(j)
            if win is not None and win.get_name() == TITLE:
                return app
    return None


def find(root, name, roles):
    for acc in walk(root):
        try:
            if acc.get_role_name() in roles and acc.get_name() == name:
                return acc
        except Exception:
            continue
    return None


def popover_items(title):
    """The items under menu bar title `title`, in order: GTK gives them no accessible name."""
    bar_item = find(app_acc(), title, MENU_ITEM_ROLES)
    menu = next((a for a in walk(bar_item) if a.get_role_name() == "menu"), None) if bar_item else None
    return [a for a in walk(menu) if a is not menu and a.get_role_name() in MENU_ITEM_ROLES] if menu else []


def dump(root, depth=0):
    """Prints the accessible tree, for when a lookup fails."""
    if root is None or depth > 30:
        return
    try:
        states = [s.value_nick for s in root.get_state_set().get_states()
                  if s.value_nick in ("checked", "checkable", "showing", "sensitive")]
        print(f"  {'  ' * depth}{root.get_role_name()} {root.get_name()!r} {extents(root)} {states}", flush=True)
        if root.get_role_name() in PRUNE:
            return
        for i in range(root.get_child_count()):
            dump(root.get_child_at_index(i), depth + 1)
    except Exception as e:
        print(f"  {'  ' * depth}error {e!r}", flush=True)


def extents(acc):
    try:
        e = acc.get_extents(Atspi.CoordType.WINDOW)
        return e.x, e.y, e.width, e.height
    except Exception:
        return None


def is_checked(acc):
    acc.clear_cache()
    return acc.get_state_set().contains(Atspi.StateType.CHECKED)


MENU_ITEM_ROLES = {"menu item", "check menu item", "radio menu item"}


# --- the steps ---

class Driver:
    def __init__(self, app, wid):
        self.app = app
        self.wid = wid

    def focus_main(self):
        xdo("windowactivate", "--sync", self.wid)
        time.sleep(0.3)

    def open_menu(self, title):
        """Clicks a menu bar title; answers the popover's rectangle and its items."""
        bar_item = find(app_acc(), title, MENU_ITEM_ROLES)
        e = extents(bar_item) if bar_item else None
        ox, oy = origin(self.wid)
        if not e:
            return None, []
        before = popups()
        click_at(ox + e[0] + e[2] / 2, oy + e[1] + e[3] / 2)
        pop = new_popup(before)
        time.sleep(0.3)
        items = popover_items(title)
        print(f"  {title} menu: popover {pop}, items {[extents(i) for i in items]}", flush=True)
        if pop is None or not items:
            dump(app_acc().get_child_at_index(0))
        return pop, items

    def click_item(self, pop, item):
        """Item extents are relative to the popover's own surface."""
        px, py, _, _ = pop
        x, y, w, h = extents(item)
        click_at(px + x + w / 2, py + y + h / 2)

    def menu_by_mouse(self):
        self.focus_main()
        pop, items = self.open_menu("File")
        if not check("File menu opens on a click", pop is not None and len(items) > 0, f"popover {pop}"):
            return
        check("File menu holds its 6 items and the appended Quit", len(items) == 7, f"{len(items)} items")
        mark = self.app.mark()
        self.click_item(pop, items[0])
        expect_check(self.app, "menu item chosen by mouse", r"menu hello", mark)

    def checkmarks(self):
        self.focus_main()
        pop, items = self.open_menu("View")
        if not check("View menu opens on a click", pop is not None and len(items) == 1, f"popover {pop}"):
            return
        state = is_checked(items[0])
        check("Checked item starts checked", state is True, f"checked={state}")
        mark = self.app.mark()
        self.click_item(pop, items[0])
        expect_check(self.app, "checked item chosen by mouse", r"menu wrap", mark)
        expect_check(self.app, "app unchecks it", r"wrap false", mark)
        self.read_checkmark(False, "checkmark cleared after set_menu_state")
        self.focus_main()
        mark = self.app.mark()
        key("ctrl+l")
        expect_check(self.app, "Ctrl+L chooses the checked item", r"menu wrap", mark)
        expect_check(self.app, "app checks it", r"wrap true", mark)
        self.read_checkmark(True, "checkmark set after set_menu_state")

    def read_checkmark(self, want, name):
        time.sleep(0.3)
        self.focus_main()
        _, items = self.open_menu("View")
        state = is_checked(items[0]) if items else None
        check(name, state is want, f"checked={state}")
        key("Escape")

    def dialog(self, shortcut, title):
        self.focus_main()
        before = windows_of(self.app.proc.pid)
        mark = self.app.mark()
        key(shortcut)
        wid = window_named(title, 10) if title else new_window(self.app.proc.pid, before, 10)
        check(f"{shortcut} opens the {title or 'message'} dialog", wid is not None, f"window {wid}")
        time.sleep(0.8)
        return wid, mark

    def open_dialog(self, path):
        wid, mark = self.dialog("ctrl+o", "Open")
        if wid:
            key("ctrl+l")
            type_path(path)
            screenshot("open-typed")
            key("Return")
            time.sleep(1)
            screenshot("open-entered")
        expect_check(self.app, "open dialog answers the typed path", re.escape(f"open {json.dumps([path])}"), mark)
        wid, mark = self.dialog("ctrl+o", "Open")
        if wid:
            key("Escape")
        expect_check(self.app, "open dialog answers [] on Escape", r"open \[\]", mark)

    def save_dialog(self, path):
        wid, mark = self.dialog("ctrl+s", "Save")
        if wid:
            type_path(path)
            key("Return")
        expect_check(self.app, "save dialog answers the typed path", re.escape(f"save {path}"), mark)
        wid, mark = self.dialog("ctrl+s", "Save")
        if wid:
            key("Escape")
        expect_check(self.app, "save dialog answers None on Escape", r"save none", mark)

    def message_dialog(self):
        wid, mark = self.dialog("ctrl+k", None)
        if wid:
            key("Return")
        expect_check(self.app, "message dialog: Enter answers the default button", r"ask 0", mark)
        wid, mark = self.dialog("ctrl+k", None)
        if wid:
            key("Escape")
        expect_check(self.app, "message dialog: Escape answers -1", r"ask -1", mark)
        wid, mark = self.dialog("ctrl+k", None)
        if wid:
            self.click_button(wid, "No")
        expect_check(self.app, "message dialog: clicking the second button answers 1", r"ask 1", mark)

    def click_button(self, wid, label):
        acc = find(Atspi.get_desktop(0), label, {"push button", "button"})
        e = extents(acc) if acc else None
        ox, oy = origin(wid)
        print(f"  button {label}: extents {e}, dialog origin {(ox, oy)}", flush=True)
        if e and e[2] > 0:
            click_at(ox + e[0] + e[2] / 2, oy + e[1] + e[3] / 2)
        else:
            key("Tab", "space")

    def shell(self, opened_log, fm_log, fm_proc, target):
        self.focus_main()
        mark = self.app.mark()
        key("ctrl+e")
        expect_check(self.app, "Ctrl+E runs shell open", r"shell-open sent", mark)
        wait_log(opened_log, os.path.basename(target), "shell open launches the default application")
        mark = self.app.mark()
        key("ctrl+r")
        expect_check(self.app, "Ctrl+R runs shell reveal", r"reveal sent", mark)
        uri = "file://" + target
        wait_log(fm_log, f'ShowItems ["{uri}"]', "reveal calls FileManager1.ShowItems")
        fm_proc.terminate()
        fm_proc.wait()
        time.sleep(0.5)
        self.focus_main()
        key("ctrl+r")
        wait_log(opened_log, os.path.dirname(target), "reveal without a file manager opens the folder")

    def drop(self, source_wid, target):
        sx, sy = origin(source_wid)
        tx, ty = origin(self.wid)
        mark = self.app.mark()
        xdo("mousemove", sx + 120, sy + 80)
        xdo("mousedown", 1)
        for i in range(1, 21):
            xdo("mousemove", sx + 120 + (tx + 320 - sx - 120) * i // 20, sy + 80 + (ty + 220 - sy - 80) * i // 20)
        time.sleep(0.5)
        xdo("mousemove", tx + 322, ty + 222)
        time.sleep(0.5)
        xdo("mouseup", 1)
        expect_check(self.app, "file dropped on the window", re.escape(f"drop 1 {json.dumps([target])}"), mark)

    def focus_blur(self, source_wid):
        self.focus_main()
        sx, sy = origin(source_wid)
        mark = self.app.mark()
        click_at(sx + 120, sy + 80)
        expect_check(self.app, "clicking another window blurs", r"blur 1", mark)
        tx, ty = origin(self.wid)
        mark = self.app.mark()
        click_at(tx + 320, ty + 220)
        expect_check(self.app, "clicking the window focuses it", r"focus 1", mark)

    def resize(self):
        mark = self.app.mark()
        xdo("windowsize", self.wid, 820, 560)
        expect_check(self.app, "resizing reports resize", r"resize 1", mark)

    def quit_and_close(self):
        self.focus_main()
        mark = self.app.mark()
        key("ctrl+q")
        expect_check(self.app, "Ctrl+Q asks to quit", r"quit-requested", mark)
        self.focus_main()
        pop, items = self.open_menu("File")
        if items:
            mark = self.app.mark()
            self.click_item(pop, items[-1])
            expect_check(self.app, "the appended Quit item, chosen by mouse, asks to quit", r"quit-requested", mark)
        self.focus_main()
        mark = self.app.mark()
        key("alt+F4")
        expect_check(self.app, "Alt+F4 asks to close", r"close-requested 1", mark)
        expect_check(self.app, "closing the last window ends the event loop", r"run returned 0", mark)
        try:
            code = self.app.proc.wait(60)
        except subprocess.TimeoutExpired:
            code = None
        check("the program exits", code == 0, f"exit status {code}")


def wait_log(path, needle, name, timeout=10):
    deadline = time.time() + timeout
    text = ""
    while time.time() < deadline:
        if os.path.exists(path):
            with open(path) as f:
                text = f.read()
            if needle in text:
                break
        time.sleep(0.1)
    last = text.strip().splitlines()[-1] if text.strip() else "nothing logged"
    check(name, needle in text, last)


# --- setup ---

def write(path, text, mode=0o644):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)
    os.chmod(path, mode)


def default_app(work, opened_log):
    """A .desktop file that logs what it is asked to open, default for text and folders."""
    opener = os.path.join(work, "opener.sh")
    write(opener, f'#!/bin/sh\nprintf "%s\\n" "$*" >> {opened_log}\n', 0o755)
    write(os.path.join(work, "data/applications/motu-opener.desktop"),
          f"[Desktop Entry]\nType=Application\nName=Motu opener\nExec={opener} %u\n"
          "MimeType=text/plain;inode/directory;\nNoDisplay=true\n")
    write(os.path.join(work, "config/mimeapps.list"),
          "[Default Applications]\ntext/plain=motu-opener.desktop\ninode/directory=motu-opener.desktop\n")


def start(name, cmd, ready):
    """Starts a helper, waits for its `ready` line and echoes the rest of its output."""
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True)
    line = p.stdout.readline().strip()
    check(f"{name} starts", line == ready, line)

    def echo():
        for rest in p.stdout:
            print(f"  {name}| {rest.rstrip()}", flush=True)
    threading.Thread(target=echo, daemon=True).start()
    return p


def main():
    binary, work = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    os.makedirs(work, exist_ok=True)
    target = os.path.join(work, "files", "a.txt")
    write(target, "hello\n")
    opened_log, fm_log = os.path.join(work, "opened.log"), os.path.join(work, "fm.log")
    default_app(work, opened_log)
    env = dict(os.environ, XDG_DATA_HOME=os.path.join(work, "data"), XDG_CONFIG_HOME=os.path.join(work, "config"),
               MOTU_TEST_FILE=target, GDK_BACKEND="x11")

    wm = subprocess.Popen(["openbox"])
    time.sleep(1)
    fm = start("file manager", [sys.executable, os.path.join(HERE, "file_manager.py"), fm_log], "file manager ready")
    app = App(binary, env)
    expect_check(app, "page loaded and called a command", r"ready", 0, 30)
    wid = window_named(TITLE)
    if not check("window mapped", wid is not None, f"window {wid}"):
        return
    xdo("windowmove", wid, 40, 40)
    drv = Driver(app, wid)
    steps = [
        ("menu", drv.menu_by_mouse),
        ("checkmarks", drv.checkmarks),
        ("open", lambda: drv.open_dialog(target)),
        ("save", lambda: drv.save_dialog(os.path.join(work, "files", "saved.txt"))),
        ("message", drv.message_dialog),
        ("shell", lambda: drv.shell(opened_log, fm_log, fm, target)),
    ]
    run_steps(steps)
    src = start("drag source", [sys.executable, os.path.join(HERE, "drag_source.py"), target], "drag source ready")
    swid = window_named("drag source")
    if check("drag source mapped", swid is not None, f"window {swid}"):
        xdo("windowmove", swid, 900, 100)
        time.sleep(0.5)
        run_steps([("drop", lambda: drv.drop(swid, target)), ("focus", lambda: drv.focus_blur(swid))])
    src.terminate()
    run_steps([("resize", drv.resize), ("quit", drv.quit_and_close)])
    if app.proc.poll() is None:
        app.proc.kill()
    wm.terminate()


def run_steps(steps):
    for name, step in steps:
        print(f"-- {name}", flush=True)
        before = len(FAILED)
        try:
            step()
        except Exception as e:
            check(f"{name} step", False, repr(e))
        if len(FAILED) > before:
            screenshot(name)


if __name__ == "__main__":
    main()
    print(f"{len(FAILED)} failed: {', '.join(FAILED)}" if FAILED else "all checks passed", flush=True)
    sys.exit(1 if FAILED else 0)
