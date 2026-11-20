// SPDX-FileCopyrightText: 2024-2026 Tobias Junghans <tobydox@veyon.io>
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include "PortalSession.h"
#include "VeyonCore.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <linux/input.h>
#include <sys/stat.h>
#include <unistd.h>

// XDG Desktop Portal service and interface names
static const auto PortalService = QStringLiteral("org.freedesktop.portal.Desktop");
static const auto PortalObjectPath = QStringLiteral("/org/freedesktop/portal/desktop");
static const auto MutterRemoteDesktopService = QStringLiteral("org.gnome.Mutter.RemoteDesktop");

namespace {

// the portal fails the whole SelectDevices call for an invalid token
constexpr int MaximumRestoreTokenLength = 64;

bool isValidRestoreToken(const QString& token)
{
	// xdg-desktop-portal < 1.23 generates UUIDs (QUuid would also accept one in braces), newer
	// versions D-Bus object path elements ([A-Za-z0-9_]) and still accept UUIDs
	static const QRegularExpression pathElement(QStringLiteral("^[A-Za-z0-9_]+$"));
	return (token.size() == 36 && QUuid::fromString(token).isNull() == false) ||
		(token.size() <= MaximumRestoreTokenLength && pathElement.match(token).hasMatch());
}



// the token grants sharing the screen without asking, so it lives in a directory only the user can
// enter; a directory of someone else or a symbolic link (e.g. planted into a shared home) is refused
bool preparePrivateDirectory(const QString& directory, bool create)
{
	const auto path = QFile::encodeName(directory);

	struct stat status{};
	if (::lstat(path.constData(), &status) != 0)
	{
		if (errno != ENOENT || create == false)
		{
			return false;
		}
		if (QDir().mkpath(QFileInfo(directory).path()) == false ||
			(::mkdir(path.constData(), S_IRWXU) != 0 && errno != EEXIST) ||
			::lstat(path.constData(), &status) != 0)
		{
			vWarning() << "could not create directory" << directory << "for the portal restore token";
			return false;
		}
	}

	if (S_ISDIR(status.st_mode) == false || status.st_uid != ::getuid())
	{
		vWarning() << "refusing to use" << directory
				   << "for the portal restore token: not a directory owned by the user";
		return false;
	}

	// created by an older version or by hand with the default umask
	if ((status.st_mode & (S_IRWXG | S_IRWXO)) != 0 && ::chmod(path.constData(), S_IRWXU) != 0)
	{
		vWarning() << "could not restrict the permissions of" << directory;
		return false;
	}

	return true;
}



// an empty string if there is no usable token in the file
QString readRestoreTokenFile(const QString& filePath)
{
	// O_NOFOLLOW: a symbolic link could make another file of the user (or of someone else) the
	// token; O_NONBLOCK: a FIFO must not block the helper
	const int fd = ::open(QFile::encodeName(filePath).constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
	{
		if (errno == ELOOP)
		{
			vWarning() << "refusing to read the portal restore token from the symbolic link" << filePath;
		}
		else if (errno != ENOENT)
		{
			vWarning() << "could not open" << filePath << ::strerror(errno);
		}
		return {};
	}

	QString token;
	struct stat status{};
	if (::fstat(fd, &status) != 0 || S_ISREG(status.st_mode) == false || status.st_uid != ::getuid())
	{
		vWarning() << "refusing to read the portal restore token from" << filePath
				   << "- not a regular file owned by the user";
	}
	else
	{
		if ((status.st_mode & (S_IRWXG | S_IRWXO)) != 0)
		{
			::fchmod(fd, S_IRUSR | S_IWUSR);
		}

		char buffer[MaximumRestoreTokenLength + 2]{};
		const auto size = ::read(fd, buffer, sizeof(buffer));
		if (size > 0)
		{
			token = QString::fromUtf8(buffer, size).trimmed();
		}
	}
	::close(fd);

	return isValidRestoreToken(token) ? token : QString{};
}



bool removeOwnFile(const QString& filePath)
{
	// also removes a symbolic link itself, never its target
	struct stat status{};
	const auto path = QFile::encodeName(filePath);
	if (::lstat(path.constData(), &status) != 0)
	{
		return errno == ENOENT;
	}

	return status.st_uid == ::getuid() && ::unlink(path.constData()) == 0;
}

} // namespace



QDBusConnection PortalSession::portalConnection()
{
	// a session bus connection of its own, so that registerHostAppId() surely comes first on it:
	// Qt (< 6.10) may call the portal on the shared connection already while constructing
	// QGuiApplication, e.g. the GNOME platform theme reading the Settings portal, after which the
	// portal has derived the app ID of that connection and refuses Register(); Qt >= 6.10 registers
	// the shared connection itself
	return QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("veyon-portal-session"));
}



