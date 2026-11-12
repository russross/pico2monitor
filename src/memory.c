#include "monitor.h"

#include <string.h>

/* Required access is separate from debugging preferences. Trusted bytes never
 * receive a fallback grant, even when an enclosing NAPOT region crosses them. */
typedef enum { TRUSTED, UNUSED, GUARD, TEXT, RODATA, WRITABLE } Role;
typedef struct {
    uint8_t required, roles;
    bool trusted;
} Cell;
typedef struct {
    uint64_t score;
    uint8_t used, left;
    bool valid, install;
} Choice;
typedef struct {
    Choice best[5][8], left[5][8];
} Workspace;
/* Planning runs only before segment initialization, using disposable guest SRAM. */
#define workspace ((Workspace *)physical(GUEST_BASE + 8192))
_Static_assert(8192 + sizeof(Workspace) * 15 <= GUEST_END - GUEST_BASE,
               "planning workspace must fit guest SRAM");
static const uint8_t modes[] = {0, 4, 5, 6, 7};
static const Image *solving;

static bool intersects(uint32_t a, uint32_t n, uint32_t b, uint32_t m) {
    return n && m && a < b + m && b < a + n;
}

static Cell cell(uint32_t address) {
    const MemoryPlan *p = &solving->plan;
    Cell c = {0};
    c.trusted = address < GUEST_BASE || address >= p->snapshot_base;
    if (c.trusted)
        return c;
    c.roles = 1u << UNUSED;
    for (uint32_t i = 0; i < solving->segment_count; ++i) {
        const Segment *s = &solving->segments[i];
        if (!intersects(address, 32, s->address, s->memory_size))
            continue;
        c.required |= s->permissions;
        c.roles |= 1u << ((s->permissions & PERM_W)   ? WRITABLE
                          : (s->permissions & PERM_X) ? TEXT
                                                      : RODATA);
    }
    if (intersects(address, 32, p->heap_base, p->heap_limit - p->heap_base) ||
        intersects(address, 32, p->stack_base, p->stack_top - p->stack_base)) {
        c.required |= PERM_R | PERM_W;
        c.roles |= 1u << WRITABLE;
    }
    if (address >= p->heap_limit && address < p->stack_base)
        c.roles |= 1u << GUARD;
    return c;
}

/* A four-byte instruction can start two bytes before a PMP boundary. Hazard3
 * faults if that fetch matches two entries, even when both entries grant X. */
static bool executable_boundary(uint32_t address) {
    if (address <= 0x20000000 || address >= GUEST_END)
        return false;
    return (cell(address - 32).required & PERM_X) && (cell(address).required & PERM_X);
}

static bool region_legal(uint32_t base, uint32_t size) {
    return !executable_boundary(base) && !executable_boundary(base + size);
}

/* Each score field counts 32-byte cells. Four 14-bit fields cover the arena;
 * the highest field rewards at most a 1 KiB guard immediately below the stack. */
static uint64_t reward(Cell c, uint8_t permissions, uint32_t base, uint32_t size) {
    if (c.trusted)
        return 0;
    uint64_t units = size / 32, score = 0;
    if (!permissions && (c.roles & (1u << GUARD))) {
        uint32_t guard = solving->plan.stack_base - 1024;
        uint32_t begin = base > guard ? base : guard;
        uint32_t end =
            base + size < solving->plan.stack_base ? base + size : solving->plan.stack_base;
        if (begin < end)
            score += (uint64_t)((end - begin) / 32) << 56;
    }
    if (!(permissions & PERM_W)) {
        if (c.roles & (1u << TEXT))
            score += units << 42;
        if (c.roles & (1u << RODATA))
            score += units << 28;
    }
    if (!(permissions & PERM_X) && (c.roles & (1u << WRITABLE)))
        score += units << 14;
    if (!permissions && !c.required)
        score += units;
    return score;
}

static bool better(Choice a, Choice b) {
    return a.valid && (!b.valid || a.score > b.score || (a.score == b.score && a.used < b.used));
}

/* Uniform subtrees terminate immediately. Boundary subtrees combine child
 * budgets, with an optional parent permission override after child exceptions. */
