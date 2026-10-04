#include <cassert>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include "../src/Controls/DeviceRegistrationScreen.h"

int main()
{
    oledcare::Policy policy;
    policy.Begin(100);
    assert(policy.Evaluate(60099).contrast == 96);
    assert(policy.Evaluate(60100).contrast == 24);
    assert(policy.Evaluate(300099).asleep == false);
    assert(policy.Evaluate(300100).asleep == true);
    policy.Activity(300200);
    assert(!policy.Evaluate(300200).asleep);
    policy.Begin(0xffff0000);
    assert(policy.Evaluate(uint32_t(0xffff0000u + 60000u)).contrast == 24);
    assert(policy.Evaluate(uint32_t(0xffff0000u + 300000u)).asleep);
    policy.SetRegistration(true, 0);
    assert(policy.Evaluate(89999).contrast == 255);
    assert(policy.Evaluate(90000).contrast == 64);
    assert(policy.Evaluate(300000).contrast == 32);
    assert(!policy.Evaluate(3600000).asleep);
    policy.Activity(3600000);
    assert(policy.Evaluate(3600000).contrast == 255);

    M5GFX panel;
    OledCare::Configure(&panel, true, 0);
    OledCare::Poll(0);
    assert(panel.commands.size() == 2 && panel.commands[0] == 0x81 && panel.commands[1] == 96);
    for (int i = 1; i < 100; ++i) OledCare::Poll(i);
    assert(panel.commands.size() == 2 && panel.sleepWrites == 1);
    OledCare::Poll(300000);
    assert(panel.asleep);
    OledCare::TakeRedraw();
    OledCare::RequestWake();
    OledCare::Poll(300001);
    assert(!panel.asleep && OledCare::TakeRedraw());
    OledCare::Configure(&panel, false, 0);
    const auto writes = panel.commands.size();
    const auto sleepWrites = panel.sleepWrites;
    OledCare::Poll(300000);
    assert(panel.commands.size() == writes && panel.sleepWrites == sleepWrites);

    assert(lgfx_qrcode_getBufferSize(2) <= 128);
    OledCare::Configure(&panel, true, 0);
    OledCare::SetRegistration(true, 0);
    const String password("Factory#1229#25");
    panel.setTextSize(2);
    for (const String& testPassword : {password, String(63, 'X')})
    {
        ShowDeviceRegistrationScreen(&panel, String("0123456789abcdef0123456789abcdef"), testPassword);
        std::string reconstructed;
        for (size_t line = 1; line < panel.drawnStrings.size(); ++line) reconstructed += panel.drawnStrings[line];
        assert(reconstructed == testPassword);
        assert(panel.style.size_x == 2 && panel.style.size_y == 2);
        assert(panel.getFont() == &fonts::Font0);
    }
    unsigned long long random = 0x123456781234abcdULL;
    std::ofstream ids("build/registration-check/ids.txt");
    for (int fixture = 0; fixture < 32; ++fixture)
    {
        std::ostringstream id;
        id << std::hex << std::setfill('0');
        for (int part = 0; part < 4; ++part)
        {
            random ^= random << 13;
            random ^= random >> 7;
            random ^= random << 17;
            id << std::setw(8) << uint32_t(random);
        }
        if (fixture == 0) id.str("0123456789abcdef0123456789abcdef");
        if (fixture == 1) id.str("24ec4a07fda800000000000000000000");
        if (fixture == 2) id.str("ffffffffffffffffffffffffffffffff");
        ids << id.str() << '\n';
        for (int phase = 0; phase < 9; ++phase)
        {
            OledCare::Poll(phase * 30000);
            ShowDeviceRegistrationScreen(&panel, String(id.str().c_str()), password);
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 128; ++x)
                    if (panel.pixels[y * 128 + x]) assert(x >= 4 && x < 124 && y >= 11 && y < 53);
            const auto path = "build/registration-check/qr_" + std::to_string(fixture) + "_" + std::to_string(phase) + ".pgm";
            std::ofstream image(path, std::ios::binary);
            image << "P5\n128 64\n255\n";
            image.write(reinterpret_cast<const char*>(panel.pixels.data()), panel.pixels.size());
        }
    }
    puts("PASS: OLED timing, wraparound, sleep/wake, idle I2C, LCD isolation; 288 QR fixtures exported");
}
