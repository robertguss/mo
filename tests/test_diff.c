#define _POSIX_C_SOURCE 200809L
#include "test.h"
#include "tools/tools.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_file_str(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    MO_ASSERT(f != NULL);
    size_t len = strlen(text);
    MO_ASSERT(fwrite(text, 1, len, f) == len && fclose(f) == 0);
}

static int run_diff_cmd(int argc, const char *const argv[], struct mo_buffer *out,
                        struct mo_buffer *err) {
    struct mo_cli cli;
    const char *parse_err = NULL;
    MO_ASSERT(mo_cli_parse(argc, argv, &cli, &parse_err));
    FILE *fout = tmpfile(), *ferr = tmpfile();
    MO_ASSERT(fout != NULL && ferr != NULL);
    int status = mo_cli_execute(&cli, fout, ferr);
    rewind(fout);
    rewind(ferr);
    mo_buffer_clear(out);
    mo_buffer_clear(err);
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, fout)) > 0)
        MO_ASSERT(mo_buffer_append(out, buf, n));
    while ((n = fread(buf, 1, sizeof buf, ferr)) > 0)
        MO_ASSERT(mo_buffer_append(err, buf, n));
    MO_ASSERT(mo_buffer_append_byte(out, 0) && mo_buffer_append_byte(err, 0));
    (void)fclose(fout);
    (void)fclose(ferr);
    return status;
}

