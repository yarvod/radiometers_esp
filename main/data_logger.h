#pragma once

#include <string>

// Build a timestamped CSV filename for a new log file.
std::string BuildLogFilename(const std::string& postfix_raw);

// Flush and fsync the current log file to storage.
bool FlushLogFile();

// Open (or reopen) the log file with the given postfix; writes CSV header.
bool OpenLogFileWithPostfix(const std::string& postfix);
