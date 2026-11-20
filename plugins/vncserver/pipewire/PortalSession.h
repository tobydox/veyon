// SPDX-FileCopyrightText: 2024-2026 Tobias Junghans <tobydox@veyon.io>
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include "OrgFreedesktopPortalRemoteDesktop.h"
#include "OrgFreedesktopPortalRequest.h"
#include "OrgFreedesktopPortalScreenCast.h"
#include "OrgFreedesktopPortalSession.h"
#include "PortalInputDispatcher.h"
#include "PortalPointerMapping.h"

/**
 * @brief Manages an XDG Desktop Portal RemoteDesktop session.
 *
 * Follows the portal flow:
 *   CreateSession → SelectDevices → SelectSources → Start → OpenPipeWireRemote
 *
 * All portal responses arrive asynchronously via the /org/freedesktop/portal/desktop/response
 * signal and are dispatched to the appropriate handler.
 *
 * The app_id used for permission-store lookups is "io.veyon.veyon-server".
 * KDE Plasma admins can pre-authorize unattended access via:
 *   flatpak permission-set kde-authorized screencast io.veyon.veyon-server yes
 * or directly through the DBus PermissionStore interface.
 */
class PortalSession : public QObject
{
	Q_OBJECT

public:
	enum class State {
		Idle,
		CreatingSession,
		SelectingDevices,
		SelectingSources,
		Starting,
		Running,
		Failed
	};
	Q_ENUM(State)

	/**
	 * @param persistRestoreToken keep the portal's restore token in the user's state directory,
	 * so the screen sharing approval outlives this process (see PipeWireVncConfiguration)
	 */
	explicit PortalSession(bool persistRestoreToken, QObject* parent = nullptr);
	~PortalSession() override;

	/**
	 * @brief Start the portal session flow.
	 * Emits started() on success or failed() on error.
	 */
	void start();

	/**
	 * @brief Close and clean up the portal session.
	 *
	 * Keys and buttons still pressed are released first, waiting at most ReleaseOnCloseTimeout
	 * for the replies.
	 */
	void close();

	// how long close() waits at most for the replies to the releases of held keys and buttons
	static constexpr int ReleaseOnCloseTimeout = 500;

	State state() const { return m_state; }

	/**
	 * @brief How long to wait for the reply to an input request, see PortalInputDispatcher.
	 */
	void setInputReplyTimeout(int milliseconds);

	/**
	 * @brief PipeWire remote file descriptor (valid after started() signal).
	 * The caller takes ownership (must close when done).
	 */
	int pipewireFd() const { return m_pipewireFd; }

	/**
	 * @brief PipeWire node ID for the screen stream (valid after started() signal).
	 */
	quint32 pipeWireNodeId() const { return m_pipeWireNodeId; }

	/**
	 * @brief Position and size of the screen stream as reported by the portal (valid after started() signal).
	 * The size is empty if the portal doesn't report it.
	 */
	const PortalStreamGeometry& streamGeometry() const { return m_streamGeometry; }

	/**
	 * @brief How pointer positions are mapped to the stream, depending on the compositor (valid
	 * after started() signal).
	 */
	PortalPointerMappingMode pointerMappingMode() const { return m_pointerMappingMode; }

	/**
	 * @brief Forward a keyboard keysym event from a VNC client to the portal session.
	 *
	 * The @p keySym value is the X11 keysym received from the VNC client (rfbKeySym).
	 * No-op when the session is not in the Running state. Input events are sent in order,
	 * see PortalInputDispatcher.
	 */
	void notifyKeyboardKeysym(quint32 keySym, bool down);

	/**
	 * @brief Forward a pointer (mouse) event from a VNC client to the portal session.
	 *
	 * Sends NotifyPointerMotionAbsolute when the position changes and
	 * NotifyPointerButton / NotifyPointerAxisDiscrete when button state changes.
	 * No-op when the session is not in the Running state.
	 *
	 * @p x and @p y are framebuffer coordinates of the VNC client, @p framebufferSize is the
	 * current size of the framebuffer; they are mapped to the stream's coordinate space, see
	 * mapPointerToPortalStream().
	 *
	 * @p buttonMask is the LibVNCServer button bitmask (bit 0 = left, 1 = middle,
	 * 2 = right, 3 = scroll-up, 4 = scroll-down, 5 = scroll-left, 6 = scroll-right,
	 * 7 = side, 8 = extra).
	 */
	void notifyPointer(int buttonMask, int x, int y, QSize framebufferSize);

	// version of org.freedesktop.portal.RemoteDesktop with persist_mode and restore_token
	// (xdg-desktop-portal >= 1.17)
	static constexpr uint MinimumPersistenceVersion = 2;

	/**
	 * @brief Whether the portal can remember the approval (persist_mode, restore_token); known once
	 * the portal answered, i.e. usually after construction.
	 */
	bool isPersistenceSupported() const { return m_portalVersion >= MinimumPersistenceVersion; }

