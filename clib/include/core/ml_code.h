/*
 * ml_code — Unified error code definitions and string conversion.
 *
 * Single source of truth for all error codes used throughout clib.
 * All error codes are negative; ML_OK (0) indicates success.
 * ml_code_string() converts a code to a human-readable message.
 */
#ifndef __ML_CODE_H__
#define __ML_CODE_H__

#include "core/ml_types.h"

#define ML_OK       0

#define ML_ERROR    (-1)
#define ML_EAGAIN   (-2)
#define ML_ENOMEM   (-3)
#define ML_EPARAM   (-4)
#define ML_EINVAL   (-5)
#define ML_ECRYPTO  (-6)
#define ML_EBACKEND (-7)
#define ML_ETINFN   (-8)
#define ML_ENODST   (-9)
#define ML_ENOREG   (-10)
#define ML_ESOCKT   (-11)
#define ML_ECONNT   (-12)
#define ML_ENOEXT   (-13)
#define ML_EEXIST   (-14)
#define ML_ENOSES   (-15)
#define ML_ERRPID   (-16)
#define ML_EALGOM   (-17)
#define ML_EMCAST   (-18)
#define ML_EJOIN    (-19)
#define ML_ELEAVE   (-20)
#define ML_EMORE    (-21)
#define ML_EFREG    (-22)

static inline const char *ml_code_string(ml_int_t code)
{
    switch (code) {
        case ML_OK:       return "success";
        case ML_ERROR:    return "system error";
        case ML_EAGAIN:   return "operation would block";
        case ML_ENOMEM:   return "no memory";
        case ML_EPARAM:   return "invalid parameter";
        case ML_EINVAL:   return "invalid argument";
        case ML_ECRYPTO:  return "crypto auth failure";
        case ML_EBACKEND: return "unsupported backend";
        case ML_ETINFN:   return "interface error";
        case ML_ENODST:   return "no destination";
        case ML_ENOREG:   return "no file or register";
        case ML_ESOCKT:   return "socket error";
        case ML_ECONNT:   return "connection error";
        case ML_ENOEXT:   return "not exist";
        case ML_EEXIST:   return "already exist";
        case ML_ENOSES:   return "no session";
        case ML_ERRPID:   return "pid error";
        case ML_EALGOM:   return "algorithm error";
        case ML_EMCAST:   return "not multicast ip";
        case ML_EJOIN:    return "join multicast error";
        case ML_ELEAVE:   return "leave multicast error";
        case ML_EMORE:    return "multicast limit reached";
        case ML_EFREG:    return "file format error";
    }
    return "unknown error";
}

#endif
