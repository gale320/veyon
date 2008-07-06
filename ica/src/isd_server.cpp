/*
 * isd_server.cpp - ISD Server
 *
 * Copyright (c) 2006-2008 Tobias Doerffel <tobydox/at/users/dot/sf/dot/net>
 *  
 * This file is part of iTALC - http://italc.sourceforge.net
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */


#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef BUILD_WIN32

#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <psapi.h>
#endif


#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryFile>
#include <QtCore/QTimer>
#include <QtGui/QMessageBox>
#include <QtGui/QPushButton>
#include <QtNetwork/QHostInfo>
#include <QtNetwork/QTcpSocket>

#include "isd_server.h"
#include "isd_connection.h"
#include "dsa_key.h"
#include "local_system_ica.h"
#include "ivs.h"
#include "lock_widget.h"
#include "messagebox.h"
#include "demo_client.h"
#include "demo_server.h"
#include "ica_main.h"

static isdServer * __isd_server = NULL;

QStringList isdServer::s_allowedDemoClients;



isdServer::isdServer( const quint16 _ivs_port, int _argc, char * * _argv ) :
	QTcpServer(),
	m_readyReadMapper( this ),
	m_ivs( NULL ),
	m_demoClient( NULL ),
	m_lockWidget( NULL )
{
	if( __isd_server ||
			listen( QHostAddress::Any, __isd_port ) == FALSE )
	{
		// uh oh, already an ISD running or port isn't available...
		qCritical( "isdServer::isdServer(...): "
				"could not start ISD server: %s",
					errorString().toUtf8().constData() );
		messageBox::trySysTrayMessage( tr( "ISD-server error" ),
			tr( "The ISD-server could not be started because "
				"port %1 is already in use. Please make sure "
				"that no other application is using this "
				"port and try again." ).
					arg( QString::number( __isd_port ) ),
							messageBox::Critical );
	}

	connect( this, SIGNAL( newConnection() ),
			this, SLOT( acceptNewConnection() ) );

	connect( &m_readyReadMapper, SIGNAL( mapped( QObject * ) ),
			this, SLOT( processClient( QObject * ) ) );

	QTimer * t = new QTimer( this );
	connect( t, SIGNAL( timeout() ), this,
					SLOT( checkForPendingActions() ) );
	// as things like creating a demo-window, remote-control-view etc. can
	// only be done by GUI-thread we push all actions into a list and
	// process this list later in a slot called by the GUI-thread every 500s
	t->start( 300 );

	// finally we set the global pointer to ourself
	__isd_server = this;

	m_ivs = new IVS( _ivs_port, _argc, _argv );
	m_ivs->start(/* QThread::HighPriority*/ );
}




isdServer::~isdServer()
{
	delete m_ivs;
	delete m_lockWidget;
	__isd_server = NULL;
}




