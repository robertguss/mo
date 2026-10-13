#include "heap_fixture.h"
#include "host/host.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>

static void test_console(struct heap_fixture *f) {
    struct mo_type *storage = calloc(f->types.count + MO_HOST_PRIVATE_TYPES, sizeof *storage);
    MO_ASSERT(storage != NULL);
    struct mo_host_types types;
    mo_host_prepare_types(&types, &f->types, storage);
    struct mo_limits limits = mo_limits_default();
    struct mo_heap_image *image = mo_heap_image_new(&types.types, &limits);
    MO_ASSERT(image != NULL);
    struct mo_heap *heap = mo_heap_new(image, &limits);
    MO_ASSERT(heap != NULL);
    struct mo_host host;
    mo_host_init(&host, &types);
    struct mo_host_error error;
    struct mo_cell console, text, result, snapshot;
    CHECK(mo_host_call(&host, heap, MO_HOST_CONSOLE_FAKE, f->capabilities[MO_CAP_CONSOLE], NULL,
                       &console, &error) == MO_HOST_RETURNED);
    CHECK(mo_value_text(heap, "a\0\xc3\xa9", 4, &text));
    struct mo_cell args[] = {console, text};
    CHECK(mo_host_call(&host, heap, MO_HOST_CONSOLE_PRINT, f->builtin[MO_TYPE_UNIT], args, &result,
                       &error) == MO_HOST_RETURNED);
    CHECK(mo_host_call(&host, heap, MO_HOST_CONSOLE_PRINTED, types.texts, args, &snapshot,
                       &error) == MO_HOST_RETURNED);
    CHECK(mo_host_call(&host, heap, MO_HOST_CONSOLE_PRINT, f->builtin[MO_TYPE_UNIT], args, &result,
                       &error) == MO_HOST_RETURNED);
    CHECK(mo_host_call(&host, heap, MO_HOST_CONSOLE_PRINTED, types.texts, args, &result, &error) ==
          MO_HOST_RETURNED);
    CHECK(mo_list_size(snapshot) == 1 && mo_list_size(result) == 2);
    struct mo_cell item;
    CHECK(mo_list_get(result, 1, &item));
    size_t length;
    CHECK(memcmp(mo_value_data(item, &length), "a\0\xc3\xa9", 4) == 0 && length == 4);
    mo_value_drop(heap, types.texts, result);
    mo_value_drop(heap, types.texts, snapshot);
    mo_value_drop(heap, types.text, text);
    mo_value_drop(heap, f->capabilities[MO_CAP_CONSOLE], console);
    CHECK(mo_heap_objects(heap) == 0);
    mo_heap_free(heap);
    mo_heap_image_free(image);
    free(storage);
}

int main(void) {
    struct heap_fixture fixture;
    heap_fixture_init(&fixture);
    test_console(&fixture);
    mo_heap_image_free(fixture.image);
    mo_arena_free(&fixture.arena);
    return mo_test_report("host");
}
