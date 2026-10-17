#include "tools/fakes.h"

#include <string.h>

/* The contents come from small fixed palettes, so a fake's size is bounded and its texts are
 * static. Paths keep a folder apart from the files in it, as a fake requires (reference 12.4). */
static const char *const paths[] = {"a.txt", "b.md", "notes/c.txt", "notes/d.md",
                                    "notes/deep/e.txt"};
static const char *const file_texts[] = {"",
                                         "x",
                                         "one\ntwo\n",
                                         "x marks the spot\nbakery\n",
                                         "caf\xc3\xa9 \xe2\x82\xac\n",
                                         "no trailing newline",
                                         "\n\n",
                                         "a, b\r\nc\r\n"};
static const char *const lines[] = {"", "x", "hello world", "caf\xc3\xa9", "x y z", "  padded  "};
static const char *const arguments[] = {"",         "a",           "--help",   "-x",
                                        "file.txt", "caf\xc3\xa9", "two words"};
static const char *const env_names[] = {"HOME", "PATH", "USER", "EMPTY", "LANG"};
static const char *const env_values[] = {"/home/mo", "/usr/bin", "mo", "", "en_US.UTF-8"};

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define MAX_ITEMS 8
#define BOUNDARIES 3

static uint64_t next_random(uint64_t *state) {
    uint64_t z = (*state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

/* What a fake holds: up to MAX_ITEMS texts, or key/value pairs, picked from a palette. */
struct contents {
    const char *keys[MAX_ITEMS];
    const char *values[MAX_ITEMS];
    uint32_t count;
};

static bool charge(struct mo_input_limits *remaining, const struct contents *contents) {
    size_t bytes = 0;
    for (uint32_t i = 0; i < contents->count; i++) {
        bytes += (contents->keys[i] != NULL ? strlen(contents->keys[i]) : 0) +
                 (contents->values[i] != NULL ? strlen(contents->values[i]) : 0);
    }
    uint64_t nodes = 1 + (uint64_t)contents->count;
    uint64_t work = nodes + bytes;
    if (nodes > remaining->nodes || bytes > remaining->bytes || work > remaining->work)
        return false;
    remaining->nodes -= (uint32_t)nodes;
    remaining->bytes -= bytes;
    remaining->work -= work;
    return true;
}

/* A sample of k distinct entries of an n-entry palette. */
static void pick(const struct mo_input_request *request, uint64_t *random, uint32_t n, uint32_t k,
                 uint32_t *out) {
    uint32_t all[MAX_ITEMS];
    MO_ASSERT(n <= MAX_ITEMS && k <= n);
    for (uint32_t i = 0; i < n; i++)
        all[i] = i;
    /* Boundaries take the palette in order, so they are the same on every run and every promise. */
    for (uint32_t i = 0; i < k && request->kind == MO_INPUT_BOUNDARY; i++)
        out[i] = all[i];
    for (uint32_t i = 0; i < k && request->kind != MO_INPUT_BOUNDARY; i++) {
        uint32_t j = i + (uint32_t)(next_random(random) % (n - i));
        uint32_t swap = all[i];
        all[i] = all[j];
        all[j] = swap;
        out[i] = all[i];
    }
}

static uint32_t amount(const struct mo_input_request *request, uint64_t *random, uint32_t most) {
    if (request->kind == MO_INPUT_BOUNDARY)
        return request->boundary == 0 ? 0 : request->boundary == 1 ? (most > 1 ? 1 : most) : most;
    uint32_t limit = request->size < most ? request->size : most;
    return (uint32_t)(next_random(random) % (limit + 1));
}

static void file_contents(const struct mo_input_request *request, struct contents *out) {
    uint64_t random = request->seed;
    uint32_t chosen[MAX_ITEMS];
    uint32_t count = amount(request, &random, (uint32_t)COUNT(paths));
    pick(request, &random, (uint32_t)COUNT(paths), count, chosen);
    out->count = count;
    for (uint32_t i = 0; i < count; i++) {
        out->keys[i] = paths[chosen[i]];
        out->values[i] = request->kind == MO_INPUT_BOUNDARY
                             ? file_texts[(i + (count == 1 ? 1 : 0)) % COUNT(file_texts)]
                             : file_texts[next_random(&random) % COUNT(file_texts)];
    }
}

static void list_contents(const char *const *palette, uint32_t size,
                          const struct mo_input_request *request, struct contents *out) {
    uint64_t random = request->seed;
    uint32_t chosen[MAX_ITEMS];
    uint32_t count = amount(request, &random, size);
    pick(request, &random, size, count, chosen);
    out->count = count;
    for (uint32_t i = 0; i < count; i++)
        out->values[i] = palette[request->kind == MO_INPUT_BOUNDARY && count == 1 ? 1 : chosen[i]];
}

static void env_contents(const struct mo_input_request *request, struct contents *out) {
    uint64_t random = request->seed;
    uint32_t chosen[MAX_ITEMS];
    uint32_t count = amount(request, &random, (uint32_t)COUNT(env_names));
    pick(request, &random, (uint32_t)COUNT(env_names), count, chosen);
    out->count = count;
    for (uint32_t i = 0; i < count; i++) {
        out->keys[i] = env_names[chosen[i]];
        out->values[i] = env_values[chosen[i]];
    }
}

static enum mo_input_result outcome(enum mo_host_outcome result) {
    MO_ASSERT(result == MO_HOST_RETURNED || result == MO_HOST_OUT_OF_MEMORY);
    return result == MO_HOST_RETURNED ? MO_INPUT_OK : MO_INPUT_NO_MEMORY;
}

/* An owned List(Text), Set(Text) (empty) or Map(Text, Text) in the host's own types. */
static bool texts_list(struct mo_host *host, struct mo_heap *heap, const struct contents *contents,
                       struct mo_cell *out) {
    struct mo_cell items[MAX_ITEMS];
    uint32_t made = 0;
    bool ok = true;
    while (ok && made < contents->count) {
        ok = mo_value_text(heap, contents->values[made], strlen(contents->values[made]),
                           &items[made]);
        if (ok)
            made++;
    }
    ok = ok && mo_value_list(heap, host->types->texts, items, contents->count, out);
    for (uint32_t i = 0; i < made; i++)
        mo_value_drop(heap, host->types->text, items[i]);
    return ok;
}

static bool texts_map(struct mo_host *host, struct mo_heap *heap, const struct contents *contents,
                      struct mo_cell *out) {
    struct mo_cell keys[MAX_ITEMS], values[MAX_ITEMS];
    uint32_t made_keys = 0, made_values = 0;
    bool ok = true;
    while (ok && made_values < contents->count) {
        ok = mo_value_text(heap, contents->keys[made_keys], strlen(contents->keys[made_keys]),
                           &keys[made_keys]);
        if (!ok)
            break;
        made_keys++;
        ok = mo_value_text(heap, contents->values[made_values],
                           strlen(contents->values[made_values]), &values[made_values]);
        if (ok)
            made_values++;
    }
    ok = ok && mo_value_map(heap, host->types->files, keys, values, contents->count, out);
    for (uint32_t i = 0; i < made_keys; i++)
        mo_value_drop(heap, host->types->text, keys[i]);
    for (uint32_t i = 0; i < made_values; i++)
        mo_value_drop(heap, host->types->text, values[i]);
    return ok;
}

static bool empty_texts(struct mo_host *host, struct mo_heap *heap, struct mo_cell *out) {
    return texts_list(host, heap, &(struct contents){0}, out);
}

static bool empty_folders(struct mo_host *host, struct mo_heap *heap, struct mo_cell *out) {
    struct mo_cell none[1] = {mo_cell_unit()};
    return mo_value_set(heap, host->types->folders, none, 0, out);
}

#define INSTANT_FIRST INT64_C(-62135596800000000)
#define INSTANT_LAST INT64_C(253402300799999999)

static enum mo_input_result make(struct mo_host *host, struct mo_heap *heap, uint32_t type,
                                 const struct mo_input_request *request,
                                 struct mo_input_limits *remaining, struct mo_cell *out) {
    enum mo_capability kind = host->types->types.items[type].capability;
    struct mo_host_fake_state state = {.kind = kind};
    struct contents contents = {0};
    struct mo_cell made[3];
    bool ok = true;
    int64_t number = 0;
    switch (kind) {
    case MO_CAP_ARGS:
        list_contents(arguments, (uint32_t)COUNT(arguments), request, &contents);
        break;
    case MO_CAP_ENV:
        env_contents(request, &contents);
        break;
    case MO_CAP_CONSOLE:
        list_contents(lines, (uint32_t)COUNT(lines), request, &contents);
        break;
    case MO_CAP_FILES:
    case MO_CAP_READ_FILES:
        file_contents(request, &contents);
        break;
    case MO_CAP_CLOCK:
        number = request->kind == MO_INPUT_RANDOM
                     ? INSTANT_FIRST +
                           (int64_t)(request->seed % (uint64_t)(INSTANT_LAST + 1 - INSTANT_FIRST))
                 : request->boundary == 0 ? 0
                 : request->boundary == 1 ? INSTANT_FIRST
                                          : INSTANT_LAST;
        break;
    case MO_CAP_RANDOM:
        number = request->kind == MO_INPUT_RANDOM ? (int64_t)request->seed
                 : request->boundary == 0         ? 0
                 : request->boundary == 1         ? 1
                                                  : -1;
        break;
    }
    if (!charge(remaining, &contents))
        return MO_INPUT_LIMIT;
    struct mo_host_error error;
    switch (kind) {
    case MO_CAP_ARGS:
        ok = texts_list(host, heap, &contents, &state.as.args);
        break;
    case MO_CAP_ENV:
        ok = texts_map(host, heap, &contents, &state.as.env);
        break;
    case MO_CAP_CONSOLE:
        ok = texts_list(host, heap, &contents, &made[0]);
        if (ok && !(ok = empty_texts(host, heap, &made[1])))
            mo_value_drop(heap, host->types->texts, made[0]);
        if (ok && !(ok = empty_texts(host, heap, &made[2]))) {
            mo_value_drop(heap, host->types->texts, made[0]);
            mo_value_drop(heap, host->types->texts, made[1]);
        }
        state.as.console.input = made[0];
        state.as.console.output = made[1];
        state.as.console.errors = made[2];
        break;
    case MO_CAP_FILES:
    case MO_CAP_READ_FILES:
        ok = texts_map(host, heap, &contents, &made[0]);
        if (ok && !(ok = empty_folders(host, heap, &made[1])))
            mo_value_drop(heap, host->types->files, made[0]);
        state.as.files.contents = made[0];
        state.as.files.folders = made[1];
        break;
    case MO_CAP_CLOCK:
        state.as.time = number;
        break;
    case MO_CAP_RANDOM: {
        struct mo_cell seed = mo_cell_int(number);
        return outcome(mo_host_call(host, heap, MO_HOST_RANDOM_SEEDED, type, &seed, out, &error));
    }
    }
    if (!ok)
        return MO_INPUT_NO_MEMORY;
    enum mo_input_result result =
        outcome(mo_host_fake_restore(host, heap, type, &state, out, &error));
    mo_host_fake_state_drop(host, heap, &state);
    return result;
}

static enum mo_input_result register_recipe(struct mo_fakes *fakes, struct mo_heap *heap,
                                            uint32_t type, const struct mo_input_request *request,
                                            struct mo_cell *out) {
    if (fakes->pending_count == MO_CLI_FAKE_RECIPES) {
        mo_value_drop(heap, type, *out);
        return MO_INPUT_LIMIT;
    }
    fakes->pending[fakes->pending_count++] =
        (struct mo_fake_recipe){.object = mo_cell_get_ref(*out), .type = type, .request = *request};
    return MO_INPUT_OK;
}

static enum mo_input_result build(void *context, struct mo_heap *heap,
                                  const struct mo_input_request *request,
                                  struct mo_input_limits *remaining, struct mo_cell *out) {
    const struct mo_fake_context *fake = context;
    struct mo_fakes *fakes = fake->fakes;
    MO_ASSERT(fakes->host != NULL);
    struct mo_cell made;
    enum mo_input_result result = make(fakes->host, heap, fake->type, request, remaining, &made);
    if (result != MO_INPUT_OK)
        return result;
    result = register_recipe(fakes, heap, fake->type, request, &made);
    if (result == MO_INPUT_OK)
        *out = made;
    return result;
}

/* A copy is made again from the recipe of the fake it copies, not from its state now, which a
 * call may have changed. */
static enum mo_input_result copy(void *context, struct mo_heap *heap, struct mo_cell source,
                                 struct mo_input_limits *remaining, struct mo_cell *out) {
    const struct mo_fake_context *fake = context;
    struct mo_fakes *fakes = fake->fakes;
    const void *object = mo_cell_get_ref(source);
    for (uint32_t i = fakes->kept_count; i-- > 0;) {
        if (fakes->kept[i].object != object)
            continue;
        struct mo_input_request request = fakes->kept[i].request;
        return build(context, heap, &request, remaining, out);
    }
    return MO_INPUT_UNSUPPORTED;
}

void mo_fakes_init(struct mo_fakes *fakes, const struct mo_types *types) {
    *fakes = (struct mo_fakes){0};
    for (uint32_t i = 0; i < types->count; i++) {
        if (types->items[i].kind != MO_TYPE_CAPABILITY)
            continue;
        uint32_t slot = fakes->factory_count++;
        MO_ASSERT(slot <= MO_CAP_RANDOM);
        fakes->contexts[slot] = (struct mo_fake_context){fakes, i};
        fakes->factories[slot] = (struct mo_input_factory){.type = i,
                                                           .boundaries = BOUNDARIES,
                                                           .context = &fakes->contexts[slot],
                                                           .build = build,
                                                           .copy = copy};
    }
}

void mo_fakes_bind(struct mo_fakes *fakes, struct mo_host *host) {
    fakes->host = host;
    fakes->pending_count = 0;
}

void mo_fakes_keep(struct mo_fakes *fakes) {
    memcpy(fakes->kept, fakes->pending, fakes->pending_count * sizeof fakes->pending[0]);
    fakes->kept_count = fakes->pending_count;
}