int isdServer::processClient( socketDispatcher _sd, void * _user )
{
	socketDevice sdev( _sd, _user );
	char cmd;
	if( sdev.read( &cmd, sizeof( cmd ) ) == 0 )
	{
		qCritical( "isdServer::processClient(...): couldn't read "
					"iTALC-request from client..." );
		return( FALSE );
	}

	if( cmd == rfbItalcServiceRequest )
	{
		return( processClient( _sd, _user ) );
	}

	// in every case receive message-arguments, even if it's an empty list
	// because this is at leat the int32 with number of items in the list
	ISD::msg msg_in( &sdev, static_cast<ISD::commands>( cmd ) );
	msg_in.receive();

	QString action;

	switch( cmd )
	{
		case ISD::GetUserInformation:
		{
			ISD::msg( &sdev, ISD::UserInformation ).
					addArg( "username",
						localSystem::currentUser() ).
					addArg( "homedir", QDir::homePath() ).
									send();
			break;
		}

		case ISD::ExecCmds:
		{
			const QString cmds = msg_in.arg( "cmds" ).toString();
			if( !cmds.isEmpty() )
			{
#ifdef BUILD_WIN32
	// run process as the user which is logged on
	DWORD aProcesses[1024], cbNeeded;

	if( !EnumProcesses( aProcesses, sizeof( aProcesses ), &cbNeeded ) )
	{
		break;
	}

	DWORD cProcesses = cbNeeded / sizeof(DWORD);

	for( DWORD i = 0; i < cProcesses; i++ )
	{
		HANDLE hProcess = OpenProcess( PROCESS_ALL_ACCESS,
							false, aProcesses[i] );
		HMODULE hMod;
		if( hProcess == NULL ||
			!EnumProcessModules( hProcess, &hMod, sizeof( hMod ),
								&cbNeeded ) )
	        {
			continue;
		}

		TCHAR szProcessName[MAX_PATH];
		GetModuleBaseName( hProcess, hMod, szProcessName, 
                       		  sizeof( szProcessName ) / sizeof( TCHAR) );
		for( TCHAR * ptr = szProcessName; *ptr; ++ptr )
		{
			*ptr = tolower( *ptr );
		}

		if( strcmp( szProcessName, "explorer.exe" ) )
		{
			CloseHandle( hProcess );
			continue;
		}
	
		HANDLE hToken;
		OpenProcessToken( hProcess, MAXIMUM_ALLOWED, &hToken );
		ImpersonateLoggedOnUser( hToken );

		STARTUPINFO si;
		PROCESS_INFORMATION pi;
		ZeroMemory( &si, sizeof( STARTUPINFO ) );
		si.cb= sizeof( STARTUPINFO );
		si.lpDesktop = (CHAR *) "winsta0\\default";
		HANDLE hNewToken = NULL;

		DuplicateTokenEx( hToken, MAXIMUM_ALLOWED, NULL,
					SecurityImpersonation, TokenPrimary,
								&hNewToken );

		CreateProcessAsUser(
				hNewToken,            // client's access token
				NULL,              // file to execute
				(CHAR *)cmds.toAscii().constData(),     // command line
				NULL,              // pointer to process SECURITY_ATTRIBUTES
				NULL,              // pointer to thread SECURITY_ATTRIBUTES
				FALSE,             // handles are not inheritable
				NORMAL_PRIORITY_CLASS | CREATE_NEW_CONSOLE,   // creation flags
				NULL,              // pointer to new environment block 
				NULL,              // name of current directory 
				&si,               // pointer to STARTUPINFO structure
				&pi                // receives information about new process
				); 
		CloseHandle( hNewToken );
		RevertToSelf();
		CloseHandle( hToken );
		CloseHandle( hProcess );
		break;
	}
#else
				QProcess::startDetached( cmds );
#endif
			}
			break;
		}

		case ISD::StartFullScreenDemo:
		case ISD::StartWindowDemo:
		{
			QString port = msg_in.arg( "port" ).toString();
			if( port == "" )
			{
				port = "5858";
			}
			if( !port.contains( ':' ) )
			{
				const int MAX_HOST_LEN = 255;
				char host[MAX_HOST_LEN+1];
				_sd( host, MAX_HOST_LEN, SocketGetPeerAddress,
									_user );
				host[MAX_HOST_LEN] = 0;
				action = host + QString( ":" ) + port;
			}
			else
			{
				action = port;
			}
			break;
		}

		case ISD::DisplayTextMessage:
			action = msg_in.arg( "msg" ).toString();
			break;

		case ISD::LockDisplay:
		case ISD::UnlockDisplay:
		case ISD::StopDemo:
			action = "123";	// something to make the action being
					// added to action-list processed by
					// GUI-thread
			break;

		case ISD::LogonUserCmd:
			localSystem::logonUser(
					msg_in.arg( "uname" ).toString(),
					msg_in.arg( "passwd" ).toString(),
					msg_in.arg( "domain" ).toString() );
			break;

		case ISD::LogoutUser:
			localSystem::logoutUser();
			break;

		case ISD::WakeOtherComputer:
			localSystem::broadcastWOLPacket( 
					msg_in.arg( "mac" ).toString() );
			break;

		case ISD::PowerDownComputer:
			localSystem::powerDown();
			break;

		case ISD::RestartComputer:
			localSystem::reboot();
			break;

		case ISD::DisableLocalInputs:
			localSystem::disableLocalInputs(
					msg_in.arg( "disabled" ).toBool() );
			break;

		case ISD::SetRole:
		{
			const int role = msg_in.arg( "role" ).toInt();
			if( role > ISD::RoleNone && role < ISD::RoleCount )
			{
				__role = static_cast<ISD::userRoles>( role );
#ifdef BUILD_LINUX
				// under Linux/X11, IVS runs in separate process
				// therefore we need to restart it with new
				// role, bad hack but there's no clean solution
				// for the time being
				m_ivs->restart();
#endif
			}
			break;
		}

		case ISD::DemoServer_Run:
			if( _sd == &qtcpsocketDispatcher )
			{
				// start demo-server on local IVS and make it
				// child of our socket so that it automatically
				// gets destroyed as soon as the socket is
				// closed and thus destroyed
				QTcpSocket * ts = static_cast<QTcpSocket *>(
								_user );
				new demoServer( m_ivs,
					msg_in.arg( "quality" ).toInt(),
					msg_in.arg( "port" ).toInt(), ts );
			}
			else
			{
				qCritical( "socket-dispatcher is not a "
						"qtcpsocketDispatcher!\n" );
			}
			break;

		case ISD::HideTrayIcon:
#ifdef SYSTEMTRAY_SUPPORT
			if( _sd == &qtcpsocketDispatcher )
			{
				// start demo-server on local IVS and make it
				// child of our socket so that it automatically
				// gets destroyed as soon as the socket is
				// closed and thus destroyed
				QTcpSocket * ts = static_cast<QTcpSocket *>(
								_user );
				__systray_icon->hide();
				connect( ts, SIGNAL( disconnected() ),
					__systray_icon, SLOT( show() ) );
			}
			else
			{
				qCritical( "socket-dispatcher is not a "
						"qtcpsocketDispatcher!\n" );
			}
#endif
			break;

			break;

		case ISD::DemoServer_AllowClient:
			allowDemoClient( msg_in.arg( "client" ).toString() );
			break;

		case ISD::DemoServer_DenyClient:
			denyDemoClient( msg_in.arg( "client" ).toString() );
			break;

		default:
			qCritical( "isdServer::processClient(...): "
					"cmd %d not implemented!", cmd );
			break;
	}

	if( !action.isEmpty() )
	{
		m_actionMutex.lock();
		m_pendingActions.push_back( qMakePair(
				static_cast<ISD::commands>( cmd ), action ) );
		m_actionMutex.unlock();
	}

	return( TRUE );
}




