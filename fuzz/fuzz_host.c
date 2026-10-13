#include "../tests/host_fixture.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static const struct {
    const char *module, *modes;
} operations[] = {
#define HOSTOP(id, module, name, signature, params, bugs, spec_only) {module, params},
#include "base/hostops.def"
};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0)
        return 0;
    struct host_fixture f;
    size_t limit = 8192 + (size_t)data[0] * 256;
    host_fixture_init(&f, limit);
    struct mo_cell values[32];
    uint32_t types[32] = {0};
    bool live[32] = {false};
    struct mo_cell map, list;
    MO_ASSERT(mo_value_map(f.heap, f.types.files, NULL, NULL, 0, &map));
    MO_ASSERT(mo_value_list(f.heap, f.types.texts, NULL, 0, &list));
    const uint32_t constructors[] = {MO_HOST_ARGS_OF,      MO_HOST_ENV_OF,
                                     MO_HOST_CONSOLE_FAKE, MO_HOST_FILES_IN_MEMORY,
                                     MO_HOST_CLOCK_FIXED,  MO_HOST_RANDOM_SEEDED};
    for (unsigned i = 0; i < 6; i++) {
        uint32_t op = constructors[i];
        unsigned slot = i >= 4 ? i + 1 : i;
        struct mo_cell input = op == MO_HOST_ARGS_OF ? mo_value_dup(f.heap, f.types.texts, list)
                               : op == MO_HOST_ENV_OF || op == MO_HOST_FILES_IN_MEMORY
                                   ? mo_value_dup(f.heap, f.types.files, map)
                                   : mo_cell_int(0);
        struct mo_host_error error;
        types[slot] = host_result_type(&f, op);
        MO_ASSERT(mo_host_call(&f.host, f.heap, op, types[slot], &input, &values[slot], &error) ==
                  MO_HOST_RETURNED);
        live[slot] = true;
    }
    struct mo_host_error readonly_error;
    types[4] = f.capabilities[MO_CAP_READ_FILES];
    MO_ASSERT(mo_host_call(&f.host, f.heap, MO_HOST_FILES_READ_ONLY, types[4], &values[3],
                           &values[4], &readonly_error) == MO_HOST_RETURNED);
    live[4] = true;
    mo_value_drop(f.heap, f.types.files, map);
    mo_value_drop(f.heap, f.types.texts, list);
    for (size_t at = 1; at + 4 < size && at < 1024; at += 5) {
        uint32_t source = data[at] % 32u, destination = data[at + 1] % 32u;
        if (data[at + 2] >= MO_HOST_COUNT) {
            if (live[destination]) {
                mo_value_drop(f.heap, types[destination], values[destination]);
                live[destination] = false;
            }
            if (source != destination && live[source] && (data[at + 2] & 1) != 0) {
                values[destination] = mo_value_dup(f.heap, types[source], values[source]);
                types[destination] = types[source];
                live[destination] = true;
            }
            continue;
        }
        uint32_t op = data[at + 2];
        struct mo_cell args[3], p, text;
        static const char *const paths[] = {".", "a", "a/b", "../x", "a/../b", "a//b", "a/"};
        const char *path = paths[data[at + 3] % 7u];
        if (!mo_value_text(f.heap, path, strlen(path), &p))
            continue;
        uint8_t payload[8];
        size_t length = size - at < sizeof payload ? size - at : sizeof payload;
        for (size_t i = 0; i < length; i++)
            payload[i] = data[at + i] & 0x7fu;
        if (!mo_value_text(f.heap, (const char *)payload, length, &text)) {
            mo_value_drop(f.heap, f.types.text, p);
            continue;
        }
        args[1] = p;
        args[2] = text;
        uint32_t temporary = MO_NO_INDEX;
        bool ready = true;
        if (op == MO_HOST_FILES_IN_MEMORY || op == MO_HOST_ENV_OF) {
            temporary = f.types.files;
            ready = mo_value_map(f.heap, temporary, &p, &text, data[at + 4] & 1u, &args[0]);
        } else if (op == MO_HOST_ARGS_OF || op == MO_HOST_CONSOLE_FAKE_INPUT) {
            temporary = f.types.texts;
            ready = mo_value_list(f.heap, temporary, &p, 1, &args[0]);
        } else if (op == MO_HOST_CONSOLE_FAKE)
            args[0] = mo_cell_unit();
        else if (op == MO_HOST_CLOCK_FIXED)
            args[0] = mo_cell_time((int64_t)data[at + 4] - 128);
        else if (op == MO_HOST_RANDOM_SEEDED)
            args[0] = mo_cell_int((int64_t)data[at + 4] - 128);
        else {
            ready = live[source] && f.types.types.items[types[source]].kind == MO_TYPE_CAPABILITY;
            if (ready) {
                static const char *const modules[] = {"Args",      "Env",   "Console", "Files",
                                                      "ReadFiles", "Clock", "Random"};
                enum mo_capability cap = f.types.types.items[types[source]].capability;
                ready = strcmp(modules[cap], operations[op].module) == 0;
                if (ready)
                    args[0] = values[source];
            }
            if (op == MO_HOST_RANDOM_INT) {
                args[1] = mo_cell_int((int64_t)data[at + 3] - 128);
                args[2] = mo_cell_int((int64_t)data[at + 4] - 128);
            }
        }
        if (!ready) {
            mo_value_drop(f.heap, f.types.text, p);
            mo_value_drop(f.heap, f.types.text, text);
            continue;
        }
        /* Retain snapshots before applying pressure. A failed operation must
         * preserve the shared state, not merely the argument cell's pointer. */
        struct mo_state *state = NULL;
        struct mo_cell saved[4];
        uint32_t saved_types[4];
        uint32_t slots = 0;
        if (temporary == MO_NO_INDEX && operations[op].modes[0] != '\0' &&
            op != MO_HOST_CLOCK_FIXED && op != MO_HOST_RANDOM_SEEDED) {
            state = mo_capability_get(args[0])->state;
            if (state != NULL) {
                enum mo_capability cap = f.types.types.items[types[source]].capability;
                slots = cap == MO_CAP_CONSOLE                             ? 4
                        : cap == MO_CAP_FILES || cap == MO_CAP_READ_FILES ? 2
                                                                          : 1;
                MO_ASSERT(slots <= 4);
                for (uint32_t i = 0; i < slots; i++) {
                    saved_types[i] = mo_state_type(state, i);
                    saved[i] = mo_value_dup(f.heap, saved_types[i], mo_state_get(state, i));
                }
            }
        }
        struct mo_scratch pressure;
        mo_scratch_init(&pressure, f.heap);
        if ((data[at + 1] & 4) != 0) {
            size_t used = mo_heap_used(f.heap);
            size_t room = used >= limit ? 0 : limit - used;
            if (room > 4096) {
                void *held = mo_scratch_resize(&pressure, NULL, 0, room - 128);
                if (held != NULL)
                    MO_ASSERT(mo_heap_used(f.heap) > used);
            }
        }
        uint32_t type = host_result_type(&f, op);
        struct mo_cell result = mo_cell_int(123);
        struct mo_host_error error;
        enum mo_host_outcome outcome =
            mo_host_call(&f.host, f.heap, op, type, args, &result, &error);
        mo_scratch_free(&pressure);
        if (outcome == MO_HOST_RETURNED) {
            if (live[destination])
                mo_value_drop(f.heap, types[destination], values[destination]);
            values[destination] = result;
            types[destination] = type;
            live[destination] = true;
        } else {
            MO_ASSERT(mo_cell_get_int(result) == 123);
            for (uint32_t i = 0; i < slots; i++) {
                bool equal;
                MO_ASSERT(mo_value_equal(f.heap, mo_state_type(state, i), saved[i],
                                         mo_state_get(state, i), &equal) &&
                          equal);
            }
            if (temporary != MO_NO_INDEX)
                mo_value_drop(f.heap, temporary, args[0]);
        }
        for (uint32_t i = 0; i < slots; i++)
            mo_value_drop(f.heap, saved_types[i], saved[i]);
        mo_value_drop(f.heap, f.types.text, p);
        mo_value_drop(f.heap, f.types.text, text);
    }
    for (unsigned i = 0; i < 32; i++)
        if (live[i])
            mo_value_drop(f.heap, types[i], values[i]);
    MO_ASSERT(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
    return 0;
}
