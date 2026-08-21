#pragma once
// Host-test shim for Arduino's WString.h (Windows x64, MSVC).
//
// Provides just enough of the Arduino String class for the firmware headers
// to COMPILE and for their ToString()/formatting helpers to run when invoked.
// Most of these helpers are never called by the render path; the API surface
// below covers every String construction/concatenation used by src/Math,
// src/Render and src/Materials headers.
//
// Kept in sync with: src/Math/*.h, src/Render/*.h, src/Materials/*.h.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cfloat>
#include <string>

// GCC's math.h defines __FLT_MAX__; MSVC's does not. Mathematics.h uses it
// in a constexpr, so provide the standard value when missing.
#ifndef __FLT_MAX__
#define __FLT_MAX__ FLT_MAX
#endif

class String {
public:
    String() : mImpl("") {}

    String(const char* s) : mImpl(s ? s : "") {}

    String(char c) : mImpl(1, c) {}

    String(int value) : mImpl(std::to_string(value)) {}

    String(unsigned int value) : mImpl(std::to_string(value)) {}

    String(long value) : mImpl(std::to_string(value)) {}

    String(unsigned long value) : mImpl(std::to_string(value)) {}

    String(float value, unsigned int decimals = 2) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", static_cast<int>(decimals), static_cast<double>(value));
        mImpl = buf;
    }

    String(double value, unsigned int decimals = 2) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", static_cast<int>(decimals), value);
        mImpl = buf;
    }

    String(const String& other) : mImpl(other.mImpl) {}

    String(const std::string& s) : mImpl(s) {}

    String& operator=(const String& other) {
        if (this != &other) mImpl = other.mImpl;
        return *this;
    }

    const char* c_str() const { return mImpl.c_str(); }

    unsigned int length() const { return static_cast<unsigned int>(mImpl.size()); }

    bool isEmpty() const { return mImpl.empty(); }

    bool operator==(const String& other) const { return mImpl == other.mImpl; }

    bool operator!=(const String& other) const { return mImpl != other.mImpl; }

    String operator+(const String& other) const { return String(mImpl + other.mImpl); }

    String operator+(const char* other) const { return String(mImpl + (other ? other : "")); }

    String operator+(char c) const { return String(mImpl + c); }

    char charAt(unsigned int index) const {
        return index < mImpl.size() ? mImpl[index] : '\0';
    }

    String substring(unsigned int begin, unsigned int end = static_cast<unsigned int>(-1)) const {
        if (begin >= mImpl.size()) return String();
        if (end == static_cast<unsigned int>(-1) || end > mImpl.size()) end = static_cast<unsigned int>(mImpl.size());
        if (end < begin) end = begin;
        return String(mImpl.substr(begin, end - begin).c_str());
    }

private:
    std::string mImpl;
};

// Global concatenation for `literal + String` (Arduino's String provides
// these as friends; the firmware headers rely on them, e.g.
// `"[" + String(A)` in IndexGroup::ToString).
inline String operator+(const char* lhs, const String& rhs) {
    return String(std::string(lhs ? lhs : "") + rhs.c_str());
}

inline String operator+(char lhs, const String& rhs) {
    return String(std::string(1, lhs) + rhs.c_str());
}

inline String operator+(const String& lhs, char rhs) {
    return String(lhs.c_str() + std::string(1, rhs));
}
