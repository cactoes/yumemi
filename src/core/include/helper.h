//==========================================
/// @file       helper.h
/// @brief      
//==========================================

#pragma once

#ifndef HELPER_H
#define HELPER_H

#define MASK_IS_SET(v, m)      (((v) & (m)) == (m))
#define MASK_ANY_SET(v, m)     (((v) & (m)) != 0)
#define MASK_SET(v, m)         ((v) |= (m))
#define MASK_CLEAR(v, m)       ((v) &= ~(m))
#define MASK_TOGGLE(v, m)      ((v) ^= (m))

#endif // HELPER_H