void PortalSession::registerHostAppId(const QString& appId)
{
	// the portal accepts only one registration per connection, and all PortalSession instances use
	// the same one
	static bool registered = false;
	if (registered || appId.isEmpty())
	{
		return;
	}

	// exported on the same object as all portal interfaces
	auto call = QDBusMessage::createMethodCall(PortalService, PortalObjectPath,
											   QStringLiteral("org.freedesktop.host.portal.Registry"),
											   QStringLiteral("Register"));
	call << appId << QVariantMap{};

	const auto reply = m_sessionBus.call(call, QDBus::Block, RegistryCallTimeout);
	if (reply.type() != QDBusMessage::ErrorMessage)
	{
		vDebug() << "registered app ID" << appId << "with xdg-desktop-portal";
		registered = true;
		return;
	}

	if (reply.errorName() == QLatin1String("org.freedesktop.DBus.Error.UnknownMethod") ||
		reply.errorName() == QLatin1String("org.freedesktop.DBus.Error.UnknownObject") ||
		reply.errorName() == QLatin1String("org.freedesktop.DBus.Error.UnknownInterface"))
	{
		vDebug() << "xdg-desktop-portal < 1.19.4 without host app registry, app ID derived from the systemd scope";
	}
	else if (reply.errorName() == QLatin1String("org.freedesktop.portal.Error.Failed") &&
			 reply.errorMessage().contains(QLatin1String("already associated")))
	{
		// e.g. by Qt >= 6.10, which registers QGuiApplication::desktopFileName() when constructing
		// QGuiApplication, if this were the shared connection
		vDebug() << "app ID already registered with xdg-desktop-portal on this connection";
		registered = true;
	}
	else
	{
		// e.g. a sandboxed process; the portal derives the app ID itself then
		vWarning() << "could not register app ID" << appId << "with xdg-desktop-portal:" << reply.errorName()
				   << reply.errorMessage();
	}
}



PortalSession::PortalSession(bool persistRestoreToken, QObject* parent) :
	QObject(parent), m_sessionBus(portalConnection()), m_persistRestoreToken(persistRestoreToken)
{
	// the portal is gone, does not respond or input can't keep up: release the session and let
	// the owner restart it like after any other failure
	m_inputDispatcher.setFailureHandler([this](const QDBusError&) {
		close();
		fail();
	});

	const auto appId = QGuiApplication::desktopFileName();

	// xdg-desktop-portal derives the app ID from the process behind the unique name of the
	// calling connection: from its systemd unit/scope (sd_pid_get_user_unit(), "app-<id>-*.scope")
	// or, with portal >= 1.19.4, from org.freedesktop.host.portal.Registry.Register() on that
	// connection. Well-known bus names are not consulted, so no name is registered. With portal
	// >= 1.20 the server is started without a scope, so the app ID has to be registered here,
	// before any other portal call on this connection; otherwise the app ID is empty, restore
	// tokens are stored for the empty app ID and e.g. KDE's kde-authorized pre-authorization doesn't
	// apply. Older portals don't provide the registry and keep using the scope.
	registerHostAppId(appId);

	// Apply KDE pre-authorization so the consent dialog can be suppressed
	// in unattended/kiosk scenarios. These calls are best-effort and may
	// fail silently if flatpak is not installed.
	QProcess::execute(QStringLiteral("flatpak"),
					  {QStringLiteral("permission-set"), QStringLiteral("kde-authorized"),
					   QStringLiteral("screencast"), appId, QStringLiteral("yes")});
	QProcess::execute(QStringLiteral("flatpak"),
					  {QStringLiteral("permission-set"), QStringLiteral("kde-authorized"),
					   QStringLiteral("remote-desktop"), appId, QStringLiteral("yes")});

	m_remoteDesktop = new OrgFreedesktopPortalRemoteDesktopInterface(PortalService, PortalObjectPath, m_sessionBus, this);
	m_screenCast = new OrgFreedesktopPortalScreenCastInterface(PortalService, PortalObjectPath, m_sessionBus, this);

	// a token left from when persistence was enabled is removed so it no longer grants anything
	if (m_persistRestoreToken == false)
	{
		removeRestoreTokenFiles();
	}

	detectPersistenceSupport();
}



PortalSession::~PortalSession()
{
	close();
}



void PortalSession::start()
{
	if (m_state != State::Idle && m_state != State::Failed)
	{
		return;
	}
	// a failed session may still be open at the portal
	close();
	m_restoringSession = false;
	// the portal may not have been reachable before
	detectPersistenceSupport();
	m_inputDispatcher.reset();
	m_pipewireFd = -1;
	m_pipeWireNodeId = 0;
	m_streamGeometry = {};
	m_pointerMappingMode = PortalPointerMappingMode::Scale;
	createSession();
}