int main(void) {
    char dir[] = "/tmp/mo-test-diff-XXXXXX";
    MO_ASSERT(mkdtemp(dir) != NULL);
    char old_p[256], new_p[256];
    MO_ASSERT(snprintf(old_p, sizeof old_p, "%s/old", dir) > 0 && mkdir(old_p, 0700) == 0);
    MO_ASSERT(snprintf(new_p, sizeof new_p, "%s/new", dir) > 0 && mkdir(new_p, 0700) == 0);
    char old_main[300], new_main[300];
    MO_ASSERT(snprintf(old_main, sizeof old_main, "%s/main.mo", old_p) > 0);
    MO_ASSERT(snprintf(new_main, sizeof new_main, "%s/main.mo", new_p) > 0);

    /* 1. Authority, public surface, and passing contracts with private helper preservation */
    write_file_str(old_main, "module Main\n"
                             "expose clamp, Box\n"
                             "struct Box\n  v: Int\nend\n"
                             "limit = 10\n"
                             "fn within?(n: Int) : Bool\n  n >= 0 and n <= limit\nend\n"
                             "fn clamp(n: Int) : Int\n"
                             "  promise\n    within?(result)\n  end\n"
                             "  examples\n    clamp(3) == 3\n    clamp(25) == 10\n  end\n"
                             "  return 0 if n < 0\n"
                             "  return limit if n > limit\n"
                             "  n\n"
                             "end\n"
                             "fn main(files: Files)\n"
                             "  _sub = files.within(\"./data\")\n"
                             "end\n");

    write_file_str(new_main, "module Main\n"
                             "expose clamp, Box(..)\n"
                             "struct Box\n  v: Int\nend\n"
                             "limit = 999\n"
                             "fn clamp(val: Int) : Int\n"
                             "  if val < 0: 0 else if val > 10: 10 else: val\n"
                             "end\n"
                             "fn main(files: Files, net: Network)\n"
                             "  _sub = files.within(\"./data\")\n"
                             "  _restricted = net.allow_hosts([\"api.stripe.com:443\"])\n"
                             "end\n");

    struct mo_buffer out, err;
    mo_buffer_init(&out, 65536);
    mo_buffer_init(&err, 16384);

    const char *argv_text[] = {"mo", "diff", old_main, new_main};
    CHECK(run_diff_cmd(4, argv_text, &out, &err) == 0);
    const char *text = (const char *)out.bytes;
    CHECK(strstr(text, "+ capability Network") != NULL);
    CHECK(strstr(text, "+ attenuation Network.allow_hosts([\"api.stripe.com:443\"])") != NULL);
    CHECK(strstr(text, "~ type Main.Box: opaque struct -> open struct Box(v: Int)") != NULL);
    CHECK(strstr(text, "contracts: 3 passed, 0 failed, 0 signature_changed, 0 removed") != NULL);

    /* 2. Regression caught by OLD's promise/example even when NEW altered limit */
    write_file_str(new_main, "module Main\n"
                             "expose clamp, Box\n"
                             "struct Box\n  v: Int\nend\n"
                             "limit = 999\n"
                             "fn clamp(n: Int) : Int\n"
                             "  return 0 if n < 0\n"
                             "  return limit if n > limit\n"
                             "  n\n"
                             "end\n"
                             "fn main(files: Files)\n"
                             "  _sub = files.within(\"./data\")\n"
                             "end\n");
    const char *argv_json[] = {"mo", "diff", "--json", old_main, new_main};
    CHECK(run_diff_cmd(5, argv_json, &out, &err) == 1);
    const char *jtext = (const char *)out.bytes;
    CHECK(strstr(jtext, "\"command\":\"diff\"") != NULL);
    CHECK(strstr(jtext, "\"failed\":2") != NULL);
    CHECK(strstr(jtext, "\"passed\":1") != NULL);
    CHECK(strstr(jtext, "\"status\":1") != NULL);

    /* 3. Service state schema change + service promise */
    write_file_str(old_main,
                   "module Main\n"
                   "service\n"
                   "  state Int = 0\n"
                   "  tools add\n"
                   "  token from env \"TOK\"\n"
                   "  promise on add\n"
                   "    after == before + n\n"
                   "  end\n"
                   "end\n"
                   "fn add(s: Int, _t: Time, n: Int) : (Int, Int)\n  (s + n, s + n)\nend\n");
    write_file_str(new_main, "module Main\n"
                             "struct State\n  total: Int\nend\n"
                             "service\n"
                             "  state State = State(total: 0)\n"
                             "  tools add\n"
                             "  token from env \"TOK\"\n"
                             "end\n"
                             "fn add(s: State, _t: Time, n: Int) : (State, Int)\n"
                             "  (State(total: s.total + n), s.total + n)\n"
                             "end\n");
    CHECK(run_diff_cmd(5, argv_json, &out, &err) == 0);
    jtext = (const char *)out.bytes;
    CHECK(strstr(jtext, "\"migration_required\":true") != NULL);
    CHECK(strstr(jtext, "\"signature_changed\":1") != NULL);

    /* 4. Single-file Git HEAD mode */
    char repo[256], repo_main[300], cmd[1024];
    MO_ASSERT(snprintf(repo, sizeof repo, "%s/repo", dir) > 0 && mkdir(repo, 0700) == 0);
    MO_ASSERT(snprintf(repo_main, sizeof repo_main, "%s/main.mo", repo) > 0);
    write_file_str(repo_main, "module Main\n"
                              "fn double(n: Int) : Int\n"
                              "  promise\n    result == n * 2\n  end\n"
                              "  n + n\n"
                              "end\n"
                              "fn main()\nend\n");
    MO_ASSERT(snprintf(cmd, sizeof cmd,
                       "git -C '%s' init -q && git -C '%s' config user.email t@e.com && "
                       "git -C '%s' config user.name T && git -C '%s' add main.mo && "
                       "git -C '%s' commit -q -m init",
                       repo, repo, repo, repo, repo) > 0);
    MO_ASSERT(system(cmd) == 0);
    write_file_str(repo_main, "module Main\n"
                              "fn double(n: Int) : Int\n"
                              "  n * 3\n"
                              "end\n"
                              "fn main()\nend\n");
    const char *argv_head[] = {"mo", "diff", repo_main};
    CHECK(run_diff_cmd(3, argv_head, &out, &err) == 1);
    CHECK(strstr((const char *)out.bytes, "failed Main.double.promise.1") != NULL);

    /* Outside git repo with 1 file reports E1404 */
    const char *argv_nongit[] = {"mo", "diff", old_main};
    CHECK(run_diff_cmd(3, argv_nongit, &out, &err) == 1);
    CHECK(strstr((const char *)err.bytes, "E1404") != NULL);

    /* 5. Api server route, auth, cors, and capability authority diff */
    write_file_str(old_main, "module Main\n"
                             "api\n"
                             "  get \"/health\" health\n"
                             "end\n"
                             "fn health() : Int\n  1\nend\n");
    write_file_str(
        new_main,
        "module Main\n"
        "expose RateLimiter\n"
        "use Api\n"
        "struct User\n  login: Text\nend\n"
        "enum AuthErr\n  Denied\nend\n"
        "actor RateLimiter\n  state Int = 0\n  calls check_rate\nend\n"
        "fn check_rate(s: Int, _t: Time) : (Int, Bool)\n  (s + 1, true)\nend\n"
        "api\n"
        "  cors \"*\"\n"
        "  sqlite db from \"app.db\"\n"
        "  actor limiter: RateLimiter\n"
        "  auth User via check_auth\n"
        "  get \"/health\" health\n"
        "  post \"/items\" add_item\n"
        "  status Denied = 401\n"
        "end\n"
        "fn check_auth(_r: Api.Request, _env: Env) : Result(User, AuthErr)\n"
        "  Ok(User(login: \"ada\"))\n"
        "end\n"
        "fn health() : Int\n  1\nend\n"
        "fn add_item(_u: User, _db: Sqlite.Database, _limiter: RateLimiter) : Int\n  2\nend\n");
    CHECK(run_diff_cmd(4, argv_text, &out, &err) == 0);
    text = (const char *)out.bytes;
    CHECK(strstr(text, "+ capability Sqlite") != NULL);
    CHECK(strstr(text, "+ capability Env") != NULL);
    CHECK(strstr(text, "+ actor Main.RateLimiter: actor calls(check_rate)") != NULL);
    CHECK(strstr(text, "+ api_actor api.actor.limiter: Main.RateLimiter") != NULL);
    CHECK(strstr(text, "+ api_cors api.cors: *") != NULL);
    CHECK(strstr(text, "+ route POST /items: [auth Main.User] add_item") != NULL);

    mo_buffer_free(&err);
    mo_buffer_free(&out);
    MO_ASSERT(snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir) > 0 && system(cmd) == 0);
    return mo_test_report("diff");
}
