// Included inside lgfx::v1::i2c in the generated M5GFX component.
// S31 uses Arduino's IDF-backed Wire API instead of obsolete I2C registers.
struct S31WireState {
    int sda = -1, scl = -1;
    uint8_t address = 0;
    bool initialized = false, open = false, reading = false, writeStarted = false, failed = false;
};
static S31WireState s31WireState[2];
cpp::result<void, error_t> endTransaction(int port);
static TwoWire* s31Wire(int port) { return port == 0 ? &Wire : (port == 1 ? &Wire1 : nullptr); }
static auto s31Failure() { return cpp::fail(error_t::connection_lost); }

cpp::result<void, error_t> setPins(int port, int sda, int scl) {
    if (!s31Wire(port) || !GPIO_IS_VALID_GPIO(sda) || !GPIO_IS_VALID_GPIO(scl)) return cpp::fail(error_t::invalid_arg);
    s31WireState[port].sda = sda; s31WireState[port].scl = scl;
    return {};
}
cpp::result<int, error_t> getPinSDA(int port) {
    if (!s31Wire(port)) return cpp::fail(error_t::invalid_arg);
    return s31WireState[port].sda;
}
cpp::result<int, error_t> getPinSCL(int port) {
    if (!s31Wire(port)) return cpp::fail(error_t::invalid_arg);
    return s31WireState[port].scl;
}
cpp::result<void, error_t> release(int port) {
    auto* wire = s31Wire(port);
    if (!wire) return cpp::fail(error_t::invalid_arg);
    if (s31WireState[port].open) endTransaction(port);
    wire->end(); s31WireState[port].initialized = false;
    return {};
}
cpp::result<void, error_t> init(int port) {
    auto* wire = s31Wire(port);
    if (!wire) return cpp::fail(error_t::invalid_arg);
    auto& state = s31WireState[port];
    if (state.sda < 0 || state.scl < 0) return cpp::fail(error_t::invalid_arg);
    if (state.initialized) release(port);
    // Persistent buffers accommodate the firmware's 64x32 preview transactions.
    if (wire->setBufferSize(8192) != 8192 || !wire->begin(state.sda, state.scl)) return s31Failure();
    wire->setTimeOut(50); state.initialized = true;
    return {};
}
cpp::result<void, error_t> init(int port, int sda, int scl) {
    auto result = setPins(port, sda, scl);
    return result.has_error() ? result : init(port);
}
cpp::result<void, error_t> beginTransaction(int port, int address, uint32_t frequency, bool reading) {
    auto* wire = s31Wire(port);
    if (!wire || !s31WireState[port].initialized || address < 0 || address > 127) return cpp::fail(error_t::invalid_arg);
    auto& state = s31WireState[port];
    if (state.open) return cpp::fail(error_t::invalid_arg);
    wire->setClock(frequency);
    // Panel initialization expects a missing device to fail before writes are
    // buffered. An address-only probe preserves that detection contract.
    wire->beginTransmission(uint8_t(address));
    if (wire->endTransmission(true) != 0) return s31Failure();
    state.address = address; state.reading = reading; state.writeStarted = !reading; state.failed = false; state.open = true;
    if (!reading) wire->beginTransmission(state.address);
    return {};
}
cpp::result<void, error_t> writeBytes(int port, const uint8_t* data, size_t length) {
    auto* wire = s31Wire(port);
    if (!wire || !s31WireState[port].open || s31WireState[port].reading) return cpp::fail(error_t::invalid_arg);
    auto& state = s31WireState[port];
    if (wire->write(data, length) != length) state.failed = true;
    return state.failed ? cpp::result<void, error_t>(s31Failure()) : cpp::result<void, error_t>();
}
cpp::result<void, error_t> restart(int port, int address, uint32_t frequency, bool reading) {
    auto* wire = s31Wire(port);
    if (!wire || !s31WireState[port].open) return cpp::fail(error_t::invalid_arg);
    auto& state = s31WireState[port];
    // Wire preserves the write buffer for a combined write/read transaction.
    if (!state.reading && reading && address == state.address && !state.failed) {
        if (wire->endTransmission(false) != 0) state.failed = true;
        state.reading = true;
        return state.failed ? cpp::result<void, error_t>(s31Failure()) : cpp::result<void, error_t>();
    }
    auto result = endTransaction(port);
    return result.has_error() ? result : beginTransaction(port, address, frequency, reading);
}
cpp::result<void, error_t> readBytes(int port, uint8_t* data, size_t length, bool lastNack) {
    auto* wire = s31Wire(port);
    if (!wire || !s31WireState[port].open || !s31WireState[port].reading || length > 8192) return cpp::fail(error_t::invalid_arg);
    auto& state = s31WireState[port];
    if (state.failed) return s31Failure();
    const size_t got = wire->requestFrom(state.address, length, lastNack);
    state.writeStarted = false;
    for (size_t i = 0; i < got; ++i) data[i] = wire->read();
    if (got != length) state.failed = true;
    return state.failed ? cpp::result<void, error_t>(s31Failure()) : cpp::result<void, error_t>();
}
cpp::result<void, error_t> endTransaction(int port) {
    auto* wire = s31Wire(port);
    if (!wire) return cpp::fail(error_t::invalid_arg);
    auto& state = s31WireState[port];
    if (!state.open) return {};
    if (state.writeStarted) {
        // Also release Wire's lock after an aborted repeated-start operation.
        if (wire->endTransmission(true) != 0) state.failed = true;
    }
    state.open = false;
    return state.failed ? cpp::result<void, error_t>(s31Failure()) : cpp::result<void, error_t>();
}
