#define _POSIX_C_SOURCE 200809L
#include "test.h"
#include "tools/tools.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static void write_file_str(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    MO_ASSERT(f != NULL);
    size_t len = strlen(text);
    MO_ASSERT(fwrite(text, 1, len, f) == len && fclose(f) == 0);
}

static bool send_all(int fd, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, data + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        off += (size_t)n;
    }
    return true;
}

static int http_exchange(uint16_t port, const char *raw_req, char *resp_buf, size_t cap) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    MO_ASSERT(fd >= 0);
    struct timeval tv = {.tv_sec = 10, .tv_usec = 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    MO_ASSERT(connect(fd, (const struct sockaddr *)&addr, sizeof addr) == 0);
    MO_ASSERT(send_all(fd, raw_req, strlen(raw_req)));

    size_t total = 0;
    while (total + 1 < cap) {
        ssize_t n = recv(fd, resp_buf + total, cap - 1 - total, 0);
        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        total += (size_t)n;
    }
    resp_buf[total] = '\0';
    (void)close(fd);

    int status = 0;
    if (strncmp(resp_buf, "HTTP/1.1 ", 9) == 0)
        status = atoi(resp_buf + 9);
    return status;
}

struct client_job {
    uint16_t port;
    int worker_id;
    bool ok;
};

static void *concurrent_client(void *arg) {
    struct client_job *job = arg;
    char req[512], resp[8192], body[128], expected_field[128];
    int blen = snprintf(body, sizeof body, "{\"name\":\"item-%d\"}", job->worker_id);
    int flen =
        snprintf(expected_field, sizeof expected_field, "\"name\":\"item-%d\"", job->worker_id);
    MO_ASSERT(blen > 0 && flen > 0);
    int rlen = snprintf(req, sizeof req,
                        "POST /items HTTP/1.1\r\n"
                        "Host: 127.0.0.1:%u\r\n"
                        "Authorization: Bearer secret-key\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: %d\r\n"
                        "Connection: close\r\n\r\n%s",
                        (unsigned)job->port, blen, body);
    MO_ASSERT(rlen > 0 && (size_t)rlen < sizeof req);
    int status = http_exchange(job->port, req, resp, sizeof resp);
    if (status != 200 || strstr(resp, expected_field) == NULL) {
        job->ok = false;
        return NULL;
    }

    rlen = snprintf(req, sizeof req,
                    "GET /items?limit=20 HTTP/1.1\r\n"
                    "Host: 127.0.0.1:%u\r\n"
                    "Connection: close\r\n\r\n",
                    (unsigned)job->port);
    MO_ASSERT(rlen > 0);
    status = http_exchange(job->port, req, resp, sizeof resp);
    job->ok = (status == 200 && strstr(resp, "\"name\":\"item-") != NULL);
    return NULL;
}

