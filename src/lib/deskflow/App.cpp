/*
 * Deskflow -- mouse and keyboard sharing utility
 * Copyright (C) 2012 Symless Ltd.
 * Copyright (C) 2002 Chris Schoeneman
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "deskflow/App.h"

#include "DisplayInvalidException.h"
#include "arch/Arch.h"
#include "arch/XArch.h"
#include "base/EventQueue.h"
#include "base/Log.h"
#include "base/TMethodEventJob.h"
#include "base/XBase.h"
#include "base/log_outputters.h"
#include "common/constants.h"
#include "common/copyright.h"
#include "common/ipc.h"
#include "common/version.h"
#include "deskflow/ArgsBase.h"
#include "deskflow/Config.h"
#include "deskflow/XDeskflow.h"
#include "deskflow/protocol_types.h"
#include "ipc/IpcMessage.h"
#include "ipc/IpcServerProxy.h"

#if SYSAPI_WIN32
#include "arch/win32/ArchMiscWindows.h"
#include "base/IEventQueue.h"
#include "base/TMethodJob.h"
#endif

#if WINAPI_CARBON
#include "platform/OSXDragSimulator.h"
#endif

#include <charconv>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <stdio.h>
#include <vector>

#if WINAPI_CARBON
#include <ApplicationServices/ApplicationServices.h>
#endif

#if HAVE_CLI11
#include <CLI/CLI.hpp>
#endif

using namespace deskflow;

App *App::s_instance = nullptr;

//
// App
//

App::App(
    IEventQueue *events, CreateTaskBarReceiverFunc createTaskBarReceiver, deskflow::ArgsBase *args, bool runEventLoop
)
    : m_bye(&exit),
      m_taskBarReceiver(NULL),
      m_suspended(false),
      m_events(events),
      m_args(args),
      m_fileLog(nullptr),
      m_createTaskBarReceiver(createTaskBarReceiver),
      m_appUtil(events, runEventLoop),
      m_ipcClient(nullptr),
      m_socketMultiplexer(nullptr)
{
  assert(s_instance == nullptr);
  s_instance = this;
}

App::~App()
{
  s_instance = nullptr;
  delete m_args;
}

void App::version()
{
  const auto version = deskflow::version();
  const auto copyright = deskflow::copyright();

  const auto kBufferLength = 1024;
  std::vector<char> buffer(kBufferLength);
  std::snprintf(                                                   // NOSONAR
      buffer.data(), kBufferLength, "%s v%s, protocol v%d.%d\n%s", //
      argsBase().m_pname, version.c_str(), kProtocolMajorVersion, kProtocolMinorVersion, copyright.c_str()
  );

  std::cout << std::string(buffer.data()) << std::endl;
}

int App::run(int argc, char **argv)
{
#if MAC_OS_X_VERSION_10_7
  // dock hide only supported on lion :(
  ProcessSerialNumber psn = {0, kCurrentProcess};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  GetCurrentProcess(&psn);
#pragma GCC diagnostic pop

  TransformProcessType(&psn, kProcessTransformToBackgroundApplication);
#endif

  // install application in to arch
  appUtil().adoptApp(this);

  // HACK: fail by default (saves us setting result in each catch)
  int result = kExitFailed;

  try {
    result = appUtil().run(argc, argv);
  } catch (XExitApp &e) {
    // instead of showing a nasty error, just exit with the error code.
    // not sure if i like this behaviour, but it's probably better than
    // using the exit(int) function!
    result = e.getCode();
  } catch (DisplayInvalidException &die) {
    LOG((CLOG_CRIT "a display invalid exception error occurred: %s\n", die.what()));
    // display invalid exceptions can occur when going to sleep. When this
    // process exits, the UI will restart us instantly. We don't really want
    // that behevior, so we quies for a bit
    ARCH->sleep(10);
  } catch (std::runtime_error &re) {
    LOG((CLOG_CRIT "a runtime error occurred: %s\n", re.what()));
  } catch (std::exception &e) {
    LOG((CLOG_CRIT "an error occurred: %s\n", e.what()));
  } catch (...) {
    LOG((CLOG_CRIT "an unknown error occurred\n"));
  }

  appUtil().beforeAppExit();

  return result;
}

int App::daemonMainLoop(int, const char **)
{
#if SYSAPI_WIN32
  SystemLogger sysLogger(daemonName(), false);
#else
  SystemLogger sysLogger(daemonName(), true);
#endif
  return mainLoop();
}

void App::setupFileLogging()
{
  if (argsBase().m_logFile != NULL) {
    m_fileLog = new FileLogOutputter(argsBase().m_logFile);
    CLOG->insert(m_fileLog);
    LOG((CLOG_DEBUG1 "logging to file (%s) enabled", argsBase().m_logFile));
  }
}

void App::loggingFilterWarning()
{
  if (CLOG->getFilter() > CLOG->getConsoleMaxLevel()) {
    if (argsBase().m_logFile == NULL) {
      LOG(
          (CLOG_WARN "log messages above %s are NOT sent to console (use file logging)",
           CLOG->getFilterName(CLOG->getConsoleMaxLevel()))
      );
    }
  }
}

void App::initApp(int argc, const char **argv)
{

  std::string configFilename;
#if HAVE_CLI11
  CLI::App cliApp{kAppDescription, kAppName};
  cliApp.add_option("--config-toml", configFilename, "Use TOML configuration file");

  // Allow legacy args.
  cliApp.allow_extras();

  cliApp.parse(argc, argv);
#endif // HAVE_CLI11

  if (!configFilename.empty()) {
    Config config(configFilename, configSection());
    if (config.load(argv[0])) {
      parseArgs(config.argc(), config.argv());
    }
  } else {
    parseArgs(argc, argv);
  }

  ARCH->setProfileDirectory(argsBase().m_profileDirectory);
  ARCH->setPluginDirectory(argsBase().m_pluginDirectory);

  // set log filter
  if (!CLOG->setFilter(argsBase().m_logFilter)) {
    LOG((
        CLOG_CRIT "%s: unrecognized log level `%s'" BYE, argsBase().m_pname, argsBase().m_logFilter, argsBase().m_pname
    ));
    m_bye(kExitArgs);
  }
  loggingFilterWarning();

  // setup file logging after parsing args
  setupFileLogging();

  // load configuration
  loadConfig();

  if (!argsBase().m_disableTray && m_createTaskBarReceiver) {

    // create a log buffer so we can show the latest message
    // as a tray icon tooltip
    BufferedLogOutputter *logBuffer = new BufferedLogOutputter(1000);
    CLOG->insert(logBuffer, true);

    // make the task bar receiver.  the user can control this app
    // through the task bar.
    m_taskBarReceiver = m_createTaskBarReceiver(logBuffer, m_events);
  }
}

void App::initIpcClient()
{
  m_ipcClient = new IpcClient(m_events, m_socketMultiplexer);
  m_ipcClient->connect();

  m_events->adoptHandler(
      m_events->forIpcClient().messageReceived(), m_ipcClient, new TMethodEventJob<App>(this, &App::handleIpcMessage)
  );
}

void App::cleanupIpcClient()
{
  m_ipcClient->disconnect();
  m_events->removeHandler(m_events->forIpcClient().messageReceived(), m_ipcClient);
  delete m_ipcClient;
}

void App::handleIpcMessage(const Event &e, void *)
{
  IpcMessage *m = static_cast<IpcMessage *>(e.getDataObject());
  if (m->type() == IpcMessageType::Shutdown) {
    LOG((CLOG_INFO "got ipc shutdown message"));
    m_events->addEvent(Event(Event::kQuit));
  }
}

void App::runEventsLoop(void *)
{
  m_events->loop();

#if defined(MAC_OS_X_VERSION_10_7)

  stopCocoaLoop();

#endif
}

//
// MinimalApp
//

MinimalApp::MinimalApp(IEventQueue *events) : App(events, NULL, new deskflow::ArgsBase(), false)
{
}

MinimalApp::~MinimalApp()
{
}

int MinimalApp::standardStartup(int argc, char **argv)
{
  return 0;
}

int MinimalApp::runInner(int argc, char **argv, ILogOutputter *outputter, StartupFunc startup)
{
  return 0;
}

void MinimalApp::startNode()
{
}

int MinimalApp::mainLoop()
{
  return 0;
}

int MinimalApp::foregroundStartup(int argc, char **argv)
{
  return 0;
}

deskflow::Screen *MinimalApp::createScreen()
{
  return NULL;
}

void MinimalApp::loadConfig()
{
}

bool MinimalApp::loadConfig(const String &pathname)
{
  return false;
}

const char *MinimalApp::daemonInfo() const
{
  return "";
}

const char *MinimalApp::daemonName() const
{
  return "";
}

void MinimalApp::parseArgs(int argc, const char *const *argv)
{
}
