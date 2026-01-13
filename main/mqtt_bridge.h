#pragma once

#include <string>

void StartMqttBridge();
void PublishMeasurementPayload(const std::string& payload);
