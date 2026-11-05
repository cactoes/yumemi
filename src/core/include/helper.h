//==========================================
/// @file       helper.h
/// @brief      
//==========================================

#pragma once

#ifndef HELPER_H
#define HELPER_H

#define MASK_IS_SET(v, m)       (((v) & (m)) == (m))
#define MASK_ANY_SET(v, m)      (((v) & (m)) != 0)
#define MASK_SET(v, m)          ((v) |= (m))
#define MASK_CLEAR(v, m)        ((v) &= ~(m))
#define MASK_TOGGLE(v, m)       ((v) ^= (m))
#define MASK_MATCHES(v, m, e)   (((v) & (m)) == (e))

#define MEM_ACCESS_DEFAULT        (0u)
#define MEM_ACCESS_NOCOUNT        (1u << 0)
#define MEM_ACCESS_NOSIDE_EFFECT  (1u << 1)

#endif // HELPER_H