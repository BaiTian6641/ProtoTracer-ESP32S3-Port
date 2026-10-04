#pragma once

#include <cstdint>

namespace oledcare
{
    struct Frame
    {
        uint8_t contrast = 96;
        int8_t x = 0;
        int8_t y = 0;
        bool asleep = false;
    };

    // Time is unsigned so inactivity and pixel shifting survive millis() rollover.
    // Animation/audio activity intentionally does not reset the inactivity timer.
    class Policy
    {
    public:
        void Begin(uint32_t now)
        {
            registration = false;
            lastActivity = shiftEpoch = now;
        }

        void SetRegistration(bool enabled, uint32_t now)
        {
            registration = enabled;
            lastActivity = shiftEpoch = now;
        }

        void Activity(uint32_t now) { lastActivity = now; }
        bool IsRegistration() const { return registration; }

        Frame Evaluate(uint32_t now) const
        {
            Frame frame;
            const uint32_t idle = now - lastActivity;
            const uint32_t phase = (now - shiftEpoch) / 30000;
            if (registration)
            {
                // Keep registration scannable even after a long wait. A button,
                // AP client change or portal state change restores full contrast.
                frame.contrast = idle < 90000 ? 255 : (idle < 300000 ? 64 : 32);
                constexpr int8_t xs[] = {0, -1, 1, -1, 0, 1, -1, 0, 1};
                constexpr int8_t ys[] = {0, -1, -1, 0, 1, 0, 1, -1, 1};
                frame.x = xs[phase % 9];
                frame.y = ys[phase % 9];
            }
            else
            {
                frame.contrast = idle < 60000 ? 96 : 24;
                frame.asleep = idle >= 300000;
                frame.x = phase % 3;
                frame.y = (phase / 3) % 2;
            }
            return frame;
        }

    private:
        uint32_t lastActivity = 0;
        uint32_t shiftEpoch = 0;
        bool registration = false;
    };
}
