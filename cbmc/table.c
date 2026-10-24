#include "heap/table.c"
#include "heap/cell.c"

#include <stdlib.h>

#pragma CPROVER check push
#pragma CPROVER check disable "bounds"
#pragma CPROVER check disable "pointer"
#pragma CPROVER check disable "pointer-overflow"
#pragma CPROVER check disable "pointer-primitive"
#pragma CPROVER check disable "signed-overflow"
#pragma CPROVER check disable "conversion"

#define CAPACITY 4
#define SLOTS 8

uint32_t nondet_u32(void);
uint64_t nondet_u64(void);
int64_t nondet_i64(void);
bool nondet_bool(void);

_Noreturn void mo_assert_failed(const char *expression, const char *file, int line) {
    (void)expression;
    (void)file;
    (void)line;
    __CPROVER_assert(0, "MO_ASSERT holds");
    __CPROVER_assume(0);
    abort();
}

void reached_end(void) {
}

/* compact moves one entry, at most two whole cells, with possible overlap. */
void *memmove(void *destination, const void *source, size_t bytes) {
    __CPROVER_assert(bytes == sizeof(struct mo_cell) || bytes == 2 * sizeof(struct mo_cell),
                     "compact: moves one entry");
    __CPROVER_assert(__CPROVER_r_ok(source, bytes) && __CPROVER_w_ok(destination, bytes),
                     "compact: move stays inside its buffers");
    struct mo_cell saved[2];
    const struct mo_cell *from = source;
    struct mo_cell *to = destination;
    for (size_t i = 0; i < bytes / sizeof(struct mo_cell); i++)
        saved[i] = from[i];
    for (size_t i = 0; i < bytes / sizeof(struct mo_cell); i++)
        to[i] = saved[i];
    return destination;
}

static bool equality_failed;
static uint64_t mixes[SLOTS];
static struct mo_allocation *current_table;
static struct mo_table_info info;
static uint32_t index_slots[SLOTS];
static uint32_t *index_base;
static uint64_t entry_hashes[CAPACITY];
static uint8_t live[CAPACITY];

/* table_layout proves that these regions fit and do not overlap. Keeping them
 * as separate CBMC objects avoids byte-array alias circuits in every probe. */
struct mo_table_info *model_mo_table_info(struct mo_allocation *table) {
    __CPROVER_assert(table == current_table, "table model: the specified allocation");
    return &info;
}

uint32_t *model_mo_table_slots(struct mo_allocation *table) {
    __CPROVER_assert(table == current_table, "table model: the specified allocation");
    return index_base;
}

uint64_t *model_mo_table_hashes(struct mo_allocation *table) {
    __CPROVER_assert(table == current_table, "table model: the specified allocation");
    return entry_hashes;
}

uint8_t *model_mo_table_live(struct mo_allocation *table) {
    __CPROVER_assert(table == current_table, "table model: the specified allocation");
    return live;
}

struct mo_allocation *mo_heap_allocate(struct mo_heap *process, uint32_t type, uint32_t layout,
                                       size_t bytes) {
    (void)process;
    if (nondet_bool())
        return NULL;
    struct mo_allocation *table = malloc(offsetof(struct mo_allocation, cells) + bytes);
    __CPROVER_assume(table != NULL);
    table->size = offsetof(struct mo_allocation, cells) + bytes;
    table->object.layout = layout;
    table->type = type;
    return table;
}

