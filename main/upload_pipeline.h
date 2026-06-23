#pragma once

#include <string>

// Update SD stats while already holding the SD lock.
void UpdateSdStatsLocked();

// Update SD stats (acquires the SD lock internally).
void UpdateSdStats();

// FreeRTOS task: periodically calls UpdateSdStats.  Start once at boot.
void SdStatsTask(void*);

// Move the current log file to the upload queue and reset log_file/current_log_path.
bool QueueCurrentLogForUpload();

// Start a one-shot FreeRTOS task that deletes up to max_files uploaded files.
bool StartUploadedClearTask(int max_files, std::string* out_status);

// FreeRTOS task: runs the upload pipeline on a timer.  Start once at boot.
void UploadTask(void*);

// Diagnostics (used at boot and in upload flow).
void LogMemoryStatus(const char* label);
const char* MbedtlsAllocModeName();