bool isdServer::protocolInitialization( socketDevice & _sd,
					italcAuthTypes _auth_type,
					bool _demo_server )
{
	if( _demo_server )
	{
		idsProtocolVersionMsg pv;
		sprintf( pv, idsProtocolVersionFormat, idsProtocolMajorVersion,
						idsProtocolMinorVersion );
		_sd.write( pv, sz_idsProtocolVersionMsg );

		idsProtocolVersionMsg pv_cl;
		_sd.read( pv_cl, sz_idsProtocolVersionMsg );
		pv_cl[sz_idsProtocolVersionMsg] = 0;
		if( memcmp( pv, pv_cl, sz_idsProtocolVersionMsg ) )
		{
			qCritical( "isdServer::protocolInitialization(...): "
							"invalid client!" );
			return FALSE;
		}
	}
	else
	{
		isdProtocolVersionMsg pv;
		sprintf( pv, isdProtocolVersionFormat, isdProtocolMajorVersion,
						isdProtocolMinorVersion );
		_sd.write( pv, sz_isdProtocolVersionMsg );

		isdProtocolVersionMsg pv_cl;
		_sd.read( pv_cl, sz_isdProtocolVersionMsg );
		pv_cl[sz_isdProtocolVersionMsg] = 0;
		if( memcmp( pv, pv_cl, sz_isdProtocolVersionMsg ) )
		{
			qCritical( "isdServer::protocolInitialization(...): "
							"invalid client!" );
			return FALSE;
		}
	}


	const char sec_type_list[2] = { 1, rfbSecTypeItalc } ;
	_sd.write( sec_type_list, sizeof( sec_type_list ) );

	Q_UINT8 chosen = 0;
	_sd.read( (char *) &chosen, sizeof( chosen ) );

	const int MAX_HOST_LEN = 255;
	char host[MAX_HOST_LEN+1];
	_sd.sockDispatcher()( host, MAX_HOST_LEN, SocketGetPeerAddress,
								_sd.user() );
	host[MAX_HOST_LEN] = 0;


	if( chosen != rfbSecTypeItalc )
	{
		errorMsgAuth( host );
		qCritical( "isdServer::protocolInitialization(...): "
			"client wants unknown security type %d", chosen );
		return( FALSE );
	}

	if( chosen != rfbSecTypeItalc ||
		!authSecTypeItalc( _sd.sockDispatcher(), _sd.user(),
								_auth_type ) )
	{
		errorMsgAuth( host );
		return( FALSE );
	}

	return( TRUE );
}




