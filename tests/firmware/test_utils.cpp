#include <iostream>
#include <string>

#include "app_utils.h"
#include "test_stubs.h"

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    failures++;
  }
}

void TestTrim() {
  Check(Trim("  abc  ") == "abc", "Trim removes spaces");
  Check(Trim("abc") == "abc", "Trim keeps content");
  Check(Trim("   ") == "", "Trim handles all-spaces");
}

void TestParseBool() {
  bool out = false;
  Check(ParseBool("true", &out) && out, "ParseBool true");
  Check(ParseBool("1", &out) && out, "ParseBool 1");
  Check(ParseBool("yes", &out) && out, "ParseBool yes");
  Check(ParseBool("on", &out) && out, "ParseBool on");
  Check(ParseBool("false", &out) && !out, "ParseBool false");
  Check(ParseBool("0", &out) && !out, "ParseBool 0");
  Check(ParseBool("no", &out) && !out, "ParseBool no");
  Check(ParseBool("off", &out) && !out, "ParseBool off");
  out = true;
  Check(!ParseBool("maybe", &out), "ParseBool rejects unknown");
}

void TestSanitizeFilename() {
  std::string out;
  Check(SanitizeFilename("data.txt", &out), "SanitizeFilename ok");
  Check(out == std::string(CONFIG_MOUNT_POINT) + "/data.txt", "SanitizeFilename path");
  Check(!SanitizeFilename("", &out), "SanitizeFilename rejects empty");
  Check(!SanitizeFilename("bad/name", &out), "SanitizeFilename rejects slash");
  Check(!SanitizeFilename("bad name", &out), "SanitizeFilename rejects space");
}

void TestSanitizePath() {
  std::string out;
  Check(SanitizePath("logs/data.txt", &out), "SanitizePath ok");
  Check(out == std::string(CONFIG_MOUNT_POINT) + "/logs/data.txt", "SanitizePath path");
  Check(SanitizePath("/logs/data.txt", &out), "SanitizePath ok with leading slash");
  Check(!SanitizePath("../secret", &out), "SanitizePath rejects parent dir");
  Check(!SanitizePath("bad//name", &out), "SanitizePath rejects double slash");
}

void TestSanitizePostfix() {
  Check(SanitizePostfix("my file") == "my_file", "SanitizePostfix spaces");
  Check(SanitizePostfix("___") == "", "SanitizePostfix trims underscores");
  Check(SanitizePostfix("name-1") == "name-1", "SanitizePostfix keeps dash");
}

void TestSanitizeId() {
  Check(SanitizeId("dev-1") == "dev-1", "SanitizeId ok");
  Check(SanitizeId("dev#1") == "dev1", "SanitizeId strips invalid");
  Check(SanitizeId("!!!") == "device", "SanitizeId fallback");
}

void TestIsoUtcNow() {
  stub_time_sec = 0;
  Check(IsoUtcNow() == "1970-01-01T00:00:00Z", "IsoUtcNow epoch");
}

void TestMaskUtils() {
  Check(ClampSensorMask(0xFFFF, 3) == 0x7, "ClampSensorMask caps");
  Check(ClampSensorMask(0x2, 1) == 0x0, "ClampSensorMask removes out of range");
  Check(FirstSetBitIndex(0) == 0, "FirstSetBitIndex zero");
  Check(FirstSetBitIndex(0b1000) == 3, "FirstSetBitIndex finds bit");
}

void TestRssiQuality() {
  Check(RssiToQuality(-100) == 0, "RssiToQuality -100");
  Check(RssiToQuality(-50) == 100, "RssiToQuality -50");
  Check(RssiToQuality(-75) == 50, "RssiToQuality -75");
}

}  // namespace

int main() {
  TestTrim();
  TestParseBool();
  TestSanitizeFilename();
  TestSanitizePath();
  TestSanitizePostfix();
  TestSanitizeId();
  TestIsoUtcNow();
  TestMaskUtils();
  TestRssiQuality();

  if (failures == 0) {
    std::cout << "OK: all firmware utils tests passed\n";
    return 0;
  }
  std::cerr << failures << " test(s) failed\n";
  return 1;
}
