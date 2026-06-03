// Windows API constants for SetThreadExecutionState
#ifndef CONSTANTS_H
#define CONSTANTS_H

#ifndef ES_CONTINUOUS
#define ES_CONTINUOUS 0x80000000
#endif

#ifndef ES_SYSTEM_REQUIRED
#define ES_SYSTEM_REQUIRED 0x00000001
#endif

#ifndef ES_DISPLAY_REQUIRED
#define ES_DISPLAY_REQUIRED 0x00000002
#endif

#ifndef ES_AWAYMODE_REQUIRED
#define ES_AWAYMODE_REQUIRED 0x00000040
#endif

#define CURRENT_VERSION "1.0.0"

#endif // CONSTANTS_H