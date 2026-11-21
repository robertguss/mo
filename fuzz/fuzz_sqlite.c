#include "../tests/host_fixture.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static struct mo_cell call_sqlite(struct host_fixture *f, uint32_t op, const struct mo_cell *args) {
    struct mo_cell out;
    struct mo_host_error error = {0};
    MO_ASSERT(mo_host_call(&f->host, f->heap, op, host_result_type(f, op), args, &out, &error) ==
              MO_HOST_RETURNED);
    return out;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 4)
        return 0;
    size_t img_len = (size_t)data[0] % (size - 3);
    size_t sql1_len = (size_t)data[1] % (size - 3 - img_len + 1);
    if (sql1_len > size - 4 - img_len)
        sql1_len = size - 4 - img_len;
    const uint8_t *img = data + 4;
    const uint8_t *sql1 = img + img_len;
    const uint8_t *sql2 = sql1 + sql1_len;
    size_t sql2_len = size - (size_t)(sql2 - data);

    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    struct mo_cell fake;
    if ((data[2] & 1) == 0) {
        fake = call_sqlite(&f, MO_HOST_SQLITE_FAKE, NULL);
    } else {
        struct mo_cell k = host_text(&f, "fuzz.db"), v, init;
        MO_ASSERT(mo_value_bytes(f.heap, img, img_len, &v));
        MO_ASSERT(mo_value_map(f.heap, f.types.databases, &k, &v, 1, &init));
        mo_value_drop(f.heap, f.types.text, k);
        mo_value_drop(f.heap, f.types.bytes, v);
        struct mo_host_fake_state state = {.kind = MO_CAP_SQLITE, .as.sqlite = init};
        struct mo_host_error error = {0};
        MO_ASSERT(mo_host_fake_restore(&f.host, f.heap, f.capabilities[MO_CAP_SQLITE], &state,
                                       &fake, &error) == MO_HOST_RETURNED);
        mo_value_drop(f.heap, f.types.databases, init);
    }

    struct mo_cell path = host_text(&f, "fuzz.db");
    struct mo_cell o_args[] = {fake, path};
    struct mo_cell opened = call_sqlite(&f, MO_HOST_SQLITE_OPEN, o_args);
    mo_value_drop(f.heap, f.types.text, path);

    if (mo_cell_get_tag(opened) == 0) {
        struct mo_cell db = mo_cell_get_payload(opened);
        if (mo_case_shape(&f.types.types, f.result_database, 0) == MO_CASE_BOXED)
            db = mo_value_field(db, 0);

        uint32_t rows_matrix = f.types.types.items[f.sqlite_rows].fields[1].type;
        uint32_t params_ty = f.types.types.items[rows_matrix].parameters[0];
        struct mo_cell params;
        MO_ASSERT(mo_value_list(f.heap, params_ty, NULL, 0, &params));

        char buf1[256], buf2[256];
        size_t n1 = sql1_len < sizeof buf1 ? sql1_len : sizeof buf1 - 1;
        size_t n2 = sql2_len < sizeof buf2 ? sql2_len : sizeof buf2 - 1;
        for (size_t i = 0; i < n1; i++)
            buf1[i] = (sql1[i] >= 0x20 && sql1[i] < 0x7f) ? (char)sql1[i] : ' ';
        for (size_t i = 0; i < n2; i++)
            buf2[i] = (sql2[i] >= 0x20 && sql2[i] < 0x7f) ? (char)sql2[i] : ' ';

        struct mo_cell t1, t2;
        MO_ASSERT(mo_value_text(f.heap, buf1, n1, &t1) && mo_value_text(f.heap, buf2, n2, &t2));
        struct mo_cell r_args[] = {db, t1, params};
        struct mo_cell run_res = call_sqlite(&f, MO_HOST_SQLITE_RUN, r_args);
        mo_value_drop(f.heap, f.result_sqlite_unit, run_res);

        struct mo_cell q_args[] = {db, t2, params};
        struct mo_cell q_res = call_sqlite(&f, MO_HOST_SQLITE_QUERY, q_args);
        mo_value_drop(f.heap, f.result_sqlite_rows, q_res);

        mo_value_drop(f.heap, f.types.text, t1);
        mo_value_drop(f.heap, f.types.text, t2);
        mo_value_drop(f.heap, params_ty, params);

        struct mo_host_fake_state snap = {0};
        MO_ASSERT(mo_host_fake_snapshot(&f.host, f.heap, fake, &snap));
        mo_host_fake_state_drop(&f.host, f.heap, &snap);
    }

    mo_value_drop(f.heap, f.result_database, opened);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_SQLITE], fake);
    MO_ASSERT(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
    return 0;
}
