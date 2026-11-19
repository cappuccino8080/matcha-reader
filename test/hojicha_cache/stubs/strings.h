#pragma once
#ifdef _MSC_VER
#include <cstring>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include_next <strings.h>
#endif
