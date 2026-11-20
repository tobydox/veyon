# Veyon PipeWire VNC Server Plugin

Wayland VNC server backend for the Veyon Server using
**PipeWire** and the **XDG Desktop Portal RemoteDesktop** API.

## Overview

Under Wayland, X11-based screen capture (e.g., x11vnc) is not available.
This plugin uses the standard `org.freedesktop.portal.RemoteDesktop` D-Bus
interface to request screen content and input device access via
[xdg-desktop-portal](https://github.com/flatpak/xdg-desktop-portal),
then receives the video stream through a PipeWire remote and feeds it into
LibVNCServer so that VNC clients can connect.

### Portal flow

```
CreateSession  →  SelectDevices  →  SelectSources  →  Start  →  OpenPipeWireRemote
```

All steps are asynchronous via the portal `Response` signal.
If the stream ends (e.g., compositor restart) or the portal closes the session
(`Session.Closed`, e.g. screen lock or the user stopping screen sharing), the
session is automatically restarted after a short delay (2 s after the end of
the stream, 5 s after a failure or a closed session), on GNOME not while the
screen is locked, see [Remembering the approval](#remembering-the-approval-restore-token).

### Process layout: `veyon-capture-helper`

`veyon-server` makes itself non-dumpable (`PR_SET_DUMPABLE 0`) so that other
processes of the session user cannot read its memory (IPC keys, host key).
xdg-desktop-portal rejects such callers (`Unable to open /proc/<pid>/root`), so
every portal call and the PipeWire stream run in the separate executable
`veyon-capture-helper` (`PipeWireCaptureServer`, `PortalSession`,
`PipeWireFramebuffer`):

* `veyon-server` (this plugin, `CaptureHelperProcess`) starts the helper as its
  direct child, i.e. as the session user in the same systemd scope/cgroup, so
  the portal derives the same app ID as for the server.
* The only data passed is one stdin line `VEYONCAP1 <hex64> <socketName>`: a
  random 32-byte VNC password generated per server start and the name of the
  helper's socket `capture-<sessionId>-<random>.sock`. The helper never
  receives the IPC token, the host key, logon credentials or session data.
* The helper listens on no TCP/UDP port at all (so other local users can't
  reach it, and there are no WebSockets): it serves RFB on a Unix domain socket
  in the private directory `$XDG_RUNTIME_DIR/veyon` (mode 0700, socket 0600)
  and hands only connections of its own uid (`SO_PEERCRED`) to LibVNCServer, at
  most 16 at a time, each blocking it for at most 2 s.
* `veyon-server` connects only if `SO_PEERCRED` of the listener is its current
  helper process (the demo server worker: a child of the server) and never
  without VNC authentication. VNC authentication uses only the first 8 password
  bytes (56 bits) and is defence in depth.
* The helper refuses to run as root, stays dumpable (only `RLIMIT_CORE` 0),
  loads no plugins besides the platform plugin, has no service IPC and dies
  with the server (`PR_SET_PDEATHSIG`); when it exits, the server restarts it
  with an increasing delay (1 s … 30 s).
* Residual risk: the session user can kill, trace or replace the helper (or
  take its socket path while it restarts, which the server rejects) and thus
  show fake screen content or drop input of their own session; they cannot read
  the server's memory.

## Dependencies

| Dependency | Debian package |
|---|---|
| PipeWire ≥ 0.3 | `libpipewire-0.3-dev` |
| xdg-desktop-portal | `xdg-desktop-portal` |
| xdg-desktop-portal-kde (KDE) | `xdg-desktop-portal-kde` |
| LibVNCServer ≥ 0.9.8 | `libvncserver-dev` |
| Qt5 DBus | `qtbase5-dev` (included) |

## Application identifier

The plugin registers itself under the stable application identifier:

```
io.veyon.veyon-server
```

This identifier is used by the XDG Desktop Portal permission store.
A consistent, non-Flatpak identifier is needed so that permissions granted by
an administrator survive service restarts.

xdg-desktop-portal derives the app ID of a host application from the process
behind the calling D-Bus connection: from its systemd unit or scope
(`app-io.veyon.veyon\x2dserver-<random>.scope`, see `LinuxServerProcess`) or,
with xdg-desktop-portal ≥ 1.19.4, from a registration via
`org.freedesktop.host.portal.Registry.Register()` (exported on
`/org/freedesktop/portal/desktop`). Well-known bus names are not used, so
`veyon-capture-helper` does not register one, and there is no D-Bus activation
file (`veyon-server` must only be started by `veyon-service`).

The portal accepts the registration only once per connection and only before
any other portal call on it, since it derives the app ID with the first call.
`PortalSession` therefore uses a session bus connection of its own and calls
`Register("io.veyon.veyon-server", {})` on it first. The shared connection of
the process is left to Qt: Qt ≥ 6.10 registers
`QGuiApplication::desktopFileName()` (set to `io.veyon.veyon-server` before
constructing `QGuiApplication`) on it by itself, and older Qt versions may call
e.g. the Settings portal on it before. If the registration is refused because
the connection is registered already, a warning is logged, since only a portal
call before the registration can cause that on this connection; portals
without the registry keep using the systemd scope. When the portal restarts
(e.g. after an update), the registration is repeated before the next session.
If the portal doesn't answer, no session is started (another call would make
the portal derive the app ID itself); the start is retried after 5 s.
Restore tokens are stored per app ID, so a changed app ID silently
invalidates them.

The **XDG `.desktop` file** `io.veyon.veyon-server.desktop` is installed to
`/usr/share/applications/`. The portal reads `Name=Veyon Server` from it to
display "Veyon Server" in the permission dialog.

## Remembering the approval (restore token)

The RemoteDesktop portal can remember the approval of the consent dialog: with
`persist_mode` 2 the `Start` response contains a `restore_token`, and passing
it to `SelectDevices` the next time starts the session without the dialog. On
GNOME, which has no pre-authorization like KDE's `kde-authorized` table, this
is the only supported way to avoid a dialog per user and login.

* **Availability:** `persist_mode` and `restore_token` are sent only if the
  `version` property of `org.freedesktop.portal.RemoteDesktop` is 2 or newer
  (xdg-desktop-portal ≥ 1.17; with xdg-desktop-portal-gnome ≥ 45, i.e.
  GNOME 45 and newer, e.g. Ubuntu 24.04). Otherwise (e.g. Ubuntu 22.04 /
  GNOME 42) it is logged once at info level that approvals can't be
  remembered, and the dialog appears on every start. If the version can't
  be read (e.g. the portal is still starting at login), the start fails
  without touching the token and is retried after 5 s, so that the stored
  approval isn't bypassed; after three attempts the session is started
  without persistence.
* **Configuration:** *Remember the screen sharing approval across restarts*
  (`PipeWireVnc/PersistRestoreToken`, advanced view) is enabled by default
  since 4.12; a configuration with an explicit `false` keeps it disabled.
  Disabled, the token is kept in memory only (restarts of the session within
  one server run don't ask again) and a stored token file is removed.
* **Storage:** `$XDG_STATE_HOME/veyon/portal-restore-token`
  (`~/.local/state/veyon/portal-restore-token` by default) with mode 0600 in
  the directory `veyon` with mode 0700, which is created that way or
  restricted if it already exists and is owned by the user. A directory not
  owned by the user, a token file that is a symbolic link, not a regular file
  or not owned by the user is not used. The file is replaced atomically. A
  token in the location used by builds with Qt < 6.7
  (`~/.local/share/veyon/portal-restore-token`) is moved once and the old file
  removed.
* **Shared home directories:** the token file is per home directory, the
  approval it names per computer (the portal's permission store and the
  monitor stored with it). With one account whose home (e.g. on NFS) is used
  on several computers, a login on another computer finds a token for a
  monitor that isn't there, takes the restore-without-stream path below and
  asks again, and the computers overwrite each other's token. That is no worse
  than without persistence, but persistence doesn't help such setups.
* **Token lifecycle:** xdg-desktop-portal deletes its entry for a token when
  `SelectDevices` receives it and stores it again only if `Start` succeeds.
  The token returned by `Start` therefore always replaces the stored one, and
  a failed attempt with a token (or a `Start` without a new token) drops it in
  memory and on disk, so the dialog appears with the next start.
* **Restore without a stream:** if a monitor stored with the approval is gone,
  xdg-desktop-portal-gnome restores the session without any stream instead of
  asking again. The token is then dropped, the session closed and started once
  more without the token, so that the dialog appears. That retry carries no
  token, so it can't repeat.
* **Closed session and screen lock:** GNOME closes all screen sharing sessions
  when the screen is locked (and refuses new ones until it is unlocked), and
  the user can stop sharing via the indicator. The capture helper restarts the
  session after 5 s with the stored token, i.e. without a dialog, and logs
  that. As an attempt while locked would use up the token, the helper asks
  gnome-shell (`org.gnome.ScreenSaver.GetActive`) whether the screen is
  locked before starting the session and checks again every 5 s until it is
  unlocked. This applies only if mutter is the compositor
  (`org.gnome.Mutter.RemoteDesktop` on the session bus): KWin and others keep
  sharing while locked, so there the session is started right away and a
  locked computer stays visible in Veyon Master. No service is started for
  the check.
* **Residual risk:** any program running as the same user can read the token
  and start a screen sharing session without a dialog. This is accepted: the
  portal's own permission store (`~/.local/share/flatpak/db/remote-desktop`,
  `org.freedesktop.impl.portal.PermissionStore`) is equally readable and
  writable by such programs, so the token file adds no new capability for
  them. Other users can't read it.

## Pre-authorization for KDE Plasma 6.3+

By default the portal will display a consent dialog the first time the Veyon
Server requests screen access.  In a classroom or kiosk scenario this dialog
must be suppressed so that the server daemon can start without user
interaction.

KDE Plasma 6.3+ supports permanent pre-authorization via the portal permission
store table `kde-authorized`.

### Method 1: `flatpak permission-set` (recommended)

```bash
flatpak permission-set kde-authorized screencast io.veyon.veyon-server yes
```

This command must be run **as the target user** (the user whose desktop is to
be captured), not as root.

### Method 2: D-Bus PermissionStore

If the `flatpak` CLI is not installed, you can use `dbus-send` directly:

```bash
dbus-send \
  --session \
  --print-reply \
  --dest=org.freedesktop.impl.portal.PermissionStore \
  /org/freedesktop/impl/portal/PermissionStore \
  org.freedesktop.impl.portal.PermissionStore.SetPermission \
  string:kde-authorized \
  boolean:false \
  string:screencast \
  string:io.veyon.veyon-server \
  array:string:yes
```

### Helper script

A convenience script is installed to `/usr/share/veyon/veyon-preauth-kde.sh`.
It tries Method 1 first and falls back to Method 2:

```bash
sudo -u <desktop-user> \
  DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$(id -u <desktop-user>)/bus" \
  /usr/share/veyon/veyon-preauth-kde.sh
```

### Debian post-install integration

In a Debian package, add a `postinst` snippet that runs the helper for each
auto-login user:

```sh
# debian/veyon-server.postinst
VEYON_USER="student"  # adjust to the auto-login user

if [ -S "/run/user/$(id -u ${VEYON_USER})/bus" ]; then
    sudo -u "${VEYON_USER}" \
        DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$(id -u ${VEYON_USER})/bus" \
        /usr/share/veyon/veyon-preauth-kde.sh || true
fi
```

### systemd service considerations

The Veyon Server is typically started as a system-level service (`root` or a
dedicated system user).  Under Wayland the portal, however, runs inside the
**user session** of the logged-in user whose screen is being shared.

The recommended setup is:

1. Run `veyon-server` as the **desktop user** (not root), or use `systemd --user`
   with `WantedBy=graphical-session.target`.
2. Ensure `DBUS_SESSION_BUS_ADDRESS` and `WAYLAND_DISPLAY` are forwarded to
   the service environment (see `ImportCredential=` / `PassEnvironment=`).
3. Run the pre-authorization script once during provisioning (post-install or
   cloud-init).

Example `veyon-server.service` override (`/etc/systemd/user/veyon-server.service`):

```ini
[Unit]
Description=Veyon Server (Wayland/PipeWire)
PartOf=graphical-session.target
After=graphical-session.target

[Service]
Type=simple
ExecStart=/usr/sbin/veyon-server
Restart=on-failure
RestartSec=5s

[Install]
WantedBy=graphical-session.target
```

Enable it as the desktop user:

```bash
systemctl --user enable --now veyon-server
```

## Limitations and known issues

* **GNOME**: GNOME's portal backend does not currently support the
  `kde-authorized` pre-authorization table or any other pre-authorization.
  An interactive consent dialog appears on first use per user; with
  GNOME 45 and newer the approval is remembered afterwards (see
  [Remembering the approval](#remembering-the-approval-restore-token)), with
  GNOME 44 and older it is asked for on every start.

* **Input forwarding**: The plugin requests both keyboard and pointer device
  types via `SelectDevices`.  Key presses and releases are forwarded using
  `NotifyKeyboardKeysym`.  Pointer motion is forwarded as absolute coordinates
  via `NotifyPointerMotionAbsolute`, button presses/releases via
  `NotifyPointerButton`, and scroll wheel events via
  `NotifyPointerAxisDiscrete`.  All input events are ignored until the portal
  session is in the `Running` state.

* **Pointer coordinates and display scaling**: VNC clients send pointer
  positions in framebuffer pixels, i.e. in the size of the PipeWire buffers.
  xdg-desktop-portal (1.14 and later) only accepts positions for
  `NotifyPointerMotionAbsolute` inside the `size` the portal reports for the
  stream in the `Start` response and rejects all others ("Invalid position").
  With display scaling, KDE Plasma and mutter both report the logical size of
  the output (e.g. 2560×1440 for a 4K output at 150 %), but they interpret the
  positions differently:

  * **KDE Plasma** takes positions in logical pixels of the output, so they
    are scaled by stream size / framebuffer size and clamped to the stream.
  * **mutter (GNOME)** takes positions in buffer pixels and divides them by
    the output's scale itself if stage views are scaled (fractional scaling,
    `scale-monitor-framebuffer`). Positions are therefore passed unscaled and
    clamped to the stream. Since the portal limits them to the logical size,
    the area beyond it (right and bottom part of the screen) can't be reached
    with fractional scaling. This is an upstream limitation of the portal's
    `Notify*` methods; reaching it would need input via `ConnectToEIS`. Without
    scaled stage views stream and buffer size are the same and the pointer
    covers the whole screen.

  mutter is detected by its `org.gnome.Mutter.RemoteDesktop` service on the
  session bus (`XDG_CURRENT_DESKTOP` is only used if the bus can't be asked).
  If the portal doesn't report a size, positions are passed unscaled, which
  only works with portals older than 1.14; a warning is logged. The stream's
  `position` is not added, since positions are relative to the stream.

  The stream size is read once per portal session. If the size of the
  PipeWire buffers changes (e.g. after a change of the output's resolution),
  the portal session is restarted to read it again. A change of the scale
  alone leaves the buffers as they are and can't be noticed; the pointer is
  off then until the session is restarted (e.g. by restarting the Veyon
  Server).

* **Input errors**: An error reply to a single input request only drops that
  request and is logged at most every 10 s. A request without reply within
  5 s drops the pending input but keeps the session; key and button releases
  and the last pointer position are never dropped. A release the portal
  doesn't reply to is sent once more (an error reply would only be repeated),
  a motion without reply is sent again with the next pointer event, and keys
  and buttons still pressed are released when the session is closed or
  restarted, waiting at most 500 ms for the replies before the session is
  closed. The session is closed
  and restarted (which also restarts capture) only if the portal is gone,
  after 8 error replies in a row (pointer positions rejected by the portal
  don't count) or after two requests in a row without reply.

* **Multi-monitor**: `SelectSources` currently requests a single monitor.
  Multi-monitor support can be added by iterating stream node IDs returned by
  the portal `Start` response.
