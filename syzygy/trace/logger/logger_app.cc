// Copyright 2012 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// This file defines the trace::logger::LoggerApp class which implements the
// LoggerApp RPC interface.

#include "syzygy/trace/logger/logger_app.h"

#include "base/bind.h"
#include "base/environment.h"
#include "base/file_util.h"
#include "base/path_service.h"
#include "base/process.h"
#include "base/process_util.h"
#include "base/string_util.h"
#include "base/stringprintf.h"
#include "base/utf_string_conversions.h"
#include "base/win/scoped_handle.h"
#include "syzygy/trace/logger/logger.h"
#include "syzygy/trace/logger/logger_rpc_impl.h"
#include "syzygy/trace/protocol/call_trace_defs.h"
#include "syzygy/trace/rpc/logger_rpc.h"
#include "syzygy/trace/rpc/rpc_helpers.h"

namespace trace {
namespace logger {

namespace {

using trace::client::GetInstanceString;

// The usage string for the logger app.
const char kUsageFormatStr[] =
    "Usage: %ls [options] ACTION [-- command]\n"
    "  Supported actions:\n"
    "    start  Run a new logger instance in the foreground (blocking). You\n"
    "           may optionally specify an external command which will be\n"
    "           run behind the logger. The logger will return once the\n"
    "           external command has terminated or the logger is externally\n"
    "           stopped. If no command is specified, Ctrl-C or an invocation\n"
    "           of the stop action will stop the logger.\n"
    "    spawn  Run a new logger instance in the background (non-blocking).\n"
    "    stop   Stop a separately running logger instance.\n"
    "  Options:\n"
    "    --instance-id=ID     A unique (up to 16 character) ID to identify\n"
    "                         the logger instance.\n"
    "    --output-file=PATH>  The file path to which logs should be written.\n"
    "                         This may be stdout (the default), stderr or a\n"
    "                         file path. This option is valid for the start\n"
    "                         and spawn actions.\n"
    "    --append             Append to (instead of truncating) the output\n"
    "                         file. This option is valid for the start and\n"
    "                         spawn actions.\n";

// Names for kernel objects used to synchronize with a logger singleton.
const wchar_t kLoggerMutexRoot[] = L"syzygy-logger-mutex";
const wchar_t kLoggerStartEventRoot[] = L"syzygy-logger-started";
const wchar_t kLoggerStopEventRoot[] = L"syzygy-logger-stopped";

// A static location to which the current instance id can be saved. We
// persist it here so that OnConsoleCtrl can have access to the instance
// id when it is invoked on the signal handler thread.
wchar_t saved_instance_id[16] = { 0 };
const size_t kMaxInstanceIdLength = arraysize(saved_instance_id) - 1;

// Send a stop request via RPC to the logger instance given by @p instance_id.
bool SendStopRequest(const base::StringPiece16& instance_id) {
  std::wstring protocol(kLoggerRpcProtocol);
  std::wstring endpoint(GetInstanceString(kLoggerRpcEndpointRoot, instance_id));

  LOG(INFO) << "Stopping logging service instance at '"
            << endpoint << "' via " << protocol << '.';

  handle_t binding = NULL;
  if (!trace::client::CreateRpcBinding(protocol, endpoint, &binding)) {
    LOG(ERROR) << "Failed to connect to logging service.";
    return false;
  }

  if (!trace::client::InvokeRpc(LoggerClient_Stop, binding).succeeded()) {
    LOG(ERROR) << "Failed to stop logging service.";
    return false;
  }

  LOG(INFO) << "Logging service shutdown has been requested.";

  return true;
}

// Handler function to be called on exit signals (Ctrl-C, TERM, etc...).
BOOL WINAPI OnConsoleCtrl(DWORD ctrl_type) {
  if (ctrl_type != CTRL_LOGOFF_EVENT) {
    SendStopRequest(saved_instance_id);
    return TRUE;
  }
  return FALSE;
}

// A helper class to manage a console handler for Control-C.
// TODO(rogerm): Move this to a shared location (perhaps in common, next to
//     the application classes?).
class ScopedConsoleCtrlHandler {
 public:
  ScopedConsoleCtrlHandler() : handler_(NULL) {
  }

  ~ScopedConsoleCtrlHandler() {
    if (handler_ != NULL) {
      ignore_result(::SetConsoleCtrlHandler(handler_, FALSE));
      handler_ = NULL;
    }
  }

