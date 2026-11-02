//==========================================
/// @file       wtime.h
/// @brief      windows based timing functions
//==========================================

#pragma once

#ifndef WTIME_H
#define WTIME_H

#include "typedef.h"

u64 time_now_ns();
void sleep_until_ns(u64 deadline);

#endif // WTIME_H