bool isdServer::authSecTypeItalc( socketDispatcher _sd, void * _user,
						italcAuthTypes _auth_type )
{
	// find out IP of host - needed at several places
	const int MAX_HOST_LEN = 255;
	char host[MAX_HOST_LEN+1];
	_sd( host, MAX_HOST_LEN, SocketGetPeerAddress, _user );
	host[MAX_HOST_LEN] = 0;
	static QStringList __denied_hosts, __allowed_hosts;

	socketDevice sdev( _sd, _user );
	sdev.write( QVariant( (int) _auth_type ) );

	italcAuthResults result = ItalcAuthFailed;

	italcAuthTypes chosen = static_cast<italcAuthTypes>(
							sdev.read().toInt() );
	if( chosen == ItalcAuthAppInternalChallenge ||
		chosen == ItalcAuthChallengeViaAuthFile )
	{
		_auth_type = chosen;
	}
	else if( chosen == ItalcAuthDSA && _auth_type == ItalcAuthLocalDSA )
	{
		// this case is ok as well
	}
	else if( chosen != _auth_type )
	{
		errorMsgAuth( host );
		qCritical( "isdServer::authSecTypeItalc(...): "
				"client chose other auth-type than offered!" );
		return( result );
	}

	switch( _auth_type )
	{
		// no authentication
		case ItalcAuthNone:
			result = ItalcAuthOK;
			break;

		// host has to be in list of allowed hosts
		case ItalcAuthHostBased:
		{
			if( s_allowedDemoClients.isEmpty() )
			{
				break;
			}
			QStringList allowed;
			foreach( const QString a, s_allowedDemoClients )
			{
				const QString h = a.split( ':' )[0];
				if( !allowed.contains( h ) )
				{
					allowed.push_back( h );
				}
			}
			// already valid IP?
			if( QHostAddress().setAddress( host ) )
			{
				if( allowed.contains( host ) )
				{
					result = ItalcAuthOK;
				}
			}
			else
			{
			// create a list of all known addresses of host
			QList<QHostAddress> addr =
					QHostInfo::fromName( host ).addresses();
			if( !addr.isEmpty() )
			{
				// check each address for existence in allowed-
				// client-list
				foreach( const QHostAddress a, addr )
				{
	if( allowed.contains( a.toString() ) ||
		a.toString() == QHostAddress( QHostAddress::LocalHost ).toString() )
					{
						result = ItalcAuthOK;
						break;
					}
				}
			}
			}
			break;
		}

		// authentication via DSA-challenge/-response
		case ItalcAuthLocalDSA:
		case ItalcAuthDSA:
		{
			// generate data to sign and send to client
			const QByteArray chall = dsaKey::generateChallenge();
			sdev.write( QVariant( chall ) );

			// get user-role
			const ISD::userRoles urole =
				static_cast<ISD::userRoles>(
							sdev.read().toInt() );
			if( __role != ISD::RoleOther &&
					_auth_type != ItalcAuthLocalDSA )
			{
				if( __denied_hosts.contains( host ) )
				{
					result = ItalcAuthFailed;
					break;
				}
				if( !__allowed_hosts.contains( host ) )
				{
					bool failed = TRUE;
					switch(
#ifdef BUILD_LINUX
	QProcess::execute( QCoreApplication::applicationFilePath() +
					QString( " %1 %2" ).
						arg( ACCESS_DIALOG_ARG ).
								arg( host ) )
#else
					showAccessDialog( host )
#endif
									)
					{
						case Always:
							__allowed_hosts += host;
						case Yes:
							failed = FALSE;
							break;
						case Never:
							__denied_hosts += host;
						case No:
							break;
					}
					if( failed )
					{
						result = ItalcAuthFailed;
						break;
					}
				}
				else
				{
					result = ItalcAuthFailed;
				}
			}
			
			// now try to verify received signed data using public
			// key of the user under which the client claims to run
			const QByteArray sig = sdev.read().toByteArray();
			// (publicKeyPath does range-checking of urole)
			publicDSAKey pub_key( localSystem::publicKeyPath(
								urole ) );
			result = pub_key.verifySignature( chall, sig ) ?
						ItalcAuthOK : ItalcAuthFailed;
			break;
		}

		// used for demo-purposes (demo-server connects to local IVS)
		case ItalcAuthAppInternalChallenge:
		{
			// generate challenge
			__appInternalChallenge = dsaKey::generateChallenge();
			sdev.write( QVariant() );
			// is our client able to read this byte-array? if so,
			// it's for sure running inside the same app
			result = ( sdev.read().toByteArray() ==
						__appInternalChallenge ) ?
						ItalcAuthOK : ItalcAuthFailed;
			break;
		}

		// used for demo-purposes (demo-server connects to local IVS)
		case ItalcAuthChallengeViaAuthFile:
		{
			// generate challenge
			QByteArray chall = dsaKey::generateChallenge();
			QTemporaryFile tf;
			tf.setPermissions( QFile::ReadOwner |
							QFile::WriteOwner );
			tf.open();
			tf.write( chall );
			tf.flush();
			sdev.write( tf.fileName() );
			// is our client able to read the file? if so,
			// it's running as the same user as this piece of
			// code does so we can assume that it's our parent-
			// process
			result = ( sdev.read().toByteArray() == chall ) ?
						ItalcAuthOK : ItalcAuthFailed;
			break;
		}
	}

	sdev.write( QVariant( (int) result ) );
	if( result != ItalcAuthOK )
	{
		errorMsgAuth( host );
	}

	return( result == ItalcAuthOK );
}




