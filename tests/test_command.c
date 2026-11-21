#define _POSIX_C_SOURCE 200809L
#include "host/host.h"
#include "host_fixture.h"
#include "test.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static struct mo_cell call_op(struct host_fixture *f, uint32_t op, const struct mo_cell *args) {
    struct mo_cell out;
    struct mo_host_error error = {0};
    MO_ASSERT(mo_host_call(&f->host, f->heap, op, host_result_type(f, op), args, &out, &error) ==
              MO_HOST_RETURNED);
    return out;
}

static struct mo_cell unwrap_ok_tuple(const struct host_fixture *f, struct mo_cell res) {
    MO_ASSERT(mo_cell_get_tag(res) == 0);
    struct mo_cell payload = mo_cell_get_payload(res);
    if (mo_case_shape(&f->types.types, f->result_command, 0) == MO_CASE_BOXED)
        return mo_value_field(payload, 0);
    return payload;
}

static struct mo_cell unwrap_err_text(const struct host_fixture *f, struct mo_cell res) {
    MO_ASSERT(mo_cell_get_tag(res) == 1);
    struct mo_cell payload = mo_cell_get_payload(res);
    if (mo_case_shape(&f->types.types, f->result_command, 1) == MO_CASE_BOXED)
        return mo_value_field(payload, 0);
    return payload;
}

static bool cell_text_equals(struct mo_cell cell, const char *expected) {
    size_t len = 0;
    const uint8_t *bytes = mo_value_data(cell, &len);
    return len == strlen(expected) && memcmp(bytes, expected, len) == 0;
}

static bool cell_text_starts_with(struct mo_cell cell, const char *prefix) {
    size_t len = 0;
    const uint8_t *bytes = mo_value_data(cell, &len);
    size_t plen = strlen(prefix);
    return len >= plen && memcmp(bytes, prefix, plen) == 0;
}

static bool cell_bytes_contains(struct mo_cell cell, const char *needle) {
    size_t len = 0;
    const uint8_t *bytes = mo_value_data(cell, &len);
    size_t nlen = strlen(needle);
    if (nlen == 0 || len < nlen)
        return false;
    for (size_t i = 0; i + nlen <= len; i++) {
        if (memcmp(bytes + i, needle, nlen) == 0)
            return true;
    }
    return false;
}

static struct mo_cell make_texts(struct host_fixture *f, const char *const *items, uint32_t count) {
    struct mo_cell cells[68];
    MO_ASSERT(count <= 68);
    for (uint32_t i = 0; i < count; i++)
        cells[i] = host_text(f, items[i]);
    struct mo_cell list;
    MO_ASSERT(mo_value_list(f->heap, f->types.texts, cells, count, &list));
    for (uint32_t i = 0; i < count; i++)
        mo_value_drop(f->heap, f->types.text, cells[i]);
    return list;
}

static struct mo_cell run_cmd(struct host_fixture *f, struct mo_cell cmd, const char *program,
                              const char *const *argv, uint32_t argc, int64_t timeout_us) {
    struct mo_cell prog = host_text(f, program);
    struct mo_cell args_list = make_texts(f, argv, argc);
    struct mo_cell call_args[] = {cmd, prog, args_list, mo_cell_duration(timeout_us)};
    struct mo_cell res = call_op(f, MO_HOST_COMMAND_RUN_RAW, call_args);
    mo_value_drop(f->heap, f->types.texts, args_list);
    mo_value_drop(f->heap, f->types.text, prog);
    return res;
}

static void write_file_str(const char *path, const char *text) {
    FILE *fp = fopen(path, "wb");
    MO_ASSERT(fp != NULL);
    size_t len = strlen(text);
    MO_ASSERT(fwrite(text, 1, len, fp) == len && fclose(fp) == 0);
}

void test_real_command(void);
void test_containment_and_allowlist(void);
void test_fake_command(void);

