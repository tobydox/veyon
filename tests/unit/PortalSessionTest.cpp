// SPDX-FileCopyrightText: 2026 Tobias Junghans <tobydox@veyon.io>
// SPDX-FileCopyrightText: 2026 Ajrat Makhmutov <rauty@altlinux.org>
// SPDX-License-Identifier: GPL-2.0-or-later

// PortalSession input forwarding and restore token handling against a fake XDG Desktop Portal on
// a private session bus (dbus-run-session). The fake portal runs in a child process since
// PortalSession waits for OpenPipeWireRemote() synchronously.

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDBusMessage>
#include <QDBusUnixFileDescriptor>
#include <QDBusVirtualObject>
#include <QFile>
#include <QGuiApplication>
#include <QProcess>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QtTest>

#include "PortalSession.h"

#include <fcntl.h>
#include <linux/input.h>
#include <sys/stat.h>
#include <unistd.h>


namespace {

const auto FakePortalService = QStringLiteral("org.freedesktop.portal.Desktop");
const auto FakePortalObjectPath = QStringLiteral("/org/freedesktop/portal/desktop");
const auto SessionPathPrefix = QStringLiteral("/org/freedesktop/portal/desktop/session/test/");
const auto TestInterface = QStringLiteral("io.veyon.Test.FakePortal");
const auto MutterService = QStringLiteral("org.gnome.Mutter.RemoteDesktop");
const auto ScreenSaverService = QStringLiteral("org.gnome.ScreenSaver");
const auto ScreenSaverObjectPath = QStringLiteral("/org/gnome/ScreenSaver");
const auto RegistryInterface = QStringLiteral("org.freedesktop.host.portal.Registry");
const auto TestAppId = QStringLiteral("io.veyon.PortalSessionTest");

// keysyms the fake portal handles specially
constexpr quint32 SlowKeySym = 0xffe1; // Shift_L: forwarded late, like by a busy portal thread
constexpr quint32 DeniedKeySym = 'x';  // error reply, e.g. no access to the keyboard
constexpr quint32 HangingKeySym = 'h'; // never replied to

constexpr int SlowKeyDelay = 100;
constexpr int TestReplyTimeout = 300;

constexpr uint StreamNodeId = 42;

// the D-Bus types of the streams in the Start response, a(ua{sv}) with (ii) for position and size
struct IntPair
{
	int first = 0;
	int second = 0;
};

struct Stream
{
	uint nodeId = 0;
	QVariantMap properties;
};

} // namespace

Q_DECLARE_METATYPE(IntPair)
Q_DECLARE_METATYPE(Stream)

QDBusArgument& operator<<(QDBusArgument& argument, const IntPair& pair)
{
	argument.beginStructure();
	argument << pair.first << pair.second;
	argument.endStructure();
	return argument;
}

const QDBusArgument& operator>>(const QDBusArgument& argument, IntPair& pair)
{
	argument.beginStructure();
	argument >> pair.first >> pair.second;
	argument.endStructure();
	return argument;
}

QDBusArgument& operator<<(QDBusArgument& argument, const Stream& stream)
{
	argument.beginStructure();
	argument << stream.nodeId << stream.properties;
	argument.endStructure();
	return argument;
}

const QDBusArgument& operator>>(const QDBusArgument& argument, Stream& stream)
{
	argument.beginStructure();
	argument >> stream.nodeId >> stream.properties;
	argument.endStructure();
	return argument;
}



class FakePortal : public QDBusVirtualObject
{
public:
	QString introspect(const QString& path) const override
	{
		Q_UNUSED(path)
		return {};
	}

	bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override
	{
		const auto method = message.member();

		if (message.interface() != TestInterface && m_firstCalls.contains(message.service()) == false)
		{
			m_firstCalls[message.service()] = method;
		}

		if (message.path() == ScreenSaverObjectPath && method == QLatin1String("GetActive"))
		{
			connection.send(message.createReply(QVariant(m_screenLocked)));
			return true;
		}

		if (message.interface() == TestInterface && method == QLatin1String("Persistence"))
		{
			QStringList firstCalls;
			for (auto it = m_firstCalls.constBegin(); it != m_firstCalls.constEnd(); ++it)
			{
				firstCalls.append(it.key() + QLatin1Char('=') + it.value());
			}
			connection.send(message.createReply({m_selectDevicesLog, m_dialogs, m_starts, m_registrations, firstCalls,
												 m_lastToken, m_sessionCount}));
			return true;
		}

		if (message.interface() == TestInterface && method == QLatin1String("SetVersion"))
		{
			m_version = message.arguments().value(0).toUInt();
			connection.send(message.createReply());
			return true;
		}

		if (message.interface() == TestInterface && method == QLatin1String("SetStreams"))
		{
			m_streamsMode = StreamsMode(message.arguments().value(0).toInt());
			connection.send(message.createReply());
			return true;
		}

		if (message.interface() == TestInterface && method == QLatin1String("SetRotateTokens"))
		{
			m_rotateTokens = message.arguments().value(0).toBool();
			connection.send(message.createReply());
			return true;
		}

		if (message.interface() == TestInterface && method == QLatin1String("AddToken"))
		{
			m_tokens.insert(message.arguments().value(0).toString());
			connection.send(message.createReply());
			return true;
		}

		if (message.interface() == TestInterface && method == QLatin1String("CloseSession"))
		{
			// what the portal does when GNOME locks the screen or the user stops sharing
			const QString sessionPath = SessionPathPrefix + QString::number(m_sessionCount);
			auto closedSignal = QDBusMessage::createSignal(sessionPath, QStringLiteral("org.freedesktop.portal.Session"),
														   QStringLiteral("Closed"));
			closedSignal.setArguments({QVariant::fromValue(QVariantMap{})});
			const auto sent = connection.send(closedSignal);
			connection.send(message.createReply(QVariant(sent)));
			return true;
		}

		if (message.interface() == TestInterface && method == QLatin1String("SetScreenLocked"))
		{
			// -1: no screen saver service
			const auto locked = message.arguments().value(0).toInt();
			if (locked < 0)
			{
				QDBusConnection(connection).unregisterService(ScreenSaverService);
			}
			else
			{
				m_screenLocked = locked > 0;
				QDBusConnection(connection).registerService(ScreenSaverService);
			}
			connection.send(message.createReply());
			return true;
		}

		if (message.interface() == QLatin1String("org.freedesktop.DBus.Properties") && method == QLatin1String("Get"))
		{
			if (message.arguments().value(0).toString() == QLatin1String("org.freedesktop.portal.RemoteDesktop") &&
				message.arguments().value(1).toString() == QLatin1String("version"))
			{
				connection.send(message.createReply({QVariant::fromValue(QDBusVariant(m_version))}));
			}
			else
			{
				connection.send(message.createErrorReply(QDBusError::InvalidArgs, QStringLiteral("no such property")));
			}
			return true;
		}

		if (message.interface() == RegistryInterface && method == QLatin1String("Register"))
		{
			// like xdg-desktop-portal: once per connection, and only before any other portal call
			if (m_registeredSenders.contains(message.service()) ||
				m_firstCalls.value(message.service()) != QLatin1String("Register"))
			{
				connection.send(message.createErrorReply(QStringLiteral("org.freedesktop.portal.Error.Failed"),
														 QStringLiteral("Connection already associated with an application ID")));
				return true;
			}
			m_registeredSenders.insert(message.service());
			m_registrations.append(message.service() + QLatin1Char(':') + message.arguments().value(0).toString());
			connection.send(message.createReply());
			return true;
		}

		if (message.interface() == TestInterface && method == QLatin1String("State"))
		{
			connection.send(message.createReply({m_closedSessions, m_forwardedInput, m_rejectedInput}));
			return true;
		}

		if (message.interface() == TestInterface && method == QLatin1String("SetStream"))
		{
			const auto arguments = message.arguments();
			m_streamPosition = QPoint(arguments.value(0).toInt(), arguments.value(1).toInt());
			m_streamSize = QSize(arguments.value(2).toInt(), arguments.value(3).toInt());
			connection.send(message.createReply());
			return true;
		}

		if (message.interface() == TestInterface && method == QLatin1String("HangMotions"))
		{
			m_hangMotions = message.arguments().value(0).toInt();
			connection.send(message.createReply());
			return true;
		}

		if (message.interface() == TestInterface && method == QLatin1String("SetMutter"))
		{
			// what PortalSession takes as sign of mutter
			if (message.arguments().value(0).toBool())
			{
				QDBusConnection(connection).registerService(MutterService);
			}
			else
			{
				QDBusConnection(connection).unregisterService(MutterService);
			}
			connection.send(message.createReply());
			return true;
		}

		if (message.interface() == TestInterface && method == QLatin1String("FaultReleases"))
		{
			// keysym, number of releases to reply to with an error, then number not to reply to
			const auto arguments = message.arguments();
			m_releaseFaults[arguments.value(0).toUInt()] = {arguments.value(1).toInt(), arguments.value(2).toInt()};
			connection.send(message.createReply());
			return true;
		}

		if (message.interface() == TestInterface && method == QLatin1String("SetRejectAllMotions"))
		{
			m_rejectAllMotions = message.arguments().value(0).toBool();
			connection.send(message.createReply());
			return true;
		}

		if (message.interface() == TestInterface && method == QLatin1String("DenyNextStart"))
		{
			m_denyNextStart = true;
			connection.send(message.createReply());
			return true;
		}

		if (method == QLatin1String("Close"))
		{
			m_closedSessions.append(message.path());
			connection.send(message.createReply());
			return true;
		}

		if (method == QLatin1String("NotifyKeyboardKeysym"))
		{
			handleKeysym(message, connection);
			return true;
		}

		if (method == QLatin1String("NotifyPointerMotionAbsolute"))
		{
			handlePointerMotion(message, connection);
			return true;
		}

		if (method == QLatin1String("NotifyPointerButton"))
		{
			const auto arguments = message.arguments();
			m_forwardedInput.append(QStringLiteral("%1:button:%2:%3")
										.arg(sessionNumber(arguments.value(0)))
										.arg(arguments.value(2).toInt())
										.arg(arguments.value(3).toUInt()));
			connection.send(message.createReply());
			return true;
		}

		if (method == QLatin1String("OpenPipeWireRemote"))
		{
			const int fd = ::open("/dev/null", O_RDONLY);
			const QDBusUnixFileDescriptor descriptor(fd);
			::close(fd);
			connection.send(message.createReply({QVariant::fromValue(descriptor)}));
			return true;
		}

		if (method == QLatin1String("CreateSession") || method == QLatin1String("SelectDevices") ||
			method == QLatin1String("SelectSources") || method == QLatin1String("Start"))
		{
			handleRequest(message, connection);
			return true;
		}

		return false;
	}

private:
	static QString sessionNumber(const QVariant& sessionHandle)
	{
		return qvariant_cast<QDBusObjectPath>(sessionHandle).path().mid(SessionPathPrefix.size());
	}

