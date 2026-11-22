#ifndef GOS_BUILDID_H
#define GOS_BUILDID_H

/* make passes the real values with -D. These are only a fallback. */
#ifndef GOS_BUILD_HASH
#define GOS_BUILD_HASH "unknown"
#endif
#ifndef GOS_BUILD_UTC
#define GOS_BUILD_UTC "unknown"
#endif

#endif