quint16 isdServer::isdPort( void )
{
	return( __isd_server ? __isd_server->serverPort() : PortOffsetISD );
}




isdServer::accessDialogResult isdServer::showAccessDialog(
							const QString & _host )
{
	QMessageBox m( QMessageBox::Question,
			tr( "Confirm access" ),
			tr( "Somebody at host %1 tries to access your screen. "
				"Do you want to grant him/her access?" ).
								arg( _host ),
				QMessageBox::Yes | QMessageBox::No );

	QPushButton * never_btn = m.addButton( tr( "Never for this session" ),
							QMessageBox::NoRole );
	QPushButton * always_btn = m.addButton( tr( "Always for this session" ),
							QMessageBox::YesRole );
	m.setDefaultButton( never_btn );
	m.setEscapeButton( m.button( QMessageBox::No ) );

	localSystem::activateWindow( &m );

	const int res = m.exec();
	if( m.clickedButton() == never_btn )
	{
		return( Never );
	}
	else if( m.clickedButton() == always_btn )
	{
		return( Always );
	}
	else if( res == QMessageBox::No )
	{
		return( No );
	}
	return( Yes );
}




void isdServer::acceptNewConnection( void )
{
	QTcpSocket * sock = nextPendingConnection();
	socketDevice sd( qtcpsocketDispatcher, sock );

	if( !protocolInitialization( sd, ItalcAuthLocalDSA ) )
	{
		delete sock;
		return;
	}

	// now we're ready to start the normal interaction with the client,
	// so make sure, we get informed about new requests
	connect( sock, SIGNAL( readyRead() ),
			&m_readyReadMapper, SLOT( map() ) );
	connect( sock, SIGNAL( disconnected() ),
			sock, SLOT( deleteLater() ) );
	m_readyReadMapper.setMapping( sock, sock );
}




void isdServer::processClient( QObject * _sock )
{
	QTcpSocket * sock = qobject_cast<QTcpSocket *>( _sock );
	while( sock->bytesAvailable() > 0 )
	{
		processClient( qtcpsocketDispatcher, sock );
	}
}