static void check_layout(uint32_t capacity, uint32_t slots) {
    bool map = nondet_bool();
    struct mo_type item = {.kind = map ? MO_TYPE_MAP : MO_TYPE_SET};
    struct mo_types types = {.items = &item, .count = 1};
    struct mo_heap process = {.types = &types};
    struct mo_allocation *table = new_table(&process, 0, capacity, slots);
    if (table == NULL) {
        reached_end();
        return;
    }
    uint32_t stride = map ? 2 : 1;
    __CPROVER_assert(mo_table_stride(table) == stride, "layout: map pairs and set keys");
    __CPROVER_assert((void *)mo_table_hashes(table) == (void *)(table->cells + capacity * stride),
                     "layout: hashes follow the cells");
    __CPROVER_assert((void *)mo_table_live(table) == (void *)(mo_table_hashes(table) + capacity),
                     "layout: live flags follow the hashes");
    __CPROVER_assert((void *)mo_table_slots(table) == (void *)(mo_table_live(table) + capacity),
                     "layout: slots follow the padded live flags");
    __CPROVER_assert((uint8_t *)(mo_table_slots(table) + slots) <= (uint8_t *)mo_table_info(table),
                     "layout: slots end before the trailer");
    __CPROVER_assert(__CPROVER_w_ok(table->cells, capacity * stride * sizeof(struct mo_cell)) &&
                         __CPROVER_w_ok(mo_table_hashes(table), capacity * sizeof(uint64_t)) &&
                         __CPROVER_w_ok(mo_table_live(table), capacity) &&
                         __CPROVER_w_ok(mo_table_slots(table), slots * sizeof(uint32_t)) &&
                         __CPROVER_w_ok(mo_table_info(table), sizeof info),
                     "layout: every modelled region fits");
    reached_end();
}

void harness_table_layout(void) {
    check_layout(CAPACITY, SLOTS);
}

void harness_table_layout_limit(void) {
    check_layout(128, 256);
}

bool mo_value_equal(struct mo_heap *process, uint32_t type, struct mo_cell left,
                    struct mo_cell right, bool *out) {
    (void)process;
    __CPROVER_assert(type == 0, "lookup: compares keys with the key type");
    if (nondet_bool()) {
        equality_failed = true;
        return false;
    }
    *out = mo_cell_get_int(left) == mo_cell_get_int(right);
    return true;
}

/* A deterministic but otherwise arbitrary private hash, including collisions. */
uint64_t mo_heap_mix(const uint64_t key[2], uint64_t left, uint64_t right) {
    (void)key;
    __CPROVER_assert(left < SLOTS && right == 0, "union: hashes an index slot");
    return mixes[left];
}

static struct mo_allocation *any_table(bool half_full, bool map) {
    uint32_t stride = map ? 2 : 1;
    size_t bytes = offsetof(struct mo_allocation, cells) +
                   CAPACITY * stride * sizeof(struct mo_cell) + CAPACITY * sizeof(uint64_t) +
                   CAPACITY + SLOTS * sizeof(uint32_t) + sizeof(struct mo_table_info) + 4;
    struct mo_allocation *table = malloc(bytes);
    __CPROVER_assume(table != NULL);
    current_table = table;
    index_base = index_slots;
    table->size = bytes;
    table->object.layout = MO_BUILTIN_LAYOUT(map ? MO_TYPE_MAP : MO_TYPE_SET);
    table->length = nondet_u32();
    __CPROVER_assume(table->length <= CAPACITY);
    *mo_table_info(table) = (struct mo_table_info){.capacity = CAPACITY, .slots = SLOTS};
    for (uint32_t i = 0; i < CAPACITY; i++) {
        table->cells[stride * i] = mo_cell_int(nondet_i64());
        if (map)
            table->cells[stride * i + 1] = mo_cell_int(nondet_i64());
        mo_table_hashes(table)[i] = nondet_u64();
        mo_table_live(table)[i] = nondet_bool() ? 1 : 0;
    }
    uint32_t used = 0;
    for (uint32_t i = 0; i < SLOTS; i++) {
        uint32_t entry = nondet_u32();
        __CPROVER_assume(entry == MO_NO_INDEX || entry <= table->length);
        mo_table_slots(table)[i] = entry;
        if (entry != 0)
            used++;
    }
    __CPROVER_assume(!half_full || used <= SLOTS / 2);
    return table;
}