  bool Init(PHANDLER_ROUTINE handler) {
    DCHECK(handler != NULL);
    DCHECK(handler_ == NULL);

    if (!::SetConsoleCtrlHandler(handler, TRUE)) {
      DWORD err = ::GetLastError();
      LOG(ERROR) << "Failed to register console control handler: "
                 << com::LogWe(err) << ".";
      return false;
    }

    handler_ = handler;
    return true;
  }

 protected:
  PHANDLER_ROUTINE handler_;
};

// Helper function to acquire a named mutex.
// TODO(rogerm): Move this to a shared location (perhaps in common, next to
//     the application classes?).
bool AcquireMutex(const std::wstring& mutex_name,
                  base::win::ScopedHandle* mutex) {
  DCHECK(mutex != NULL);
  DCHECK(!mutex->IsValid());

  base::win::ScopedHandle tmp_mutex(
      ::CreateMutex(NULL, FALSE, mutex_name.c_str()));
  if (!tmp_mutex.IsValid()) {
    DWORD error = ::GetLastError();
    LOG(ERROR) << "Failed to create mutex: " << com::LogWe(error) << ".";
    return false;
  }
  const DWORD kOneSecondInMs = 1000;

  switch (::WaitForSingleObject(tmp_mutex, kOneSecondInMs)) {
    case WAIT_ABANDONED:
      LOG(WARNING) << "Orphaned service mutex found!";
      // Fall through...

    case WAIT_OBJECT_0:
      VLOG(1) << "Service mutex acquired.";
      mutex->Set(tmp_mutex.Take());
      return true;

    case WAIT_TIMEOUT:
      LOG(ERROR) << "A synonymous instance of the logger is already running.";
      break;

    default: {
      DWORD error = ::GetLastError();
      LOG(ERROR) << "Failed to acquire mutex: " << com::LogWe(error) << ".";
      break;
    }
  }
  return false;
}

// Helper function to initialize a named event.
// TODO(rogerm): Move this to a shared location (perhaps in common, next to
//     the application classes?).
bool InitEvent(const std::wstring& event_name,
               base::win::ScopedHandle* handle) {
  DCHECK(handle != NULL);
  DCHECK(!handle->IsValid());
  handle->Set(::CreateEvent(NULL, TRUE, FALSE, event_name.c_str()));
  if (!handle->IsValid())
    return false;
  return true;
}

// A helper function to signal an event. This is passable as a callback to
// a Logger instance to be called on logger start/stop.
bool SignalEvent(HANDLE event_handle, Logger* /* logger */) {
  DCHECK_NE(INVALID_HANDLE_VALUE, event_handle);
  if (!::SetEvent(event_handle))
    return false;
  return true;
}

// A helper to split a command line into two command lines. The split will
// occur after the first non-switch parameter. The logger command line will
// be populated by the the switches and arguments up to and including the
// fist non-switch parameter. All remaining arguments and switches will be
// added the app command line. This function understands the "--" marker
// which is used to allow switches to appear after the first non-switch
// argument (otherwise CommandLine will sort the entire command line before
// we get a chance to inspect it.).
bool SplitCommandLine(const CommandLine* orig_command_line,
                      CommandLine* logger_command_line,
                      scoped_ptr<CommandLine>* app_command_line) {
  DCHECK(orig_command_line != NULL);
  DCHECK(!orig_command_line->argv().empty());
  DCHECK(logger_command_line != NULL);
  DCHECK(app_command_line != NULL);

  // Copy the initial parts of the command-line, up to and including the
  // first non-switch argument (which should be the "action"), into a
  // string vector for the logger command line.
  CommandLine::StringVector logger_argv;
  CommandLine::StringVector::const_iterator it =
      orig_command_line->argv().begin();
  logger_argv.push_back(*(it++));  // Always copy the program.
  for (; it != orig_command_line->argv().end(); ++it) {
    logger_argv.push_back(*it);
    if ((*it)[0] != L'-') {
      ++it;
      break;
    }
  }

  // Strip out the (optional) sentinel which marks the split between the
  // two command-lines.
  if (it != orig_command_line->argv().end() && *it == L"--")
    ++it;

  // Copy the rest of the command-line arguments into a string vector for the
  // app command line.
  CommandLine::StringVector app_argv;
  for (; it != orig_command_line->argv().end(); ++it) {
    app_argv.push_back(*it);
  }

  // Initialize the output command lines with the new arguments.
  logger_command_line->InitFromArgv(logger_argv);
  if (!app_argv.empty())
    app_command_line->reset(new CommandLine(app_argv));

  return true;
}

// A helper function which sets the syzygy RPC instance id environment variable
// then runs a given command line to completion.
bool RunApp(const CommandLine& command_line,
            const std::wstring& instance_id,
            int* exit_code) {
  DCHECK(exit_code != NULL);
  scoped_ptr<base::Environment> env(base::Environment::Create());
  CHECK(env != NULL);
  env->SetVar(kSyzygyRpcInstanceIdEnvVar, WideToUTF8(instance_id));

  LOG(INFO) << "Launching '" << command_line.GetProgram().value() << "'.";
  VLOG(1) << "Command Line: " << command_line.GetCommandLineString();

  // Launch a new process in the background.
  base::ProcessHandle process_handle;
  base::LaunchOptions options;
  options.start_hidden = false;
  if (!base::LaunchProcess(command_line, options, &process_handle)) {
    LOG(ERROR)
        << "Failed to launch '" << command_line.GetProgram().value() << "'.";
    return false;
  }

  // Wait for and return the processes exit code.
  // Note that this closes the process handle.
  if (!base::WaitForExitCode(process_handle, exit_code)) {
    LOG(ERROR) << "Failed to get exit code.";
    return false;
  }

  return true;
}

}  // namespace

// Keywords appearing on the command-line
const wchar_t LoggerApp::kSpawn[] = L"spawn";
const wchar_t LoggerApp::kStart[] = L"start";
const wchar_t LoggerApp::kStatus[] = L"status";
const wchar_t LoggerApp::kStop[] = L"stop";
const char LoggerApp::kInstanceId[] = "instance-id";
const char LoggerApp::kOutputFile[] = "output-file";
const char LoggerApp::kAppend[] = "append";
const wchar_t LoggerApp::kStdOut[] = L"stdout";
const wchar_t LoggerApp::kStdErr[] = L"stderr";

// A table mapping action keywords to their handler implementations.
const LoggerApp::ActionTableEntry LoggerApp::kActionTable[] = {
    { LoggerApp::kSpawn, &LoggerApp::Spawn },
    { LoggerApp::kStart, &LoggerApp::Start },
    { LoggerApp::kStatus, &LoggerApp::Status },
    { LoggerApp::kStop, &LoggerApp::Stop },
};

LoggerApp::LoggerApp()
    : common::AppImplBase("Logger"),
      logger_command_line_(CommandLine::NO_PROGRAM),
      action_handler_(NULL),
      append_(false) {
}

LoggerApp::~LoggerApp() {
}

bool LoggerApp::ParseCommandLine(const CommandLine* command_line) {
  DCHECK(command_line != NULL);

  if (!SplitCommandLine(command_line,
                        &logger_command_line_,
                        &app_command_line_)) {
    LOG(ERROR) << "Failed to split command_line into logger and app parts.";
    return false;
  }

  // Save the command-line in case we need to spawn.
  command_line = &logger_command_line_;

  // Parse the instance id.
  instance_id_ = command_line->GetSwitchValueNative(kInstanceId);
  if (instance_id_.size() > kMaxInstanceIdLength) {
    return Usage(command_line,
                 base::StringPrintf("The instance id '%ls' is too long. "
                                    "The max length is %d characters.",
                                    instance_id_.c_str(),
                                    kMaxInstanceIdLength));
  }

  // Save the output file parameter.
  output_file_path_ = command_line->GetSwitchValuePath(kOutputFile);

  // Make sure there's exactly one action.
  if (command_line->GetArgs().size() != 1) {
    return Usage(command_line,
                 "Exactly 1 action is expected on the command line.");
  }

  // Check for the append flag.
  append_ = command_line->HasSwitch(kAppend);

  // Parse the action.
  action_ = command_line->GetArgs()[0];
  const ActionTableEntry* entry = FindActionHandler(action_);
  if (entry == NULL) {
    return Usage(
        command_line,
        base::StringPrintf("Unrecognized action: %s.", action_.c_str()));
  }

  // Setup the action handler.
  DCHECK(entry->handler != NULL);
  action_handler_ = entry->handler;

  return true;
}

int LoggerApp::Run() {
  DCHECK(action_handler_ != NULL);
  if (!(this->*action_handler_)())
    return 1;
  return 0;
}

// A helper function to find the handler method for a given action.
const LoggerApp::ActionTableEntry* LoggerApp::FindActionHandler(
    const base::StringPiece16& action) {
  const ActionTableEntry* const begin  = &kActionTable[0];
  const ActionTableEntry* const end = begin + arraysize(kActionTable);
  ActionTableEntryCompare compare_func;

  // Make sure that the array is sorted.
  DCHECK(std::is_sorted(begin, end, compare_func));

  const ActionTableEntry* entry =
      std::lower_bound(begin, end, action, compare_func);
  if (entry == end)
    return NULL;

  return entry;
}

bool LoggerApp::Start() {
  std::wstring logger_name(
      GetInstanceString(kLoggerRpcEndpointRoot, instance_id_));

  // Acquire the logger mutex.
  base::win::ScopedHandle mutex;
  std::wstring mutex_name(GetInstanceString(kLoggerMutexRoot, instance_id_));
  if (!AcquireMutex(mutex_name, &mutex))
    return false;

  // Setup the start event.
  base::win::ScopedHandle start_event;
  std::wstring start_event_name(
      GetInstanceString(kLoggerStartEventRoot, instance_id_));
  if (!InitEvent(start_event_name, &start_event)) {
    LOG(ERROR) << "Unable to init start event for '" << logger_name << "'.";
    return false;
  }

  // Setup the stop event.
  base::win::ScopedHandle stop_event;
  std::wstring stop_event_name(
      GetInstanceString(kLoggerStopEventRoot, instance_id_));
  if (!InitEvent(stop_event_name, &stop_event)) {
    LOG(ERROR) << "Unable to init stop event for '" << logger_name << "'.";
    return false;
  }

  // Get the log file output_file.
  FILE* output_file = NULL;
  bool must_close_output_file = false;
  file_util::ScopedFILE auto_close;
  if (!OpenOutputFile(&output_file, &must_close_output_file)) {
    LOG(ERROR) << "Unable to open '" << output_file_path_.value() << "'.";
    return false;
  }

  // Setup auto_close as appropriate.
  if (must_close_output_file)
    auto_close.reset(output_file);

  // Initialize the logger instance.
  Logger logger;
  logger.set_destination(output_file);
  logger.set_instance_id(instance_id_);
  logger.set_logger_started_callback(
      base::Bind(&SignalEvent, start_event.Get()));
  logger.set_logger_stopped_callback(
      base::Bind(&SignalEvent, stop_event.Get()));

  // Save the instance_id for the Ctrl-C handler.
  ::wcsncpy_s(saved_instance_id,
              arraysize(saved_instance_id),
              instance_id_.c_str(),
              -1);

  // Register the handler for Ctrl-C.
  if (!SetConsoleCtrlHandler(&OnConsoleCtrl, TRUE)) {
    DWORD error = ::GetLastError();
    LOG(ERROR) << "Failed to register shutdown handler: "
               << com::LogWe(error) << ".";
    return false;
  }

  // Start the logger.
  RpcLoggerInstanceManager instance_manager(&logger);
  if (!logger.Start()) {
    LOG(ERROR) << "Failed to start '" << logger_name << "'.";
    return false;
  }

  bool error = false;

  // Run the logger, either standalone or as the parent of some application.
  ScopedConsoleCtrlHandler ctrl_handler;
  if (app_command_line_.get() != NULL) {
    // We have a command to run, so launch that command and when it finishes
    // stop the logger.
    int exit_code = 0;
    if (!RunApp(*app_command_line_, instance_id_, &exit_code) ||
        exit_code != 0) {
      error = true;
    }
    ignore_result(logger.Stop());
  } else {
    // There is no command to wait for, so just register the control handler
    // (we stop the logger if this fails) and then let the logger run until
    // the control handler stops it or someone externally stops it using the
    // stop command.
    if (!ctrl_handler.Init(&OnConsoleCtrl)) {
      ignore_result(logger.Stop());
      error = true;
    }
  }

  // Run the logger to completion.
  if (!logger.RunToCompletion()) {
    LOG(ERROR) << "Failed running to completion '" << logger_name << "'.";
    error = true;
  }

  // And we're done.
  return !error;
}

bool LoggerApp::Status() {
  // TODO(rogerm): Implement me.
  return false;
}

bool LoggerApp::Spawn() {
  std::wstring logger_name(
      GetInstanceString(kLoggerRpcEndpointRoot, instance_id_));

  LOG(INFO) << "Launching background logging service '" << logger_name << "'.";

  // Get the path to ourselves.
  FilePath self_path;
  PathService::Get(base::FILE_EXE, &self_path);

  // Build a command line for starting a new instance of the logger.
  CommandLine new_command_line(self_path);
  new_command_line.AppendArg("start");

  // Copy over any other switches.
  CommandLine::SwitchMap::const_iterator it =
      logger_command_line_.GetSwitches().begin();
  for (; it != logger_command_line_.GetSwitches().end(); ++it)
    new_command_line.AppendSwitchNative(it->first, it->second);

  // Launch a new process in the background.
  base::ProcessHandle service_process;
  base::LaunchOptions options;
  options.start_hidden = true;
  if (!base::LaunchProcess(new_command_line, options, &service_process)) {
    LOG(ERROR) << "Failed to launch process.";
    return false;
  }
  DCHECK_NE(base::kNullProcessHandle, service_process);

  // Setup the start event.
  base::win::ScopedHandle start_event;
  std::wstring start_event_name(
      GetInstanceString(kLoggerStartEventRoot, instance_id_));
  if (!InitEvent(start_event_name, &start_event)) {
    LOG(ERROR) << "Unable to init start event for '" << logger_name << "'.";
    return false;
  }

  // We wait on both the start event and the process, as if the process fails
  // for any reason, it'll exit and its handle will become signaled.
  HANDLE handles[] = { start_event, service_process };
  if (::WaitForMultipleObjects(arraysize(handles),
                               handles,
                               FALSE,
                               INFINITE) != WAIT_OBJECT_0) {
    LOG(ERROR) << "The logger '" << logger_name << "' exited in error.";
    return false;
  }

  LOG(INFO) << "Background logger '" << logger_name << "' is running.";

  return true;
}

bool LoggerApp::Stop() {
  std::wstring logger_name(
      GetInstanceString(kLoggerRpcEndpointRoot, instance_id_));

  // Setup the stop event.
  base::win::ScopedHandle stop_event;
  std::wstring stop_event_name(
      GetInstanceString(kLoggerStopEventRoot, instance_id_));
  if (!InitEvent(stop_event_name, &stop_event)) {
    LOG(ERROR) << "Unable to init stop event for '" << logger_name << "'.";
    return false;
  }

  // Send the stop request.
  if (!SendStopRequest(instance_id_))
    return false;

  // We wait on both the RPC event and the process, as if the process fails for
  // any reason, it'll exit and its handle will become signaled.
  if (::WaitForSingleObject(stop_event, INFINITE) != WAIT_OBJECT_0) {
    LOG(ERROR) << "Timed out waiting for '" << logger_name << "' to stop.";
    return false;
  }

  LOG(INFO) << "The logger instance has stopped.";

  return true;
}

// Helper to resolve @p path to an open file. This will set @p must_close
// to true if @path denotes a newly opened file, and false if it denotes
// stderr or stdout.
bool LoggerApp::OpenOutputFile(FILE** output_file, bool* must_close) {
  DCHECK(output_file != NULL);
  DCHECK(must_close != NULL);

  *output_file = NULL;
  *must_close = false;

  // Check for stdout.
  if (output_file_path_.empty() ||
      ::_wcsnicmp(output_file_path_.value().c_str(),
                  kStdOut,
                  arraysize(kStdOut)) == 0) {
    *output_file = stdout;
    return true;
  }

  // Check for stderr.
  if (::_wcsnicmp(output_file_path_.value().c_str(),
                  kStdErr,
                  arraysize(kStdErr)) == 0) {
    *output_file = stderr;
    return true;
  }

  // Setup the write mode.
  const char* mode = "wb";
  if (append_)
    mode = "ab";

  // Create a new file, which the caller is responsible for closing.
  *output_file = file_util::OpenFile(output_file_path_, mode);
  if (*output_file == NULL)
    return false;

  *must_close = true;
  return true;
}

// Print the usage/help text, plus an optional @p message.
bool LoggerApp::Usage(const CommandLine* command_line,
                      const base::StringPiece& message) const {
  if (!message.empty()) {
    ::fwrite(message.data(), 1, message.length(), err());
    ::fprintf(err(), "\n\n");
  }

  ::fprintf(err(),
            kUsageFormatStr,
            command_line->GetProgram().BaseName().value().c_str());

  return false;
}

}  // namespace logger
}  // namespace trace