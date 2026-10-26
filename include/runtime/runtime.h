/* the runtime's face to the games' C (the decomp's and 3dsrecomp's): plain
 * C, so that either calls it the same way */

#ifndef RUNTIME_RUNTIME_H
#define RUNTIME_RUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* how the runtime answers a service */
typedef enum {
    RUNTIME_NATIVE,  /* implemented: the game needs it to run */
    RUNTIME_OFFLINE, /* "not connected" / "not present", as a 3DS with no
                        network and no camera would answer */
    RUNTIME_UNKNOWN, /* never seen in these games */
} runtime_answer;

/* the 3DS name of a syscall the runtime answers, or 0 */
const char *runtime_syscall_name(uint8_t number);

/* how a service (by its port name, "fs:USER") is answered */
runtime_answer runtime_service_answer(const char *name);

#ifdef __cplusplus
}
#endif

#endif