void harness_table_insert(void) {
    struct mo_allocation *table = any_table(false, false);
    uint64_t hash = nondet_u64();
    uint32_t expected = MO_NO_INDEX;
    for (uint32_t p = 0; p < SLOTS; p++) {
        uint32_t slot = (uint32_t)((hash % SLOTS + p) % SLOTS);
        if (mo_table_slots(table)[slot] == 0) {
            expected = slot;
            break;
        }
    }
    uint32_t out = MO_NO_INDEX;
    bool ok = insertion_slot(table, hash, &out);
    __CPROVER_assert(ok == (expected != MO_NO_INDEX),
                     "insert: succeeds exactly with an empty slot");
    __CPROVER_assert(out == expected, "insert: returns the first empty slot, wrapping at the end");
    reached_end();
}

void harness_table_probe_limit(void) {
    struct mo_allocation table = {0};
    current_table = &table;
    info = (struct mo_table_info){.slots = 256};
    index_base = malloc(256 * sizeof(uint32_t));
    __CPROVER_assume(index_base != NULL);
    uint32_t start = nondet_u32();
    __CPROVER_assume(start < 256);
    bool within = nondet_bool();
    uint32_t occupied = within ? MO_LIMIT_HEAP_PROBES - 1 : MO_LIMIT_HEAP_PROBES;
    for (uint32_t i = 0; i < 256; i++)
        index_base[i] = (i + 256 - start) % 256 < occupied ? 1 : 0;
    uint32_t out = MO_NO_INDEX;
    bool ok = insertion_slot(&table, start, &out);
    __CPROVER_assert(ok == within, "insert: accepts the last allowed probe, refuses the next");
    __CPROVER_assert(out == (within ? (start + occupied) % 256 : MO_NO_INDEX),
                     "insert: exact probe-limit result, including wraparound");
    reached_end();
}

static void check_lookup(bool map) {
    struct mo_allocation *table = any_table(true, map);
    uint32_t parameters[] = {0, 0};
    struct mo_type items[] = {{.kind = MO_TYPE_INT}, {.parameters = parameters}};
    struct mo_types types = {.items = items, .count = 2};
    struct mo_heap process = {.types = &types};
    uint64_t hash = nondet_u64();
    struct mo_cell key = mo_cell_int(nondet_i64());
    uint32_t expected = MO_NO_INDEX, vacancy = MO_NO_INDEX, at = 0, probes = 0;
    for (; probes < SLOTS; probes++) {
        at = (uint32_t)((hash % SLOTS + probes) % SLOTS);
        uint32_t entry = mo_table_slots(table)[at];
        if (entry == 0) {
            if (vacancy == MO_NO_INDEX)
                vacancy = at;
            break;
        }
        if (entry == MO_NO_INDEX) {
            if (vacancy == MO_NO_INDEX)
                vacancy = at;
        } else if (mo_table_hashes(table)[entry - 1] == hash &&
                   mo_cell_get_int(table->cells[mo_table_stride(table) * (entry - 1)]) ==
                       mo_cell_get_int(key)) {
            expected = entry - 1;
            break;
        }
    }
    uint32_t position = 71, slot = 72, distance = 73;
    bool ok = lookup(&process, 1, table, key, hash, &position, &slot, &distance);
    __CPROVER_assert(ok == !equality_failed, "lookup: propagates equality failure");
    if (ok) {
        __CPROVER_assert(position == expected, "lookup: returns the first matching key or absent");
        __CPROVER_assert(slot == (expected == MO_NO_INDEX ? vacancy : at),
                         "lookup: reuses the first tombstone only for an absent key");
        __CPROVER_assert(distance == probes, "lookup: reports the distance to match or empty");
    } else {
        __CPROVER_assert(position == 71 && slot == 72 && distance == 73,
                         "lookup: failure leaves outputs alone");
    }
    reached_end();
}

void harness_table_lookup(void) {
    check_lookup(true);
}

void harness_table_lookup_set(void) {
    check_lookup(false);
}

