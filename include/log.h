/**
 * Copyright (c) 2020 rxi
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the MIT license. See `log.c` for details.
 */

#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <time.h>
#include <se_libs.h>

#define LOG_VERSION "0.1.0"

typedef struct {
  va_list ap;
  const char *fmt;
  const char *file;
  const char *func;
  struct tm *time;
  void *udata;
  int line;
  int level;
  size_t task_id;
} log_Event;

typedef void (*log_LogFn)(log_Event *ev);
typedef void (*log_LockFn)(bool lock, void *udata);

enum { LOG_TRACE, LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR, LOG_FATAL };

#define log_trace(...) log_log(0, LOG_TRACE, __FUNCTION__, __FILE__, __LINE__, __VA_ARGS__)
#define log_debug(...) log_log(0, LOG_DEBUG, __FUNCTION__, __FILE__, __LINE__, __VA_ARGS__)
#define log_info(...)  log_log(0, LOG_INFO,  __FUNCTION__, __FILE__, __LINE__, __VA_ARGS__)
#define log_warn(...)  log_log(0, LOG_WARN,  __FUNCTION__, __FILE__, __LINE__, __VA_ARGS__)
#define log_error(...) log_log(0, LOG_ERROR, __FUNCTION__, __FILE__, __LINE__, __VA_ARGS__)
#define log_fatal(...) log_log(0, LOG_FATAL, __FUNCTION__, __FILE__, __LINE__, __VA_ARGS__)

#define alog_trace(...) log_log(get_current_task_id(__await__), LOG_TRACE, __FUNCTION__, __FILE__, __LINE__, __VA_ARGS__)
#define alog_debug(...) log_log(get_current_task_id(__await__), LOG_DEBUG, __FUNCTION__, __FILE__, __LINE__, __VA_ARGS__)
#define alog_info(...)  log_log(get_current_task_id(__await__), LOG_INFO,  __FUNCTION__, __FILE__, __LINE__, __VA_ARGS__)
#define alog_warn(...)  log_log(get_current_task_id(__await__), LOG_WARN,  __FUNCTION__, __FILE__, __LINE__, __VA_ARGS__)
#define alog_error(...) log_log(get_current_task_id(__await__), LOG_ERROR, __FUNCTION__, __FILE__, __LINE__, __VA_ARGS__)
#define alog_fatal(...) log_log(get_current_task_id(__await__), LOG_FATAL, __FUNCTION__, __FILE__, __LINE__, __VA_ARGS__)

const char* log_level_string(int level);
void log_set_lock(log_LockFn fn, void *udata);
void log_set_level(int level);
void log_set_quiet(bool enable);
int log_add_callback(log_LogFn fn, void *udata, int level);
int log_add_fp(FILE *fp, int level);
void log_set_systemd(bool enable);

void log_log(size_t task_id, int level, const char *func, const char *file, int line, const char *fmt, ...);
void log_vlog(size_t task_id, int level, const char *func, const char *file, int line, const char *fmt, va_list ap);

#endif