void PortalSession::setInputReplyTimeout(int milliseconds)
{
	m_inputDispatcher.setReplyTimeout(milliseconds);
}



void PortalSession::close()
{
	// input state of this session must not leak into the next one, and nothing may stay pressed
	// in the compositor
	m_inputDispatcher.clear();
	releaseHeldInputs();
	m_lastButtonMask = 0;
	m_lastPointerX = 0;
	m_lastPointerY = 0;

	disconnectResponseSignal();
	// the portal doesn't emit Closed for Close(), but it must not be taken for one closing on its own
	disconnectSessionClosedSignal();

	if (!m_sessionHandle.isEmpty())
	{
		OrgFreedesktopPortalSessionInterface sessionIface(
			PortalService, m_sessionHandle, m_sessionBus);
		sessionIface.Close();
		m_sessionHandle.clear();
	}

	setState(State::Idle);
}



void PortalSession::detectPersistenceSupport()
{
	if (m_portalVersion > 0)
	{
		return;
	}

	auto call = QDBusMessage::createMethodCall(PortalService, PortalObjectPath,
											   QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
	call << m_remoteDesktop->interface() << QStringLiteral("version");

	const QDBusReply<QVariant> reply = m_sessionBus.call(call, QDBus::Block, PropertyCallTimeout);
	if (reply.isValid() == false || reply.value().toUInt() == 0)
	{
		vWarning() << "could not read the version of the RemoteDesktop portal:" << reply.error().message();
		return;
	}

	m_portalVersion = reply.value().toUInt();
	vDebug() << "RemoteDesktop portal version" << m_portalVersion;

	if (isPersistenceSupported() == false)
	{
		vInfo() << "the RemoteDesktop portal (version" << m_portalVersion << ") can't remember screen sharing approvals"
				<< "on this system (needs xdg-desktop-portal >= 1.17, e.g. not on Ubuntu 22.04 / GNOME 42),"
				<< "so the approval is asked for on every start";
		return;
	}

	// persist_mode 2 only pays off across server runs if the token outlives this process
	if (m_persistRestoreToken)
	{
		loadRestoreToken();
	}
}



void PortalSession::createSession()
{
	setState(State::CreatingSession);

	const QString token = makeRequestToken();

	QVariantMap options;
	options[QStringLiteral("handle_token")] = token;
	options[QStringLiteral("session_handle_token")] = makeRequestToken();

	// Subscribe to the Response signal BEFORE making the async call to avoid a
	// race condition where the portal emits Response before we can connect.
	connectResponseSignal(makeRequestPath(token));

	auto call = m_remoteDesktop->CreateSession(options);
	auto* watcher = new QDBusPendingCallWatcher(call, this);
	connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* w) {
		w->deleteLater();
		QDBusPendingReply<QDBusObjectPath> reply = *w;
		if (reply.isError())
		{
			vCritical() << "CreateSession call failed:" << reply.error().message();
			disconnectResponseSignal();
			fail();
		}
	});
}



void PortalSession::selectDevices()
{
	setState(State::SelectingDevices);

	const QString token = makeRequestToken();

	QVariantMap options;
	options[QStringLiteral("handle_token")] = token;
	// Request keyboard + pointer devices (1=keyboard, 2=pointer, 3=both)
	options[QStringLiteral("types")] = QVariant::fromValue<uint>(3u);
	// RemoteDesktop version 1 doesn't know these options
	if (isPersistenceSupported())
	{
		// persist_mode 2 = persist until explicitly revoked; without persisting the token to a file,
		// it still spares the dialog when the session is restarted by this process
		options[QStringLiteral("persist_mode")] = QVariant::fromValue<uint>(2u);
		if (m_restoreToken.isEmpty() == false)
		{
			vInfo() << "restoring the screen sharing session with the stored approval";
			options[QStringLiteral("restore_token")] = m_restoreToken;
			// the portal deletes its entry for the token now and stores it again only if Start succeeds
			m_restoringSession = true;
		}
	}

	connectResponseSignal(makeRequestPath(token));

	auto call = m_remoteDesktop->SelectDevices(QDBusObjectPath(m_sessionHandle), options);
	connect(new QDBusPendingCallWatcher(call, this), &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* w) {
		w->deleteLater();
		QDBusPendingReply<QDBusObjectPath> reply = *w;
		if (reply.isError())
		{
			vCritical() << "SelectDevices call failed:" << reply.error().message();
			disconnectResponseSignal();
			fail();
		}
	});
}