void harness_table_compact(void) {
    struct mo_allocation *table = any_table(true, true);
    __CPROVER_assume(table->length <= 2);
    struct mo_cell keys[CAPACITY], values[CAPACITY];
    uint64_t hashes[CAPACITY];
    uint32_t count = 0;
    for (uint32_t i = 0; i < table->length; i++) {
        if (mo_table_live(table)[i] != 0) {
            keys[count] = table->cells[2 * i];
            values[count] = table->cells[2 * i + 1];
            hashes[count++] = mo_table_hashes(table)[i];
        }
    }
    mo_table_info(table)->active = count;
    compact(table);
    __CPROVER_assert(table->length == count && mo_table_info(table)->active == count,
                     "compact: keeps exactly the live entries");
    for (uint32_t i = 0; i < CAPACITY; i++) {
        __CPROVER_assert(mo_table_live(table)[i] == (i < count ? 1 : 0),
                         "compact: leaves a dense live prefix");
        if (i < count) {
            __CPROVER_assert(mo_cell_get_int(table->cells[2 * i]) == mo_cell_get_int(keys[i]) &&
                                 mo_cell_get_int(table->cells[2 * i + 1]) ==
                                     mo_cell_get_int(values[i]) &&
                                 mo_table_hashes(table)[i] == hashes[i],
                             "compact: preserves insertion order, keys, values and hashes");
            uint32_t occurrences = 0;
            bool reachable = true;
            for (uint32_t p = 0; p < SLOTS; p++) {
                uint32_t at = (uint32_t)((hashes[i] % SLOTS + p) % SLOTS);
                if (mo_table_slots(table)[at] == i + 1) {
                    occurrences++;
                    __CPROVER_assert(reachable, "compact: a key is reachable before an empty slot");
                }
                if (mo_table_slots(table)[at] == 0)
                    reachable = false;
            }
            __CPROVER_assert(occurrences == 1, "compact: indexes each live entry once");
        }
    }
    for (uint32_t i = 0; i < SLOTS; i++)
        __CPROVER_assert(mo_table_slots(table)[i] <= count, "compact: no stale entry or tombstone");
    reached_end();
}

void harness_table_union(void) {
    struct mo_allocation *table = any_table(true, false);
    bool full = nondet_bool();
    if (full)
        for (uint32_t i = 0; i < SLOTS; i++)
            mo_table_slots(table)[i] = 1;
    struct mo_heap_image image = {0};
    struct mo_heap process = {.image = &image};
    struct union_entry entries[3];
    uint32_t expected[3], overlay[SLOTS], count = nondet_u32();
    __CPROVER_assume(count <= 3);
    if (full)
        count = 1;
    bool allowed = true;
    bool taken[SLOTS];
    for (uint32_t i = 0; i < SLOTS; i++) {
        uint32_t entry = mo_table_slots(table)[i];
        taken[i] = entry != 0 && entry != MO_NO_INDEX;
        mixes[i] = nondet_u64();
    }
    for (uint32_t i = 0; i < count; i++) {
        entries[i].hash = nondet_u64();
        expected[i] = MO_NO_INDEX;
        for (uint32_t p = 0; p < SLOTS; p++) {
            uint32_t at = (uint32_t)((entries[i].hash % SLOTS + p) % SLOTS);
            if (!taken[at]) {
                taken[at] = true;
                expected[i] = at;
                break;
            }
        }
        if (expected[i] == MO_NO_INDEX) {
            allowed = false;
            break;
        }
    }
    bool ok = union_slots(&process, table, entries, count, overlay, SLOTS);
    __CPROVER_assert(ok == allowed, "union: succeeds exactly with room for the planned keys");
    if (ok)
        for (uint32_t i = 0; i < count; i++)
            __CPROVER_assert(entries[i].slot == expected[i],
                             "union: first vacant unreserved slot for each key, in order");
    reached_end();
}

#pragma CPROVER check pop
