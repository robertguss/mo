#include "../tests/host_fixture.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static struct mo_cell call_cmd(struct host_fixture *f, uint32_t op, const struct mo_cell *args) {
    struct mo_cell out;
    struct mo_host_error error = {0};
    MO_ASSERT(mo_host_call(&f->host, f->heap, op, host_result_type(f, op), args, &out, &error) ==
              MO_HOST_RETURNED);
    return out;
}

static struct mo_cell ascii_text(struct host_fixture *f, const uint8_t *bytes, size_t len) {
    char buf[128];
    size_t n = len < sizeof buf ? len : sizeof buf;
    for (size_t i = 0; i < n; i++)
        buf[i] = (bytes[i] >= 0x20 && bytes[i] < 0x7f) ? (char)bytes[i] : '_';
    struct mo_cell cell;
    MO_ASSERT(mo_value_text(f->heap, buf, n, &cell));
    return cell;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 6)
        return 0;
    size_t rest = size - 4;
    size_t klen = (size_t)data[0] % (rest + 1);
    size_t vlen = (size_t)data[1] % (rest - klen + 1);
    size_t flen = (size_t)data[2] % (rest - klen - vlen + 1);
    size_t plen = rest - klen - vlen - flen;

    const uint8_t *kbytes = data + 4;
    const uint8_t *vbytes = kbytes + klen;
    const uint8_t *fbytes = vbytes + vlen;
    const uint8_t *pbytes = fbytes + flen;

    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);

    struct mo_cell k, v, replies;
    MO_ASSERT(mo_value_bytes(f.heap, kbytes, klen, &k));
    MO_ASSERT(mo_value_bytes(f.heap, vbytes, vlen, &v));
    MO_ASSERT(mo_value_map(f.heap, f.types.replies, &k, &v, 1, &replies));
    mo_value_drop(f.heap, f.types.bytes, k);
    mo_value_drop(f.heap, f.types.bytes, v);

    struct mo_cell fake_args[] = {replies};
    struct mo_cell fake = call_cmd(&f, MO_HOST_COMMAND_FAKE, fake_args);

    struct mo_cell folder = ascii_text(&f, fbytes, flen);
    struct mo_cell w_args[] = {fake, folder};
    struct mo_cell narrowed = call_cmd(&f, MO_HOST_COMMAND_WITHIN, w_args);
    mo_value_drop(f.heap, f.types.text, folder);

    struct mo_cell prog = ascii_text(&f, pbytes, plen);
    struct mo_cell allow_list;
    MO_ASSERT(mo_value_list(f.heap, f.types.texts, &prog, (data[3] & 1) ? 1 : 0, &allow_list));
    if ((data[3] & 2) != 0) {
        struct mo_cell a_args[] = {narrowed, allow_list};
        struct mo_cell next = call_cmd(&f, MO_HOST_COMMAND_ALLOW_PROGRAMS, a_args);
        mo_value_drop(f.heap, f.capabilities[MO_CAP_COMMAND], narrowed);
        narrowed = next;
    }

    struct mo_cell r_args[] = {narrowed, prog, allow_list, mo_cell_duration(1000000)};
    struct mo_cell ran = call_cmd(&f, MO_HOST_COMMAND_RUN_RAW, r_args);
    mo_value_drop(f.heap, f.result_command, ran);

    struct mo_host_fake_state snap = {0};
    MO_ASSERT(mo_host_fake_snapshot(&f.host, f.heap, narrowed, &snap));
    struct mo_cell restored;
    struct mo_host_error err = {0};
    MO_ASSERT(mo_host_fake_restore(&f.host, f.heap, f.capabilities[MO_CAP_COMMAND], &snap,
                                   &restored, &err) == MO_HOST_RETURNED);
    mo_host_fake_state_drop(&f.host, f.heap, &snap);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_COMMAND], restored);

    mo_value_drop(f.heap, f.types.texts, allow_list);
    mo_value_drop(f.heap, f.types.text, prog);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_COMMAND], narrowed);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_COMMAND], fake);
    MO_ASSERT(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
    return 0;
}
