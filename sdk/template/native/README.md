# native/

C for the game's own CPU (its ARM11) that changes the game: the port's
features, never the game's code. `port.py native` builds every `.c` and
`.cpp` here, with Bergamota's `sdk/include/port.h`, into `decomp.bin` and
`decomp.txt`, which the recompiler and the runtime take; it runs as the
game's own code, so it can call the game's functions, including the ones
that wait.

```c
#include "port.h"

/* the game's function, called by address (ARM; t_ for Thumb) */
extern void Thing_Update(u32 self) __asm__("f_00123456");

PORT_OPTION(skip_thing, 0, "Skip the thing", "What the option does, for the menu.");

static void update(u32 self) {
    if (!port_skip_thing) Thing_Update(self);
}

/* the vtable entry that pointed at Thing_Update now points at our wrapper */
PORT_PATCH(0x00654320, update);
```

Prefer a wrapper reached through a pointer the game already follows
(`PORT_PATCH`) to replacing a function (`__asm__("r_XXXXXXXX")`). Bergamota's
`docs/porting.md` has the rest: the names, options, what the player writes
into the code, modules (CROs), and carrying addresses to another version of
the game.
