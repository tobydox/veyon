// SPDX-FileCopyrightText: 2026 Tobias Junghans <tobydox@veyon.io>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "PortalSessionRestarter.h"
#include "VeyonCore.h"


PortalSessionRestarter::PortalSessionRestarter(Check isSharingInhibited, Check hasRestoreToken,
											   Action startSession, QObject* parent) :
	QObject(parent),
	m_isSharingInhibited(std::move(isSharingInhibited)),
	m_hasRestoreToken(std::move(hasRestoreToken)),
	m_startSession(std::move(startSession))
{
	m_timer.setSingleShot(true);
	connect(&m_timer, &QTimer::timeout, this, &PortalSessionRestarter::startNow);
}



void PortalSessionRestarter::startNow()
{
	m_timer.stop();

	if (m_isSharingInhibited())
	{
		if (m_waitingForUnlock == false)
		{
			vInfo() << "screen is locked – starting the XDG Portal session once it is unlocked";
			m_waitingForUnlock = true;
		}
		schedule(m_delays.failedSession);
		return;
	}

	if (m_waitingForUnlock)
	{
		vInfo() << "screen is unlocked";
		m_waitingForUnlock = false;
	}

	if (m_hasRestoreToken())
	{
		vInfo() << "Starting XDG Portal session with the stored approval";
	}
	else
	{
		vInfo() << "Starting XDG Portal session";
	}
	m_startSession();
}



void PortalSessionRestarter::sessionFailed()
{
	schedule(m_delays.failedSession);
}



void PortalSessionRestarter::sessionClosed()
{
	if (m_hasRestoreToken())
	{
		vInfo() << "XDG Portal session closed by the portal – restoring it with the stored approval in"
				<< m_delays.failedSession << "ms";
	}
	else
	{
		vInfo() << "XDG Portal session closed by the portal – restarting it in" << m_delays.failedSession
				<< "ms, which asks for approval";
	}

	schedule(m_delays.failedSession);
}



void PortalSessionRestarter::streamEnded()
{
	schedule(m_delays.endedStream);
}



void PortalSessionRestarter::schedule(int delay)
{
	// re-arms a pending restart instead of queueing another one
	m_timer.start(delay);
}