	void handlePointerMotion(const QDBusMessage& message, const QDBusConnection& connection)
	{
		const auto arguments = message.arguments();
		const auto stream = arguments.value(2).toUInt();
		const auto x = arguments.value(3).toDouble();
		const auto y = arguments.value(4).toDouble();

		const auto event = QStringLiteral("%1:motion:%2,%3")
							   .arg(sessionNumber(arguments.value(0)))
							   .arg(x, 0, 'f', 2)
							   .arg(y, 0, 'f', 2);

		if (m_hangMotions > 0)
		{
			--m_hangMotions;
			m_rejectedInput.append(event);
			return;
		}

		// the check of xdg-desktop-portal >= 1.14 against the stream size from the Start response,
		// which rejects every position without a size
		if (stream != StreamNodeId || m_rejectAllMotions || m_streamSize.isEmpty() ||
			x < 0 || x >= m_streamSize.width() || y < 0 || y >= m_streamSize.height())
		{
			m_rejectedInput.append(event);
			connection.send(message.createErrorReply(QDBusError::Failed, QStringLiteral("Invalid position")));
			return;
		}

		m_forwardedInput.append(event);
		connection.send(message.createReply());
	}

	void handleKeysym(const QDBusMessage& message, const QDBusConnection& connection)
	{
		const auto arguments = message.arguments();
		const auto sessionPath = qvariant_cast<QDBusObjectPath>(arguments.value(0)).path();
		const auto keySym = arguments.value(2).toUInt();
		const auto state = arguments.value(3).toUInt();
		const auto event = QStringLiteral("%1:%2:%3")
							   .arg(sessionPath.mid(SessionPathPrefix.size()))
							   .arg(keySym, 0, 16)
							   .arg(state);

		if (state == 0 && m_releaseFaults.contains(keySym))
		{
			auto& faults = m_releaseFaults[keySym];
			if (faults.first > 0)
			{
				--faults.first;
				m_rejectedInput.append(event);
				connection.send(message.createErrorReply(QDBusError::Failed, QStringLiteral("release failed")));
				return;
			}
			if (faults.second > 0)
			{
				--faults.second;
				m_rejectedInput.append(event);
				return;
			}
		}

		if (keySym == DeniedKeySym)
		{
			connection.send(message.createErrorReply(QDBusError::AccessDenied, QStringLiteral("no keyboard access")));
		}
		else if (keySym == HangingKeySym)
		{
			// no reply
		}
		else if (keySym == SlowKeySym)
		{
			QTimer::singleShot(SlowKeyDelay, this, [this, message, connection, event]() {
				m_forwardedInput.append(event);
				connection.send(message.createReply());
			});
		}
		else
		{
			m_forwardedInput.append(event);
			connection.send(message.createReply());
		}
	}

