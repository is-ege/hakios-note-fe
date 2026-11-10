/* CPU cache maintenance keyed to the IOSurface content seed.
 * The caller serializes each buffer's stamp. Externally wrapped memory has no
 * enforced IOSurface write protocol and always requires maintenance.
 * A surface the submitting process says only its GPU draws (gpu_drawn: the window
 * server's targets, whose write locks only mark them changed) is keyed instead to
 * the write locks by any other process (other_writes).
 */
#ifndef HAKIOS_MALI_CPU_CACHE_H
#define HAKIOS_MALI_CPU_CACHE_H
#include <stdint.h>
#include <stdbool.h>

struct mali_cpu_cache_stamp { uint32_t seed, other_writes; bool valid; };

static inline bool
mali_cpu_cache_needs_clean(const struct mali_cpu_cache_stamp *stamp, uint32_t seed, uint32_t other_writes,
    bool gpu_drawn, bool external)
{
    if (external || !stamp->valid) return true;
    return gpu_drawn ? stamp->other_writes != other_writes : stamp->seed != seed;
}

static inline void
mali_cpu_cache_cleaned(struct mali_cpu_cache_stamp *stamp, uint32_t before, uint32_t after, uint32_t other_writes,
    bool external)
{
    stamp->seed = after;
    stamp->other_writes = other_writes;
    stamp->valid = !external && before == after;
}
#endif
