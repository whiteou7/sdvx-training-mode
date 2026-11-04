#pragma once

namespace tr {

void log_init(const wchar_t* path);

void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace tr