	void handleRequest(const QDBusMessage& message, const QDBusConnection& connection)
	{
		const auto options = qdbus_cast<QVariantMap>(message.arguments().constLast());
		const auto sender = QString(message.service()).remove(QLatin1Char(':')).replace(QLatin1Char('.'), QLatin1Char('_'));
		const auto requestPath = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2")
									 .arg(sender, options.value(QStringLiteral("handle_token")).toString());

		// 2 = cancelled by the user
		uint response = 0;
		if (message.member() == QLatin1String("Start") && m_denyNextStart)
		{
			m_denyNextStart = false;
			response = 2;
		}

		const auto sessionHandle = qvariant_cast<QDBusObjectPath>(message.arguments().value(0)).path();

		QVariantMap results;
		if (message.member() == QLatin1String("CreateSession"))
		{
			results[QStringLiteral("session_handle")] = QString(SessionPathPrefix + QString::number(++m_sessionCount));
		}
		else if (message.member() == QLatin1String("SelectDevices"))
		{
			handleSelectDevices(sessionHandle, options);
		}
		else if (message.member() == QLatin1String("Start"))
		{
			++m_starts;
			const auto session = m_sessions.take(sessionHandle);
			if (session.restored == false)
			{
				// the consent dialog
				++m_dialogs;
			}
			// like xdg-desktop-portal: the token is stored again (or a new one) with a successful Start
			if (response == 0 && session.persist)
			{
				m_lastToken = session.restored && m_rotateTokens == false ? session.token : makeToken();
				m_tokens.insert(m_lastToken);
				results[QStringLiteral("restore_token")] = m_lastToken;
			}

			const auto withStreams = m_streamsMode == StreamsMode::Always ||
				(m_streamsMode == StreamsMode::NotOnRestore && session.restored == false);
			Stream stream{StreamNodeId, {}};
			stream.properties[QStringLiteral("position")] =
				QVariant::fromValue(IntPair{m_streamPosition.x(), m_streamPosition.y()});
			// old portals don't report the size
			if (m_streamSize.isEmpty() == false)
			{
				stream.properties[QStringLiteral("size")] =
					QVariant::fromValue(IntPair{m_streamSize.width(), m_streamSize.height()});
			}
			results[QStringLiteral("streams")] =
				QVariant::fromValue(withStreams ? QList<Stream>{stream} : QList<Stream>{});
		}

		connection.send(message.createReply({QVariant::fromValue(QDBusObjectPath(requestPath))}));

		QTimer::singleShot(10, this, [connection, requestPath, response, results]() {
			auto responseSignal = QDBusMessage::createSignal(requestPath, QStringLiteral("org.freedesktop.portal.Request"),
															 QStringLiteral("Response"));
			responseSignal.setArguments({response, results});
			connection.send(responseSignal);
		});
	}

	enum class StreamsMode {
		Always,
		NotOnRestore,
		Never
	};

	struct Session
	{
		bool persist = false;
		bool restored = false;
		QString token;
	};

	void handleSelectDevices(const QString& sessionHandle, const QVariantMap& options)
	{
		QStringList entry;
		Session session;
		if (options.contains(QStringLiteral("persist_mode")))
		{
			entry.append(QStringLiteral("persist_mode=%1").arg(options.value(QStringLiteral("persist_mode")).toUInt()));
			session.persist = options.value(QStringLiteral("persist_mode")).toUInt() == 2;
		}
		if (options.contains(QStringLiteral("restore_token")))
		{
			const auto token = options.value(QStringLiteral("restore_token")).toString();
			entry.append(QStringLiteral("restore_token=") + token);
			// like xdg-desktop-portal: the entry is deleted when looked up
			session.restored = m_tokens.remove(token);
			session.token = token;
		}
		m_selectDevicesLog.append(entry.join(QLatin1Char(';')));
		m_sessions[sessionHandle] = session;
	}

	QString makeToken()
	{
		// alternately like xdg-desktop-portal < 1.23 (UUID) and newer versions
		++m_tokenCount;
		if (m_tokenCount % 2 == 0)
		{
			return QStringLiteral("Tok_%1abcdefghijklmnop").arg(m_tokenCount);
		}
		return QUuid::createUuid().toString(QUuid::WithoutBraces);
	}

	uint m_version = 2;
	StreamsMode m_streamsMode = StreamsMode::Always;
	bool m_rotateTokens = false;
	bool m_screenLocked = false;
	int m_tokenCount = 0;
	int m_dialogs = 0;
	int m_starts = 0;
	QString m_lastToken;
	QSet<QString> m_tokens;
	QHash<QString, Session> m_sessions;
	QStringList m_selectDevicesLog;
	QStringList m_registrations;
	QSet<QString> m_registeredSenders;
	QMap<QString, QString> m_firstCalls;

	int m_sessionCount = 0;
	bool m_denyNextStart = false;
	QPoint m_streamPosition;
	QSize m_streamSize{1920, 1080};
	bool m_rejectAllMotions = false;
	int m_hangMotions = 0;
	QStringList m_closedSessions;
	QStringList m_forwardedInput;
	QStringList m_rejectedInput;
	QHash<quint32, QPair<int, int>> m_releaseFaults;
};