void PortalSession::selectSources()
{
	setState(State::SelectingSources);

	const QString token = makeRequestToken();

	QVariantMap options;
	options[QStringLiteral("handle_token")] = token;
	options[QStringLiteral("types")] = QVariant::fromValue<uint>(1u); // 1=monitor
	options[QStringLiteral("multiple")] = false;
	// ScreenCast interface only supports persist_mode=0; persist_mode=2 is
	// only valid on the RemoteDesktop interface (used in selectDevices()).
	options[QStringLiteral("persist_mode")] = QVariant::fromValue<uint>(0u);

	connectResponseSignal(makeRequestPath(token));

	auto call = m_screenCast->SelectSources(QDBusObjectPath(m_sessionHandle), options);
	auto* watcher = new QDBusPendingCallWatcher(call, this);
	connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* w) {
		w->deleteLater();
		QDBusPendingReply<QDBusObjectPath> reply = *w;
		if (reply.isError())
		{
			vCritical() << "SelectSources call failed:" << reply.error().message();
			disconnectResponseSignal();
			fail();
		}
	});
}



void PortalSession::startSession()
{
	setState(State::Starting);

	const QString token = makeRequestToken();

	QVariantMap options;
	options[QStringLiteral("handle_token")] = token;

	connectResponseSignal(makeRequestPath(token));

	auto call = m_remoteDesktop->Start(
		QDBusObjectPath(m_sessionHandle),
		QStringLiteral(""), // parent_window handle (empty for daemon/unattended use)
		options);
	auto* watcher = new QDBusPendingCallWatcher(call, this);
	connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* w) {
		w->deleteLater();
		QDBusPendingReply<QDBusObjectPath> reply = *w;
		if (reply.isError())
		{
			vCritical() << "Start call failed:" << reply.error().message();
			disconnectResponseSignal();
			fail();
		}
	});
}



void PortalSession::openPipeWireRemote()
{
	auto pending = m_screenCast->OpenPipeWireRemote(
		QDBusObjectPath(m_sessionHandle), QVariantMap{});
	pending.waitForFinished();

	if (pending.isError())
	{
		vCritical() << "OpenPipeWireRemote failed:" << pending.error().message();
		fail();
		return;
	}

	// dup() the FD so we own it independently of the QDBus reply lifetime
	m_pipewireFd = ::dup(pending.value().fileDescriptor());
	if (m_pipewireFd < 0)
	{
		vCritical() << "dup() of PipeWire FD failed";
		fail();
		return;
	}

	setState(State::Running);
	Q_EMIT started();
}



void PortalSession::connectResponseSignal(const QString& requestPath)
{
	disconnectResponseSignal();

	m_connectedRequestPath = requestPath;
	m_requestInterface = new OrgFreedesktopPortalRequestInterface(
		PortalService, requestPath, m_sessionBus, this);

	connect(m_requestInterface, &OrgFreedesktopPortalRequestInterface::Response,
			this, &PortalSession::onPortalResponse);
}



void PortalSession::disconnectResponseSignal()
{
	delete m_requestInterface;
	m_requestInterface = nullptr;
	m_connectedRequestPath.clear();
}



void PortalSession::connectSessionClosedSignal()
{
	disconnectSessionClosedSignal();

	if (m_sessionBus.connect(PortalService, m_sessionHandle, QStringLiteral("org.freedesktop.portal.Session"),
							 QStringLiteral("Closed"), this, SLOT(onSessionClosed(QVariantMap))))
	{
		m_connectedSessionPath = m_sessionHandle;
	}
	else
	{
		vWarning() << "could not connect to the Closed signal of the portal session";
	}
}



void PortalSession::disconnectSessionClosedSignal()
{
	if (m_connectedSessionPath.isEmpty() == false)
	{
		m_sessionBus.disconnect(PortalService, m_connectedSessionPath, QStringLiteral("org.freedesktop.portal.Session"),
								QStringLiteral("Closed"), this, SLOT(onSessionClosed(QVariantMap)));
		m_connectedSessionPath.clear();
	}
}



void PortalSession::onSessionClosed(const QVariantMap& details)
{
	Q_UNUSED(details)

	vInfo() << "the portal closed the screen sharing session (e.g. screen locked or sharing stopped by the user)";

	// nothing left to close at the portal
	m_sessionHandle.clear();
	close();

	// a running session keeps the token valid, the portal stored it with Start
	dropUnusableRestoreToken();
	setState(State::Failed);
	Q_EMIT closedByPortal();
}



