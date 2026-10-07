#include "../tools/s31_parlio_frame_sync.h"
#include <cassert>
#include <cstdio>

int main() {
    int a = 0, b = 0, unrelated = 0;
    FakeUnit unit;
    S31ParlioFrameSync sync;
    parlio_transmit_config_t config;
    assert(sync.Initialize(&unit));
    assert(sync.Queue(&unit, &a, &b, 128, &config));
    assert(!sync.Wait(0) && sync.Pending()); // A timeout retains both DMA leases.
    assert(!sync.Queue(&unit, &b, &a, 128, &config));
    unit.Complete(&unrelated, &b);
    assert(!sync.Wait(0) && sync.Pending());
    unit.Complete(&a, &unrelated);
    assert(!sync.Wait(0) && sync.Pending());
    unit.Complete(&a, &b);
    assert(sync.Wait(0) && !sync.Pending());
    unit.current = &b;
    unit.immediate = true; // ISR acknowledgment may precede transmit's return.
    assert(sync.Queue(&unit, &b, &a, 128, &config));
    assert(sync.Wait(0) && !sync.Pending());
    unit.fail = true;
    assert(!sync.Queue(&unit, &a, &b, 128, &config) && !sync.Pending());
    std::puts("PASS S31 frame ownership: timeout, mismatched callback, early ISR, queue error");
}
