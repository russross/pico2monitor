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
    uint8_t used, permission, left_budget;
    bool valid, install;
} Choice;
typedef enum { SCAN, LEAF, LEFT_CHILD, RIGHT_CHILD, COMBINE, SOLVED } SolveStage;
typedef struct {
    Choice best[5][8], left[5][8];
    uint32_t base, size, cursor;
    uint8_t inherited, budget, permission, left_budget;
    Cell first;
    SolveStage stage;
    bool uniform, legal;
} Workspace;
/* Planning runs only before segment initialization, using disposable guest SRAM. */
#define workspace ((Workspace *)physical(GUEST_BASE + 8192))
_Static_assert(8192 + sizeof(Workspace) * 15 <= GUEST_END - GUEST_BASE,
               "planning workspace must fit guest SRAM");
static const uint8_t modes[] = {0, 4, 5, 6, 7};
static const Image *solving;
static unsigned solve_depth, solve_floor;

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
static void solve_node(uint32_t base, uint32_t size, unsigned depth) {
    Workspace *w = &workspace[depth];
    memset(w, 0, sizeof(*w));
    w->base = base;
    w->size = size;
    w->cursor = base + 32;
    w->first = cell(base);
    w->uniform = true;
    w->legal = region_legal(base, size);
}

static void solve_begin(uint32_t base, uint32_t size, unsigned depth) {
    solve_depth = solve_floor = depth;
    solve_node(base, size, depth);
}

/* A solver step evaluates a bounded number of cells or budget combinations.
 * Its traversal stack lives in disposable guest memory, never on the C stack. */
static bool solve_step(void) {
    for (unsigned work = 0; work < 512; ++work) {
        Workspace *w = &workspace[solve_depth];
        if (w->stage == SCAN) {
            if (w->uniform && w->cursor < w->base + w->size) {
                Cell c = cell(w->cursor);
                w->uniform = c.required == w->first.required && c.roles == w->first.roles &&
                             c.trusted == w->first.trusted;
                w->cursor += 32;
            } else if (w->uniform)
                w->stage = LEAF;
            else {
                w->stage = LEFT_CHILD;
                ++solve_depth;
                solve_node(w->base, w->size / 2, solve_depth);
            }
            continue;
        }
        if (w->stage == SOLVED) {
            if (solve_depth == solve_floor)
                return true;
            --solve_depth;
            Workspace *parent = &workspace[solve_depth];
            if (parent->stage == LEFT_CHILD) {
                memcpy(parent->left, w->best, sizeof(parent->left));
                parent->stage = RIGHT_CHILD;
                ++solve_depth;
                solve_node(parent->base + parent->size / 2, parent->size / 2, solve_depth);
            } else
                parent->stage = COMBINE;
            continue;
        }

        /* Candidate counters advance in inherited/budget/permission/left order.
         * Each combination remains a separate bounded unit of work. */
        bool leaf = w->stage == LEAF;
        unsigned m = w->inherited, b = w->budget, q = w->permission, left = w->left_budget;
        if (m == 5) {
            w->stage = SOLVED;
            continue;
        }
        if (leaf || ++w->left_budget == 8) {
            w->left_budget = 0;
            if (++w->permission == 5) {
                w->permission = 0;
                if (++w->budget == 8) {
                    w->budget = 0;
                    ++w->inherited;
                }
            }
        }
        uint8_t permission = modes[q];
        bool install = q != m;
        if (install && (!b || !w->legal))
            continue;
        Choice v = {.permission = (uint8_t)q, .left_budget = (uint8_t)left, .install = install};
        if (leaf) {
            if ((w->first.trusted && permission) ||
                (permission & w->first.required) != w->first.required)
                continue;
            v.valid = true;
            v.used = install;
            v.score = reward(w->first, permission, w->base, w->size);
        } else {
            unsigned available = b - install;
            if (left > available)
                continue;
            Choice l = w->left[q][left];
            Choice r = workspace[solve_depth + 1].best[q][available - left];
            v.valid = l.valid && r.valid;
            v.score = l.score + r.score;
            v.used = (uint8_t)(l.used + r.used + install);
        }
        if (better(v, w->best[m][b]))
            w->best[m][b] = v;
    }
    return false;
}