class PortalSessionTest : public QObject
{
	Q_OBJECT
private Q_SLOTS:
	void initTestCase()
	{
		if (qEnvironmentVariableIsSet("VEYON_TEST_PRIVATE_SESSION_BUS") == false)
		{
			QSKIP("requires a private session bus (run through ctest with dbus-run-session)");
		}

		QVERIFY(m_home.isValid());

		// keep the restore token file and the KDE pre-authorization (flatpak) inside the fixture
		QFile flatpak(m_home.filePath(QStringLiteral("flatpak")));
		QVERIFY(flatpak.open(QFile::WriteOnly));
		flatpak.write("#!/bin/sh\nexit 0\n");
		flatpak.close();
		QVERIFY(flatpak.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
		qputenv("PATH", m_home.path().toUtf8() + ':' + qgetenv("PATH"));
		qputenv("HOME", m_home.path().toUtf8());
		qputenv("XDG_STATE_HOME", m_home.filePath(QStringLiteral("state")).toUtf8());

		m_portal.setProcessChannelMode(QProcess::ForwardedErrorChannel);
		m_portal.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--fake-portal")});
		QVERIFY(m_portal.waitForStarted());
		QVERIFY(m_portal.waitForReadyRead());
		QCOMPARE(m_portal.readAllStandardOutput().trimmed(), QByteArrayLiteral("ready"));
	}

	void cleanupTestCase()
	{
		m_portal.terminate();
		if (m_portal.waitForFinished(2000) == false)
		{
			m_portal.kill();
			m_portal.waitForFinished();
		}
	}

	void keepsInputOrder()
	{
		PortalSession session(false);
		QVERIFY(startSession(session));
		const auto sessionNumber = currentSessionNumber();

		session.notifyKeyboardKeysym(SlowKeySym, true);
		session.notifyKeyboardKeysym('A', true);
		session.notifyKeyboardKeysym('A', false);
		session.notifyKeyboardKeysym(SlowKeySym, false);

		const QStringList expected{
			keyEvent(sessionNumber, SlowKeySym, 1),
			keyEvent(sessionNumber, 'A', 1),
			keyEvent(sessionNumber, 'A', 0),
			keyEvent(sessionNumber, SlowKeySym, 0),
		};
		QTRY_COMPARE(forwardedInput(sessionNumber).size(), expected.size());
		QCOMPARE(forwardedInput(sessionNumber), expected);
	}

	void errorReplyKeepsSession()
	{
		PortalSession session(false);
		QSignalSpy failedSpy(&session, &PortalSession::failed);
		QVERIFY(startSession(session));
		const auto sessionNumber = currentSessionNumber();

		session.notifyKeyboardKeysym(DeniedKeySym, true);
		session.notifyKeyboardKeysym('b', true);
		session.notifyKeyboardKeysym('b', false);

		QTRY_COMPARE(forwardedInput(sessionNumber),
					 (QStringList{keyEvent(sessionNumber, 'b', 1), keyEvent(sessionNumber, 'b', 0)}));
		QCOMPARE(failedSpy.size(), 0);
		QCOMPARE(session.state(), PortalSession::State::Running);
	}

	// P23-2: a portal that stalls briefly must not restart the session
	void singleMissingReplyKeepsSession()
	{
		PortalSession session(false);
		QSignalSpy failedSpy(&session, &PortalSession::failed);
		QVERIFY(startSession(session));
		const auto sessionNumber = currentSessionNumber();

		session.notifyKeyboardKeysym(HangingKeySym, true);
		session.notifyKeyboardKeysym('c', true);
		QTest::qWait(TestReplyTimeout + 300);
		QCOMPARE(failedSpy.size(), 0);
		QCOMPARE(session.state(), PortalSession::State::Running);

		// the pending input has been dropped
		session.notifyKeyboardKeysym('d', true);
		session.notifyKeyboardKeysym('d', false);
		QTRY_COMPARE(forwardedInput(sessionNumber),
					 (QStringList{keyEvent(sessionNumber, 'd', 1), keyEvent(sessionNumber, 'd', 0)}));
		QCOMPARE(failedSpy.size(), 0);
		QVERIFY(closedSessions().contains(SessionPathPrefix + QString::number(sessionNumber)) == false);
	}

	void consecutiveMissingRepliesCloseSessionAndRestartWorks()
	{
		PortalSession session(false);
		QSignalSpy failedSpy(&session, &PortalSession::failed);
		QVERIFY(startSession(session));
		const auto failedSessionNumber = currentSessionNumber();

		session.notifyKeyboardKeysym(HangingKeySym, true);
		session.notifyKeyboardKeysym('c', true);
		QTest::qWait(TestReplyTimeout + 300);
		QCOMPARE(failedSpy.size(), 0);
		session.notifyKeyboardKeysym(HangingKeySym, false);

		QTRY_COMPARE_WITH_TIMEOUT(failedSpy.size(), 1, TestReplyTimeout + 3000);
		QCOMPARE(session.state(), PortalSession::State::Failed);
		QTRY_VERIFY(closedSessions().contains(SessionPathPrefix + QString::number(failedSessionNumber)));

		// how PipeWireCaptureServer retries
		QVERIFY(startSession(session));
		const auto sessionNumber = currentSessionNumber();
		QVERIFY(sessionNumber != failedSessionNumber);

		session.notifyKeyboardKeysym('d', true);
		session.notifyKeyboardKeysym('d', false);
		QTRY_COMPARE(forwardedInput(sessionNumber),
					 (QStringList{keyEvent(sessionNumber, 'd', 1), keyEvent(sessionNumber, 'd', 0)}));
		// the press of 'c' has been dropped, but whatever is held is released when closing
		QCOMPARE(forwardedInput(failedSessionNumber), QStringList{keyEvent(failedSessionNumber, 'c', 0)});
		QCOMPARE(failedSpy.size(), 1);
	}

	// the report from KDE Plasma: a client moving the pointer outside of the stream made the
	// session fail and restart
	void rejectedPositionsKeepSession()
	{
		setStream({}, {1920, 1080});

		PortalSession session(false);
		QSignalSpy failedSpy(&session, &PortalSession::failed);
		QVERIFY(startSession(session));
		const auto sessionNumber = currentSessionNumber();

		// the fake portal rejects every position now, as with a stream size that doesn't match
		setRejectAllMotions(true);
		for (int i = 0; i < PortalInputDispatcher::MaximumConsecutiveErrors * 2; ++i)
		{
			session.notifyPointer(0, 100 + i, 100, {1920, 1080});
			QTest::qWait(10);
		}
		session.notifyKeyboardKeysym('m', true);
		QTRY_COMPARE(forwardedInput(sessionNumber), QStringList{keyEvent(sessionNumber, 'm', 1)});
		setRejectAllMotions(false);

		session.notifyPointer(0, 500, 400, {1920, 1080});
		QTRY_COMPARE(forwardedInput(sessionNumber).size(), 2);
		QCOMPARE(forwardedInput(sessionNumber).constLast(), pointerEvent(sessionNumber, 500, 400));
		QCOMPARE(failedSpy.size(), 0);
		QCOMPARE(session.state(), PortalSession::State::Running);
	}

	void restartClosesFailedSession()
	{
		PortalSession session(false);
		QSignalSpy failedSpy(&session, &PortalSession::failed);

		QDBusConnection::sessionBus().call(QDBusMessage::createMethodCall(
			FakePortalService, FakePortalObjectPath, TestInterface, QStringLiteral("DenyNextStart")));
		session.start();
		QTRY_COMPARE(failedSpy.size(), 1);
		QCOMPARE(session.state(), PortalSession::State::Failed);
		const auto failedSessionNumber = ++m_sessionNumber;
		const QString failedSessionPath = SessionPathPrefix + QString::number(failedSessionNumber);
		QVERIFY(closedSessions().contains(failedSessionPath) == false);

		QVERIFY(startSession(session));
		QVERIFY(closedSessions().contains(failedSessionPath));

		const auto sessionNumber = currentSessionNumber();
		session.notifyKeyboardKeysym('e', true);
		QTRY_COMPARE(forwardedInput(sessionNumber), QStringList{keyEvent(sessionNumber, 'e', 1)});
	}

	void consecutiveErrorsCloseSession()
	{
		PortalSession session(false);
		QSignalSpy failedSpy(&session, &PortalSession::failed);
		QVERIFY(startSession(session));
		const auto sessionNumber = currentSessionNumber();

		for (int i = 0; i < PortalInputDispatcher::MaximumConsecutiveErrors; ++i)
		{
			session.notifyKeyboardKeysym(DeniedKeySym, i % 2 == 0);
		}

		QTRY_COMPARE(failedSpy.size(), 1);
		QCOMPARE(session.state(), PortalSession::State::Failed);
		QTRY_VERIFY(closedSessions().contains(SessionPathPrefix + QString::number(sessionNumber)));
	}

	// KDE Plasma with a 4K output at 150 %: logical stream size, buffers in physical pixels
	void pointerUsesStreamCoordinates()
	{
		setStream({3840, 0}, {2560, 1440});

		PortalSession session(false);
		QSignalSpy failedSpy(&session, &PortalSession::failed);
		QVERIFY(startSession(session));
		const auto sessionNumber = currentSessionNumber();
		QCOMPARE(session.pipeWireNodeId(), StreamNodeId);
		QCOMPARE(session.streamGeometry().position, QPoint(3840, 0));
		QCOMPARE(session.streamGeometry().size, QSize(2560, 1440));

		const QSize framebufferSize(3840, 2160);
		session.notifyPointer(0, 1920, 1080, framebufferSize);
		QTRY_COMPARE(forwardedInput(sessionNumber), QStringList{pointerEvent(sessionNumber, 1280, 720)});

		// beyond the logical width: "Invalid position" before
		session.notifyPointer(0, 3000, 2000, framebufferSize);
		QTRY_COMPARE(forwardedInput(sessionNumber).size(), 2);
		QCOMPARE(forwardedInput(sessionNumber).constLast(), pointerEvent(sessionNumber, 2000, 2000.0 * 1440 / 2160));

		// the last pixel and anything beyond is clamped to the stream
		session.notifyPointer(0, 3839, 2159, framebufferSize);
		QTRY_COMPARE(forwardedInput(sessionNumber).size(), 3);
		QCOMPARE(forwardedInput(sessionNumber).constLast(),
				 pointerEvent(sessionNumber, 3839.0 * 2560 / 3840, 2159.0 * 1440 / 2160));
		session.notifyPointer(1, 5000, 3000, framebufferSize);
		QTRY_COMPARE(forwardedInput(sessionNumber).size(), 4);
		QCOMPARE(forwardedInput(sessionNumber).constLast(), buttonEvent(sessionNumber, BTN_LEFT, 1));

		// framebuffer resized at runtime
		session.notifyPointer(0, 960, 540, {1920, 1080});
		QTRY_COMPARE(forwardedInput(sessionNumber).size(), 6);
		QCOMPARE(forwardedInput(sessionNumber).mid(4),
				 (QStringList{pointerEvent(sessionNumber, 1280, 720), buttonEvent(sessionNumber, BTN_LEFT, 0)}));

		QCOMPARE(failedSpy.size(), 0);
		QCOMPARE(session.state(), PortalSession::State::Running);
	}

	// mutter takes positions in buffer pixels and divides by the scale itself
	void pointerOnMutterIsNotScaled()
	{
		setStream({}, {2560, 1440});
		setMutter(true);

		PortalSession session(false);
		QSignalSpy failedSpy(&session, &PortalSession::failed);
		QVERIFY(startSession(session));
		const auto sessionNumber = currentSessionNumber();
		QCOMPARE(session.pointerMappingMode(), PortalPointerMappingMode::ClampOnly);

		const QSize framebufferSize(3840, 2160);
		session.notifyPointer(0, 1920, 1080, framebufferSize);
		QTRY_COMPARE(forwardedInput(sessionNumber), QStringList{pointerEvent(sessionNumber, 1920, 1080)});

		// beyond the logical size: unreachable through Notify*, clamped to the stream
		session.notifyPointer(0, 3000, 2000, framebufferSize);
		QTRY_COMPARE(forwardedInput(sessionNumber).size(), 2);
		QCOMPARE(forwardedInput(sessionNumber).constLast(), pointerEvent(sessionNumber, 2559, 1439));
		QCOMPARE(failedSpy.size(), 0);

		setMutter(false);
		session.close();

		PortalSession kdeSession(false);
		QVERIFY(startSession(kdeSession));
		QCOMPARE(kdeSession.pointerMappingMode(), PortalPointerMappingMode::Scale);

		setStream({}, {1920, 1080});
	}

	void pointerWithoutStreamSize()
	{
		setStream({}, {});

		PortalSession session(false);
		QSignalSpy failedSpy(&session, &PortalSession::failed);
		QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("no stream size")));
		QVERIFY(startSession(session));
		const auto sessionNumber = currentSessionNumber();
		QVERIFY(session.streamGeometry().size.isEmpty());