void test_real_command(void) {
    char workdir[] = "/tmp/mo-cmd-work-XXXXXX";
    char outdir[] = "/tmp/mo-cmd-out-XXXXXX";
    MO_ASSERT(mkdtemp(workdir) != NULL);
    MO_ASSERT(mkdtemp(outdir) != NULL);

    char app_path[300], secret_path[300], pwn_path[300];
    MO_ASSERT(snprintf(app_path, sizeof app_path, "%s/app.mo", workdir) > 0);
    MO_ASSERT(snprintf(secret_path, sizeof secret_path, "%s/secret.txt", outdir) > 0);
    MO_ASSERT(snprintf(pwn_path, sizeof pwn_path, "%s/pwned.txt", outdir) > 0);
    write_file_str(app_path, "module App\n\ntest \"ok\"\n  assert 2 + 2 == 4\nend\n");
    write_file_str(secret_path, "top-secret\n");

    int fd = open(workdir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    MO_ASSERT(fd >= 0);

    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    struct mo_cell cmd;
    struct mo_host_error err = {0};
    MO_ASSERT(mo_host_real(&f.host, f.heap, f.capabilities[MO_CAP_COMMAND], NULL, fd, &cmd, &err) ==
              MO_HOST_RETURNED);
    (void)close(fd);

    /* 1. Run echo directly without a shell */
    const char *echo_args[] = {"hello", "mo command"};
    struct mo_cell echo_res = run_cmd(&f, cmd, "echo", echo_args, 2, 5000000);
    CHECK(mo_cell_get_tag(echo_res) == 0);
    struct mo_cell tup = unwrap_ok_tuple(&f, echo_res);
    CHECK(mo_cell_get_int(mo_value_field(tup, 0)) == 0);
    CHECK(cell_text_equals(mo_value_field(tup, 1), "hello mo command\n"));
    CHECK(cell_text_equals(mo_value_field(tup, 2), ""));
    mo_value_drop(f.heap, f.result_command, echo_res);

    /* 2. Run sibling mo binary inside confined workspace */
    const char *mo_args[] = {"test", "app.mo"};
    struct mo_cell mo_res = run_cmd(&f, cmd, "mo", mo_args, 2, 15000000);
    CHECK(mo_cell_get_tag(mo_res) == 0);
    if (mo_cell_get_tag(mo_res) == 0) {
        struct mo_cell mtup = unwrap_ok_tuple(&f, mo_res);
        CHECK(mo_cell_get_int(mo_value_field(mtup, 0)) == 0);
        CHECK(cell_bytes_contains(mo_value_field(mtup, 1), "1 passed"));
    }
    mo_value_drop(f.heap, f.result_command, mo_res);

    /* 3. Kernel Landlock confinement refuses reading/writing outside workspace */
    if (mo_compile_can_confine()) {
        const char *cat_args[] = {secret_path};
        struct mo_cell cat_res = run_cmd(&f, cmd, "cat", cat_args, 1, 5000000);
        CHECK(mo_cell_get_tag(cat_res) == 0);
        struct mo_cell ctup = unwrap_ok_tuple(&f, cat_res);
        CHECK(mo_cell_get_int(mo_value_field(ctup, 0)) != 0);
        CHECK(cell_text_equals(mo_value_field(ctup, 1), ""));
        mo_value_drop(f.heap, f.result_command, cat_res);

        const char *touch_args[] = {pwn_path};
        struct mo_cell touch_res = run_cmd(&f, cmd, "touch", touch_args, 1, 5000000);
        CHECK(mo_cell_get_tag(touch_res) == 0);
        CHECK(mo_cell_get_int(mo_value_field(unwrap_ok_tuple(&f, touch_res), 0)) != 0);
        struct stat st;
        CHECK(stat(pwn_path, &st) != 0);
        mo_value_drop(f.heap, f.result_command, touch_res);
    }

    mo_value_drop(f.heap, f.capabilities[MO_CAP_COMMAND], cmd);
    CHECK(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);

    char cleanup[700];
    MO_ASSERT(snprintf(cleanup, sizeof cleanup, "rm -rf '%s' '%s'", workdir, outdir) > 0 &&
              system(cleanup) == 0);
}

void test_containment_and_allowlist(void) {
    char workdir[] = "/tmp/mo-cmd-scope-XXXXXX";
    MO_ASSERT(mkdtemp(workdir) != NULL);
    int fd = open(workdir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    MO_ASSERT(fd >= 0);

    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);
    struct mo_cell cmd;
    struct mo_host_error err = {0};
    MO_ASSERT(mo_host_real(&f.host, f.heap, f.capabilities[MO_CAP_COMMAND], NULL, fd, &cmd, &err) ==
              MO_HOST_RETURNED);
    (void)close(fd);

    /* Deferred within("sub") works once sub is created */
    struct mo_cell sub_name = host_text(&f, "sub");
    struct mo_cell within_args[] = {cmd, sub_name};
    struct mo_cell sub_cmd = call_op(&f, MO_HOST_COMMAND_WITHIN, within_args);
    mo_value_drop(f.heap, f.types.text, sub_name);

    char sub_dir[300];
    MO_ASSERT(snprintf(sub_dir, sizeof sub_dir, "%s/sub", workdir) > 0 &&
              mkdir(sub_dir, 0700) == 0);
    const char *echo_args[] = {"in-sub"};
    struct mo_cell sub_res = run_cmd(&f, sub_cmd, "echo", echo_args, 1, 5000000);
    CHECK(mo_cell_get_tag(sub_res) == 0);
    mo_value_drop(f.heap, f.result_command, sub_res);

    /* Escaping within("../etc") poisons capability */
    struct mo_cell esc_name = host_text(&f, "../etc");
    struct mo_cell esc_args[] = {cmd, esc_name};
    struct mo_cell esc_cmd = call_op(&f, MO_HOST_COMMAND_WITHIN, esc_args);
    mo_value_drop(f.heap, f.types.text, esc_name);
    struct mo_cell esc_res = run_cmd(&f, esc_cmd, "echo", echo_args, 1, 5000000);
    CHECK(mo_cell_get_tag(esc_res) == 1);
    CHECK(cell_text_equals(unwrap_err_text(&f, esc_res), "outside_folder:../etc"));
    mo_value_drop(f.heap, f.result_command, esc_res);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_COMMAND], esc_cmd);

    /* Monotonic allow_programs intersection + path rejection */
    const char *p1[] = {"echo", "sleep"};
    struct mo_cell l1 = make_texts(&f, p1, 2);
    struct mo_cell a1_args[] = {sub_cmd, l1};
    struct mo_cell allowed1 = call_op(&f, MO_HOST_COMMAND_ALLOW_PROGRAMS, a1_args);
    mo_value_drop(f.heap, f.types.texts, l1);

    struct mo_cell dis_res = run_cmd(&f, allowed1, "ls", NULL, 0, 5000000);
    CHECK(mo_cell_get_tag(dis_res) == 1);
    CHECK(cell_text_equals(unwrap_err_text(&f, dis_res), "not_permitted:ls"));
    mo_value_drop(f.heap, f.result_command, dis_res);

    struct mo_cell slash_res = run_cmd(&f, cmd, "/bin/echo", echo_args, 1, 5000000);
    CHECK(mo_cell_get_tag(slash_res) == 1);
    CHECK(cell_text_equals(unwrap_err_text(&f, slash_res), "not_permitted:/bin/echo"));
    mo_value_drop(f.heap, f.result_command, slash_res);

    const char *p2[] = {"sleep", "ls"};
    struct mo_cell l2 = make_texts(&f, p2, 2);
    struct mo_cell a2_args[] = {allowed1, l2};
    struct mo_cell allowed2 = call_op(&f, MO_HOST_COMMAND_ALLOW_PROGRAMS, a2_args);
    mo_value_drop(f.heap, f.types.texts, l2);

    struct mo_cell echo_denied = run_cmd(&f, allowed2, "echo", echo_args, 1, 5000000);
    CHECK(mo_cell_get_tag(echo_denied) == 1);
    CHECK(cell_text_equals(unwrap_err_text(&f, echo_denied), "not_permitted:echo"));
    mo_value_drop(f.heap, f.result_command, echo_denied);

    /* Timeout kills long-running child */
    const char *sleep_args[] = {"5"};
    struct mo_cell to_res = run_cmd(&f, allowed2, "sleep", sleep_args, 1, 40000);
    CHECK(mo_cell_get_tag(to_res) == 1);
    CHECK(cell_text_equals(unwrap_err_text(&f, to_res), "timed_out"));
    mo_value_drop(f.heap, f.result_command, to_res);

    /* Bug limits on duration and arg count */
    struct mo_cell prog = host_text(&f, "echo");
    struct mo_cell empty_list = make_texts(&f, NULL, 0);
    struct mo_cell bad_dur_args[] = {cmd, prog, empty_list, mo_cell_duration(500)};
    struct mo_cell unused;
    CHECK(mo_host_call(&f.host, f.heap, MO_HOST_COMMAND_RUN_RAW, f.result_command, bad_dur_args,
                       &unused, &err) == MO_HOST_BUG);
    mo_value_drop(f.heap, f.types.texts, empty_list);

    const char *many[65];
    for (unsigned i = 0; i < 65; i++)
        many[i] = "x";
    struct mo_cell big_list = make_texts(&f, many, 65);
    struct mo_cell bad_argc_args[] = {cmd, prog, big_list, mo_cell_duration(1000000)};
    CHECK(mo_host_call(&f.host, f.heap, MO_HOST_COMMAND_RUN_RAW, f.result_command, bad_argc_args,
                       &unused, &err) == MO_HOST_BUG);
    mo_value_drop(f.heap, f.types.texts, big_list);
    mo_value_drop(f.heap, f.types.text, prog);

    mo_value_drop(f.heap, f.capabilities[MO_CAP_COMMAND], allowed2);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_COMMAND], allowed1);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_COMMAND], sub_cmd);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_COMMAND], cmd);
    CHECK(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);

    char cleanup[400];
    MO_ASSERT(snprintf(cleanup, sizeof cleanup, "rm -rf '%s'", workdir) > 0 &&
              system(cleanup) == 0);
}

