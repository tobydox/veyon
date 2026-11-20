// SPDX-FileCopyrightText: 2026 Tobias Junghans <tobydox@veyon.io>
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QObject>
#include <QTimer>

#include <functional>

/**
 * @brief Decides when the capture helper (re)starts the portal session (see PipeWireCaptureServer).
 *
 * All restarts go through a single timer, so that e.g. the end of the stream and the closed
 * session don't queue two restarts. While a locked screen keeps the compositor from sharing the
 * screen (see PortalSession::isSharingInhibitedByScreenLock()), no session is started, since an
 * attempt would use up the restore token; this is checked again after FailedSessionDelay.
 */
class PortalSessionRestarter : public QObject
{
	Q_OBJECT
public:
	using Check = std::function<bool()>;
	using Action = std::function<void()>;

	struct Delays
	{
		// after a failed or closed session, also while waiting for the screen to be unlocked
		int failedSession = 5000;
		// after the end of the stream or a change of its size
		int endedStream = 2000;
	};

	PortalSessionRestarter(Check isSharingInhibited, Check hasRestoreToken, Action startSession,
						   QObject* parent = nullptr);

	void setDelays(Delays delays) { m_delays = delays; }

	// starts the session unless sharing is inhibited
	void startNow();

	void sessionFailed();
	// closed by the portal, e.g. screen lock or the user stopping sharing
	void sessionClosed();
	void streamEnded();

	bool isPending() const { return m_timer.isActive(); }
	bool isWaitingForUnlock() const { return m_waitingForUnlock; }

private:
	void schedule(int delay);

	const Check m_isSharingInhibited;
	const Check m_hasRestoreToken;
	const Action m_startSession;
	Delays m_delays;
	QTimer m_timer;
	bool m_waitingForUnlock{false};
};
