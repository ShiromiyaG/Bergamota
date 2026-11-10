/* fdk-aac includes Android's own log.h (the system's, not the NDK's) to
 * report a malformed stream it recovers from: nothing reported here */
#ifndef BERGAMOTA_ANDROID_LOG_H
#define BERGAMOTA_ANDROID_LOG_H
#define android_errorWriteLog(tag, subtag) ((void)0)
#endif
