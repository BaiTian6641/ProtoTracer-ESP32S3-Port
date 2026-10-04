#include "../src/Debug/SerialDebug.h"
#include "../src/Debug/BestEffortLog.h"
#include <sstream>
#include <vector>

uint8_t live[1024];
bool Resolve(const char* name, SerialDebug::View& view)
{
    if (strcmp(name, "test")) return false;
    view = {live, sizeof(live), "u8", 1024, 1};
    return true;
}
void Stats(char* text, size_t capacity) { snprintf(text, capacity, "@DBG TIMING ok\n"); }
bool profileActive = false;
bool Profile(const char* mode)
{
    if (!strcmp(mode, "normal")) { profileActive = false; return true; }
    if (!strcmp(mode, "spiral")) { profileActive = true; return true; }
    return false;
}
void Poll()
{
    Serial.capacity = 4096;
    const unsigned before = unsigned(std::count(Serial.tx.begin(), Serial.tx.end(), '\n'));
    SerialDebug::Poll(Resolve, Stats, 1, Profile);
    const unsigned after = unsigned(std::count(Serial.tx.begin(), Serial.tx.end(), '\n'));
    if (after - before > 4) exit(10);
}
void Reset(const char* command)
{
    SerialDebug::Release();
    SerialDebug::output[0] = 0;
    SerialDebug::inputLength = 0;
    SerialDebug::overflow = false;
    Serial.rx = command;
    Serial.tx.clear();
    Serial.cursor = 0;
    debugClock = 0;
}
int main()
{
    for (unsigned i = 0; i < sizeof(live); ++i) live[i] = uint8_t(i);
    Reset("dbg dump test\n");
    Poll();
    memset(live, 255, sizeof(live)); // producer changes after the snapshot
    for (unsigned i = 0; i < 20; ++i) Poll();
    std::vector<uint8_t> data;
    std::istringstream stream(Serial.tx);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.find("@DBG DATA ") != 0) continue;
        const std::string hex = line.substr(line.find_last_of(' ') + 1);
        for (size_t i = 0; i < hex.size(); i += 2)
            data.push_back(uint8_t(strtoul(hex.substr(i, 2).c_str(), nullptr, 16)));
    }
    if (data.size() != sizeof(live) || debugAllocations || SerialDebug::snapshot) return 1;
    for (unsigned i = 0; i < data.size(); ++i) if (data[i] != uint8_t(i)) return 2;

    Reset("dbg dump test\n");
    Serial.capacity = 0;
    SerialDebug::Poll(Resolve, Stats, 2);
    if (!SerialDebug::snapshot || debugAllocations != 1) return 3;
    debugClock = 5001;
    Poll();
    if (SerialDebug::snapshot || debugAllocations || Serial.tx.find("timeout") == std::string::npos) return 4;

    Reset("dbg dump test\n"); Poll();
    Serial.rx += "dbg cancel\n"; Poll();
    if (SerialDebug::snapshot || debugAllocations || Serial.tx.find("CANCEL") == std::string::npos) return 5;

    for (const char* command : {"dbg dump test 1024\n", "dbg dump test -1\n", "dbg dump test 0 65537\n",
                               "dbg dump test 0 1 extra\n", "dbg dump missing\n"}) {
        Reset(command); Poll();
        if (debugAllocations || Serial.tx.find("ERROR") == std::string::npos) return 6;
    }
    std::string longLine = "dbg " + std::string(120, 'x') + "\n";
    Reset(longLine.c_str()); Poll(); Poll();
    if (Serial.tx.find("line too long") == std::string::npos) return 7;
    Reset("dbg dump test\n"); debugAllocationFailure = true; Poll();
    debugAllocationFailure = false;
    if (debugAllocations || Serial.tx.find("allocation failed") == std::string::npos) return 8;
    Reset(""); for (unsigned i = 0; i < 100; ++i) Poll();
    if (debugAllocations || !Serial.tx.empty()) return 9;
    Reset("dbg profile spiral\n"); Poll();
    if (!profileActive || Serial.tx.find("PROFILE material=spiral") == std::string::npos) return 11;
    Reset("dbg profile normal\n"); Poll();
    if (profileActive) return 12;
    for (const char* command : {"dbg profile\n", "dbg profile unknown\n", "dbg profile spiral extra\n"}) {
        Reset(command); Poll();
        if (profileActive || Serial.tx.find("ERROR") == std::string::npos) return 13;
    }
    Reset("dbg profile spiral\n");
    SerialDebug::Poll(Resolve, Stats, 1);
    if (profileActive || Serial.tx.find("disabled") == std::string::npos) return 14;
    Reset("dbg dump test\n"); Poll();
    Serial.rx += "dbg profile spiral\n"; Poll();
    if (profileActive || Serial.tx.find("busy") == std::string::npos) return 15;
    SerialDebug::Release();
    Reset("dbg help\n"); Poll();
    if (Serial.tx.find("profile") == std::string::npos) return 16;
    Reset("dbg help\n"); SerialDebug::Poll(Resolve, Stats, 1);
    if (Serial.tx.find("profile") != std::string::npos) return 17;
    Reset(""); Serial.capacity = 4;
    BestEffortLog::Printf("hello %u\n", 7u);
    if (!Serial.tx.empty()) return 18;
    Serial.capacity = 128;
    BestEffortLog::Printf("hello %u\n", 7u);
    if (Serial.tx != "hello 7\n") return 19;
    Serial.tx.clear(); Serial.capacity = 4096;
    BestEffortLog::Printf("%s", std::string(250, 'x').c_str());
    if (!Serial.tx.empty()) return 20;
    puts("PASS serial snapshots: immutable data, bounded TX, timeout/cancel, invalid ranges, allocation failure, idle, optional profiles");
}