void isdServer::checkForPendingActions( void )
{
	QMutexLocker ml( &m_actionMutex );
	while( !m_pendingActions.isEmpty() )
	{
		QString data = m_pendingActions.front().second;
		switch( m_pendingActions.front().first )
		{
			case ISD::StartFullScreenDemo:
			case ISD::StartWindowDemo:
				startDemo( data,
	( m_pendingActions.front().first == ISD::StartFullScreenDemo ) );
				break;

			case ISD::StopDemo:
				stopDemo();
				break;

			case ISD::LockDisplay:
				lockDisplay();
				break;

			case ISD::UnlockDisplay:
				unlockDisplay();
				break;

			case ISD::DisplayTextMessage:
				displayTextMessage( data );
				break;

			default:
				qWarning( "isdServer::checkForPendingActions():"
						" unhandled command %d",
					(int) m_pendingActions.front().first );
				break;
		}
		m_pendingActions.removeFirst();
	}
}




void isdServer::demoWindowClosed( QObject * )
{
	m_demoClient = NULL;
}




void isdServer::startDemo( const QString & _master_host, bool _fullscreen )
{
	delete m_demoClient;
	m_demoClient = NULL;
	// if a demo-server is started, it's likely that the demo was started
	// on master-computer as well therefore we deny starting a demo on
	// hosts on which a demo-server is running
	if( demoServer::numOfInstances() > 0 )
	{
		return;
	}

	m_demoClient = new demoClient( _master_host, _fullscreen );
	connect( m_demoClient, SIGNAL( destroyed( QObject * ) ),
				this, SLOT( demoWindowClosed( QObject * ) ) );
}




void isdServer::stopDemo( void )
{
	delete m_demoClient;
	m_demoClient = NULL;
}




void isdServer::lockDisplay( void )
{
	if( demoServer::numOfInstances() )
	{
		return;
	}
	delete m_lockWidget;
	m_lockWidget = new lockWidget();
}




void isdServer::unlockDisplay( void )
{
	delete m_lockWidget;
	m_lockWidget = NULL;
}




void isdServer::displayTextMessage( const QString & _msg )
{
	new messageBox( tr( "Message from teacher" ), _msg,
					QPixmap( ":/resources/message.png" ) );
}




void isdServer::allowDemoClient( const QString & _host )
{
	const QString h = _host.split( ':' )[0];
	const QString p = _host.contains( ':' ) ? ':'+_host.split( ':' )[1] : "";
	// already valid IP?
	if( QHostAddress().setAddress( h ) )
	{
		if( !s_allowedDemoClients.contains( _host ) )
		{
			s_allowedDemoClients.push_back( _host );
		}
		return;
	}
	foreach( const QHostAddress a,
				QHostInfo::fromName( h ).addresses() )
	{
		const QString h2 = a.toString();
		if( !s_allowedDemoClients.contains( h2+p ) )
		{
			s_allowedDemoClients.push_back( h2+p );
		}
	}
}




void isdServer::denyDemoClient( const QString & _host )
{
	const QString h = _host.split( ':' )[0];
	const QString p = _host.contains( ':' ) ? ':'+_host.split( ':' )[1] : "";
	// already valid IP?
	if( QHostAddress().setAddress( h ) )
	{
		s_allowedDemoClients.removeAll( _host );
		return;
	}
	foreach( const QHostAddress a,
				QHostInfo::fromName( h ).addresses() )
	{
		s_allowedDemoClients.removeAll( a.toString()+p );
	}
}




void isdServer::errorMsgAuth( const QString & _ip )
{
	messageBox::trySysTrayMessage( tr( "Authentication error" ),
			tr( "Somebody (IP: %1) tried to access this computer "
					"but could not authenticate itself "
					"successfully!" ).arg( QString( _ip ) ),
						messageBox::Critical );
}




#ifdef BUILD_LINUX
// helper-class which forwards commands destined to ISD-server. We need this
// when running VNC-server in separate process and VNC-server receives iTALC-
// commands which it can't process
class isdForwarder : public isdConnection
{
public:
	isdForwarder() :
		isdConnection( QHostAddress( QHostAddress::LocalHost ).
						toString() + ":" +
						QString::number( __isd_port ) )
	{
	}


