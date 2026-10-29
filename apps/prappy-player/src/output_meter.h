#pragma once
#include <memory>

// Windows session peaks are measured at output, after mpv's decode-ahead buffer.
// COM and endpoint discovery stay on a dedicated thread; reads never block UI.
class OutputMeter {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    OutputMeter();
    ~OutputMeter();
    OutputMeter(const OutputMeter&) = delete;
    OutputMeter& operator=(const OutputMeter&) = delete;
    void enable(bool value);
    double read();
    bool available() const;
};