static uint8_t solve(uint32_t base, uint32_t size, unsigned depth) {
    Workspace *w = &workspace[depth];
    memset(w->best, 0, sizeof(w->best));
    Cell first = cell(base);
    bool legal = region_legal(base, size);
    bool uniform = true;
    uint8_t required = first.required;
    for (uint32_t a = base + 32; a < base + size; a += 32) {
        Cell c = cell(a);
        required |= c.required;
        uniform = uniform && c.required == first.required && c.roles == first.roles &&
                  c.trusted == first.trusted;
    }
    if (uniform) {
        for (unsigned m = 0; m < 5; ++m) {
            for (unsigned b = 0; b < 8; ++b) {
                for (unsigned q = 0; q < 5; ++q) {
                    uint8_t permission = modes[q];
                    bool install = permission != modes[m];
                    if ((first.trusted && permission) || (permission & required) != required ||
                        (install && (!b || !legal)))
                        continue;
                    Choice v = {.valid = true,
                                .install = install,
                                .used = install,
                                .left = (uint8_t)q,
                                .score = reward(first, permission, base, size)};
                    if (better(v, w->best[m][b]))
                        w->best[m][b] = v;
                }
            }
        }
        return required;
    }
    solve(base, size / 2, depth + 1);
    memcpy(w->left, workspace[depth + 1].best, sizeof(w->left));
    solve(base + size / 2, size / 2, depth + 1);
    for (unsigned m = 0; m < 5; ++m) {
        for (unsigned b = 0; b < 8; ++b) {
            for (unsigned q = 0; q < 5; ++q) {
                bool install = q != m;
                if (install && (!b || !legal))
                    continue;
                unsigned available = b - install;
                for (unsigned left = 0; left <= available; ++left) {
                    Choice l = w->left[q][left];
                    Choice r = workspace[depth + 1].best[q][available - left];
                    Choice v = {.valid = l.valid && r.valid,
                                .score = l.score + r.score,
                                .used = (uint8_t)(l.used + r.used + install),
                                .left = (uint8_t)(left | (q << 4)),
                                .install = install};
                    if (better(v, w->best[m][b]))
                        w->best[m][b] = v;
                }
            }
        }
    }
    return required;
}

static void reconstruct(uint32_t base, uint32_t size, unsigned depth, unsigned inherited,
                        unsigned budget, MemoryPlan *plan) {
    solve(base, size, depth);
    Choice choice = workspace[depth].best[inherited][budget];
    Cell first = cell(base);
    bool uniform = true;
    for (uint32_t a = base + 32; a < base + size; a += 32) {
        Cell c = cell(a);
        uniform = uniform && c.required == first.required && c.roles == first.roles &&
                  c.trusted == first.trusted;
    }
    unsigned permission = uniform ? choice.left : choice.left >> 4;
    if (!uniform) {
        unsigned left = choice.left & 15;
        reconstruct(base, size / 2, depth + 1, permission, left, plan);
        reconstruct(base + size / 2, size / 2, depth + 1, permission,
                    budget - choice.install - left, plan);
    }
    if (choice.install)
        plan->regions[plan->region_count++] = (PmpRegion){base, size, modes[permission]};
}

uint8_t memory_permissions(const MemoryPlan *plan, uint32_t address) {
    for (uint32_t i = 0; i < plan->region_count; ++i) {
        const PmpRegion *r = &plan->regions[i];
        if (range_contains(r->base, r->size, address, 1))
            return r->permissions;
    }
    return 0;
}

/* Verification decodes the selected first-match map independently of DP scores.
 * Every granule is checked, including trusted space around guest allocations. */