		// passed unscaled (portals < 1.14), but rejected by newer ones; the session stays usable
		session.notifyPointer(0, 3000, 2000, {3840, 2160});
		QTRY_COMPARE(rejectedInput(sessionNumber), QStringList{pointerEvent(sessionNumber, 3000, 2000)});
		session.notifyPointer(0, 4000, 2000, {3840, 2160});
		QTRY_COMPARE(rejectedInput(sessionNumber).size(), 2);
		QCOMPARE(rejectedInput(sessionNumber).constLast(), pointerEvent(sessionNumber, 3839, 2000));

		session.notifyKeyboardKeysym('w', true);
		QTRY_COMPARE(forwardedInput(sessionNumber), QStringList{keyEvent(sessionNumber, 'w', 1)});
		QCOMPARE(failedSpy.size(), 0);

		setStream({}, {1920, 1080});
	}

	// P23-3: a missing reply must not drop releases, else keys stay pressed in the compositor
	void timeoutKeepsAndRetriesReleases()
	{
		PortalSession session(false);
		QSignalSpy failedSpy(&session, &PortalSession::failed);
		QVERIFY(startSession(session));
		const auto sessionNumber = currentSessionNumber();

		session.notifyKeyboardKeysym('t', true);
		QTRY_COMPARE(forwardedInput(sessionNumber), QStringList{keyEvent(sessionNumber, 't', 1)});

		// the release of 't' is not replied to, the release of 'a' and a motion are pending meanwhile
		faultReleases('t', 0, 1);
		session.notifyKeyboardKeysym('t', false);
		session.notifyKeyboardKeysym('a', true);
		session.notifyKeyboardKeysym('a', false);
		session.notifyPointer(0, 100, 100, {1920, 1080});

		QTest::qWait(TestReplyTimeout + 300);
		// the press of 'a' is dropped, the release, the motion and 't' again arrive
		QTRY_COMPARE(forwardedInput(sessionNumber),
					 (QStringList{keyEvent(sessionNumber, 't', 1), keyEvent(sessionNumber, 'a', 0),
								  pointerEvent(sessionNumber, 100, 100), keyEvent(sessionNumber, 't', 0)}));
		QCOMPARE(rejectedInput(sessionNumber), QStringList{keyEvent(sessionNumber, 't', 0)});
		QCOMPARE(failedSpy.size(), 0);

		// nothing is held anymore, so closing sends no further releases
		session.close();
		QTRY_VERIFY(closedSessions().contains(SessionPathPrefix + QString::number(sessionNumber)));
		QCOMPARE(forwardedInput(sessionNumber).size(), 4);
	}

	void releaseWithoutReplyIsReleasedOnClose()
	{
		PortalSession session(false);
		QSignalSpy failedSpy(&session, &PortalSession::failed);
		QVERIFY(startSession(session));
		const auto sessionNumber = currentSessionNumber();

		session.notifyKeyboardKeysym('t', true);
		QTRY_COMPARE(forwardedInput(sessionNumber), QStringList{keyEvent(sessionNumber, 't', 1)});

		// the release and its retry are not replied to, the release of 'a' in between is
		faultReleases('t', 0, 2);
		QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("could not release key")));
		session.notifyKeyboardKeysym('t', false);
		session.notifyKeyboardKeysym('a', true);
		session.notifyKeyboardKeysym('a', false);
		QTRY_COMPARE_WITH_TIMEOUT(rejectedInput(sessionNumber).size(), 2, 3000);
		QTest::qWait(TestReplyTimeout + 300);
		QCOMPARE(forwardedInput(sessionNumber),
				 (QStringList{keyEvent(sessionNumber, 't', 1), keyEvent(sessionNumber, 'a', 0)}));
		QCOMPARE(failedSpy.size(), 0);

		session.close();
		QTRY_VERIFY(closedSessions().contains(SessionPathPrefix + QString::number(sessionNumber)));
		QCOMPARE(forwardedInput(sessionNumber),
				 (QStringList{keyEvent(sessionNumber, 't', 1), keyEvent(sessionNumber, 'a', 0),
							  keyEvent(sessionNumber, 't', 0)}));
	}

	// an error reply would be repeated, e.g. without access to the keyboard
	void rejectedReleaseIsNotRetried()
	{
		PortalSession session(false);
		QSignalSpy failedSpy(&session, &PortalSession::failed);
		QVERIFY(startSession(session));
		const auto sessionNumber = currentSessionNumber();

		faultReleases('r', 1, 0);
		session.notifyKeyboardKeysym('r', true);
		session.notifyKeyboardKeysym('r', false);
		QTRY_COMPARE(rejectedInput(sessionNumber), QStringList{keyEvent(sessionNumber, 'r', 0)});
		QTest::qWait(100);
		QCOMPARE(forwardedInput(sessionNumber), QStringList{keyEvent(sessionNumber, 'r', 1)});

		// held button
		session.notifyPointer(1, 10, 10, {1920, 1080});
		QTRY_COMPARE(forwardedInput(sessionNumber).size(), 3);
		QCOMPARE(forwardedInput(sessionNumber).constLast(), buttonEvent(sessionNumber, BTN_LEFT, 1));
		QCOMPARE(failedSpy.size(), 0);

		// the rejected release isn't sent again, the button is released
		session.close();
		QTRY_VERIFY(closedSessions().contains(SessionPathPrefix + QString::number(sessionNumber)));
		const auto input = forwardedInput(sessionNumber);
		QCOMPARE(input, (QStringList{keyEvent(sessionNumber, 'r', 1), pointerEvent(sessionNumber, 10, 10),
									 buttonEvent(sessionNumber, BTN_LEFT, 1), buttonEvent(sessionNumber, BTN_LEFT, 0)}));
		QCOMPARE(rejectedInput(sessionNumber).size(), 1);
	}

	// a click without moving after a missing reply must land at the client's position
	void pointerPositionSurvivesMissingReply()
	{
		PortalSession session(false);
		QSignalSpy failedSpy(&session, &PortalSession::failed);
		QVERIFY(startSession(session));
		const auto sessionNumber = currentSessionNumber();
		const QSize framebufferSize(1920, 1080);

		// the motion itself is not replied to
		hangMotions(1);
		session.notifyPointer(0, 500, 400, framebufferSize);
		QTRY_COMPARE(rejectedInput(sessionNumber), QStringList{pointerEvent(sessionNumber, 500, 400)});
		QTest::qWait(TestReplyTimeout + 300);
		session.notifyPointer(1, 500, 400, framebufferSize);
		QTRY_COMPARE(forwardedInput(sessionNumber),
					 (QStringList{pointerEvent(sessionNumber, 500, 400), buttonEvent(sessionNumber, BTN_LEFT, 1)}));

		// the motion is pending while another request is not replied to
		session.notifyKeyboardKeysym(HangingKeySym, true);
		session.notifyPointer(1, 600, 300, framebufferSize);
		QTest::qWait(TestReplyTimeout + 300);
		session.notifyPointer(0, 600, 300, framebufferSize);
		QTRY_COMPARE(forwardedInput(sessionNumber).size(), 4);
		QCOMPARE(forwardedInput(sessionNumber).mid(2),
				 (QStringList{pointerEvent(sessionNumber, 600, 300), buttonEvent(sessionNumber, BTN_LEFT, 0)}));
		QCOMPARE(failedSpy.size(), 0);
	}

	// close() waits for the releases, but not for long if the portal doesn't reply
	void closeWaitsBoundedForReleases()
	{
		PortalSession session(false);
		QVERIFY(startSession(session));
		const auto sessionNumber = currentSessionNumber();

		session.notifyKeyboardKeysym('z', true);
		session.notifyKeyboardKeysym('y', true);
		QTRY_COMPARE(forwardedInput(sessionNumber).size(), 2);

		faultReleases('z', 0, 1);
		QElapsedTimer timer;
		timer.start();
		session.close();
		const auto elapsed = timer.elapsed();
		QVERIFY2(elapsed < PortalSession::ReleaseOnCloseTimeout + 500, qPrintable(QString::number(elapsed)));

		QTRY_VERIFY(closedSessions().contains(SessionPathPrefix + QString::number(sessionNumber)));
		QCOMPARE(rejectedInput(sessionNumber), QStringList{keyEvent(sessionNumber, 'z', 0)});
		QCOMPARE(forwardedInput(sessionNumber).constLast(), keyEvent(sessionNumber, 'y', 0));
		QCOMPARE(session.state(), PortalSession::State::Idle);
	}

	// the app ID must be known before the portal derives one from the first call of the connection
	void registersAppIdBeforeAnyOtherCall()
	{
		PortalSession session(false);
		QVERIFY(startSession(session));
		PortalSession secondSession(false);
		QVERIFY(startSession(secondSession));

		// once per connection, though both sessions use it
		const auto registrations = persistenceState().value(3).toStringList();
		QCOMPARE(registrations.size(), 1);
		const auto sender = registrations.constFirst().section(QLatin1Char(':'), 0, -2);
		QCOMPARE(registrations.constFirst().section(QLatin1Char(':'), -1), TestAppId);
		QVERIFY(persistenceState().value(4).toStringList().contains(sender + QStringLiteral("=Register")));
	}

	// RemoteDesktop version 1 (xdg-desktop-portal < 1.17, e.g. Ubuntu 22.04) has no persistence
	void noPersistModeBeforeVersion2()
	{
		removeTokenFiles();
		setVersion(1);
		writeTokenFile(PortalSession::restoreTokenFilePath(), QUuid::createUuid().toString(QUuid::WithoutBraces));

		PortalSession session(true);
		QVERIFY(session.isPersistenceSupported() == false);
		QVERIFY(session.hasRestoreToken() == false);
		QVERIFY(startSession(session));
		QCOMPARE(lastSelectDevices(), QString{});

		setVersion(2);
		removeTokenFiles();
	}

	void restoreTokenRoundTrip()
	{
		removeTokenFiles();
		const auto dialogs = dialogCount();

		{
			PortalSession session(true);
			QVERIFY(session.isPersistenceSupported());
			QVERIFY(session.hasRestoreToken() == false);
			QVERIFY(startSession(session));
			QCOMPARE(lastSelectDevices(), QStringLiteral("persist_mode=2"));
			QCOMPARE(dialogCount(), dialogs + 1);
		}

		const auto token = lastToken();
		QCOMPARE(readTokenFile(), token);

		// the next run of the helper doesn't ask
		PortalSession session(true);
		QVERIFY(session.hasRestoreToken());
		QVERIFY(startSession(session));
		QCOMPARE(lastSelectDevices(), QStringLiteral("persist_mode=2;restore_token=") + token);
		QCOMPARE(dialogCount(), dialogs + 1);
		QCOMPARE(readTokenFile(), token);
	}

	// the portal may hand out a new token on every start, which replaces the old one
	void restoreTokenIsReplaced()
	{
		removeTokenFiles();
		{
			PortalSession session(true);
			QVERIFY(startSession(session));
		}
		const auto oldToken = readTokenFile();
		QVERIFY(oldToken.isEmpty() == false);

		setRotateTokens(true);
		PortalSession session(true);
		QVERIFY(startSession(session));
		setRotateTokens(false);
		QCOMPARE(lastSelectDevices(), QStringLiteral("persist_mode=2;restore_token=") + oldToken);

		const auto newToken = lastToken();
		QVERIFY(newToken != oldToken);
		QCOMPARE(readTokenFile(), newToken);
		// replaced by renaming, no temporary file is left
		QCOMPARE(QDir(tokenDirectory()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
				 QStringList{QStringLiteral("portal-restore-token")});
		QCOMPARE(fileMode(PortalSession::restoreTokenFilePath()), 0600);
	}

	void tokenFileAndDirectoryModes()
	{
		removeTokenFiles();
		QDir().rmdir(tokenDirectory());

		{
			PortalSession session(true);
			QVERIFY(startSession(session));
		}
		QCOMPARE(fileMode(tokenDirectory()), 0700);
		QCOMPARE(fileMode(PortalSession::restoreTokenFilePath()), 0600);

		// e.g. created by an older version with the default umask
		QVERIFY(::chmod(QFile::encodeName(tokenDirectory()).constData(), 0755) == 0);
		QVERIFY(::chmod(QFile::encodeName(PortalSession::restoreTokenFilePath()).constData(), 0644) == 0);
		PortalSession session(true);
		QVERIFY(session.hasRestoreToken());
		QCOMPARE(fileMode(tokenDirectory()), 0700);
		QCOMPARE(fileMode(PortalSession::restoreTokenFilePath()), 0600);
	}

	void symbolicLinkIsRefused()
	{
		removeTokenFiles();
		const auto token = QUuid::createUuid().toString(QUuid::WithoutBraces);
		addToken(token);
		const auto target = m_home.filePath(QStringLiteral("elsewhere"));
		writeTokenFile(target, token);
		QDir().mkpath(tokenDirectory());
		QVERIFY(QFile::link(target, PortalSession::restoreTokenFilePath()));

		QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("symbolic link")));
		PortalSession session(true);
		QVERIFY(session.hasRestoreToken() == false);
		QVERIFY(startSession(session));
		QCOMPARE(lastSelectDevices(), QStringLiteral("persist_mode=2"));

		// the new token replaces the link, not the file it points to
		QVERIFY(QFileInfo(PortalSession::restoreTokenFilePath()).isSymLink() == false);
		QCOMPARE(readTokenFile(), lastToken());
		QFile targetFile(target);
		QVERIFY(targetFile.open(QFile::ReadOnly));
		QCOMPARE(QString::fromUtf8(targetFile.readAll()), token);
		targetFile.remove();
	}

	void foreignTokenFileIsRefused()
	{
		if (::getuid() != 0)
		{
			QSKIP("changing the owner of a file needs root");
		}

		removeTokenFiles();
		writeTokenFile(PortalSession::restoreTokenFilePath(), QUuid::createUuid().toString(QUuid::WithoutBraces));
		QVERIFY(::chown(QFile::encodeName(PortalSession::restoreTokenFilePath()).constData(), 65534, 65534) == 0);

		QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("not a regular file owned by the user")));
		PortalSession session(true);
		QVERIFY(session.hasRestoreToken() == false);
		removeTokenFiles();
	}

	void legacyTokenIsMigrated()
	{
		removeTokenFiles();
		const auto token = QUuid::createUuid().toString(QUuid::WithoutBraces);
		addToken(token);
		const auto legacyFilePath = PortalSession::legacyRestoreTokenFilePath();
		QVERIFY(legacyFilePath != PortalSession::restoreTokenFilePath());
		writeTokenFile(legacyFilePath, token);

		PortalSession session(true);
		QVERIFY(session.hasRestoreToken());
		QCOMPARE(readTokenFile(), token);
		QCOMPARE(fileMode(PortalSession::restoreTokenFilePath()), 0600);
		QVERIFY(QFileInfo::exists(legacyFilePath) == false);
		QVERIFY(QFileInfo::exists(QFileInfo(legacyFilePath).path()) == false);

		QVERIFY(startSession(session));
		QCOMPARE(lastSelectDevices(), QStringLiteral("persist_mode=2;restore_token=") + token);
	}

	void disabledPersistenceRemovesTokenFiles()
	{
		writeTokenFile(PortalSession::restoreTokenFilePath(), QUuid::createUuid().toString(QUuid::WithoutBraces));
		writeTokenFile(PortalSession::legacyRestoreTokenFilePath(), QUuid::createUuid().toString(QUuid::WithoutBraces));

		PortalSession session(false);
		QVERIFY(session.hasRestoreToken() == false);
		QVERIFY(QFileInfo::exists(PortalSession::restoreTokenFilePath()) == false);
		QVERIFY(QFileInfo::exists(PortalSession::legacyRestoreTokenFilePath()) == false);
	}

	// the portal deleted its entry when looking the token up, so it is of no use anymore
	void failedStartDropsToken()
	{
		removeTokenFiles();
		{
			PortalSession session(true);
			QVERIFY(startSession(session));
		}
		QVERIFY(readTokenFile().isEmpty() == false);

		PortalSession session(true);
		QSignalSpy failedSpy(&session, &PortalSession::failed);
		denyNextStart();
		session.start();
		QTRY_COMPARE(failedSpy.size(), 1);
		QVERIFY(lastSelectDevices().contains(QStringLiteral("restore_token=")));
		QVERIFY(session.hasRestoreToken() == false);
		QVERIFY(QFileInfo::exists(PortalSession::restoreTokenFilePath()) == false);

		// asks again
		const auto dialogs = dialogCount();
		QVERIFY(startSession(session));
		QCOMPARE(lastSelectDevices(), QStringLiteral("persist_mode=2"));
		QCOMPARE(dialogCount(), dialogs + 1);
	}

	// xdg-desktop-portal-gnome restores a session without its monitor instead of asking again
	void restoreWithoutStreamRetriesOnceWithoutToken()
	{
		removeTokenFiles();
		{
			PortalSession session(true);
			QVERIFY(startSession(session));
		}
		const auto oldToken = readTokenFile();
		const auto starts = startCount();
		const auto dialogs = dialogCount();
		const auto selectDevicesCount = selectDevicesLog().size();

		setStreams(1); // none when restored
		PortalSession session(true);
		QSignalSpy failedSpy(&session, &PortalSession::failed);
		QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("without a usable stream")));
		QVERIFY(startSession(session));
		setStreams(0);

		QCOMPARE(selectDevicesLog().mid(selectDevicesCount),
				 (QStringList{QStringLiteral("persist_mode=2;restore_token=") + oldToken, QStringLiteral("persist_mode=2")}));
		QCOMPARE(startCount(), starts + 2);
		QCOMPARE(dialogCount(), dialogs + 1);
		QCOMPARE(failedSpy.size(), 0);
		QCOMPARE(session.pipeWireNodeId(), StreamNodeId);
		QCOMPARE(readTokenFile(), lastToken());
		QVERIFY(lastToken() != oldToken);
	}

	// no endless loop if the portal never provides a stream
	void sessionWithoutStreamIsRetriedOnlyOnce()
	{
		removeTokenFiles();
		{
			PortalSession session(true);
			QVERIFY(startSession(session));
		}
		const auto starts = startCount();

		setStreams(2); // never
		PortalSession session(true);
		QSignalSpy failedSpy(&session, &PortalSession::failed);
		QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("without a usable stream")));
		QTest::ignoreMessage(QtCriticalMsg, QRegularExpression(QStringLiteral("without a usable stream")));
		session.start();
		QTRY_COMPARE(failedSpy.size(), 1);
		QTest::qWait(200);
		setStreams(0);

		QCOMPARE(startCount(), starts + 2);
		QCOMPARE(failedSpy.size(), 1);
		QCOMPARE(session.state(), PortalSession::State::Failed);
		syncSessionNumber();
	}

	// GNOME closes the session on screen lock or when the user stops sharing
	void closedSessionIsRestoredWithToken()
	{
		removeTokenFiles();
		PortalSession session(true);
		QSignalSpy closedSpy(&session, &PortalSession::closedByPortal);
		QSignalSpy failedSpy(&session, &PortalSession::failed);
		QVERIFY(startSession(session));
		const auto token = lastToken();
		const QString closedSessionPath = SessionPathPrefix + QString::number(currentSessionNumber());
		const auto dialogs = dialogCount();

		closePortalSession();
		QTRY_COMPARE(closedSpy.size(), 1);
		QCOMPARE(failedSpy.size(), 0);
		QCOMPARE(session.state(), PortalSession::State::Failed);
		QVERIFY(session.hasRestoreToken());
		// nothing to close anymore
		QVERIFY(closedSessions().contains(closedSessionPath) == false);

		// how PipeWireCaptureServer restarts it
		QVERIFY(startSession(session));
		QCOMPARE(lastSelectDevices(), QStringLiteral("persist_mode=2;restore_token=") + token);
		QCOMPARE(dialogCount(), dialogs);
		QCOMPARE(closedSpy.size(), 1);

		// a session closed by PortalSession itself is not reported
		session.close();
		QTest::qWait(100);
		QCOMPARE(closedSpy.size(), 1);
	}

	void detectsLockedScreen()
	{
		PortalSession session(false);
		QVERIFY(session.isScreenLocked() == false);

		setScreenLocked(1);
		QVERIFY(session.isScreenLocked());
		setScreenLocked(0);
		QVERIFY(session.isScreenLocked() == false);
		setScreenLocked(-1);
		QVERIFY(session.isScreenLocked() == false);
	}

