#pragma once

#include <string>

void StartMqttBridge();
void PublishMeasurementPayload(const std::string& payload);
void PublishErrorPayload(const std::string& payload);