bool PortalSession::isScreenLocked() const
{
	const auto busInterface = m_sessionBus.interface();
	if (busInterface == nullptr)
	{
		return false;
	}

	struct ScreenSaver
	{
		QString service;
		QString path;
	};
	// gnome-shell, and e.g. KWin/ksmserver, xfce4-screensaver
	static const ScreenSaver screenSavers[] = {
		{QStringLiteral("org.gnome.ScreenSaver"), QStringLiteral("/org/gnome/ScreenSaver")},
		{QStringLiteral("org.freedesktop.ScreenSaver"), QStringLiteral("/org/freedesktop/ScreenSaver")},
	};

	for (const auto& screenSaver : screenSavers)
	{
		// only ask running services, the bus could start one otherwise
		const QDBusReply<bool> registered = busInterface->isServiceRegistered(screenSaver.service);
		if (registered.isValid() == false || registered.value() == false)
		{
			continue;
		}

		const auto call = QDBusMessage::createMethodCall(screenSaver.service, screenSaver.path, screenSaver.service,
														 QStringLiteral("GetActive"));
		const QDBusReply<bool> active = m_sessionBus.call(call, QDBus::Block, ScreenSaverCallTimeout);
		if (active.isValid())
		{
			return active.value();
		}
	}

	return false;
}



void PortalSession::onPortalResponse(uint response, const QVariantMap& results)
{
	disconnectResponseSignal();

	if (response != 0)
	{
		vWarning() << "Portal request in state" << static_cast<int>(m_state)
				   << "denied/cancelled (response=" << response << ")";
		fail();
		return;
	}

	switch (m_state)
	{
	case State::CreatingSession:
	{
		// session_handle can arrive as QDBusObjectPath or as a plain string
		QDBusObjectPath sessionPath = results.value(QStringLiteral("session_handle")).value<QDBusObjectPath>();
		m_sessionHandle = sessionPath.path().isEmpty() ? results.value(QStringLiteral("session_handle")).toString() : sessionPath.path();

		if (m_sessionHandle.isEmpty())
		{
			vCritical() << "CreateSession response missing session_handle";
			fail();
			return;
		}
		vDebug() << "Portal session created:" << m_sessionHandle;
		connectSessionClosedSignal();
		selectDevices();
		break;
	}

	case State::SelectingDevices:
		selectSources();
		break;

	case State::SelectingSources:
		startSession();
		break;

	case State::Starting:
	{
		const auto restored = m_restoringSession;
		m_restoringSession = false;
		updateRestoreToken(results);

		if (readStreams(results) == false)
		{
			if (restored)
			{
				// xdg-desktop-portal-gnome restores a session without its monitor (e.g. replaced or
				// connected elsewhere) instead of asking again; once without the token shows the dialog
				vWarning() << "the portal restored the screen sharing session without a usable stream - dropping "
							  "the stored approval and asking for it again";
				dropRestoreToken();
				close();
				start();
				return;
			}

			vCritical() << "the portal started the screen sharing session without a usable stream";
			fail();
			return;
		}

		openPipeWireRemote();
		break;
	}

	default:
		vWarning() << "Unexpected portal response in state" << static_cast<int>(m_state);
		break;
	}
}

void PortalSession::updateRestoreToken(const QVariantMap& results)
{
	if (isPersistenceSupported() == false)
	{
		return;
	}

	// the portal may hand out a new token on every start, so always replace the old one
	const auto token = results.value(QStringLiteral("restore_token")).toString();
	if (isValidRestoreToken(token))
	{
		if (token != m_restoreToken)
		{
			vDebug() << "got a new portal restore token";
		}
		m_restoreToken = token;
		if (m_persistRestoreToken)
		{
			storeRestoreToken();
		}
	}
	else if (m_restoreToken.isEmpty() == false)
	{
		// e.g. the user unticked "Remember This Selection"; the portal deleted the old entry already
		vInfo() << "the portal returned no restore token - the screen sharing approval is not remembered";
		dropRestoreToken();
	}
}



bool PortalSession::readStreams(const QVariantMap& results)
{
	// a(ua{sv}): PipeWire node ID and properties of each stream; only one monitor is selected
	const auto streams = results.value(QStringLiteral("streams")).value<QDBusArgument>();
	if (streams.currentSignature() == QLatin1String("a(ua{sv})"))
	{
		streams.beginArray();
		if (!streams.atEnd())
		{
			QVariantMap properties;
			streams.beginStructure();
			streams >> m_pipeWireNodeId >> properties;
			streams.endStructure();

			m_streamGeometry.position = readIntPair(properties.value(QStringLiteral("position"))).value_or(QPoint{});
			const auto size = readIntPair(properties.value(QStringLiteral("size")));
			if (size.has_value() && size->x() > 0 && size->y() > 0)
			{
				m_streamGeometry.size = QSize(size->x(), size->y());
			}
		}
		streams.endArray();
	}

	m_pointerMappingMode = detectPointerMappingMode();

	vDebug() << "Portal session started, PipeWire node ID:" << m_pipeWireNodeId
			 << "stream position:" << m_streamGeometry.position << "size:" << m_streamGeometry.size
			 << "pointer mapping:" << (m_pointerMappingMode == PortalPointerMappingMode::Scale ? "scale" : "clamp only");

	if (m_pipeWireNodeId == 0)
	{
		return false;
	}

	if (m_streamGeometry.size.isEmpty())
	{
		vWarning() << "the portal reported no stream size - pointer positions are passed unscaled, "
					  "xdg-desktop-portal >= 1.14 rejects all of them";
	}

	return true;
}