private:
	static QVariantList persistenceState()
	{
		const auto request = QDBusMessage::createMethodCall(FakePortalService, FakePortalObjectPath, TestInterface,
															QStringLiteral("Persistence"));
		return QDBusConnection::sessionBus().call(request).arguments();
	}

	static void callFakePortal(const QString& method, const QVariantList& arguments = {})
	{
		auto request = QDBusMessage::createMethodCall(FakePortalService, FakePortalObjectPath, TestInterface, method);
		request.setArguments(arguments);
		QDBusConnection::sessionBus().call(request);
	}

	static void setVersion(uint version) { callFakePortal(QStringLiteral("SetVersion"), {version}); }
	static void setStreams(int mode) { callFakePortal(QStringLiteral("SetStreams"), {mode}); }
	static void setRotateTokens(bool rotate) { callFakePortal(QStringLiteral("SetRotateTokens"), {rotate}); }
	static void addToken(const QString& token) { callFakePortal(QStringLiteral("AddToken"), {token}); }
	static void closePortalSession()
	{
		const auto reply = QDBusConnection::sessionBus().call(QDBusMessage::createMethodCall(
			FakePortalService, FakePortalObjectPath, TestInterface, QStringLiteral("CloseSession")));
		QVERIFY(reply.arguments().value(0).toBool());
	}
	static void setScreenLocked(int locked) { callFakePortal(QStringLiteral("SetScreenLocked"), {locked}); }
	static void denyNextStart() { callFakePortal(QStringLiteral("DenyNextStart")); }

	static QStringList selectDevicesLog() { return persistenceState().value(0).toStringList(); }
	static QString lastSelectDevices() { return selectDevicesLog().value(selectDevicesLog().size() - 1); }
	static int dialogCount() { return persistenceState().value(1).toInt(); }
	static int startCount() { return persistenceState().value(2).toInt(); }
	static QString lastToken() { return persistenceState().value(5).toString(); }

	void syncSessionNumber() { m_sessionNumber = persistenceState().value(6).toInt(); }

	static QString tokenDirectory() { return QFileInfo(PortalSession::restoreTokenFilePath()).path(); }

	static void removeTokenFiles()
	{
		QFile::remove(PortalSession::restoreTokenFilePath());
		QFile::remove(PortalSession::legacyRestoreTokenFilePath());
	}

	static void writeTokenFile(const QString& filePath, const QString& token)
	{
		QDir().mkpath(QFileInfo(filePath).path());
		QFile file(filePath);
		QVERIFY(file.open(QFile::WriteOnly | QFile::Truncate));
		file.write(token.toUtf8());
	}

	static QString readTokenFile()
	{
		QFile file(PortalSession::restoreTokenFilePath());
		if (file.open(QFile::ReadOnly) == false)
		{
			return {};
		}
		return QString::fromUtf8(file.readAll());
	}

	static int fileMode(const QString& path)
	{
		struct stat status{};
		if (::lstat(QFile::encodeName(path).constData(), &status) != 0)
		{
			return -1;
		}
		return int(status.st_mode & 07777);
	}

	static void setMutter(bool mutter)
	{
		auto request = QDBusMessage::createMethodCall(FakePortalService, FakePortalObjectPath, TestInterface,
													  QStringLiteral("SetMutter"));
		request.setArguments({mutter});
		QDBusConnection::sessionBus().call(request);
	}

	static void hangMotions(int count)
	{
		auto request = QDBusMessage::createMethodCall(FakePortalService, FakePortalObjectPath, TestInterface,
													  QStringLiteral("HangMotions"));
		request.setArguments({count});
		QDBusConnection::sessionBus().call(request);
	}

	static void faultReleases(quint32 keySym, int errors, int missingReplies)
	{
		auto request = QDBusMessage::createMethodCall(FakePortalService, FakePortalObjectPath, TestInterface,
													  QStringLiteral("FaultReleases"));
		request.setArguments({keySym, errors, missingReplies});
		QDBusConnection::sessionBus().call(request);
	}

	static QStringList rejectedInput(int sessionNumber)
	{
		const auto prefix = QStringLiteral("%1:").arg(sessionNumber);
		return portalState().value(2).toStringList().filter(QRegularExpression(QLatin1Char('^') + prefix));
	}

	static void setStream(QPoint position, QSize size)
	{
		auto request = QDBusMessage::createMethodCall(FakePortalService, FakePortalObjectPath, TestInterface,
													  QStringLiteral("SetStream"));
		request.setArguments({position.x(), position.y(), size.width(), size.height()});
		QDBusConnection::sessionBus().call(request);
	}

	static void setRejectAllMotions(bool reject)
	{
		auto request = QDBusMessage::createMethodCall(FakePortalService, FakePortalObjectPath, TestInterface,
													  QStringLiteral("SetRejectAllMotions"));
		request.setArguments({reject});
		QDBusConnection::sessionBus().call(request);
	}

	static QString pointerEvent(int sessionNumber, double x, double y)
	{
		return QStringLiteral("%1:motion:%2,%3").arg(sessionNumber).arg(x, 0, 'f', 2).arg(y, 0, 'f', 2);
	}

	static QString buttonEvent(int sessionNumber, int button, uint state)
	{
		return QStringLiteral("%1:button:%2:%3").arg(sessionNumber).arg(button).arg(state);
	}

	bool startSession(PortalSession& session)
	{
		QSignalSpy startedSpy(&session, &PortalSession::started);
		session.setInputReplyTimeout(TestReplyTimeout);
		session.start();
		if (startedSpy.wait(5000) == false || session.state() != PortalSession::State::Running)
		{
			return false;
		}
		// PipeWireFramebuffer takes ownership of the descriptor
		::close(session.pipewireFd());
		// also counts the sessions of a retry
		m_sessionNumber = persistenceState().value(6).toInt();
		return true;
	}

	int currentSessionNumber() const
	{
		return m_sessionNumber;
	}

	static QString keyEvent(int sessionNumber, quint32 keySym, uint state)
	{
		return QStringLiteral("%1:%2:%3").arg(sessionNumber).arg(keySym, 0, 16).arg(state);
	}

	static QVariantList portalState()
	{
		const auto request = QDBusMessage::createMethodCall(FakePortalService, FakePortalObjectPath, TestInterface,
															QStringLiteral("State"));
		return QDBusConnection::sessionBus().call(request).arguments();
	}

	static QStringList closedSessions()
	{
		return portalState().value(0).toStringList();
	}

	static QStringList forwardedInput(int sessionNumber)
	{
		const auto prefix = QStringLiteral("%1:").arg(sessionNumber);
		return portalState().value(1).toStringList().filter(QRegularExpression(QLatin1Char('^') + prefix));
	}

	QTemporaryDir m_home;
	QProcess m_portal;
	int m_sessionNumber = 0;
};



int main(int argc, char** argv)
{
	QGuiApplication application(argc, argv);
	QGuiApplication::setDesktopFileName(TestAppId);

	if (application.arguments().contains(QStringLiteral("--fake-portal")))
	{
		qDBusRegisterMetaType<IntPair>();
		qDBusRegisterMetaType<Stream>();
		qDBusRegisterMetaType<QList<Stream>>();

		FakePortal portal;
		auto connection = QDBusConnection::sessionBus();
		if (connection.registerService(FakePortalService) == false ||
			connection.registerVirtualObject(QStringLiteral("/org/freedesktop/portal"), &portal,
											 QDBusConnection::SubPath) == false ||
			connection.registerVirtualObject(ScreenSaverObjectPath, &portal) == false)
		{
			return 1;
		}
		if (::write(STDOUT_FILENO, "ready\n", 6) != 6)
		{
			return 1;
		}
		return application.exec();
	}

	PortalSessionTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "PortalSessionTest.moc"