	int processClient( socketDispatcher _sd, void * _user )
	{
		socketDevice sdev( _sd, _user );
		char cmd;
		if( sdev.read( &cmd, sizeof( cmd ) ) == 0 )
		{
			qCritical( "isdForwarder::processClient(...): "
				"couldn't read iTALC-request from client..." );
			return( FALSE );
		}

		if( cmd == rfbItalcServiceRequest )
		{
			return( processClient( _sd, _user ) );
		}

		// in every case receive message-arguments, even if it's an empty list
		// because this is at leat the int32 with number of items in the list
		ISD::msg msg_in( &sdev, static_cast<ISD::commands>( cmd ) );
		msg_in.receive();

		switch( cmd )
		{
			case ISD::GetUserInformation:
		ISD::msg( &sdev, ISD::UserInformation ).
				addArg( "username",
					localSystem::currentUser() ).
				addArg( "homedir", QDir::homePath() ).send();
				break;

			case ISD::ExecCmds:
				execCmds( msg_in.arg( "cmds" ).toString() );
				break;

			case ISD::StartFullScreenDemo:
			case ISD::StartWindowDemo:
			{
				const int MAX_HOST_LEN = 255;
				char host[MAX_HOST_LEN+1];
				_sd( host, MAX_HOST_LEN, SocketGetPeerAddress,
									_user );
				host[MAX_HOST_LEN] = 0;
				QString port = msg_in.arg( "port" ).toString();
				if( port == "" )
				{
					port = "5858";
				}
				startDemo( host + QString( ":" ) + port,
					cmd == ISD::StartFullScreenDemo );
				break;
			}

			case ISD::DisplayTextMessage:
				displayTextMessage( msg_in.arg( "msg" ).
								toString() );
				break;

			case ISD::LockDisplay:
				lockDisplay();
				break;

			case ISD::UnlockDisplay:
				unlockDisplay();
				break;

			case ISD::StopDemo:
				stopDemo();
				break;

			case ISD::LogonUserCmd:
				logonUser( msg_in.arg( "uname" ).toString(),
					msg_in.arg( "passwd" ).toString(),
					msg_in.arg( "domain" ).toString() );
				break;

			case ISD::LogoutUser:
				logoutUser();
				break;

			case ISD::WakeOtherComputer:
				wakeOtherComputer( 
					msg_in.arg( "mac" ).toString() );
				break;

			case ISD::PowerDownComputer:
				powerDownComputer();
				break;

			case ISD::RestartComputer:
				restartComputer();
				break;

			case ISD::DemoServer_Run:
				demoServerRun( msg_in.arg( "quality" ).toInt(),
						msg_in.arg( "port" ).toInt() );
				break;

			case ISD::DemoServer_AllowClient:
				demoServerAllowClient(
					msg_in.arg( "client" ).toString() );
				break;

			case ISD::DemoServer_DenyClient:
				demoServerDenyClient(
					msg_in.arg( "client" ).toString() );
				break;

			default:
				qCritical( "isdForwarder::processClient(...): "
					"cmd %d not implemented!", cmd );
				break;
		}

		return( TRUE );
	}


protected:
	virtual states authAgainstServer( const italcAuthTypes _try_auth_type )
	{
		return( isdConnection::authAgainstServer(
					ItalcAuthChallengeViaAuthFile ) );
	}

} ;

static isdForwarder * __isd_forwarder = NULL;

#endif


int processItalcClient( socketDispatcher _sd, void * _user )
{
	if( __isd_server )
	{
		return( __isd_server->processClient( _sd, _user ) );
	}

#ifdef BUILD_LINUX
	if( !__isd_forwarder )
	{
		__isd_forwarder = new isdForwarder();
	}

	if( __isd_forwarder->state() != isdForwarder::Connected )
	{
		__isd_forwarder->open();
	}

	//__isd_forwarder->handleServerMessages();

	return( __isd_forwarder->processClient( _sd, _user ) );
#endif
	return( 0 );
}




#include "isd_server.moc"