PortalPointerMappingMode PortalSession::detectPointerMappingMode() const
{
	// mutter takes positions in buffer pixels (see PortalPointerMappingMode::ClampOnly); its
	// RemoteDesktop service, which xdg-desktop-portal-gnome talks to, is the most reliable sign
	// that mutter is the compositor, also for other mutter based desktops
	if (const auto busInterface = m_sessionBus.interface())
	{
		const QDBusReply<bool> registered = busInterface->isServiceRegistered(MutterRemoteDesktopService);
		if (registered.isValid())
		{
			return registered.value() ? PortalPointerMappingMode::ClampOnly : PortalPointerMappingMode::Scale;
		}
	}

	// the bus could not be asked
	const auto desktops = qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(QLatin1Char(':'));
	if (desktops.contains(QStringLiteral("GNOME"), Qt::CaseInsensitive))
	{
		return PortalPointerMappingMode::ClampOnly;
	}

	return PortalPointerMappingMode::Scale;
}



std::optional<QPoint> PortalSession::readIntPair(const QVariant& value)
{
	if (value.canConvert<QDBusArgument>() == false)
	{
		return std::nullopt;
	}

	const auto argument = value.value<QDBusArgument>();
	if (argument.currentSignature() != QLatin1String("(ii)"))
	{
		return std::nullopt;
	}

	int first = 0;
	int second = 0;
	argument.beginStructure();
	argument >> first >> second;
	argument.endStructure();

	return QPoint(first, second);
}

// ---------------------------------------------------------------------------
// Restore token persistence
// ---------------------------------------------------------------------------



QString PortalSession::restoreTokenFilePath()
{
	// the same location with every Qt version: QStandardPaths::GenericStateLocation (Qt >= 6.7)
	// is $XDG_STATE_HOME or ~/.local/state
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	const auto stateDirectory = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation);
#else
	// relative paths are invalid according to the XDG Base Directory Specification
	auto stateDirectory = qEnvironmentVariable("XDG_STATE_HOME");
	if (QDir::isAbsolutePath(stateDirectory) == false)
	{
		stateDirectory = QDir::homePath() + QStringLiteral("/.local/state");
	}
#endif

	return stateDirectory + QStringLiteral("/veyon/portal-restore-token");
}



QString PortalSession::legacyRestoreTokenFilePath()
{
	return QDir::homePath() + QStringLiteral("/.local/share/veyon/portal-restore-token");
}



void PortalSession::loadRestoreToken()
{
	const auto filePath = restoreTokenFilePath();
	const auto directory = QFileInfo(filePath).path();

	if (preparePrivateDirectory(directory, false))
	{
		m_restoreToken = readRestoreTokenFile(filePath);
	}

	// builds with Qt < 6.7 kept the token in the data directory, move it once
	const auto legacyFilePath = legacyRestoreTokenFilePath();
	if (legacyFilePath == filePath || QFileInfo(legacyFilePath).exists() == false)
	{
		return;
	}

	if (m_restoreToken.isEmpty())
	{
		m_restoreToken = readRestoreTokenFile(legacyFilePath);
		if (m_restoreToken.isEmpty() == false)
		{
			if (storeRestoreToken() == false)
			{
				// keep the old file for another try
				return;
			}
			vInfo() << "moved the portal restore token from" << legacyFilePath << "to" << filePath;
		}
	}

	if (removeOwnFile(legacyFilePath) == false)
	{
		vWarning() << "could not remove" << legacyFilePath;
	}
	// only if empty, the directory may hold other data
	QDir().rmdir(QFileInfo(legacyFilePath).path());
}



bool PortalSession::storeRestoreToken() const
{
	const auto filePath = restoreTokenFilePath();
	if (preparePrivateDirectory(QFileInfo(filePath).path(), true) == false)
	{
		vWarning() << "could not store portal restore token";
		return false;
	}

	// QSaveFile writes to the target of a symbolic link, but replaces a regular file atomically by
	// renaming its temporary file, so the old token stays complete until the new one is
	if (QFileInfo(filePath).isSymLink() && removeOwnFile(filePath) == false)
	{
		vWarning() << "could not remove the symbolic link" << filePath;
		return false;
	}

	QSaveFile file(filePath);
	if (file.open(QFile::WriteOnly) == false ||
		file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) == false ||
		file.write(m_restoreToken.toUtf8()) < 0 ||
		file.commit() == false)
	{
		vWarning() << "could not store portal restore token";
		return false;
	}

	return true;
}



