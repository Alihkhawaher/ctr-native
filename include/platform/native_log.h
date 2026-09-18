#ifndef NATIVE_LOG_H
#define NATIVE_LOG_H

#include <macros.h>

int Platform_LogSetPath(const char *path);
const char *Platform_LogGetPath(void);
void Platform_LogInit(const char *appName);
void Platform_LogShutdown(void);
void Platform_LogFlush(void);
void Platform_Log(const char *fmt, ...) CTR_PRINTF_FORMAT(1, 2);
void Platform_LogWarn(const char *fmt, ...) CTR_PRINTF_FORMAT(1, 2);
void Platform_LogError(const char *fmt, ...) CTR_PRINTF_FORMAT(1, 2);

// The "[CTR Debug]" lines are the debug/verbose channel: enabled by default,
// silenced at runtime via Platform_LogSetDebugEnabled(0) (CLI: --quiet / -q).
void Platform_LogSetDebugEnabled(int enabled);
int Platform_LogDebugEnabled(void);

#endif
