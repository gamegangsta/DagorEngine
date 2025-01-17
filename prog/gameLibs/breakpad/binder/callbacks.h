/*
 * Dagor Engine 3 - Game Libraries
 * Copyright (C) 2023  Gaijin Games KFT.  All rights reserved
 *
 * (for conditions of distribution and use, see EULA in "prog/eula.txt")
 */

#ifndef DAGOR2_GAMELIBS_BREAKPAD_BINDER_CALLBACKS_H_
#define DAGOR2_GAMELIBS_BREAKPAD_BINDER_CALLBACKS_H_
#pragma once

#if _TARGET_PC_WIN
#include <client/windows/handler/exception_handler.h>
#elif _TARGET_PC_LINUX
#include <client/linux/handler/exception_handler.h>
#elif _TARGET_PC_MACOSX
#include <client/mac/handler/exception_handler.h>
#endif

namespace breakpad
{
struct Context;

#if _TARGET_PC_WIN
extern std::wstring wide_dump_path;
extern std::wstring wide_sender_cmd;
#endif

typedef google_breakpad::ExceptionHandler::FilterCallback filter_cb_t;
typedef google_breakpad::ExceptionHandler::MinidumpCallback minidump_cb_t;

wchar_t *prepare_args(const Context *ctx, const wchar_t *path, const wchar_t *id);
void reset_args();
filter_cb_t get_filter_cb();
minidump_cb_t get_upload_cb();

} // namespace breakpad

#endif // DAGOR2_GAMELIBS_BREAKPAD_BINDER_CALLBACKS_H_