bool memory_plan_verify(const Image *image) {
    const MemoryPlan *p = &image->plan;
    if (p->region_count > PMP_ENTRIES || p->heap_base < GUEST_BASE ||
        p->heap_base > p->heap_limit || p->heap_limit > p->stack_base ||
        p->stack_base > p->stack_top || p->stack_top != p->snapshot_base ||
        p->snapshot_base > GUEST_END || (p->snapshot_base & 31) ||
        p->stack_top - p->stack_base != STACK_SIZE ||
        p->snapshot_size > GUEST_END - p->snapshot_base)
        return false;
    for (uint32_t i = 0; i < p->region_count; ++i) {
        const PmpRegion *r = &p->regions[i];
        if (r->size < 32 || (r->size & (r->size - 1)) || (r->base & (r->size - 1)) ||
            !range_contains(0x20000000, 0x80000, r->base, r->size) ||
            (r->permissions != 0 && r->permissions != 4 && r->permissions != 5 &&
             r->permissions != 6 && r->permissions != 7))
            return false;
    }
    bool previous_execute = false;
    uint32_t previous_entry = PMP_ENTRIES;
    for (uint32_t a = 0x20000000; a < GUEST_END; a += 32) {
        uint8_t actual = memory_permissions(p, a);
        bool execute = false;
        uint32_t entry = PMP_ENTRIES;
        for (uint32_t i = 0; i < p->region_count; ++i)
            if (range_contains(p->regions[i].base, p->regions[i].size, a, 1)) {
                entry = i;
                break;
            }
        if ((a < GUEST_BASE || a >= p->snapshot_base) && actual)
            return false;
        for (uint32_t i = 0; i < image->segment_count; ++i) {
            const Segment *s = &image->segments[i];
            execute = execute ||
                      ((s->permissions & PERM_X) && intersects(a, 32, s->address, s->memory_size));
            if (intersects(a, 32, s->address, s->memory_size) &&
                (actual & s->permissions) != s->permissions)
                return false;
        }
        if (execute && previous_execute && entry != previous_entry)
            return false;
        previous_execute = execute;
        previous_entry = entry;
        if ((intersects(a, 32, p->heap_base, p->heap_limit - p->heap_base) ||
             intersects(a, 32, p->stack_base, p->stack_top - p->stack_base)) &&
            (actual & 6) != 6)
            return false;
    }
    return true;
}

uint32_t memory_snapshot_offset(const Image *image, uint32_t index) {
    uint32_t offset = 0;
    for (uint32_t i = 0; i < index; ++i)
        if (image->segments[i].permissions & PERM_W)
            offset += image->segments[i].file_size;
    return offset;
}

static const char *layout(Image *image, bool snapshot, uint32_t alignment) {
    MemoryPlan *p = &image->plan;
    p->snapshot = snapshot;
    p->snapshot_size = snapshot ? memory_snapshot_offset(image, image->segment_count) : 0;
    if (p->snapshot_size > GUEST_END - GUEST_BASE - STACK_SIZE)
        return "initialized writable data leaves no stack space";
    /* Align the stack/snapshot boundary to a stack-sized NAPOT block. Tiny
     * snapshots otherwise create expensive boundaries throughout the stack. */
    p->snapshot_base = (GUEST_END - p->snapshot_size) & ~(alignment - 1);
    p->stack_top = p->snapshot_base;
    p->stack_base = p->stack_top - STACK_SIZE;
    uint32_t end = GUEST_BASE;
    for (uint32_t i = 0; i < image->segment_count; ++i) {
        const Segment *s = &image->segments[i];
        if (end < s->address + s->memory_size)
            end = s->address + s->memory_size;
    }
    p->heap_base = (end + 31) & ~31u;
    if (p->heap_base > p->stack_base)
        return "image leaves no room for the stack";
    uint32_t spare = p->stack_base - p->heap_base;
    p->heap_limit = p->stack_base - (spare >= 1024 ? 1024 : 0);
    p->region_count = 0;
    solving = image;
    solve(0x20000000, 0x80000, 0);
    if (!workspace[0].best[0][PMP_ENTRIES].valid)
        return "cannot isolate this layout with available PMP entries";
    reconstruct(0x20000000, 0x80000, 0, 0, PMP_ENTRIES, p);
    return memory_plan_verify(image) ? NULL : "invalid effective memory plan";
}

const char *memory_plan_build(Image *image, SnapshotPolicy policy) {
    image->plan.requested = policy;
    if (policy != SNAP_OFF) {
        const uint32_t alignments[] = {STACK_SIZE, 32};
        for (unsigned i = 0; i < 2; ++i) {
            const char *error = layout(image, true, alignments[i]);
            if (!error && image->plan.heap_limit - image->plan.heap_base >= HEAP_RESERVE)
                return NULL;
        }
    }
    return layout(image, false, STACK_SIZE);
}

#ifdef MONITOR_NATIVE_TEST
/* Small exhaustive oracles exercise the same solver with a reduced tree. */
uint64_t memory_test_solve(Image *image, uint32_t base, uint32_t size, unsigned budget) {
    solving = image;
    solve(base, size, 0);
    Choice choice = workspace[0].best[0][budget];
    image->plan.region_count = 0;
    if (choice.valid)
        reconstruct(base, size, 0, 0, budget, &image->plan);
    return choice.valid ? choice.score : UINT64_MAX;
}
#endif
