//==========================================
/// @file       debugger.h
/// @brief      
//==========================================

#pragma once

#ifndef DEBUGGER_H
#define DEBUGGER_H

#define log(...) debug_log(__VA_ARGS__)

#include "typedef.h"

void debugger_init();
void debug_log(const char* text, ...);
void dump_crash_log();

#endif // DEBUGGER_H