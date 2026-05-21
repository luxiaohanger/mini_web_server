#include "Timer.h"

Timer::Timer(int id, struct timespec ex, std::function<void()> cb)
    : timerId(id), expiration(ex), callBack(std::move(cb)) {}

Timer::~Timer() {}
