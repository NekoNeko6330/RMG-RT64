/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CORE_CRASHLOG_HPP
#define CORE_CRASHLOG_HPP

#include <filesystem>
#include <string>

// returns the directory where crash logs are written to
std::filesystem::path CoreGetCrashLogDirectory(void);

// returns the path of the file that every crash is appended to
std::filesystem::path CoreGetCrashLogFile(void);

// installs the crash handlers, creates the crash log
// directory and writes the crash log header information,
// returns false when failed
bool CoreSetupCrashLog(void);

// remembers a log message, the most recent messages
// are included in the crash log when a crash happens
void CoreCrashLogAddMessage(const std::string& message);

// remembers the ROM that's currently running,
// it's included in the crash log when a crash happens
void CoreCrashLogSetRom(const std::string& rom);

#endif // CORE_CRASHLOG_HPP