void PortalSession::dropRestoreToken()
{
	m_restoreToken.clear();
	removeOwnFile(restoreTokenFilePath());
}



void PortalSession::removeRestoreTokenFiles()
{
	for (const auto& filePath : {restoreTokenFilePath(), legacyRestoreTokenFilePath()})
	{
		removeOwnFile(filePath);
	}
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void PortalSession::setState(State s)
{
	m_state = s;
}



void PortalSession::fail()
{
	dropUnusableRestoreToken();
	setState(State::Failed);
	Q_EMIT failed();
}



void PortalSession::dropUnusableRestoreToken()
{
	// the portal deleted its entry for the token when it was sent and only stores it again with a
	// successful Start
	if (m_restoringSession)
	{
		m_restoringSession = false;
		vInfo() << "the portal session failed while being restored - the stored screen sharing approval is "
				   "no longer valid and asked for again on the next start";
		dropRestoreToken();
	}
}

QString PortalSession::makeRequestToken() const
{
	return QStringLiteral("veyon_%1").arg(QRandomGenerator::global()->generate());
}

QString PortalSession::senderToken() const
{
	return m_sessionBus.baseService().remove(QLatin1Char(':')).replace(QLatin1Char('.'), QLatin1Char('_'));
}

QString PortalSession::makeRequestPath(const QString& token) const
{
	return QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(senderToken(), token);
}

// ---------------------------------------------------------------------------
// Input event forwarding
// ---------------------------------------------------------------------------

void PortalSession::notifyKeyboardKeysym(quint32 keySym, bool down)
{
	if (m_state != State::Running)
	{
		return;
	}

	if (down)
	{
		press({HeldInput::Type::Key, keySym});
	}
	else
	{
		release({HeldInput::Type::Key, keySym}, MaximumReleaseRetries);
	}
}

QDBusMessage PortalSession::inputMessage(HeldInput input, bool pressed) const
{
	auto message = QDBusMessage::createMethodCall(
		PortalService, PortalObjectPath, m_remoteDesktop->interface(),
		input.type == HeldInput::Type::Button ? QStringLiteral("NotifyPointerButton")
											  : QStringLiteral("NotifyKeyboardKeysym"));
	// portal state: 0 = released, 1 = pressed
	message << QVariant::fromValue(QDBusObjectPath(m_sessionHandle)) << QVariantMap{} << int(input.code)
			<< (pressed ? 1u : 0u);
	return message;
}



QDBusPendingCall PortalSession::sendInput(HeldInput input, bool pressed)
{
	return m_sessionBus.asyncCall(inputMessage(input, pressed));
}



void PortalSession::press(HeldInput input)
{
	// held until the portal accepted a release sent after this press
	m_heldInputs[input.id()] = ++m_pressSequence;

	m_inputDispatcher.enqueue(PortalInputDispatcher::RequestType::Default,
							  [this, input]() { return sendInput(input, true); });
}



void PortalSession::release(HeldInput input, int retries)
{
	const auto pressSequence = m_heldInputs.value(input.id(), 0);

	m_inputDispatcher.enqueue(PortalInputDispatcher::RequestType::Release,
							  [this, input]() { return sendInput(input, false); },
							  [this, input, pressSequence, retries](PortalInputDispatcher::Result result) {
		const auto currentPressSequence = m_heldInputs.value(input.id(), 0);
		// pressed again in the meantime: the release of that press takes over
		if (currentPressSequence != pressSequence)
		{
			return;
		}

		// an error reply is deterministic (e.g. no access to the device), a retry would fail as well
		if (result != PortalInputDispatcher::Result::NoReply || pressSequence == 0)
		{
			m_heldInputs.remove(input.id());
		}
		else if (retries > 0)
		{
			release(input, retries - 1);
		}
		else if (m_lastReleaseWarning.isValid() == false ||
				 m_lastReleaseWarning.elapsed() >= PortalInputDispatcher::ErrorLogInterval)
		{
			vWarning() << "could not release" << (input.type == HeldInput::Type::Key ? "key" : "button")
					   << input.code << "- releasing it when closing the session";
			m_lastReleaseWarning.start();
		}
	});
}



void PortalSession::releaseHeldInputs()
{
	if (m_state == State::Running && m_sessionHandle.isEmpty() == false)
	{
		// xdg-desktop-portal handles every call in a thread of its own, so Close() could overtake
		// the releases, which then fail with "Invalid session" and leave the input pressed (e.g. on
		// KWin's fake input), so wait for their replies. The wait is bounded, as an unresponsive
		// portal may be why the session is closed, and QDBus::Block doesn't process events, so
		// nothing can re-enter close() meanwhile.
		const QDeadlineTimer deadline(ReleaseOnCloseTimeout);
		for (auto it = m_heldInputs.constBegin(); it != m_heldInputs.constEnd(); ++it)
		{
			const auto input = HeldInput::fromId(it.key());
			const auto remainingTime = deadline.remainingTime();
			if (remainingTime > 0)
			{
				m_sessionBus.call(inputMessage(input, false), QDBus::Block, int(remainingTime));
			}
			else
			{
				sendInput(input, false);
			}
		}
	}

	m_heldInputs.clear();
}



void PortalSession::notifyPointer(int buttonMask, int x, int y, QSize framebufferSize)
{
	if (m_state != State::Running || m_pipeWireNodeId == 0)
	{
		return;
	}

	const auto sessionPath = QDBusObjectPath(m_sessionHandle);

	// Send motion event when the position changed
	const auto position = mapPointerToPortalStream({x, y}, framebufferSize, m_streamGeometry, m_pointerMappingMode);
	if (position.has_value() && (position->x() != m_lastPointerX || position->y() != m_lastPointerY))
	{
		const auto nodeId = m_pipeWireNodeId;
		const auto px = position->x();
		const auto py = position->y();
		m_inputDispatcher.enqueue(
			PortalInputDispatcher::RequestType::PointerMotion,
			[this, sessionPath, nodeId, px, py]() {
			return m_remoteDesktop->NotifyPointerMotionAbsolute(sessionPath, QVariantMap{}, nodeId, px, py);
		},
			[this, px, py](PortalInputDispatcher::Result result) {
			// the pointer may not be there, so send the position again with the next event, e.g. a
			// click without moving
			if (result != PortalInputDispatcher::Result::Accepted && px == m_lastPointerX && py == m_lastPointerY)
			{
				m_lastPointerX = std::numeric_limits<double>::quiet_NaN();
				m_lastPointerY = std::numeric_limits<double>::quiet_NaN();
			}
		});
		m_lastPointerX = px;
		m_lastPointerY = py;
	}

	// Send button/scroll events for every bit that changed
	if (buttonMask != m_lastButtonMask)
	{
		// Linux evdev button codes matching VNC button mask bits 0–8
		// Bits 3-6 are scroll directions (no evdev button code — use axis)
		static const int evdevButtons[] = {
			BTN_LEFT,	// bit 0
			BTN_MIDDLE, // bit 1
			BTN_RIGHT,	// bit 2
			0,			// bit 3 – scroll up   (axis event)
			0,			// bit 4 – scroll down (axis event)
			0,			// bit 5 – scroll left (axis event)
			0,			// bit 6 – scroll right (axis event)
			BTN_SIDE,	// bit 7
			BTN_EXTRA,	// bit 8
		};
		static constexpr int numButtons = static_cast<int>(sizeof(evdevButtons) / sizeof(evdevButtons[0]));

		for (int i = 0; i < numButtons; ++i)
		{
			const int prev = (m_lastButtonMask >> i) & 1;
			const int current = (buttonMask >> i) & 1;
			if (prev == current)
			{
				continue;
			}

			if (evdevButtons[i] != 0)
			{
				// Regular button press/release
				const HeldInput button{HeldInput::Type::Button, quint32(evdevButtons[i])};
				if (current != 0)
				{
					press(button);
				}
				else
				{
					release(button, MaximumReleaseRetries);
				}
			}
			else if (current != 0)
			{
				// Scroll axis: only fire on press (the release has no discrete step)
				int axis = 0; // 0 = vertical
				int steps = 0;
				switch (i)
				{
				case 3:
					axis = 0;
					steps = -1;
					break; // scroll up
				case 4:
					axis = 0;
					steps = 1;
					break; // scroll down
				case 5:
					axis = 1;
					steps = -1;
					break; // scroll left
				case 6:
					axis = 1;
					steps = 1;
					break; // scroll right
				default: break;
				}
				if (steps != 0)
				{
					m_inputDispatcher.enqueue(PortalInputDispatcher::RequestType::Default,
											  [this, sessionPath, axis, steps]() {
						return m_remoteDesktop->NotifyPointerAxisDiscrete(sessionPath, QVariantMap{},
																		  static_cast<uint>(axis), steps);
					});
				}
			}
		}

		m_lastButtonMask = buttonMask;
	}
}