void test_fake_command(void) {
    struct host_fixture f;
    host_fixture_init(&f, mo_limits_default().heap_size);

    const char *raw_key = "4:work\n2:mo\n2\n4:test\n6:app.mo\n";
    const char *raw_val = "0:9:0\n1 passed\n";
    struct mo_cell k, v, table;
    MO_ASSERT(mo_value_bytes(f.heap, (const uint8_t *)raw_key, strlen(raw_key), &k));
    MO_ASSERT(mo_value_bytes(f.heap, (const uint8_t *)raw_val, strlen(raw_val), &v));
    MO_ASSERT(mo_value_map(f.heap, f.types.replies, &k, &v, 1, &table));
    mo_value_drop(f.heap, f.types.bytes, k);
    mo_value_drop(f.heap, f.types.bytes, v);

    /* MO_HOST_COMMAND_FAKE consumes its Map(Bytes, Bytes) argument ("o") */
    struct mo_cell fake_args[] = {table};
    struct mo_cell fake = call_op(&f, MO_HOST_COMMAND_FAKE, fake_args);

    struct mo_host_fake_state snap = {0};
    CHECK(mo_host_fake_snapshot(&f.host, f.heap, fake, &snap));
    struct mo_cell restored;
    struct mo_host_error err = {0};
    CHECK(mo_host_fake_restore(&f.host, f.heap, f.capabilities[MO_CAP_COMMAND], &snap, &restored,
                               &err) == MO_HOST_RETURNED);
    mo_host_fake_state_drop(&f.host, f.heap, &snap);

    struct mo_cell work_dir = host_text(&f, "work");
    struct mo_cell w_args[] = {restored, work_dir};
    struct mo_cell scoped = call_op(&f, MO_HOST_COMMAND_WITHIN, w_args);
    mo_value_drop(f.heap, f.types.text, work_dir);

    const char *argv[] = {"test", "app.mo"};
    struct mo_cell ok_res = run_cmd(&f, scoped, "mo", argv, 2, 1000000);
    CHECK(mo_cell_get_tag(ok_res) == 0);
    struct mo_cell tup = unwrap_ok_tuple(&f, ok_res);
    CHECK(mo_cell_get_int(mo_value_field(tup, 0)) == 0);
    CHECK(cell_text_equals(mo_value_field(tup, 1), "1 passed\n"));
    mo_value_drop(f.heap, f.result_command, ok_res);

    const char *miss_argv[] = {"check", "app.mo"};
    struct mo_cell miss_res = run_cmd(&f, scoped, "mo", miss_argv, 2, 1000000);
    CHECK(mo_cell_get_tag(miss_res) == 1);
    CHECK(cell_text_starts_with(unwrap_err_text(&f, miss_res), "spawn_failed:"));
    mo_value_drop(f.heap, f.result_command, miss_res);

    mo_value_drop(f.heap, f.capabilities[MO_CAP_COMMAND], scoped);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_COMMAND], restored);
    mo_value_drop(f.heap, f.capabilities[MO_CAP_COMMAND], fake);
    CHECK(mo_heap_objects(f.heap) == 0);
    host_fixture_free(&f);
}

int main(void) {
    test_real_command();
    test_containment_and_allowlist();
    test_fake_command();
    return mo_test_report("test_command");
}
