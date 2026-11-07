/* What a port's own code for the game's ARM11 (the C in its native/) builds
 * against: the game's types, the ways into the game, and the port's options.
 * Bergamota's SDK (docs/porting.md); sdk/native.py compiles native/ with
 * this folder on the include path, for the game's CPU (armv6k, VFPv2, the
 * softfp calls the game makes), and links it against the game's functions
 * by address.
 *
 * Names the builder reads (in assembler names, 8 hex digits, no 0x):
 *
 *   f_XXXXXXXX           a function of the game's executable, ARM (call it)
 *   t_XXXXXXXX           the same, Thumb
 *   f_<Module>_XXXXXXXX  a function of a module (a CRO), XXXXXXXX its offset
 *                        from where the module loads; t_ for Thumb
 *   r_XXXXXXXX           this function replaces the game's at XXXXXXXX
 *   rt_XXXXXXXX          the same, the game's being Thumb
 *   r_<Module>_XXXXXXXX  replaces a module's, as the module loads
 *
 *   extern void Bloom_Ghosting(u32 self) __asm__("f_0042F334");
 *   u32 *CopyMtx44WithHeader(u32 *dst, const u32 *src, u32 header) __asm__("r_00109364");
 *
 * A name in one of the port's symbol files (port.toml's [native] symbols,
 * and what `3dsrecomp symbols` found) can be called by that name instead.
 * Prefer a wrapper reached through a pointer the game already follows
 * (PORT_PATCH) to replacing a function: the game's own code then runs as it
 * is. */

#ifndef BERGAMOTA_PORT_H
#define BERGAMOTA_PORT_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed char s8;
typedef signed short s16;
typedef signed int s32;
typedef signed long long s64;
typedef float f32;
typedef double f64;

#ifndef NULL
#define NULL ((void *)0)
#endif

/* a field at a byte offset, for structs not written down yet */
#define FIELD(type, base, offset) (*(type *)((u8 *)(base) + (offset)))

/* addresses in a module (a CRO), known once it loads: the runtime fills a
 * word of the port's with each as the module loads. offsets are 8 hex
 * digits. MODULE_ADDRESS: offset from where the module loads (its file's
 * offset), e.g. a function of it to hand on as a pointer (MODULE_THUMB for a
 * Thumb one); MODULE_DATA: offset in its segment n, as its relocations give
 * them, for its data and bss */
#define MODULE_ADDRESS(module, offset) \
    ({ extern u32 __slot_a_##module##_##offset; __slot_a_##module##_##offset; })
#define MODULE_THUMB(module, offset) \
    ({ extern u32 __slot_at_##module##_##offset; __slot_at_##module##_##offset; })
#define MODULE_DATA(module, n, offset) \
    ({ extern u8 *__slot_s##n##_##module##_##offset; __slot_s##n##_##module##_##offset; })

/* A word of the game's executable (a vtable's entry, a pointer in a table)
 * changed to value as the game loads: a `patch` line of decomp.txt, which
 * the runtime writes into the game before it starts. The way into the game
 * when the port only wraps what the game does: the game's own function is
 * left as it is and called from the wrapper. address as 0x and 8 hex
 * digits. */
#define PORT_PATCH(address, value) \
    __attribute__((used)) static const void *const port_patch_##address = (const void *)(value)

/* An option of the port's, shown in the player's menu (F1, the Port tab)
 * and the launcher's Graphics page, kept in the player's settings ([port]
 * name): the u32 port_<name> the code reads, which the player writes as it
 * changes (live), value its default. PORT_OPTION is a toggle (0 or 1),
 * PORT_OPTION_RANGE a number from min to max. label and help are string
 * literals; help must not hold a '|'. */
#define PORT_OPTION(name, value, label, help) \
    u32 port_##name = (value); \
    __attribute__((used)) static const char port_option_##name[] = "bool " #value " " label "|" help
#define PORT_OPTION_RANGE(name, value, min, max, label, help) \
    u32 port_##name = (value); \
    __attribute__((used)) static const char port_option_##name[] = "int " #value " " #min " " #max " " label "|" help

/* A variable the host reads or writes: any global of the port's named
 * port_<something> is a `symbol` line of decomp.txt, which the player and a
 * port's own UI (frontend/port_ui.h) find by name. The runtime's player
 * writes these when the port defines them:
 *
 *   float port_top_aspect;  the top screen's shape as shown (16:9, 21:9),
 *                           0 when it is the 3DS's own: a widescreen hook
 *                           frames the 3D that much wider */

#endif