int main(void) {
    char dir[] = "/tmp/mo-test-api-srv-XXXXXX";
    MO_ASSERT(mkdtemp(dir) != NULL);
    char data_dir[256], app_path[300];
    MO_ASSERT(snprintf(data_dir, sizeof data_dir, "%s/data", dir) > 0 &&
              mkdir(data_dir, 0700) == 0);
    MO_ASSERT(snprintf(app_path, sizeof app_path, "%s/main.mo", dir) > 0);

    write_file_str(
        app_path,
        "module Main\n"
        "use Api\n"
        "struct Principal\n  login: Text\nend\n"
        "struct NewItem\n  name: Text\nend\n"
        "struct Item\n  id: Int\n  name: Text\n  owner: Text\nend\n"
        "enum AppError\n  Unauthorized\n  ItemMissing(Int)\n  SqlProblem(Text)\nend\n"
        "api\n"
        "  port 8080\n"
        "  cors \"*\"\n"
        "  sqlite db from \"items.db\"\n"
        "  auth Principal via authenticate\n"
        "  get \"/health\" health\n"
        "  post \"/items\" create_item\n"
        "  get \"/items/:id\" get_item\n"
        "  get \"/items\" list_items\n"
        "  get \"/crash\" crash_route\n"
        "  status Unauthorized = 401\n"
        "  status ItemMissing(_) = 404\n"
        "  status SqlProblem(_) = 500\n"
        "end\n"
        "fn sql_ok(r: Result(T, SqliteError)) : Result(T, AppError)\n"
        "  r.map_error(fn(e) SqlProblem(\"#{e}\") end)\n"
        "end\n"
        "fn ensure_table(db: Sqlite.Database) : Result(Unit, AppError)\n"
        "  sql_ok(db.run(\"CREATE TABLE IF NOT EXISTS items(id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "name TEXT NOT NULL, owner TEXT NOT NULL)\", []))\n"
        "end\n"
        "fn authenticate(req: Api.Request, env: Env) : Result(Principal, AppError)\n"
        "  expected = env.get(\"API_TOKEN\") or \"secret-key\"\n"
        "  raw = req.header(\"Authorization\") or \"\"\n"
        "  tok = raw.drop_prefix(\"Bearer \") or \"\"\n"
        "  return Error(Unauthorized) if tok != expected\n"
        "  Ok(Principal(login: \"ada\"))\n"
        "end\n"
        "fn health() : Map(Text, Text)\n  {\"status\": \"ok\"}\nend\n"
        "fn create_item(u: Principal, db: Sqlite.Database, input: NewItem) : Result(Item, "
        "AppError)\n"
        "  try ensure_table(db)\n"
        "  try sql_ok(db.run(\"INSERT INTO items(name, owner) VALUES (?, ?)\", "
        "[Sqlite.String(input.name), Sqlite.String(u.login)]))\n"
        "  rows = try sql_ok(db.query(\"SELECT last_insert_rowid()\", []))\n"
        "  case rows.rows\n"
        "    [[Sqlite.Integer(id)]]: Ok(Item(id: id, name: input.name, owner: u.login))\n"
        "    _: Error(SqlProblem(\"no rowid\"))\n"
        "  end\n"
        "end\n"
        "fn get_item(db: Sqlite.Database, id: Int) : Result(Item, AppError)\n"
        "  try ensure_table(db)\n"
        "  rows = try sql_ok(db.query(\"SELECT id, name, owner FROM items WHERE id = ?\", "
        "[Sqlite.Integer(id)]))\n"
        "  case rows.rows\n"
        "    [[Sqlite.Integer(rid), Sqlite.String(name), Sqlite.String(owner)]]: Ok(Item(id: rid, "
        "name: name, owner: owner))\n"
        "    []: Error(ItemMissing(id))\n"
        "    _: Error(SqlProblem(\"bad row\"))\n"
        "  end\n"
        "end\n"
        "fn list_items(db: Sqlite.Database, limit: Option(Int)) : Result(List(Item), AppError)\n"
        "  try ensure_table(db)\n"
        "  rows = try sql_ok(db.query(\"SELECT id, name, owner FROM items ORDER BY id ASC LIMIT "
        "?\", [Sqlite.Integer(limit or 50)]))\n"
        "  var acc = []\n"
        "  for r in rows.rows\n"
        "    case r\n"
        "      [Sqlite.Integer(rid), Sqlite.String(name), Sqlite.String(owner)]:\n"
        "        acc = acc.append(Item(id: rid, name: name, owner: owner))\n"
        "      _: return Error(SqlProblem(\"bad row\"))\n"
        "    end\n"
        "  end\n"
        "  Ok(acc)\n"
        "end\n"
        "fn crash_route() : Int\n"
        "  stop \"handler crashed\"\n"
        "end\n");

    int out_pipe[2], err_pipe[2];
    MO_ASSERT(pipe(out_pipe) == 0 && pipe(err_pipe) == 0);
    pid_t pid = fork();
    MO_ASSERT(pid >= 0);
    if (pid == 0) {
        (void)close(out_pipe[0]);
        (void)close(err_pipe[0]);
        FILE *fout = fdopen(out_pipe[1], "w");
        FILE *ferr = fdopen(err_pipe[1], "w");
        MO_ASSERT(fout != NULL && ferr != NULL);
        const char *argv[] = {"mo", "serve", app_path, "--port", "0", "--data", data_dir};
        struct mo_cli cli;
        const char *perr = NULL;
        MO_ASSERT(mo_cli_parse(7, argv, &cli, &perr));
        int rc = mo_cli_execute(&cli, fout, ferr);
        (void)fflush(fout);
        (void)fflush(ferr);
        (void)fclose(fout);
        (void)fclose(ferr);
        _Exit(rc);
    }
    (void)close(out_pipe[1]);
    (void)close(err_pipe[1]);

    char line[256];
    size_t pos = 0;
    while (pos + 1 < sizeof line) {
        ssize_t n = read(out_pipe[0], line + pos, 1);
        if (n <= 0)
            break;
        if (line[pos++] == '\n')
            break;
    }
    line[pos] = '\0';
    unsigned port_num = 0;
    CHECK(sscanf(line, "mo serve: listening on http://127.0.0.1:%u", &port_num) == 1);
    CHECK(port_num > 0 && port_num <= 65535);
    uint16_t port = (uint16_t)port_num;

    /* 1. Parallel concurrent SQLite writes and reads across 8 threads */
    enum { NCLIENTS = 8 };
    pthread_t tids[NCLIENTS];
    struct client_job jobs[NCLIENTS];
    for (int i = 0; i < NCLIENTS; i++) {
        jobs[i] = (struct client_job){.port = port, .worker_id = i + 1, .ok = false};
        MO_ASSERT(pthread_create(&tids[i], NULL, concurrent_client, &jobs[i]) == 0);
    }
    for (int i = 0; i < NCLIENTS; i++) {
        MO_ASSERT(pthread_join(tids[i], NULL) == 0);
        CHECK(jobs[i].ok);
    }

    char resp[16384];
    /* 2. CORS preflight, OpenAPI 3.1, /docs, 401 auth, 422 strict JSON, 404 status mapping */
    CHECK(http_exchange(port,
                        "OPTIONS /items HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                        resp, sizeof resp) == 204);
    CHECK(strstr(resp, "Access-Control-Allow-Origin: *") != NULL);

    CHECK(http_exchange(
              port, "GET /openapi.json HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
              resp, sizeof resp) == 200);
    CHECK(strstr(resp, "\"openapi\":\"3.1.0\"") != NULL);
    CHECK(strstr(resp, "\"/items/{id}\"") != NULL);

    CHECK(http_exchange(port, "GET /docs HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                        resp, sizeof resp) == 200);
    CHECK(strstr(resp, "/openapi.json") != NULL);

    CHECK(http_exchange(port,
                        "POST /items HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: "
                        "12\r\nConnection: close\r\n\r\n{\"name\":\"x\"}",
                        resp, sizeof resp) == 401);

    CHECK(http_exchange(port,
                        "POST /items HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer "
                        "secret-key\r\nContent-Length: 22\r\nConnection: "
                        "close\r\n\r\n{\"name\":\"x\",\"extra\":1}",
                        resp, sizeof resp) == 422);

    CHECK(http_exchange(port,
                        "GET /items/999 HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                        resp, sizeof resp) == 404);

    /* Chunked Transfer-Encoding POST */
    CHECK(http_exchange(port,
                        "POST /items HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer "
                        "secret-key\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                        "12\r\n{\"name\":\"chunked\"}\r\n0\r\n\r\n",
                        resp, sizeof resp) == 200);
    CHECK(strstr(resp, "\"name\":\"chunked\"") != NULL);

    /* 3. Fault isolation: /crash stops its request VM with 500, while server & DB stay alive! */
    CHECK(http_exchange(port, "GET /crash HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                        resp, sizeof resp) == 500);
    CHECK(strstr(resp, "{\"error\":\"internal_server_error\"}") != NULL);

    CHECK(http_exchange(port,
                        "GET /items/1 HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                        resp, sizeof resp) == 200);
    CHECK(http_exchange(port,
                        "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                        resp, sizeof resp) == 200);

    /* 4. Graceful SIGTERM shutdown */
    MO_ASSERT(kill(pid, SIGTERM) == 0);
    char err_log[8192];
    size_t elen = 0;
    while (elen + 1 < sizeof err_log) {
        ssize_t n = read(err_pipe[0], err_log + elen, sizeof err_log - 1 - elen);
        if (n <= 0)
            break;
        elen += (size_t)n;
    }
    err_log[elen] = '\0';
    (void)close(out_pipe[0]);
    (void)close(err_pipe[0]);

    int wstatus = 0;
    MO_ASSERT(waitpid(pid, &wstatus, 0) == pid);
    CHECK(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0);
    CHECK(strstr(err_log, "mo serve: failure:") != NULL);
    CHECK(strstr(err_log, "handler crashed") != NULL);
    CHECK(strstr(err_log, "mo serve: stop: a signal asked the server to stop") != NULL);

    char cmd[512];
    MO_ASSERT(snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir) > 0 && system(cmd) == 0);
    return mo_test_report("api_server");
}