	/**
	 * @brief The file which keeps the restore token across server runs:
	 * `$XDG_STATE_HOME/veyon/portal-restore-token` (default `~/.local/state`) with mode 0600 in a
	 * directory with mode 0700.
	 */
	static QString restoreTokenFilePath();

	/**
	 * @brief Where builds with Qt < 6.7 kept the restore token (`~/.local/share/veyon`); a token
	 * there is moved to restoreTokenFilePath() once.
	 */
	static QString legacyRestoreTokenFilePath();

	/**
	 * @brief Whether a restore token is at hand, i.e. the next start shouldn't ask for approval.
	 */
	bool hasRestoreToken() const { return m_restoreToken.isEmpty() == false; }

	/**
	 * @brief Whether the session's screen is locked, according to the screen saver service of
	 * GNOME or the freedesktop.org one (e.g. KDE Plasma); false if neither can be asked.
	 *
	 * GNOME closes all screen sharing sessions while locked and refuses new ones, which would also
	 * use up the restore token, so the session is only restarted once unlocked.
	 */
	bool isScreenLocked() const;

Q_SIGNALS:
	void started();
	void failed();
	// the portal closed the session, e.g. because the screen got locked or the user stopped
	// sharing the screen; the state is Failed afterwards
	void closedByPortal();

private Q_SLOTS:
	void onPortalResponse(uint response, const QVariantMap& results);
	void onSessionClosed(const QVariantMap& details);

private:
	static constexpr int RegistryCallTimeout = 5000;
	static constexpr int PropertyCallTimeout = 5000;
	static constexpr int ScreenSaverCallTimeout = 1000;

	static QDBusConnection portalConnection();
	// portal >= 1.19.4: app ID of this connection (org.freedesktop.host.portal.Registry), once per
	// connection and before any other portal call
	void registerHostAppId(const QString& appId);
	// reads the version of the RemoteDesktop interface unless known already
	void detectPersistenceSupport();
	void createSession();
	void selectDevices();
	void selectSources();
	void startSession();
	void openPipeWireRemote();
	// replaces the token with the one from the Start response, see restoreTokenFilePath()
	void updateRestoreToken(const QVariantMap& results);
	// false if there is no usable stream
	bool readStreams(const QVariantMap& results);

	// a key (keysym) or button (evdev code) pressed by the VNC client
	struct HeldInput
	{
		enum class Type : quint8 {
			Key,
			Button
		};
		Type type;
		quint32 code;

		quint64 id() const { return (quint64(type) << 32) | code; }
		static HeldInput fromId(quint64 id) { return {Type(id >> 32), quint32(id)}; }
	};

	static constexpr int MaximumReleaseRetries = 1;

	QDBusMessage inputMessage(HeldInput input, bool pressed) const;
	QDBusPendingCall sendInput(HeldInput input, bool pressed);
	void press(HeldInput input);
	void release(HeldInput input, int retries);
	void releaseHeldInputs();
	static std::optional<QPoint> readIntPair(const QVariant& value);
	PortalPointerMappingMode detectPointerMappingMode() const;

	void connectResponseSignal(const QString& requestPath);
	void disconnectResponseSignal();
	void connectSessionClosedSignal();
	void disconnectSessionClosedSignal();

	void loadRestoreToken();
	bool storeRestoreToken() const;
	// in memory and on disk
	void dropRestoreToken();
	static void removeRestoreTokenFiles();

	void setState(State s);
	void fail();
	// after an attempt with the restore token that didn't start the session
	void dropUnusableRestoreToken();

	QString makeRequestToken() const;
	QString senderToken() const;
	QString makeRequestPath(const QString& token) const;

	QDBusConnection m_sessionBus;
	OrgFreedesktopPortalRemoteDesktopInterface* m_remoteDesktop{nullptr};
	OrgFreedesktopPortalScreenCastInterface* m_screenCast{nullptr};
	OrgFreedesktopPortalRequestInterface* m_requestInterface{nullptr};
	PortalInputDispatcher m_inputDispatcher{this};

	QString m_sessionHandle;
	QString m_restoreToken;
	const bool m_persistRestoreToken;
	// 0 = not known yet
	uint m_portalVersion{0};
	// the restore token has been sent with the current attempt
	bool m_restoringSession{false};
	State m_state{State::Idle};
	int m_pipewireFd{-1};
	quint32 m_pipeWireNodeId{0};
	PortalStreamGeometry m_streamGeometry;
	PortalPointerMappingMode m_pointerMappingMode{PortalPointerMappingMode::Scale};
	QString m_connectedRequestPath;
	QString m_connectedSessionPath;

	// inputs pressed without a release the portal accepted, with the sequence number of the press
	QHash<quint64, quint64> m_heldInputs;
	quint64 m_pressSequence{0};
	QElapsedTimer m_lastReleaseWarning;

	int m_lastButtonMask{0};
	double m_lastPointerX{0.0};
	double m_lastPointerY{0.0};
};