#ifdef MONITOR_NATIVE_TEST
static void solve(uint32_t base, uint32_t size, unsigned depth) {
    solve_begin(base, size, depth);
    while (!solve_step()) {
    }
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
    unsigned permission = choice.permission;
    if (!uniform) {
        unsigned left = choice.left_budget;
        reconstruct(base, size / 2, depth + 1, permission, left, plan);
        reconstruct(base + size / 2, size / 2, depth + 1, permission,
                    budget - choice.install - left, plan);
    }
    if (choice.install)
        plan->regions[plan->region_count++] = (PmpRegion){base, size, modes[permission]};
}
#endif

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
static bool verify_range(const Image *image, uint32_t begin, uint32_t end) {
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
    if (begin > GUEST_BASE) {
        for (uint32_t i = 0; i < image->segment_count; ++i) {
            const Segment *s = &image->segments[i];
            previous_execute =
                previous_execute || ((s->permissions & PERM_X) &&
                                     intersects(begin - 32, 32, s->address, s->memory_size));
        }
        for (uint32_t i = 0; i < p->region_count; ++i)
            if (range_contains(p->regions[i].base, p->regions[i].size, begin - 32, 1)) {
                previous_entry = i;
                break;
            }
    }
    for (uint32_t a = begin; a < end; a += 32) {
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

bool memory_plan_verify(const Image *image) {
    return verify_range(image, GUEST_BASE, MAIN_SRAM_END);
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
    return NULL;
}

typedef enum { PLAN_ENTER, PLAN_LEFT, PLAN_RIGHT, PLAN_EMIT } PlanStage;
typedef struct {
    uint32_t base, size;
    unsigned inherited, budget, permission, left;
    PlanStage stage;
    bool install;
} PlanNode;
static struct {
    Image *image;
    PlanNode nodes[15];
    unsigned depth, candidate;
    uint32_t verify_cursor;
    bool searching, verifying, done;
    const char *error;
} planning;

static void plan_candidate(void) {
    Image *image = planning.image;
    planning.error =
        layout(image, planning.candidate < 2, planning.candidate == 1 ? 32 : STACK_SIZE);
    if (!planning.error && planning.candidate < 2 &&
        image->plan.heap_limit - image->plan.heap_base < HEAP_RESERVE)
        planning.error = "insufficient reset headroom";
    planning.depth = 0;
    planning.nodes[0] = (PlanNode){.base = GUEST_BASE, .size = 0x80000, .budget = PMP_ENTRIES};
    planning.searching = false;
    planning.verifying = false;
}

void memory_plan_start(Image *image, SnapshotPolicy policy) {
    memset(&planning, 0, sizeof(planning));
    image->plan.requested = policy;
    planning.image = image;
    solving = image;
    planning.candidate = policy == SNAP_OFF ? 2 : 0;
    plan_candidate();
}

/* Reconstruction repeats bounded subtree solves and emits child exceptions
 * before their parent grants. Verification also advances in bounded slices. */
bool memory_plan_step(const char **error) {
    Image *image = planning.image;
    if (planning.done) {
        *error = planning.error;
        return true;
    }
    *error = NULL;
    if (planning.error) {
        if (planning.candidate++ < 2) {
            plan_candidate();
            return false;
        }
        planning.done = true;
    } else if (planning.verifying) {
        uint32_t end = planning.verify_cursor + 4096;
        if (!verify_range(image, planning.verify_cursor, end))
            planning.error = "invalid effective memory plan";
        else {
            planning.verify_cursor = end;
            planning.done = end == MAIN_SRAM_END;
        }
    } else {
        PlanNode *node = &planning.nodes[planning.depth];
        if (node->stage == PLAN_ENTER) {
            if (!planning.searching) {
                solve_begin(node->base, node->size, 0);
                planning.searching = true;
            }
            if (!solve_step())
                return false;
            planning.searching = false;
            Choice choice = workspace[0].best[node->inherited][node->budget];
            if (!choice.valid) {
                planning.error = "cannot isolate this layout with available PMP entries";
                return false;
            }
            node->permission = choice.permission;
            node->left = choice.left_budget;
            node->install = choice.install;
            node->stage = workspace[0].uniform ? PLAN_EMIT : PLAN_LEFT;
        } else if (node->stage == PLAN_LEFT || node->stage == PLAN_RIGHT) {
            bool right = node->stage == PLAN_RIGHT;
            node->stage = right ? PLAN_EMIT : PLAN_RIGHT;
            PlanNode child = {.base = node->base + (right ? node->size / 2 : 0),
                              .size = node->size / 2,
                              .inherited = node->permission,
                              .budget =
                                  right ? node->budget - node->install - node->left : node->left};
            planning.nodes[++planning.depth] = child;
        } else {
            if (node->install) {
                if (image->plan.region_count == PMP_ENTRIES) {
                    planning.error = "invalid effective memory plan";
                    return false;
                }
                image->plan.regions[image->plan.region_count++] =
                    (PmpRegion){node->base, node->size, modes[node->permission]};
            }
            if (!planning.depth) {
                planning.verifying = true;
                planning.verify_cursor = GUEST_BASE;
            } else
                --planning.depth;
        }
    }
    *error = planning.done ? planning.error : NULL;
    return planning.done;
}

const char *memory_plan_build(Image *image, SnapshotPolicy policy) {
    const char *error = NULL;
    memory_plan_start(image, policy);
    while (!memory_plan_step(&error)) {
    }
    return error;
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
