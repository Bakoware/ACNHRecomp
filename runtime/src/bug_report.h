// Problem reports: the logs of this session and the previous one (where a crash or a hang is), config.ini and a few
// lines about the machine, gathered in a folder (and a .zip of it) on the desktop, to attach to a GitHub issue. Never
// any game data: no dumps, saves or shaders.
#pragma once

#include <string>

namespace nx {

std::string log_file_path();        // loader.cpp: this session's log (NX_LOG, else next to the data folder)
bool previous_session_failed();     // the previous session's log has a CRASH, FATAL or hang line
// Writes the report; returns the .zip's path (or the folder's when zipping failed), empty with `error` set on failure.
std::string write_bug_report(std::string& error);
// NX_REPORT=1: write a report (this run's log is then just this; the previous run's, the one to report, is kept as
// nx-previous.log), print where, and return true for main to exit: for a game that crashes before its settings screen.
bool report_mode();

}  // namespace nx
