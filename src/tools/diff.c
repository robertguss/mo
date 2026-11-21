#define _XOPEN_SOURCE 700

#include "tools/diff.h"

#include "base/codes.h"
#include "base/sha256.h"
#include "base/stack.h"
#include "bytecode/bytecode.h"
#include "check/check.h"
#include "codegen/codegen.h"
#include "lower/lower.h"
#include "tools/embedded.h"
#include "tools/offline.h"
#include "tools/tests.h"
#include "tools/upgrade.h"
#include "vm/vm.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void tool_error(struct mo_diags *diags, uint16_t code, const char *message,
                       const char *detail) {
    struct mo_diag d = {.code = code,
                        .message = message,
                        .primary = {.span = {.file = UINT32_MAX}, .text = detail}};
    bool added = mo_diags_add(diags, &d);
    if (!added)
        MO_ASSERT(mo_diags_any(diags));
}

/* Runs git with no shell and captures stdout into out. Returns true iff git exits 0. */
static bool run_git(const char *const argv[], struct mo_buffer *out) {
    int pipefd[2];
    if (pipe(pipefd) != 0)
        return false;
    pid_t pid = fork();
    if (pid < 0) {
        (void)close(pipefd[0]);
        (void)close(pipefd[1]);
        return false;
    }
    if (pid == 0) {
        (void)close(pipefd[0]);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull < 0 || dup2(devnull, STDIN_FILENO) < 0 || dup2(pipefd[1], STDOUT_FILENO) < 0 ||
            dup2(devnull, STDERR_FILENO) < 0)
            _exit(125);
        if (devnull > 2)
            (void)close(devnull);
        if (pipefd[1] > 2)
            (void)close(pipefd[1]);
        char *mutable[16];
        size_t i = 0;
        for (; i < 15 && argv[i] != NULL; i++)
            memcpy(&mutable[i], &argv[i], sizeof mutable[i]);
        mutable[i] = NULL;
        execvp("git", mutable);
        _exit(125);
    }
    (void)close(pipefd[1]);
    bool ok = true;
    if (out != NULL) {
        mo_buffer_clear(out);
        uint8_t chunk[4096];
        for (;;) {
            ssize_t n = read(pipefd[0], chunk, sizeof chunk);
            if (n == 0)
                break;
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                ok = false;
                break;
            }
            if (!mo_buffer_append(out, chunk, (size_t)n)) {
                ok = false;
                break;
            }
        }
    }
    (void)close(pipefd[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return false;
    return ok && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void remove_tree(const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (dir != NULL) {
        for (struct dirent *e; (e = readdir(dir)) != NULL;) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            char child[MO_LIMIT_PATH_SIZE + 256];
            int n = snprintf(child, sizeof child, "%s/%s", dir_path, e->d_name);
            if (n <= 0 || (size_t)n >= sizeof child)
                continue;
            struct stat st;
            if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
                remove_tree(child);
            else
                (void)unlink(child);
        }
        (void)closedir(dir);
    }
    (void)rmdir(dir_path);
}

static bool write_all_file(const char *path, const uint8_t *bytes, size_t length) {
    char copy[MO_LIMIT_PATH_SIZE + 256];
    int n = snprintf(copy, sizeof copy, "%s", path);
    if (n <= 0 || (size_t)n >= sizeof copy)
        return false;
    for (char *p = copy + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            bool dir_ok = mkdir(copy, 0700) == 0 || errno == EEXIST;
            *p = '/';
            if (!dir_ok)
                return false;
        }
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    size_t at = 0;
    while (at < length) {
        ssize_t w = write(fd, bytes + at, length - at);
        if (w > 0)
            at += (size_t)w;
        else if (w == 0 || errno != EINTR)
            break;
    }
    bool closed = close(fd) == 0;
    return at == length && closed;
}

/* Materializes Git HEAD's .mo tree into tmp_dir_out and writes the OLD entry path into
 * old_entry_out. */
static bool materialize_git_head(const char *new_path, char tmp_dir_out[64],
                                 char old_entry_out[MO_LIMIT_PATH_SIZE + 128]) {
    tmp_dir_out[0] = '\0';
    char resolved_new[PATH_MAX];
    if (realpath(new_path, resolved_new) == NULL)
        return false;
    char dir_part[PATH_MAX];
    int dn = snprintf(dir_part, sizeof dir_part, "%s", resolved_new);
    if (dn <= 0 || (size_t)dn >= sizeof dir_part)
        return false;
    char *slash = strrchr(dir_part, '/');
    if (slash == NULL)
        return false;
    if (slash == dir_part)
        slash[1] = '\0';
    else
        *slash = '\0';
    struct mo_buffer top, files, blob;
    mo_buffer_init(&top, PATH_MAX + 16);
    mo_buffer_init(&files, MO_LIMIT_BUNDLE_BYTES);
    mo_buffer_init(&blob, MO_LIMIT_FILE_SIZE);
    bool ok = false;
    const char *top_argv[] = {"git", "-C", dir_part, "rev-parse", "--show-toplevel", NULL};
    if (!run_git(top_argv, &top) || top.length == 0)
        goto done;
    if (top.bytes[top.length - 1] == '\n')
        top.length--;
    if (!mo_buffer_append_byte(&top, 0))
        goto done;
    const char *repo_root = (const char *)top.bytes;
    char resolved_root[PATH_MAX];
    if (realpath(repo_root, resolved_root) == NULL)
        goto done;
    size_t root_len = strlen(resolved_root);
    if (strncmp(resolved_new, resolved_root, root_len) != 0 || resolved_new[root_len] != '/')
        goto done;
    const char *rel_entry = resolved_new + root_len + 1;
    char head_spec[PATH_MAX + 16];
    int hn = snprintf(head_spec, sizeof head_spec, "HEAD:%s", rel_entry);
    if (hn <= 0 || (size_t)hn >= sizeof head_spec)
        goto done;
    const char *cat_argv[] = {"git", "-C", resolved_root, "cat-file", "-e", head_spec, NULL};
    if (!run_git(cat_argv, NULL))
        goto done;
    const char *ls_argv[] = {"git", "-C",          resolved_root, "ls-tree", "-r",
                             "-z",  "--name-only", "HEAD",        NULL};
    if (!run_git(ls_argv, &files))
        goto done;
    char tmpl[] = "/tmp/mo-diff-XXXXXX";
    if (mkdtemp(tmpl) == NULL)
        goto done;
    memcpy(tmp_dir_out, tmpl, sizeof tmpl);
    size_t pos = 0;
    uint32_t count = 0;
    ok = true;
    while (ok && pos < files.length) {
        const char *rel = (const char *)(files.bytes + pos);
        size_t len = strnlen(rel, files.length - pos);
        pos += len + 1;
        if (len < 4 || strcmp(rel + len - 3, ".mo") != 0 || strstr(rel, "..") != NULL ||
            rel[0] == '/')
            continue;
        if (++count > MO_LIMIT_BUNDLE_FILES) {
            ok = false;
            break;
        }
        char show_spec[PATH_MAX + 16], dest[MO_LIMIT_PATH_SIZE + 128];
        int sn = snprintf(show_spec, sizeof show_spec, "HEAD:%s", rel);
        int wn = snprintf(dest, sizeof dest, "%s/%s", tmp_dir_out, rel);
        if (sn <= 0 || (size_t)sn >= sizeof show_spec || wn <= 0 || (size_t)wn >= sizeof dest) {
            ok = false;
            break;
        }
        const char *show_argv[] = {"git", "-C", resolved_root, "show", show_spec, NULL};
        ok = run_git(show_argv, &blob) && write_all_file(dest, blob.bytes, blob.length);
    }
    if (ok) {
        int en = snprintf(old_entry_out, MO_LIMIT_PATH_SIZE + 128, "%s/%s", tmp_dir_out, rel_entry);
        ok = en > 0 && (size_t)en < MO_LIMIT_PATH_SIZE + 128;
    }
    if (!ok && tmp_dir_out[0] != '\0') {
        remove_tree(tmp_dir_out);
        tmp_dir_out[0] = '\0';
    }
done:
    mo_buffer_free(&blob);
    mo_buffer_free(&files);
    mo_buffer_free(&top);
    return ok;
}

struct side {
    struct mo_arena arena;
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_module_set *set;
    struct mo_program *program;
    uint32_t entry_service;
    bool has_schema;
    uint8_t state_schema[32];
};

static void side_init(struct side *s) {
    *s = (struct side){.entry_service = MO_NO_INDEX};
    mo_arena_init(&s->arena, MO_CLI_CHECK_SIZE);
    bool hs = mo_sources_init(&s->sources);
    bool hd = mo_diags_init(&s->diags);
    MO_ASSERT(hs && hd);
}

static void side_free(struct side *s) {
    mo_arena_free(&s->arena);
    mo_sources_free(&s->sources);
    mo_diags_free(&s->diags);
}

static bool compute_service_schema(struct side *s) {
    if (s->entry_service == MO_NO_INDEX)
        return true;
    struct mo_arena lowered;
    mo_arena_init(&lowered, MO_LIMIT_LOWER_WORK);
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    struct mo_services services;
    struct mo_core *core =
        mo_lower_services(&lowered, s->program, &s->sources, &s->diags, NULL, &services);
    bool ok = core != NULL && !mo_diags_any(&s->diags) &&
              mo_core_check_services(core, &services, &s->diags) &&
              mo_compile_services(core, &services, &wire, &s->diags);
    if (ok) {
        struct mo_load_error load_err;
        struct mo_image *img = mo_load(wire.bytes, wire.length, &load_err);
        struct mo_limits lim = mo_limits_default();
        struct mo_defect def;
        if (img != NULL && mo_image_init(img, &lim, &def) == MO_RETURNED) {
            struct mo_process *proc = mo_process_new(img, &lim);
            if (proc != NULL) {
                uint32_t st_type = mo_image_bytecode(img)->services[s->entry_service].state_type;
                s->has_schema =
                    mo_type_schema_digest(mo_process_heap(proc), st_type, s->state_schema);
                mo_process_free(proc);
            }
        } else {
            ok = false;
        }
        if (img != NULL)
            mo_image_free(img);
    }
    mo_buffer_free(&wire);
    mo_arena_free(&lowered);
    return ok && s->has_schema;
}

static bool load_side(struct side *s, const char *path, const char *library) {
    s->set =
        mo_load_program_with_library(&s->arena, &s->sources, path, library,
                                     library == NULL ? mo_default_embedded_library() : NULL,
                                     library == NULL ? mo_default_library_root() : NULL, &s->diags);
    if (s->set == NULL || mo_diags_any(&s->diags))
        return false;
    s->program = mo_check(&s->arena, s->set, &s->diags);
    if (s->program == NULL || mo_diags_any(&s->diags))
        return false;
    for (uint32_t i = 0; i < s->program->service_count; i++)
        if (s->program->services[i].module == 0 && !s->program->services[i].is_actor)
            s->entry_service = i;
    return compute_service_schema(s);
}

static bool format_type(const struct mo_program *prog, const struct mo_source_type *t,
                        struct mo_buffer *out) {
    if (t == NULL)
        return mo_buffer_append_text(out, "Unit");
    switch (t->kind) {
    case MO_SOURCE_PARAMETER:
        return mo_buffer_append_text(out, t->name);
    case MO_SOURCE_NAMED: {
        bool ok = true;
        if (t->module != UINT32_MAX && t->module < prog->set->count &&
            !prog->set->library[t->module])
            ok = mo_buffer_append_text(out, prog->set->names[t->module]) &&
                 mo_buffer_append_byte(out, '.');
        ok = ok && mo_buffer_append_text(out, t->name);
        if (ok && t->count > 0) {
            ok = mo_buffer_append_byte(out, '(');
            for (uint32_t i = 0; ok && i < t->count; i++)
                ok = (i == 0 || mo_buffer_append_text(out, ", ")) &&
                     format_type(prog, t->arguments[i], out);
            ok = ok && mo_buffer_append_byte(out, ')');
        }
        return ok;
    }
    case MO_SOURCE_TUPLE: {
        bool ok = mo_buffer_append_byte(out, '(');
        for (uint32_t i = 0; ok && i < t->count; i++)
            ok = (i == 0 || mo_buffer_append_text(out, ", ")) &&
                 format_type(prog, t->arguments[i], out);
        return ok && mo_buffer_append_byte(out, ')');
    }
    case MO_SOURCE_FUNCTION: {
        bool ok = mo_buffer_append_text(out, "fn(");
        uint32_t params = t->count > 0 ? t->count - 1 : 0;
        for (uint32_t i = 0; ok && i < params; i++)
            ok = (i == 0 || mo_buffer_append_text(out, ", ")) &&
                 format_type(prog, t->arguments[i], out);
        return ok && mo_buffer_append_text(out, ") : ") &&
               format_type(prog, t->count > 0 ? t->arguments[t->count - 1] : NULL, out);
    }
    }
    MO_UNREACHABLE();
}

static const char *copy_buf_cstr(struct mo_arena *arena, struct mo_buffer *b) {
    char *s = mo_arena_allocate(arena, b->length + 1, 1);
    MO_ASSERT(s != NULL);
    if (b->length != 0)
        memcpy(s, b->bytes, b->length);
    s[b->length] = '\0';
    return s;
}

static bool is_capability_name(const char *name) {
    static const char *const caps[] = {"Console", "Clock",   "Random",    "Environment",
                                       "Env",     "Process", "ReadFiles", "Files",
                                       "Network", "Sqlite",  "Service"};
    for (size_t i = 0; i < sizeof caps / sizeof caps[0]; i++)
        if (strcmp(name, caps[i]) == 0)
            return true;
    return false;
}

static void collect_caps_in_type(const struct mo_source_type *t, const char **list, uint32_t *count,
                                 uint32_t cap) {
    if (t == NULL)
        return;
    if (t->kind == MO_SOURCE_NAMED && t->module == UINT32_MAX && is_capability_name(t->name)) {
        for (uint32_t i = 0; i < *count; i++)
            if (strcmp(list[i], t->name) == 0)
                return;
        if (*count < cap)
            list[(*count)++] = t->name;
        return;
    }
    for (uint32_t i = 0; i < t->count; i++)
        collect_caps_in_type(t->arguments[i], list, count, cap);
}

static int cmp_cstr(const void *a, const void *b) {
    const char *const *x = a;
    const char *const *y = b;
    return strcmp(*x, *y);
}

static void collect_program_caps(const struct side *s, const char **out, uint32_t *count) {
    *count = 0;
    const struct mo_program *p = s->program;
    if (p->entry.function != UINT32_MAX) {
        for (uint32_t i = 0; i < p->entry.capability_count; i++)
            collect_caps_in_type(p->entry.capabilities[i]->type, out, count, 32);
    }
    for (uint32_t f = 0; f < p->function_count; f++) {
        const struct mo_symbol *sym = p->functions[f].signature->function;
        if (!p->set->library[sym->module] && sym->exported)
            collect_caps_in_type(p->functions[f].signature->type, out, count, 32);
    }
    for (uint32_t a = 0; a < p->api_count; a++) {
        const struct mo_checked_api *api = &p->apis[a];
        if (p->set->library[api->module])
            continue;
        for (uint32_t c = 0; c < api->capability_count; c++) {
            bool found = false;
            for (uint32_t k = 0; k < *count; k++)
                if (strcmp(out[k], api->capabilities[c]) == 0)
                    found = true;
            if (!found && *count < 32)
                out[(*count)++] = api->capabilities[c];
        }
    }
    if (*count > 1)
        qsort(out, *count, sizeof(const char *), cmp_cstr);
}

static bool format_attenuation_arg(const struct side *s, const struct mo_checked_body *body,
                                   uint32_t expr_idx, struct mo_buffer *out) {
    const struct mo_checked_expression *e = &body->items[expr_idx];
    if (e->syntax->kind == MO_S_TEXT) {
        const struct mo_source *src = &s->sources.files[e->syntax->span.file];
        return mo_buffer_append(out, src->bytes + e->syntax->span.start,
                                e->syntax->span.end - e->syntax->span.start);
    }
    if (e->syntax->kind == MO_S_LIST) {
        bool ok = mo_buffer_append_byte(out, '[');
        for (uint32_t i = 0; ok && i < e->operand_count; i++) {
            const struct mo_checked_expression *item = &body->items[e->operands[i]];
            if (item->syntax->kind != MO_S_TEXT)
                return false;
            const struct mo_source *src = &s->sources.files[item->syntax->span.file];
            ok = (i == 0 || mo_buffer_append_text(out, ", ")) &&
                 mo_buffer_append(out, src->bytes + item->syntax->span.start,
                                  item->syntax->span.end - item->syntax->span.start);
        }
        return ok && mo_buffer_append_byte(out, ']');
    }
    return false;
}

static void scan_attenuations_body(struct mo_arena *arena, const struct side *s,
                                   const struct mo_checked_body *body, const char **list,
                                   uint32_t *count, uint32_t cap) {
    struct mo_buffer buf;
    mo_buffer_init(&buf, 1024);
    for (uint32_t i = 0; i < body->count; i++) {
        const struct mo_checked_expression *e = &body->items[i];
        if (e->native == NULL)
            continue;
        bool is_within =
            strcmp(e->native, "Files.within") == 0 || strcmp(e->native, "ReadFiles.within") == 0;
        bool is_ro = strcmp(e->native, "Files.read_only") == 0;
        bool is_hosts = strcmp(e->native, "Network.allow_hosts") == 0;
        if (!is_within && !is_ro && !is_hosts)
            continue;
        mo_buffer_clear(&buf);
        bool ok = mo_buffer_append_text(&buf, e->native) && mo_buffer_append_byte(&buf, '(');
        if (ok && (is_within || is_hosts) && e->operand_count >= 2) {
            struct mo_buffer arg;
            mo_buffer_init(&arg, 512);
            if (format_attenuation_arg(s, body, e->operands[1], &arg))
                ok = mo_buffer_append(&buf, arg.bytes, arg.length);
            else
                ok = mo_buffer_append_text(&buf, "<dynamic>");
            mo_buffer_free(&arg);
        }
        ok = ok && mo_buffer_append_byte(&buf, ')');
        if (!ok)
            continue;
        const char *entry = copy_buf_cstr(arena, &buf);
        bool dup = false;
        for (uint32_t k = 0; k < *count; k++)
            if (strcmp(list[k], entry) == 0)
                dup = true;
        if (!dup && *count < cap)
            list[(*count)++] = entry;
    }
    mo_buffer_free(&buf);
}

static void collect_attenuations(struct mo_arena *arena, const struct side *s, const char **list,
                                 uint32_t *count, uint32_t cap) {
    *count = 0;
    const struct mo_program *p = s->program;
    for (uint32_t f = 0; f < p->function_count; f++)
        if (!p->set->library[p->functions[f].signature->function->module])
            scan_attenuations_body(arena, s, &p->functions[f].body, list, count, cap);
    for (uint32_t c = 0; c < p->constant_count; c++)
        if (!p->set->library[p->constants[c].symbol->module])
            scan_attenuations_body(arena, s, &p->constants[c].initializer, list, count, cap);
    if (*count > 1)
        qsort(list, *count, sizeof(const char *), cmp_cstr);
}

struct pub_item {
    const char *kind;
    const char *name;
    const char *sig;
};

static int cmp_pub(const void *a, const void *b) {
    const struct pub_item *x = a, *y = b;
    int kc = strcmp(x->kind, y->kind);
    if (kc != 0)
        return kc;
    int nc = strcmp(x->name, y->name);
    return nc != 0 ? nc : strcmp(x->sig, y->sig);
}

static bool format_function_sig(const struct mo_program *p, const struct mo_function_signature *fn,
                                struct mo_buffer *out) {
    const struct mo_source_type *ft = fn->type;
    bool ok = mo_buffer_append_byte(out, '(');
    uint32_t params = ft->count > 0 ? ft->count - 1 : 0;
    for (uint32_t i = 0; ok && i < params; i++)
        ok = (i == 0 || mo_buffer_append_text(out, ", ")) && format_type(p, ft->arguments[i], out);
    ok = ok && mo_buffer_append_text(out, ") : ") &&
         format_type(p, ft->count > 0 ? ft->arguments[ft->count - 1] : NULL, out);
    bool any_req = false;
    for (uint32_t i = 0; ok && i < 26; i++) {
        uint8_t mask = fn->requirements[i];
        if (mask == 0)
            continue;
        ok = mo_buffer_append_text(out, any_req ? ", " : " where ") &&
             mo_buffer_append_byte(out, (uint8_t)('A' + i)) && mo_buffer_append_text(out, ": ");
        any_req = true;
        static const char *const ab_names[] = {"Equal", "Hash", "Order", "Text", "Number", "Json"};
        bool first = true;
        for (uint32_t b = 0; ok && b < 6; b++)
            if ((mask & (UINT8_C(1) << b)) != 0) {
                ok = (first || mo_buffer_append_text(out, " + ")) &&
                     mo_buffer_append_text(out, ab_names[b]);
                first = false;
            }
    }
    return ok;
}

static uint32_t collect_public_surface(struct mo_arena *arena, const struct side *s,
                                       struct pub_item **out_items) {
    const struct mo_program *p = s->program;
    uint32_t cap = p->function_count + p->constant_count + p->signatures->data_count + 64;
    for (uint32_t i = 0; i < p->service_count; i++)
        cap += p->services[i].tool_count + p->services[i].timer_count + 4;
    for (uint32_t i = 0; i < p->api_count; i++)
        cap += p->apis[i].route_count + p->apis[i].sqlite_count + p->apis[i].actor_count + 4;
    struct pub_item *items = mo_arena_allocate_array(arena, cap == 0 ? 1 : cap, sizeof *items,
                                                     _Alignof(struct pub_item));
    MO_ASSERT(items != NULL);
    uint32_t n = 0;
    struct mo_buffer nb, sb;
    mo_buffer_init(&nb, 1024);
    mo_buffer_init(&sb, 4096);
    for (uint32_t m = 0; m < p->set->count; m++) {
        if (p->set->library[m])
            continue;
        const struct mo_module_scope *scope = &p->interfaces->scopes[m];
        for (uint32_t k = 0; k < scope->count; k++) {
            const struct mo_symbol *sym = &scope->symbols[k];
            if (!sym->exported || sym->declaration == NULL)
                continue;
            if (sym->declaration->kind == MO_S_ACTOR) {
                mo_buffer_clear(&nb);
                mo_buffer_clear(&sb);
                bool bok = mo_buffer_append_text(&nb, p->set->names[m]) &&
                           mo_buffer_append_byte(&nb, '.') &&
                           mo_buffer_append_text(&nb, sym->name) &&
                           mo_buffer_append_text(&sb, "actor");
                for (uint32_t si = 0; bok && si < p->service_count; si++) {
                    const struct mo_checked_service *asvc = &p->services[si];
                    if (!asvc->is_actor || asvc->actor_symbol != sym)
                        continue;
                    bok = mo_buffer_append_text(&sb, " calls(");
                    for (uint32_t ti = 0; bok && ti < asvc->tool_count; ti++) {
                        if (ti > 0)
                            bok = mo_buffer_append_text(&sb, ", ");
                        bok = bok &&
                              mo_buffer_append_text(
                                  &sb,
                                  p->functions[asvc->tools[ti].function].signature->function->name);
                    }
                    bok = bok && mo_buffer_append_byte(&sb, ')');
                }
                MO_ASSERT(bok);
                items[n++] = (struct pub_item){"actor", copy_buf_cstr(arena, &nb),
                                               copy_buf_cstr(arena, &sb)};
                continue;
            }
            if (sym->declaration->kind != MO_S_STRUCT && sym->declaration->kind != MO_S_ENUM)
                continue;
            mo_buffer_clear(&nb);
            mo_buffer_clear(&sb);
            bool bok = mo_buffer_append_text(&nb, p->set->names[m]) &&
                       mo_buffer_append_byte(&nb, '.') && mo_buffer_append_text(&nb, sym->name) &&
                       mo_buffer_append_text(&sb, sym->insides ? "open " : "opaque ") &&
                       mo_buffer_append_text(&sb, sym->declaration->kind == MO_S_STRUCT ? "struct"
                                                                                        : "enum");
            if (bok && sym->insides) {
                for (uint32_t d = 0; bok && d < p->signatures->data_count; d++) {
                    const struct mo_data_signature *ds = &p->signatures->data[d];
                    if (ds->symbol == sym || (ds->symbol->owner != UINT32_MAX &&
                                              &scope->symbols[ds->symbol->owner] == sym)) {
                        bok = mo_buffer_append_text(&sb, " ") &&
                              mo_buffer_append_text(&sb, ds->symbol->name) &&
                              mo_buffer_append_byte(&sb, '(');
                        for (uint32_t fi = 0; bok && fi < ds->count; fi++) {
                            const struct mo_field_signature *fl = ds->by_index[fi];
                            if (fi > 0)
                                bok = mo_buffer_append_text(&sb, ", ");
                            if (bok && fl->name != NULL)
                                bok = mo_buffer_append(&sb, fl->name->text,
                                                       fl->name->span.end - fl->name->span.start) &&
                                      mo_buffer_append_text(&sb, ": ");
                            bok = bok && format_type(p, fl->type, &sb);
                        }
                        bok = bok && mo_buffer_append_byte(&sb, ')');
                    }
                }
            }
            MO_ASSERT(bok);
            items[n++] =
                (struct pub_item){"type", copy_buf_cstr(arena, &nb), copy_buf_cstr(arena, &sb)};
        }
    }
    for (uint32_t f = 0; f < p->function_count; f++) {
        const struct mo_symbol *sym = p->functions[f].signature->function;
        if (p->set->library[sym->module] || !sym->exported)
            continue;
        mo_buffer_clear(&nb);
        mo_buffer_clear(&sb);
        bool bok = mo_buffer_append_text(&nb, p->set->names[sym->module]) &&
                   mo_buffer_append_byte(&nb, '.') && mo_buffer_append_text(&nb, sym->name) &&
                   format_function_sig(p, p->functions[f].signature, &sb);
        MO_ASSERT(bok);
        items[n++] =
            (struct pub_item){"function", copy_buf_cstr(arena, &nb), copy_buf_cstr(arena, &sb)};
    }
    for (uint32_t c = 0; c < p->constant_count; c++) {
        const struct mo_symbol *sym = p->constants[c].symbol;
        if (p->set->library[sym->module] || !sym->exported)
            continue;
        mo_buffer_clear(&nb);
        mo_buffer_clear(&sb);
        bool bok = mo_buffer_append_text(&nb, p->set->names[sym->module]) &&
                   mo_buffer_append_byte(&nb, '.') && mo_buffer_append_text(&nb, sym->name) &&
                   format_type(p, p->constants[c].type, &sb);
        MO_ASSERT(bok);
        items[n++] =
            (struct pub_item){"constant", copy_buf_cstr(arena, &nb), copy_buf_cstr(arena, &sb)};
    }
    if (s->entry_service != MO_NO_INDEX) {
        const struct mo_checked_service *svc = &p->services[s->entry_service];
        if (svc->token_source != NULL) {
            const struct mo_source *src = &s->sources.files[svc->token_source->span.file];
            mo_buffer_clear(&sb);
            bool bok =
                mo_buffer_append(&sb, src->bytes + svc->token_source->span.start,
                                 svc->token_source->span.end - svc->token_source->span.start);
            MO_ASSERT(bok);
            items[n++] =
                (struct pub_item){"service_token", "service.token", copy_buf_cstr(arena, &sb)};
        }
        for (uint32_t t = 0; t < svc->tool_count; t++) {
            const struct mo_checked_service_tool *tl = &svc->tools[t];
            const struct mo_checked_function *fn = &p->functions[tl->function];
            mo_buffer_clear(&nb);
            mo_buffer_clear(&sb);
            const char *rk = tl->reply == MO_SERVICE_READ_ONLY ? "read-only "
                             : tl->reply == MO_SERVICE_UPDATE  ? "update "
                                                               : "result-update ";
            bool bok = mo_buffer_append_text(&nb, "service.tool.") &&
                       mo_buffer_append_text(&nb, fn->signature->function->name) &&
                       mo_buffer_append_text(&sb, rk) && format_function_sig(p, fn->signature, &sb);
            MO_ASSERT(bok);
            items[n++] =
                (struct pub_item){"tool", copy_buf_cstr(arena, &nb), copy_buf_cstr(arena, &sb)};
        }
        for (uint32_t t = 0; t < svc->timer_count; t++) {
            const struct mo_checked_service_timer *tm = &svc->timers[t];
            const struct mo_checked_function *fn = &p->functions[tm->function];
            const struct mo_syntax *iv = tm->syntax->children[0];
            const struct mo_source *src = &s->sources.files[iv->span.file];
            mo_buffer_clear(&nb);
            mo_buffer_clear(&sb);
            bool bok =
                mo_buffer_append_text(&nb, "service.timer.") &&
                mo_buffer_append_text(&nb, fn->signature->function->name) &&
                mo_buffer_append_text(&sb, "every ") &&
                mo_buffer_append(&sb, src->bytes + iv->span.start, iv->span.end - iv->span.start);
            MO_ASSERT(bok);
            items[n++] =
                (struct pub_item){"timer", copy_buf_cstr(arena, &nb), copy_buf_cstr(arena, &sb)};
        }
    }
    for (uint32_t a = 0; a < p->api_count; a++) {
        const struct mo_checked_api *api = &p->apis[a];
        if (p->set->library[api->module])
            continue;
        if (api->cors != NULL)
            items[n++] = (struct pub_item){"api_cors", "api.cors", api->cors};
        for (uint32_t ac = 0; ac < api->actor_count; ac++) {
            mo_buffer_clear(&nb);
            mo_buffer_clear(&sb);
            bool bok = mo_buffer_append_text(&nb, "api.actor.") &&
                       mo_buffer_append_text(&nb, api->actors[ac].name) &&
                       format_type(p, api->actors[ac].type, &sb);
            MO_ASSERT(bok);
            items[n++] = (struct pub_item){"api_actor", copy_buf_cstr(arena, &nb),
                                           copy_buf_cstr(arena, &sb)};
        }
        static const char *const mnames[] = {"GET", "POST", "PUT", "PATCH", "DELETE"};
        for (uint32_t r = 0; r < api->route_count; r++) {
            const struct mo_checked_api_route *rt = &api->routes[r];
            const struct mo_checked_function *fn = &p->functions[rt->function];
            mo_buffer_clear(&nb);
            mo_buffer_clear(&sb);
            bool bok = mo_buffer_append_text(&nb, mnames[rt->method]) &&
                       mo_buffer_append_byte(&nb, ' ') && mo_buffer_append_text(&nb, rt->path);
            if (bok) {
                if (rt->authenticated && api->has_auth)
                    bok = mo_buffer_append_text(&sb, "[auth ") &&
                          format_type(p, api->auth.principal, &sb) &&
                          mo_buffer_append_text(&sb, "] ");
                else
                    bok = mo_buffer_append_text(&sb, "[public] ");
                bok = bok && mo_buffer_append_text(&sb, fn->signature->function->name) &&
                      format_function_sig(p, fn->signature, &sb);
            }
            MO_ASSERT(bok);
            items[n++] =
                (struct pub_item){"route", copy_buf_cstr(arena, &nb), copy_buf_cstr(arena, &sb)};
        }
    }
    mo_buffer_free(&nb);
    mo_buffer_free(&sb);
    if (n > 1)
        qsort(items, n, sizeof *items, cmp_pub);
    *out_items = items;
    return n;
}

enum contract_outcome {
    CONTRACT_PASSED,
    CONTRACT_FAILED,
    CONTRACT_SIGNATURE_CHANGED,
    CONTRACT_REMOVED,
};

static const char *contract_outcome_name(enum contract_outcome o) {
    switch (o) {
    case CONTRACT_PASSED:
        return "passed";
    case CONTRACT_FAILED:
        return "failed";
    case CONTRACT_SIGNATURE_CHANGED:
        return "signature_changed";
    case CONTRACT_REMOVED:
        return "removed";
    }
    MO_UNREACHABLE();
}

struct contract_item {
    const char *name;
    const char *kind;
    bool is_service;
    uint32_t old_spec_idx;
    uint32_t old_svc_promise_idx;
    uint32_t old_module;
    uint32_t new_module;
    uint32_t new_fn_idx;
    enum contract_outcome outcome;
    const char *spliced_prefix;
};

static uint32_t fn_body_start(const struct mo_syntax *fn) {
    uint32_t at = fn->span.start;
    for (uint32_t i = 0; i < 5; i++) {
        const struct mo_syntax *part = fn->children[i];
        if (part != NULL && part->kind == MO_S_SEQUENCE && i == 4)
            for (uint32_t j = 0; j < part->count; j++)
                at = part->children[j]->span.end > at ? part->children[j]->span.end : at;
        else if (part != NULL && part->span.end > at)
            at = part->span.end;
    }
    return at;
}

struct text_edit {
    uint32_t start, end;
    const char *replacement;
};

static int cmp_edit(const void *a, const void *b) {
    const struct text_edit *x = a, *y = b;
    return (x->start > y->start) - (x->start < y->start);
}

static bool expr_ident_span(const struct mo_syntax *syn, struct mo_span *out) {
    if (syn == NULL)
        return false;
    if (syn->kind == MO_S_NAME) {
        *out = syn->span;
        return true;
    }
    if (syn->kind == MO_S_FIELD_ACCESS && syn->count > 0 && syn->children[0] != NULL) {
        *out = syn->children[0]->span;
        return true;
    }
    if (syn->kind == MO_S_POSTFIX && syn->count > 0) {
        const struct mo_syntax *last = syn->children[syn->count - 1];
        if (last != NULL && last->kind == MO_S_FIELD_ACCESS && last->count > 0 &&
            last->children[0] != NULL) {
            *out = last->children[0]->span;
            return true;
        }
    }
    return false;
}

struct helper_map {
    const char **fn_alias;
    const char **const_alias;
    bool *need_fn;
    bool *need_const;
};

static void mark_helpers_in_body(const struct mo_program *old_p, const struct mo_checked_body *body,
                                 uint32_t mod, uint32_t subject_fn, struct helper_map *hm,
                                 uint32_t *wf, uint32_t *wf_n, uint32_t *wc, uint32_t *wc_n) {
    for (uint32_t i = 0; i < body->count; i++) {
        const struct mo_symbol *sym = body->items[i].symbol;
        if (sym == NULL || sym->module != mod)
            continue;
        for (uint32_t f = 0; f < old_p->function_count; f++) {
            if (old_p->functions[f].signature->function == sym) {
                if (f != subject_fn && !hm->need_fn[f]) {
                    hm->need_fn[f] = true;
                    wf[(*wf_n)++] = f;
                }
                break;
            }
        }
        for (uint32_t c = 0; c < old_p->constant_count; c++) {
            if (old_p->constants[c].symbol == sym) {
                if (!hm->need_const[c]) {
                    hm->need_const[c] = true;
                    wc[(*wc_n)++] = c;
                }
                break;
            }
        }
    }
}

static bool append_rewritten_range(struct mo_arena *arena, const struct side *old_s,
                                   const struct mo_checked_body *body, uint32_t file,
                                   uint32_t range_start, uint32_t range_end, uint32_t subject_fn,
                                   const struct helper_map *hm,
                                   const struct mo_checked_function *old_fn,
                                   const struct mo_checked_function *new_fn,
                                   struct mo_buffer *out) {
    uint32_t cap = body->count == 0 ? 1 : body->count;
    struct text_edit *edits =
        mo_arena_allocate_array(arena, cap, sizeof *edits, _Alignof(struct text_edit));
    MO_ASSERT(edits != NULL);
    uint32_t en = 0;
    const struct mo_program *old_p = old_s->program;
    for (uint32_t i = 0; i < body->count; i++) {
        const struct mo_checked_expression *e = &body->items[i];
        struct mo_span sp;
        if (!expr_ident_span(e->syntax, &sp) || sp.file != file || sp.start < range_start ||
            sp.end > range_end)
            continue;
        const char *rep = NULL;
        if (e->symbol != NULL) {
            for (uint32_t f = 0; f < old_p->function_count; f++)
                if (f != subject_fn && old_p->functions[f].signature->function == e->symbol &&
                    hm->need_fn[f])
                    rep = hm->fn_alias[f];
            for (uint32_t c = 0; rep == NULL && c < old_p->constant_count; c++)
                if (old_p->constants[c].symbol == e->symbol && hm->need_const[c])
                    rep = hm->const_alias[c];
        } else if (e->binding != NULL && old_fn != NULL && new_fn != NULL) {
            for (uint32_t p = 0; p < old_fn->parameter_count && p < new_fn->parameter_count; p++)
                if (e->binding == old_fn->parameters[p] &&
                    strcmp(old_fn->parameters[p]->name, new_fn->parameters[p]->name) != 0)
                    rep = new_fn->parameters[p]->name;
        }
        if (rep != NULL)
            edits[en++] = (struct text_edit){sp.start, sp.end, rep};
    }
    if (en > 1)
        qsort(edits, en, sizeof *edits, cmp_edit);
    const uint8_t *bytes = old_s->sources.files[file].bytes;
    uint32_t at = range_start;
    bool ok = true;
    for (uint32_t i = 0; ok && i < en; i++) {
        if (edits[i].start < at)
            continue;
        ok = mo_buffer_append(out, bytes + at, edits[i].start - at) &&
             mo_buffer_append_text(out, edits[i].replacement);
        at = edits[i].end;
    }
    return ok && mo_buffer_append(out, bytes + at, range_end - at);
}

struct file_insert {
    uint32_t offset;
    const uint8_t *bytes;
    size_t length;
};

static int cmp_insert(const void *a, const void *b) {
    const struct file_insert *x = a, *y = b;
    return (x->offset > y->offset) - (x->offset < y->offset);
}

/* Attempts to synthesize NEW + the active subset of OLD contracts, check it, and run only those
 * contracts via mo_run_gated. Returns false if synthesis or mo_check fails. */
static bool try_run_contracts(const struct mo_cli *cli, const struct side *old_s,
                              const struct side *new_s, struct contract_item *items,
                              uint32_t item_count, const bool *active, FILE *quiet) {
    struct mo_arena arena;
    mo_arena_init(&arena, MO_CLI_CHECK_SIZE);
    const struct mo_program *old_p = old_s->program;
    const struct mo_program *new_p = new_s->program;
    uint32_t fn_cap = old_p->function_count == 0 ? 1 : old_p->function_count;
    uint32_t c_cap = old_p->constant_count == 0 ? 1 : old_p->constant_count;
    struct helper_map hm = {
        .fn_alias = mo_arena_allocate_array(&arena, fn_cap, sizeof(char *), _Alignof(char *)),
        .const_alias = mo_arena_allocate_array(&arena, c_cap, sizeof(char *), _Alignof(char *)),
        .need_fn = mo_arena_allocate_array(&arena, fn_cap, sizeof(bool), _Alignof(bool)),
        .need_const = mo_arena_allocate_array(&arena, c_cap, sizeof(bool), _Alignof(bool)),
    };
    uint32_t *wf = mo_arena_allocate_array(&arena, fn_cap, sizeof(uint32_t), _Alignof(uint32_t));
    uint32_t *wc = mo_arena_allocate_array(&arena, c_cap, sizeof(uint32_t), _Alignof(uint32_t));
    MO_ASSERT(hm.fn_alias != NULL && hm.const_alias != NULL && hm.need_fn != NULL &&
              hm.need_const != NULL && wf != NULL && wc != NULL);
    memset(hm.need_fn, 0, fn_cap * sizeof(bool));
    memset(hm.need_const, 0, c_cap * sizeof(bool));
    for (uint32_t k = 0; k < item_count; k++) {
        if (!active[k])
            continue;
        uint32_t wf_n = 0, wc_n = 0;
        const struct mo_checked_spec *sp =
            items[k].is_service
                ? &old_p->services[old_s->entry_service].promises[items[k].old_svc_promise_idx].spec
                : &old_p->specs[items[k].old_spec_idx];
        uint32_t subj = items[k].is_service ? UINT32_MAX : sp->subject;
        mark_helpers_in_body(old_p, &sp->body, items[k].old_module, subj, &hm, wf, &wf_n, wc,
                             &wc_n);
        uint32_t fi = 0, ci = 0;
        while (fi < wf_n || ci < wc_n) {
            while (fi < wf_n) {
                uint32_t f = wf[fi++];
                mark_helpers_in_body(old_p, &old_p->functions[f].body, items[k].old_module, subj,
                                     &hm, wf, &wf_n, wc, &wc_n);
            }
            while (ci < wc_n) {
                uint32_t c = wc[ci++];
                mark_helpers_in_body(old_p, &old_p->constants[c].initializer, items[k].old_module,
                                     subj, &hm, wf, &wf_n, wc, &wc_n);
            }
        }
    }
    for (uint32_t f = 0; f < old_p->function_count; f++)
        if (hm.need_fn[f]) {
            const char *orig = old_p->functions[f].signature->function->name;
            size_t olen = strlen(orig);
            bool qmark = olen > 0 && orig[olen - 1] == '?';
            char tmp[48];
            int n = snprintf(tmp, sizeof tmp, qmark ? "mo_diff_old_fn_%u?" : "mo_diff_old_fn_%u",
                             (unsigned)f);
            MO_ASSERT(n > 0);
            hm.fn_alias[f] = mo_arena_copy(&arena, tmp, (size_t)n + 1, 1);
        }
    for (uint32_t c = 0; c < old_p->constant_count; c++)
        if (hm.need_const[c]) {
            char tmp[48];
            int n = snprintf(tmp, sizeof tmp, "mo_diff_old_const_%u", (unsigned)c);
            MO_ASSERT(n > 0);
            hm.const_alias[c] = mo_arena_copy(&arena, tmp, (size_t)n + 1, 1);
        }

    uint32_t *added_fn_promises =
        mo_arena_allocate_array(&arena, new_p->function_count == 0 ? 1 : new_p->function_count,
                                sizeof(uint32_t), _Alignof(uint32_t));
    uint32_t *added_fn_examples =
        mo_arena_allocate_array(&arena, new_p->function_count == 0 ? 1 : new_p->function_count,
                                sizeof(uint32_t), _Alignof(uint32_t));
    memset(added_fn_promises, 0,
           (new_p->function_count == 0 ? 1 : new_p->function_count) * sizeof(uint32_t));
    memset(added_fn_examples, 0,
           (new_p->function_count == 0 ? 1 : new_p->function_count) * sizeof(uint32_t));
    uint32_t added_svc_promises = 0;

    struct mo_sources synth_sources;
    struct mo_diags synth_diags;
    bool ok = mo_sources_init(&synth_sources) && mo_diags_init(&synth_diags);
    struct mo_buffer snippet, file_buf, name_buf;
    mo_buffer_init(&snippet, MO_LIMIT_FILE_SIZE);
    mo_buffer_init(&file_buf, MO_LIMIT_FILE_SIZE);
    mo_buffer_init(&name_buf, 512);

    for (uint32_t m = 0; ok && m < new_p->set->count; m++) {
        if (new_p->set->library[m])
            continue;
        uint32_t new_file = new_p->set->modules[m]->span.file;
        const struct mo_source *nsrc = &new_s->sources.files[new_file];
        struct file_insert *ins = mo_arena_allocate_array(
            &arena, item_count + fn_cap + c_cap + 4, sizeof *ins, _Alignof(struct file_insert));
        uint32_t ins_n = 0;

        for (uint32_t k = 0; ok && k < item_count; k++) {
            if (!active[k] || items[k].new_module != m)
                continue;
            mo_buffer_clear(&snippet);
            mo_buffer_clear(&name_buf);
            if (items[k].is_service) {
                const struct mo_checked_service *old_svc = &old_p->services[old_s->entry_service];
                const struct mo_checked_service *new_svc = &new_p->services[new_s->entry_service];
                const struct mo_checked_service_promise *sp =
                    &old_svc->promises[items[k].old_svc_promise_idx];
                ok = mo_buffer_append_text(&snippet, "\n  promise \"contract\"");
                if (ok && sp->tool != UINT32_MAX) {
                    const char *tname = old_p->functions[old_svc->tools[sp->tool].function]
                                            .signature->function->name;
                    ok = mo_buffer_append_text(&snippet, " on ") &&
                         mo_buffer_append_text(&snippet, tname);
                }
                ok = ok && mo_buffer_append_byte(&snippet, '\n');
                for (uint32_t c = 0; ok && c < sp->spec.condition_count; c++) {
                    struct mo_span cs = sp->spec.conditions[c].text.span;
                    ok = mo_buffer_append_text(&snippet, "    ") &&
                         append_rewritten_range(&arena, old_s, &sp->spec.body, cs.file, cs.start,
                                                cs.end, UINT32_MAX, &hm, NULL, NULL, &snippet) &&
                         mo_buffer_append_byte(&snippet, '\n');
                }
                ok = ok && mo_buffer_append_text(&snippet, "  end\n");
                uint32_t ord = new_svc->promise_count + (++added_svc_promises);
                ok = ok && mo_buffer_append_text(&name_buf, new_p->set->names[m]) &&
                     mo_buffer_append_text(&name_buf, ".service.promise.") &&
                     mo_buffer_append_uint(&name_buf, ord);
                items[k].spliced_prefix = copy_buf_cstr(&arena, &name_buf);
                uint8_t *copy = mo_arena_copy(&arena, snippet.bytes, snippet.length, 1);
                ins[ins_n++] =
                    (struct file_insert){new_svc->syntax->span.end - 3, copy, snippet.length};
            } else {
                const struct mo_checked_spec *sp = &old_p->specs[items[k].old_spec_idx];
                const struct mo_checked_function *old_fn = &old_p->functions[sp->subject];
                const struct mo_checked_function *new_fn = &new_p->functions[items[k].new_fn_idx];
                uint32_t insert_at = fn_body_start(new_fn->signature->function->declaration);
                uint32_t existing = 0;
                for (uint32_t si = 0; si < new_p->spec_count; si++)
                    if (new_p->specs[si].subject == items[k].new_fn_idx &&
                        new_p->specs[si].kind == sp->kind)
                        existing++;
                if (sp->kind == MO_SPEC_PROMISE) {
                    ok = mo_buffer_append_text(&snippet, "\n  promise \"contract\"\n");
                    for (uint32_t c = 0; ok && c < sp->condition_count; c++) {
                        struct mo_span cs = sp->conditions[c].text.span;
                        ok = mo_buffer_append_text(&snippet, "    ") &&
                             append_rewritten_range(&arena, old_s, &sp->body, cs.file, cs.start,
                                                    cs.end, sp->subject, &hm, old_fn, new_fn,
                                                    &snippet) &&
                             mo_buffer_append_byte(&snippet, '\n');
                    }
                    ok = ok && mo_buffer_append_text(&snippet, "  end\n");
                    uint32_t ord = existing + (++added_fn_promises[items[k].new_fn_idx]);
                    ok = ok && mo_append_function_name(&name_buf, new_p, items[k].new_fn_idx) &&
                         mo_buffer_append_text(&name_buf, ".promise.") &&
                         mo_buffer_append_uint(&name_buf, ord);
                } else {
                    struct mo_span es = sp->text.span;
                    ok = mo_buffer_append_text(&snippet, "\n  examples\n    ") &&
                         append_rewritten_range(&arena, old_s, &sp->body, es.file, es.start, es.end,
                                                sp->subject, &hm, old_fn, new_fn, &snippet) &&
                         mo_buffer_append_text(&snippet, "\n  end\n");
                    uint32_t ord = existing + (++added_fn_examples[items[k].new_fn_idx]);
                    ok = ok && mo_append_function_name(&name_buf, new_p, items[k].new_fn_idx) &&
                         mo_buffer_append_text(&name_buf, ".example.") &&
                         mo_buffer_append_uint(&name_buf, ord);
                }
                items[k].spliced_prefix = copy_buf_cstr(&arena, &name_buf);
                uint8_t *copy = mo_arena_copy(&arena, snippet.bytes, snippet.length, 1);
                ins[ins_n++] = (struct file_insert){insert_at, copy, snippet.length};
            }
        }

        /* Append needed OLD helpers belonging to the matching OLD module. */
        uint32_t old_m = UINT32_MAX;
        for (uint32_t om = 0; om < old_p->set->count; om++)
            if (!old_p->set->library[om] &&
                strcmp(old_p->set->names[om], new_p->set->names[m]) == 0)
                old_m = om;
        if (ok && old_m != UINT32_MAX) {
            for (uint32_t c = 0; ok && c < old_p->constant_count; c++) {
                if (!hm.need_const[c] || old_p->constants[c].symbol->module != old_m)
                    continue;
                const struct mo_syntax *decl = old_p->constants[c].symbol->declaration;
                const struct mo_syntax *rhs = decl->children[1];
                mo_buffer_clear(&snippet);
                ok = mo_buffer_append_byte(&snippet, '\n') &&
                     mo_buffer_append_text(&snippet, hm.const_alias[c]) &&
                     mo_buffer_append_text(&snippet, " = ") &&
                     append_rewritten_range(&arena, old_s, &old_p->constants[c].initializer,
                                            rhs->span.file, rhs->span.start, rhs->span.end,
                                            UINT32_MAX, &hm, NULL, NULL, &snippet) &&
                     mo_buffer_append_byte(&snippet, '\n');
                uint8_t *copy = mo_arena_copy(&arena, snippet.bytes, snippet.length, 1);
                ins[ins_n++] = (struct file_insert){(uint32_t)nsrc->length, copy, snippet.length};
            }
            for (uint32_t f = 0; ok && f < old_p->function_count; f++) {
                if (!hm.need_fn[f] || old_p->functions[f].signature->function->module != old_m)
                    continue;
                const struct mo_syntax *decl = old_p->functions[f].signature->function->declaration;
                const struct mo_syntax *name_syn = decl->children[0];
                const struct mo_source *osrc = &old_s->sources.files[decl->span.file];
                uint32_t bstart = fn_body_start(decl);
                uint32_t header_end = decl->children[4] != NULL && decl->children[4]->count > 0
                                          ? decl->children[4]->children[0]->span.start
                                          : bstart;
                mo_buffer_clear(&snippet);
                ok = mo_buffer_append_text(&snippet, "\nfn ") &&
                     mo_buffer_append_text(&snippet, hm.fn_alias[f]) &&
                     mo_buffer_append(&snippet, osrc->bytes + name_syn->span.end,
                                      header_end - name_syn->span.end) &&
                     mo_buffer_append_byte(&snippet, '\n') &&
                     append_rewritten_range(&arena, old_s, &old_p->functions[f].body,
                                            decl->span.file, bstart, decl->span.end, UINT32_MAX,
                                            &hm, NULL, NULL, &snippet) &&
                     mo_buffer_append_byte(&snippet, '\n');
                uint8_t *copy = mo_arena_copy(&arena, snippet.bytes, snippet.length, 1);
                ins[ins_n++] = (struct file_insert){(uint32_t)nsrc->length, copy, snippet.length};
            }
        }

        if (ins_n > 1)
            qsort(ins, ins_n, sizeof *ins, cmp_insert);
        mo_buffer_clear(&file_buf);
        uint32_t at = 0;
        for (uint32_t i = 0; ok && i < ins_n; i++) {
            ok = mo_buffer_append(&file_buf, nsrc->bytes + at, ins[i].offset - at) &&
                 mo_buffer_append(&file_buf, ins[i].bytes, ins[i].length);
            at = ins[i].offset;
        }
        ok = ok && mo_buffer_append(&file_buf, nsrc->bytes + at, nsrc->length - at);
        uint32_t added_idx = 0;
        struct mo_source_error serr;
        ok = ok && mo_sources_add_bytes(&synth_sources, nsrc->path, file_buf.bytes, file_buf.length,
                                        &added_idx, &serr);
    }

    bool ran_ok = false;
    if (ok) {
        struct mo_module_set *set = mo_load_bundle_with_library(
            &arena, &synth_sources, cli->library,
            cli->library == NULL ? mo_default_embedded_library() : NULL, &synth_diags);
        struct mo_program *prog =
            set == NULL || mo_diags_any(&synth_diags) ? NULL : mo_check(&arena, set, &synth_diags);
        if (prog != NULL && !mo_diags_any(&synth_diags)) {
            struct mo_cli run_cli = {.command = MO_COMMAND_TEST,
                                     .limits = mo_limits_default(),
                                     .calls = 200,
                                     .sequences = 200,
                                     .seconds = 2,
                                     .file_separator = UINT32_MAX};
            struct mo_test_selection *sel = mo_select_tests(&arena, prog, &run_cli, &synth_diags);
            if (sel != NULL && sel->mistake == NULL && !mo_diags_any(&synth_diags)) {
                uint32_t kept = 0;
                for (uint32_t i = 0; i < sel->count; i++) {
                    bool match = false;
                    for (uint32_t k = 0; !match && k < item_count; k++)
                        if (active[k] && strcmp(sel->items[i].name, items[k].spliced_prefix) == 0)
                            match = true;
                    if (match)
                        sel->items[kept++] = sel->items[i];
                }
                sel->count = kept;
                struct mo_test_totals totals = {0};
                struct mo_gated_run gated = {.start_limit = UINT64_MAX, .shrink = true};
                mo_run_gated(&run_cli, &arena, prog, &synth_sources, sel, quiet, &synth_diags,
                             &totals, &gated);
                for (uint32_t k = 0; k < item_count; k++) {
                    if (!active[k])
                        continue;
                    bool saw_any = false, all_passed = true;
                    size_t plen = strlen(items[k].spliced_prefix);
                    for (uint32_t g = 0; g < gated.count; g++) {
                        const char *gn = gated.checks[g].name;
                        const char *colons = strstr(gn, "::");
                        const char *tail = colons != NULL ? colons + 2 : gn;
                        bool same = strcmp(gn, items[k].spliced_prefix) == 0;
                        if (!same && colons != NULL) {
                            const char *dot = strstr(items[k].spliced_prefix, ".promise.");
                            if (dot != NULL) {
                                size_t fn_len = (size_t)(dot - items[k].spliced_prefix);
                                same = strncmp(tail, items[k].spliced_prefix, fn_len) == 0 &&
                                       tail[fn_len] == '(' && strstr(tail + fn_len, dot) != NULL;
                            }
                        }
                        (void)plen;
                        if (same) {
                            saw_any = true;
                            if (gated.checks[g].outcome != MO_CHECK_PASSED)
                                all_passed = false;
                        }
                    }
                    items[k].outcome = saw_any && all_passed ? CONTRACT_PASSED : CONTRACT_FAILED;
                }
                ran_ok = true;
            }
        }
    }

    mo_buffer_free(&name_buf);
    mo_buffer_free(&file_buf);
    mo_buffer_free(&snippet);
    mo_diags_free(&synth_diags);
    mo_sources_free(&synth_sources);
    mo_arena_free(&arena);
    return ran_ok;
}

static uint32_t evaluate_contracts(struct mo_arena *arena, const struct mo_cli *cli,
                                   const struct side *old_s, const struct side *new_s,
                                   struct contract_item **out_items) {
    const struct mo_program *old_p = old_s->program;
    const struct mo_program *new_p = new_s->program;
    uint32_t total = 0;
    for (uint32_t i = 0; i < old_p->spec_count; i++) {
        const struct mo_checked_spec *sp = &old_p->specs[i];
        if (!old_p->set->library[sp->module] && sp->subject != UINT32_MAX &&
            (sp->kind == MO_SPEC_PROMISE || sp->kind == MO_SPEC_EXAMPLE))
            total++;
    }
    if (old_s->entry_service != MO_NO_INDEX)
        total += old_p->services[old_s->entry_service].promise_count;

    struct contract_item *items = mo_arena_allocate_array(
        arena, total == 0 ? 1 : total, sizeof *items, _Alignof(struct contract_item));
    bool *active =
        mo_arena_allocate_array(arena, total == 0 ? 1 : total, sizeof(bool), _Alignof(bool));
    MO_ASSERT(items != NULL && active != NULL);
    memset(active, 0, (total == 0 ? 1 : total) * sizeof(bool));

    struct mo_buffer nb, osig, nsig;
    mo_buffer_init(&nb, 512);
    mo_buffer_init(&osig, 2048);
    mo_buffer_init(&nsig, 2048);

    uint32_t n = 0;
    uint32_t *fn_prom_ord =
        mo_arena_allocate_array(arena, old_p->function_count == 0 ? 1 : old_p->function_count,
                                sizeof(uint32_t), _Alignof(uint32_t));
    uint32_t *fn_ex_ord =
        mo_arena_allocate_array(arena, old_p->function_count == 0 ? 1 : old_p->function_count,
                                sizeof(uint32_t), _Alignof(uint32_t));
    memset(fn_prom_ord, 0,
           (old_p->function_count == 0 ? 1 : old_p->function_count) * sizeof(uint32_t));
    memset(fn_ex_ord, 0,
           (old_p->function_count == 0 ? 1 : old_p->function_count) * sizeof(uint32_t));

    for (uint32_t i = 0; i < old_p->spec_count; i++) {
        const struct mo_checked_spec *sp = &old_p->specs[i];
        if (old_p->set->library[sp->module] || sp->subject == UINT32_MAX ||
            (sp->kind != MO_SPEC_PROMISE && sp->kind != MO_SPEC_EXAMPLE))
            continue;
        uint32_t ord =
            sp->kind == MO_SPEC_PROMISE ? ++fn_prom_ord[sp->subject] : ++fn_ex_ord[sp->subject];
        mo_buffer_clear(&nb);
        bool bok =
            mo_append_function_name(&nb, old_p, sp->subject) &&
            mo_buffer_append_text(&nb, sp->kind == MO_SPEC_PROMISE ? ".promise." : ".example.") &&
            mo_buffer_append_uint(&nb, ord);
        MO_ASSERT(bok);
        const char *cname = copy_buf_cstr(arena, &nb);
        const struct mo_checked_function *old_fn = &old_p->functions[sp->subject];
        const char *mod_name = old_p->set->names[sp->module];
        uint32_t new_mod = UINT32_MAX;
        for (uint32_t m = 0; m < new_p->set->count; m++)
            if (!new_p->set->library[m] && strcmp(new_p->set->names[m], mod_name) == 0)
                new_mod = m;
        enum contract_outcome outc = CONTRACT_REMOVED;
        uint32_t matched_fn = UINT32_MAX;
        if (new_mod != UINT32_MAX) {
            mo_buffer_clear(&osig);
            (void)format_function_sig(old_p, old_fn->signature, &osig);
            bool any_same_name = false;
            for (uint32_t nf = 0; nf < new_p->function_count; nf++) {
                const struct mo_symbol *nsym = new_p->functions[nf].signature->function;
                if (nsym->module != new_mod ||
                    strcmp(nsym->name, old_fn->signature->function->name) != 0)
                    continue;
                any_same_name = true;
                mo_buffer_clear(&nsig);
                (void)format_function_sig(new_p, new_p->functions[nf].signature, &nsig);
                if (osig.length == nsig.length && memcmp(osig.bytes, nsig.bytes, osig.length) == 0)
                    matched_fn = nf;
            }
            if (matched_fn != UINT32_MAX)
                outc = CONTRACT_PASSED;
            else if (any_same_name)
                outc = CONTRACT_SIGNATURE_CHANGED;
        }
        items[n] = (struct contract_item){
            .name = cname,
            .kind = sp->kind == MO_SPEC_PROMISE ? "promise" : "example",
            .is_service = false,
            .old_spec_idx = i,
            .old_module = sp->module,
            .new_module = new_mod,
            .new_fn_idx = matched_fn,
            .outcome = outc,
        };
        active[n] = matched_fn != UINT32_MAX;
        n++;
    }

    if (old_s->entry_service != MO_NO_INDEX) {
        const struct mo_checked_service *old_svc = &old_p->services[old_s->entry_service];
        for (uint32_t j = 0; j < old_svc->promise_count; j++) {
            const struct mo_checked_service_promise *sp = &old_svc->promises[j];
            mo_buffer_clear(&nb);
            bool bok = mo_buffer_append_text(&nb, old_p->set->names[old_svc->module]) &&
                       mo_buffer_append_text(&nb, ".service.promise.") &&
                       mo_buffer_append_uint(&nb, j + 1);
            MO_ASSERT(bok);
            const char *cname = copy_buf_cstr(arena, &nb);
            enum contract_outcome outc = CONTRACT_REMOVED;
            if (new_s->entry_service != MO_NO_INDEX) {
                const struct mo_checked_service *new_svc = &new_p->services[new_s->entry_service];
                bool state_same = old_s->has_schema && new_s->has_schema &&
                                  memcmp(old_s->state_schema, new_s->state_schema, 32) == 0;
                if (sp->tool == UINT32_MAX) {
                    outc = state_same ? CONTRACT_PASSED : CONTRACT_SIGNATURE_CHANGED;
                } else {
                    const char *tname = old_p->functions[old_svc->tools[sp->tool].function]
                                            .signature->function->name;
                    uint32_t new_t = UINT32_MAX;
                    for (uint32_t t = 0; t < new_svc->tool_count; t++)
                        if (strcmp(new_p->functions[new_svc->tools[t].function]
                                       .signature->function->name,
                                   tname) == 0)
                            new_t = t;
                    if (new_t == UINT32_MAX) {
                        outc = CONTRACT_REMOVED;
                    } else if (!state_same ||
                               old_svc->tools[sp->tool].reply != new_svc->tools[new_t].reply) {
                        outc = CONTRACT_SIGNATURE_CHANGED;
                    } else {
                        const struct mo_checked_function *ofn =
                            &old_p->functions[old_svc->tools[sp->tool].function];
                        const struct mo_checked_function *nfn =
                            &new_p->functions[new_svc->tools[new_t].function];
                        mo_buffer_clear(&osig);
                        mo_buffer_clear(&nsig);
                        (void)format_function_sig(old_p, ofn->signature, &osig);
                        (void)format_function_sig(new_p, nfn->signature, &nsig);
                        bool params_same = ofn->parameter_count == nfn->parameter_count &&
                                           osig.length == nsig.length &&
                                           memcmp(osig.bytes, nsig.bytes, osig.length) == 0;
                        for (uint32_t p = 2; params_same && p < ofn->parameter_count; p++)
                            params_same =
                                strcmp(ofn->parameters[p]->name, nfn->parameters[p]->name) == 0;
                        outc = params_same ? CONTRACT_PASSED : CONTRACT_SIGNATURE_CHANGED;
                    }
                }
            }
            items[n] = (struct contract_item){
                .name = cname,
                .kind = "service_promise",
                .is_service = true,
                .old_svc_promise_idx = j,
                .old_module = 0,
                .new_module = 0,
                .outcome = outc,
            };
            active[n] = outc == CONTRACT_PASSED;
            n++;
        }
    }

    mo_buffer_free(&nsig);
    mo_buffer_free(&osig);
    mo_buffer_free(&nb);

    bool any_active = false;
    for (uint32_t k = 0; k < n; k++)
        any_active = any_active || active[k];
    if (any_active) {
        FILE *quiet = fopen("/dev/null", "w");
        MO_ASSERT(quiet != NULL);
        if (!try_run_contracts(cli, old_s, new_s, items, n, active, quiet)) {
            bool *single = mo_arena_allocate_array(arena, n, sizeof(bool), _Alignof(bool));
            MO_ASSERT(single != NULL);
            for (uint32_t k = 0; k < n; k++) {
                if (!active[k])
                    continue;
                memset(single, 0, n * sizeof(bool));
                single[k] = true;
                if (!try_run_contracts(cli, old_s, new_s, items, n, single, quiet))
                    items[k].outcome = CONTRACT_SIGNATURE_CHANGED;
            }
        }
        (void)fclose(quiet);
    }

    *out_items = items;
    return n;
}

static bool json_str_array(struct mo_buffer *out, const char *const *items, uint32_t count) {
    bool ok = mo_buffer_append_byte(out, '[');
    for (uint32_t i = 0; ok && i < count; i++)
        ok = (i == 0 || mo_buffer_append_byte(out, ',')) &&
             mo_render_json_text(out, items[i], strlen(items[i]));
    return ok && mo_buffer_append_byte(out, ']');
}

struct diff_work {
    const struct mo_cli *cli;
    const char *old_path;
    const char *new_path;
    FILE *output;
    FILE *errors;
    int status;
};

static void run_diff_work(void *context) {
    struct diff_work *w = context;
    const struct mo_cli *cli = w->cli;
    struct side old_s, new_s;
    side_init(&old_s);
    side_init(&new_s);

    if (!load_side(&old_s, w->old_path, cli->library)) {
        if (cli->json && !mo_write_version(w->output, MO_COMMAND_DIFF))
            w->status = 1;
        else
            w->status = mo_write_diagnostics(w->output, w->errors, cli->json, MO_COMMAND_DIFF,
                                             &old_s.sources, &old_s.diags, NULL);
        if (w->status == 0)
            w->status = 1;
        goto cleanup;
    }
    if (!load_side(&new_s, w->new_path, cli->library)) {
        if (cli->json && !mo_write_version(w->output, MO_COMMAND_DIFF))
            w->status = 1;
        else
            w->status = mo_write_diagnostics(w->output, w->errors, cli->json, MO_COMMAND_DIFF,
                                             &new_s.sources, &new_s.diags, NULL);
        if (w->status == 0)
            w->status = 1;
        goto cleanup;
    }

    struct mo_arena diff_arena;
    mo_arena_init(&diff_arena, MO_CLI_CHECK_SIZE);

    /* 1. Authority delta */
    const char *old_caps[32], *new_caps[32], *added_caps[32], *removed_caps[32];
    uint32_t old_cap_n = 0, new_cap_n = 0, add_cap_n = 0, rem_cap_n = 0;
    collect_program_caps(&old_s, old_caps, &old_cap_n);
    collect_program_caps(&new_s, new_caps, &new_cap_n);
    for (uint32_t i = 0; i < new_cap_n; i++) {
        bool found = false;
        for (uint32_t j = 0; j < old_cap_n; j++)
            found = found || strcmp(new_caps[i], old_caps[j]) == 0;
        if (!found)
            added_caps[add_cap_n++] = new_caps[i];
    }
    for (uint32_t i = 0; i < old_cap_n; i++) {
        bool found = false;
        for (uint32_t j = 0; j < new_cap_n; j++)
            found = found || strcmp(old_caps[i], new_caps[j]) == 0;
        if (!found)
            removed_caps[rem_cap_n++] = old_caps[i];
    }

    const char *old_att[128], *new_att[128], *added_att[128], *removed_att[128];
    uint32_t old_att_n = 0, new_att_n = 0, add_att_n = 0, rem_att_n = 0;
    collect_attenuations(&diff_arena, &old_s, old_att, &old_att_n, 128);
    collect_attenuations(&diff_arena, &new_s, new_att, &new_att_n, 128);
    for (uint32_t i = 0; i < new_att_n; i++) {
        bool found = false;
        for (uint32_t j = 0; j < old_att_n; j++)
            found = found || strcmp(new_att[i], old_att[j]) == 0;
        if (!found)
            added_att[add_att_n++] = new_att[i];
    }
    for (uint32_t i = 0; i < old_att_n; i++) {
        bool found = false;
        for (uint32_t j = 0; j < new_att_n; j++)
            found = found || strcmp(old_att[i], new_att[j]) == 0;
        if (!found)
            removed_att[rem_att_n++] = old_att[i];
    }

    /* 2. Public & wire delta */
    struct pub_item *old_pub = NULL, *new_pub = NULL;
    uint32_t old_pub_n = collect_public_surface(&diff_arena, &old_s, &old_pub);
    uint32_t new_pub_n = collect_public_surface(&diff_arena, &new_s, &new_pub);
    bool *old_pub_matched = mo_arena_allocate_array(&diff_arena, old_pub_n == 0 ? 1 : old_pub_n,
                                                    sizeof(bool), _Alignof(bool));
    bool *new_pub_matched = mo_arena_allocate_array(&diff_arena, new_pub_n == 0 ? 1 : new_pub_n,
                                                    sizeof(bool), _Alignof(bool));
    memset(old_pub_matched, 0, (old_pub_n == 0 ? 1 : old_pub_n) * sizeof(bool));
    memset(new_pub_matched, 0, (new_pub_n == 0 ? 1 : new_pub_n) * sizeof(bool));
    for (uint32_t i = 0; i < old_pub_n; i++)
        for (uint32_t j = 0; j < new_pub_n; j++)
            if (!new_pub_matched[j] && strcmp(old_pub[i].kind, new_pub[j].kind) == 0 &&
                strcmp(old_pub[i].name, new_pub[j].name) == 0 &&
                strcmp(old_pub[i].sig, new_pub[j].sig) == 0) {
                old_pub_matched[i] = new_pub_matched[j] = true;
                break;
            }
    uint32_t *chg_old = mo_arena_allocate_array(&diff_arena, old_pub_n == 0 ? 1 : old_pub_n,
                                                sizeof(uint32_t), _Alignof(uint32_t));
    uint32_t *chg_new = mo_arena_allocate_array(&diff_arena, old_pub_n == 0 ? 1 : old_pub_n,
                                                sizeof(uint32_t), _Alignof(uint32_t));
    uint32_t chg_n = 0;
    for (uint32_t i = 0; i < old_pub_n; i++) {
        if (old_pub_matched[i])
            continue;
        for (uint32_t j = 0; j < new_pub_n; j++)
            if (!new_pub_matched[j] && strcmp(old_pub[i].kind, new_pub[j].kind) == 0 &&
                strcmp(old_pub[i].name, new_pub[j].name) == 0) {
                old_pub_matched[i] = new_pub_matched[j] = true;
                chg_old[chg_n] = i;
                chg_new[chg_n] = j;
                chg_n++;
                break;
            }
    }

    /* 3. State delta */
    bool has_service = old_s.entry_service != MO_NO_INDEX || new_s.entry_service != MO_NO_INDEX;
    bool migration_required = old_s.entry_service != MO_NO_INDEX &&
                              new_s.entry_service != MO_NO_INDEX &&
                              memcmp(old_s.state_schema, new_s.state_schema, 32) != 0;
    bool state_changed =
        (old_s.entry_service == MO_NO_INDEX) != (new_s.entry_service == MO_NO_INDEX) ||
        migration_required;
    struct mo_buffer old_state_type, new_state_type;
    mo_buffer_init(&old_state_type, 512);
    mo_buffer_init(&new_state_type, 512);
    if (old_s.entry_service != MO_NO_INDEX)
        (void)format_type(old_s.program, old_s.program->services[old_s.entry_service].state,
                          &old_state_type);
    if (new_s.entry_service != MO_NO_INDEX)
        (void)format_type(new_s.program, new_s.program->services[new_s.entry_service].state,
                          &new_state_type);

    /* 4. Behavioral contracts */
    struct contract_item *contracts = NULL;
    uint32_t contract_n = evaluate_contracts(&diff_arena, cli, &old_s, &new_s, &contracts);
    uint32_t passed_n = 0, failed_n = 0, sig_changed_n = 0, removed_n = 0;
    for (uint32_t i = 0; i < contract_n; i++) {
        if (contracts[i].outcome == CONTRACT_PASSED)
            passed_n++;
        else if (contracts[i].outcome == CONTRACT_FAILED)
            failed_n++;
        else if (contracts[i].outcome == CONTRACT_SIGNATURE_CHANGED)
            sig_changed_n++;
        else
            removed_n++;
    }

    int exit_code = failed_n != 0 ? 1 : 0;
    struct mo_buffer out;
    mo_buffer_init(&out, 65536);
    bool ok = true;

    if (cli->json) {
        ok =
            mo_write_version(w->output, MO_COMMAND_DIFF) &&
            mo_buffer_append_text(&out, "{\"record\":\"diff\",\"authority\":{\"capabilities\":{") &&
            mo_buffer_append_text(&out, "\"before\":") &&
            json_str_array(&out, old_caps, old_cap_n) &&
            mo_buffer_append_text(&out, ",\"after\":") &&
            json_str_array(&out, new_caps, new_cap_n) &&
            mo_buffer_append_text(&out, ",\"added\":") &&
            json_str_array(&out, added_caps, add_cap_n) &&
            mo_buffer_append_text(&out, ",\"removed\":") &&
            json_str_array(&out, removed_caps, rem_cap_n) &&
            mo_buffer_append_text(&out, "},\"attenuations\":{\"added\":") &&
            json_str_array(&out, added_att, add_att_n) &&
            mo_buffer_append_text(&out, ",\"removed\":") &&
            json_str_array(&out, removed_att, rem_att_n) &&
            mo_buffer_append_text(&out, "}},\"public\":{\"added\":[");
        bool first = true;
        for (uint32_t j = 0; ok && j < new_pub_n; j++) {
            if (new_pub_matched[j])
                continue;
            ok = (first || mo_buffer_append_byte(&out, ',')) &&
                 mo_buffer_append_text(&out, "{\"kind\":") &&
                 mo_render_json_text(&out, new_pub[j].kind, strlen(new_pub[j].kind)) &&
                 mo_buffer_append_text(&out, ",\"name\":") &&
                 mo_render_json_text(&out, new_pub[j].name, strlen(new_pub[j].name)) &&
                 mo_buffer_append_text(&out, ",\"signature\":") &&
                 mo_render_json_text(&out, new_pub[j].sig, strlen(new_pub[j].sig)) &&
                 mo_buffer_append_byte(&out, '}');
            first = false;
        }
        ok = ok && mo_buffer_append_text(&out, "],\"removed\":[");
        first = true;
        for (uint32_t i = 0; ok && i < old_pub_n; i++) {
            if (old_pub_matched[i])
                continue;
            ok = (first || mo_buffer_append_byte(&out, ',')) &&
                 mo_buffer_append_text(&out, "{\"kind\":") &&
                 mo_render_json_text(&out, old_pub[i].kind, strlen(old_pub[i].kind)) &&
                 mo_buffer_append_text(&out, ",\"name\":") &&
                 mo_render_json_text(&out, old_pub[i].name, strlen(old_pub[i].name)) &&
                 mo_buffer_append_text(&out, ",\"signature\":") &&
                 mo_render_json_text(&out, old_pub[i].sig, strlen(old_pub[i].sig)) &&
                 mo_buffer_append_byte(&out, '}');
            first = false;
        }
        ok = ok && mo_buffer_append_text(&out, "],\"changed\":[");
        for (uint32_t c = 0; ok && c < chg_n; c++) {
            const struct pub_item *op = &old_pub[chg_old[c]];
            const struct pub_item *np = &new_pub[chg_new[c]];
            ok = (c == 0 || mo_buffer_append_byte(&out, ',')) &&
                 mo_buffer_append_text(&out, "{\"kind\":") &&
                 mo_render_json_text(&out, np->kind, strlen(np->kind)) &&
                 mo_buffer_append_text(&out, ",\"name\":") &&
                 mo_render_json_text(&out, np->name, strlen(np->name)) &&
                 mo_buffer_append_text(&out, ",\"before\":") &&
                 mo_render_json_text(&out, op->sig, strlen(op->sig)) &&
                 mo_buffer_append_text(&out, ",\"after\":") &&
                 mo_render_json_text(&out, np->sig, strlen(np->sig)) &&
                 mo_buffer_append_byte(&out, '}');
        }
        ok = ok && mo_buffer_append_text(&out, "]},\"state\":{\"has_service\":") &&
             mo_buffer_append_text(&out, has_service ? "true" : "false") &&
             mo_buffer_append_text(&out, ",\"changed\":") &&
             mo_buffer_append_text(&out, state_changed ? "true" : "false") &&
             mo_buffer_append_text(&out, ",\"migration_required\":") &&
             mo_buffer_append_text(&out, migration_required ? "true" : "false") &&
             mo_buffer_append_text(&out, ",\"before\":");
        if (ok && old_s.entry_service != MO_NO_INDEX) {
            ok = mo_buffer_append_text(&out, "{\"type\":") &&
                 mo_render_json_text(&out, old_state_type.bytes, old_state_type.length) &&
                 mo_buffer_append_text(&out, ",\"schema\":\"") &&
                 mo_offline_hex(&out, old_s.state_schema, 32) && mo_buffer_append_text(&out, "\"}");
        } else if (ok) {
            ok = mo_buffer_append_text(&out, "null");
        }
        ok = ok && mo_buffer_append_text(&out, ",\"after\":");
        if (ok && new_s.entry_service != MO_NO_INDEX) {
            ok = mo_buffer_append_text(&out, "{\"type\":") &&
                 mo_render_json_text(&out, new_state_type.bytes, new_state_type.length) &&
                 mo_buffer_append_text(&out, ",\"schema\":\"") &&
                 mo_offline_hex(&out, new_s.state_schema, 32) && mo_buffer_append_text(&out, "\"}");
        } else if (ok) {
            ok = mo_buffer_append_text(&out, "null");
        }
        ok = ok && mo_buffer_append_text(&out, "},\"contracts\":{\"passed\":") &&
             mo_buffer_append_uint(&out, passed_n) && mo_buffer_append_text(&out, ",\"failed\":") &&
             mo_buffer_append_uint(&out, failed_n) &&
             mo_buffer_append_text(&out, ",\"signature_changed\":") &&
             mo_buffer_append_uint(&out, sig_changed_n) &&
             mo_buffer_append_text(&out, ",\"removed\":") &&
             mo_buffer_append_uint(&out, removed_n) && mo_buffer_append_text(&out, ",\"items\":[");
        for (uint32_t i = 0; ok && i < contract_n; i++) {
            const char *oname = contract_outcome_name(contracts[i].outcome);
            ok = (i == 0 || mo_buffer_append_byte(&out, ',')) &&
                 mo_buffer_append_text(&out, "{\"name\":") &&
                 mo_render_json_text(&out, contracts[i].name, strlen(contracts[i].name)) &&
                 mo_buffer_append_text(&out, ",\"kind\":") &&
                 mo_render_json_text(&out, contracts[i].kind, strlen(contracts[i].kind)) &&
                 mo_buffer_append_text(&out, ",\"outcome\":\"") &&
                 mo_buffer_append_text(&out, oname) && mo_buffer_append_text(&out, "\"}");
        }
        ok = ok && mo_buffer_append_text(&out, "]}}\n") &&
             fwrite(out.bytes, 1, out.length, w->output) == out.length && fflush(w->output) == 0;
        struct mo_test_totals totals = {.status = exit_code};
        if (ok)
            w->status = mo_write_diagnostics(w->output, w->errors, true, MO_COMMAND_DIFF,
                                             &new_s.sources, &new_s.diags, &totals);
        else
            w->status = 1;
    } else {
        /* Human-readable text summary */
        ok = mo_buffer_append_text(&out, "authority:\n");
        if (add_cap_n == 0 && rem_cap_n == 0 && add_att_n == 0 && rem_att_n == 0) {
            ok = ok && mo_buffer_append_text(&out, "  unchanged\n");
        } else {
            for (uint32_t i = 0; ok && i < add_cap_n; i++)
                ok = mo_buffer_append_text(&out, "  + capability ") &&
                     mo_buffer_append_text(&out, added_caps[i]) &&
                     mo_buffer_append_byte(&out, '\n');
            for (uint32_t i = 0; ok && i < rem_cap_n; i++)
                ok = mo_buffer_append_text(&out, "  - capability ") &&
                     mo_buffer_append_text(&out, removed_caps[i]) &&
                     mo_buffer_append_byte(&out, '\n');
            for (uint32_t i = 0; ok && i < add_att_n; i++)
                ok = mo_buffer_append_text(&out, "  + attenuation ") &&
                     mo_render_terminal_text(&out, added_att[i], strlen(added_att[i])) &&
                     mo_buffer_append_byte(&out, '\n');
            for (uint32_t i = 0; ok && i < rem_att_n; i++)
                ok = mo_buffer_append_text(&out, "  - attenuation ") &&
                     mo_render_terminal_text(&out, removed_att[i], strlen(removed_att[i])) &&
                     mo_buffer_append_byte(&out, '\n');
        }
        ok = ok && mo_buffer_append_text(&out, "public:\n");
        bool any_pub = chg_n != 0;
        for (uint32_t j = 0; !any_pub && j < new_pub_n; j++)
            any_pub = !new_pub_matched[j];
        for (uint32_t i = 0; !any_pub && i < old_pub_n; i++)
            any_pub = !old_pub_matched[i];
        if (!any_pub) {
            ok = ok && mo_buffer_append_text(&out, "  unchanged\n");
        } else {
            for (uint32_t j = 0; ok && j < new_pub_n; j++)
                if (!new_pub_matched[j])
                    ok = mo_buffer_append_text(&out, "  + ") &&
                         mo_buffer_append_text(&out, new_pub[j].kind) &&
                         mo_buffer_append_byte(&out, ' ') &&
                         mo_render_terminal_text(&out, new_pub[j].name, strlen(new_pub[j].name)) &&
                         mo_buffer_append_text(&out, ": ") &&
                         mo_render_terminal_text(&out, new_pub[j].sig, strlen(new_pub[j].sig)) &&
                         mo_buffer_append_byte(&out, '\n');
            for (uint32_t i = 0; ok && i < old_pub_n; i++)
                if (!old_pub_matched[i])
                    ok = mo_buffer_append_text(&out, "  - ") &&
                         mo_buffer_append_text(&out, old_pub[i].kind) &&
                         mo_buffer_append_byte(&out, ' ') &&
                         mo_render_terminal_text(&out, old_pub[i].name, strlen(old_pub[i].name)) &&
                         mo_buffer_append_text(&out, ": ") &&
                         mo_render_terminal_text(&out, old_pub[i].sig, strlen(old_pub[i].sig)) &&
                         mo_buffer_append_byte(&out, '\n');
            for (uint32_t c = 0; ok && c < chg_n; c++) {
                const struct pub_item *op = &old_pub[chg_old[c]];
                const struct pub_item *np = &new_pub[chg_new[c]];
                ok = mo_buffer_append_text(&out, "  ~ ") && mo_buffer_append_text(&out, np->kind) &&
                     mo_buffer_append_byte(&out, ' ') &&
                     mo_render_terminal_text(&out, np->name, strlen(np->name)) &&
                     mo_buffer_append_text(&out, ": ") &&
                     mo_render_terminal_text(&out, op->sig, strlen(op->sig)) &&
                     mo_buffer_append_text(&out, " -> ") &&
                     mo_render_terminal_text(&out, np->sig, strlen(np->sig)) &&
                     mo_buffer_append_byte(&out, '\n');
            }
        }
        ok = ok && mo_buffer_append_text(&out, "state:\n");
        if (!has_service) {
            ok = ok && mo_buffer_append_text(&out, "  no service\n");
        } else if (!state_changed) {
            ok = ok && mo_buffer_append_text(&out, "  unchanged (") &&
                 mo_render_terminal_text(&out, new_state_type.bytes, new_state_type.length) &&
                 mo_buffer_append_text(&out, ")\n");
        } else if (migration_required) {
            ok = ok && mo_buffer_append_text(&out, "  changed: ") &&
                 mo_render_terminal_text(&out, old_state_type.bytes, old_state_type.length) &&
                 mo_buffer_append_text(&out, " -> ") &&
                 mo_render_terminal_text(&out, new_state_type.bytes, new_state_type.length) &&
                 mo_buffer_append_text(&out, " (migration required)\n");
        } else if (old_s.entry_service == MO_NO_INDEX) {
            ok = ok && mo_buffer_append_text(&out, "  + service state ") &&
                 mo_render_terminal_text(&out, new_state_type.bytes, new_state_type.length) &&
                 mo_buffer_append_byte(&out, '\n');
        } else {
            ok = ok && mo_buffer_append_text(&out, "  - service state ") &&
                 mo_render_terminal_text(&out, old_state_type.bytes, old_state_type.length) &&
                 mo_buffer_append_byte(&out, '\n');
        }
        ok = ok && mo_buffer_append_text(&out, "contracts: ") &&
             mo_buffer_append_uint(&out, passed_n) && mo_buffer_append_text(&out, " passed, ") &&
             mo_buffer_append_uint(&out, failed_n) && mo_buffer_append_text(&out, " failed, ") &&
             mo_buffer_append_uint(&out, sig_changed_n) &&
             mo_buffer_append_text(&out, " signature_changed, ") &&
             mo_buffer_append_uint(&out, removed_n) && mo_buffer_append_text(&out, " removed\n");
        for (uint32_t i = 0; ok && i < contract_n; i++) {
            if (contracts[i].outcome == CONTRACT_PASSED)
                continue;
            ok = mo_buffer_append_text(&out, "  ") &&
                 mo_buffer_append_text(&out, contract_outcome_name(contracts[i].outcome)) &&
                 mo_buffer_append_byte(&out, ' ') &&
                 mo_render_terminal_text(&out, contracts[i].name, strlen(contracts[i].name)) &&
                 mo_buffer_append_byte(&out, '\n');
        }
        ok = ok && fwrite(out.bytes, 1, out.length, w->output) == out.length &&
             fflush(w->output) == 0;
        if (!ok)
            (void)fputs("mo diff: could not write output\n", w->errors);
        w->status = ok ? exit_code : 1;
    }

    mo_buffer_free(&out);
    mo_buffer_free(&old_state_type);
    mo_buffer_free(&new_state_type);
    mo_arena_free(&diff_arena);
cleanup:
    side_free(&new_s);
    side_free(&old_s);
}

int mo_diff(const struct mo_cli *cli, FILE *output, FILE *errors) {
    char tmp_dir[64] = "";
    char git_old_entry[MO_LIMIT_PATH_SIZE + 128] = "";
    const char *old_path = NULL;
    const char *new_path = NULL;
    if (cli->file_count == 2) {
        old_path = mo_cli_file(cli, 0);
        new_path = mo_cli_file(cli, 1);
    } else {
        new_path = mo_cli_file(cli, 0);
        if (!materialize_git_head(new_path, tmp_dir, git_old_entry)) {
            struct mo_sources sources;
            struct mo_diags diags;
            bool hs = mo_sources_init(&sources);
            bool hd = mo_diags_init(&diags);
            if (!hs || !hd)
                diags.dropped = 1;
            else
                tool_error(&diags, MO_CODE_TOOL_PIPELINE,
                           "mo diff with one file needs Git HEAD to hold that file; pass OLD NEW "
                           "explicitly",
                           new_path);
            if (cli->json && !mo_write_version(output, MO_COMMAND_DIFF)) {
                mo_sources_free(&sources);
                mo_diags_free(&diags);
                return 1;
            }
            int st = mo_write_diagnostics(output, errors, cli->json, MO_COMMAND_DIFF, &sources,
                                          &diags, NULL);
            mo_sources_free(&sources);
            mo_diags_free(&diags);
            return st == 0 ? 1 : st;
        }
        old_path = git_old_entry;
    }

    struct diff_work work = {
        .cli = cli, .old_path = old_path, .new_path = new_path, .output = output, .errors = errors};
    int err = mo_with_stack(MO_SOURCE_STACK_SIZE, run_diff_work, &work);
    if (tmp_dir[0] != '\0')
        remove_tree(tmp_dir);
    if (err != 0) {
        (void)fprintf(errors, "mo diff: could not prepare pipeline stack: %s\n", strerror(err));
        return 1;
    }
    return work.status;
}
