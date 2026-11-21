#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "tools/install.h"

#include "base/codes.h"
#include "base/runtime-identity.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void install_error(struct mo_diags *diags, const char *message, const char *detail) {
    struct mo_diag d = {.code = MO_CODE_TOOL_PIPELINE,
                        .message = message,
                        .primary = {.span = {.file = UINT32_MAX}, .text = detail}};
    bool added = mo_diags_add(diags, &d);
    if (!added)
        MO_ASSERT(mo_diags_any(diags));
}

static bool names_a_source(const char *path, const struct mo_sources *sources) {
    struct stat out_stat, in_stat;
    if (stat(path, &out_stat) != 0)
        return false;
    for (uint32_t i = 0; i < sources->count; i++)
        if (stat(sources->files[i].path, &in_stat) == 0 && out_stat.st_dev == in_stat.st_dev &&
            out_stat.st_ino == in_stat.st_ino)
            return true;
    return false;
}

static MO_MUST_USE bool derive_tool_name(const char *source_path,
                                         char out[MO_LIMIT_INSTALL_NAME + 1]) {
    const char *slash = strrchr(source_path, '/');
    const char *base = slash != NULL ? slash + 1 : source_path;
    if (strcmp(base, "main.mo") == 0) {
        char parent_buf[MO_LIMIT_PATH_SIZE];
        if (slash == NULL) {
            memcpy(parent_buf, ".", 2);
        } else if (slash == source_path) {
            memcpy(parent_buf, "/", 2);
        } else {
            size_t plen = (size_t)(slash - source_path);
            if (plen >= sizeof parent_buf)
                return false;
            memcpy(parent_buf, source_path, plen);
            parent_buf[plen] = '\0';
        }
        char resolved[PATH_MAX];
        const char *dir_ref = realpath(parent_buf, resolved);
        if (dir_ref == NULL)
            dir_ref = parent_buf;
        const char *rslash = strrchr(dir_ref, '/');
        const char *dir_name = rslash != NULL ? rslash + 1 : dir_ref;
        if (!mo_valid_install_name(dir_name))
            return false;
        size_t n = strlen(dir_name);
        memcpy(out, dir_name, n + 1);
        return true;
    }
    size_t len = strlen(base);
    if (len > 3 && strcmp(base + len - 3, ".mo") == 0)
        len -= 3;
    if (len == 0 || len > MO_LIMIT_INSTALL_NAME)
        return false;
    memcpy(out, base, len);
    out[len] = '\0';
    return mo_valid_install_name(out);
}

static MO_MUST_USE bool mkdir_parents(const char *dir) {
    size_t len = strlen(dir);
    if (len == 0 || len >= MO_LIMIT_HOST_PATH_SIZE) {
        errno = ENAMETOOLONG;
        return false;
    }
    char buf[MO_LIMIT_HOST_PATH_SIZE];
    memcpy(buf, dir, len + 1);
    while (len > 1 && buf[len - 1] == '/')
        buf[--len] = '\0';
    for (size_t i = (buf[0] == '/' ? 1 : 0); i <= len; i++) {
        if (buf[i] != '/' && buf[i] != '\0')
            continue;
        char saved = buf[i];
        buf[i] = '\0';
        struct stat st;
        if (stat(buf, &st) != 0) {
            if (mkdir(buf, 0755) != 0 && errno != EEXIST)
                return false;
        } else if (!S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            return false;
        }
        buf[i] = saved;
    }
    return true;
}

static MO_MUST_USE bool resolve_existing_dir(const char *path, char out[MO_LIMIT_HOST_PATH_SIZE]) {
    char resolved[PATH_MAX];
    if (realpath(path, resolved) == NULL)
        return false;
    struct stat st;
    if (stat(resolved, &st) != 0)
        return false;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return false;
    }
    size_t len = strlen(resolved);
    if (len == 0 || len >= MO_LIMIT_HOST_PATH_SIZE) {
        errno = ENAMETOOLONG;
        return false;
    }
    memcpy(out, resolved, len + 1);
    return true;
}

static MO_MUST_USE bool append_str(struct mo_buffer *buf, const char *s) {
    return mo_buffer_append(buf, s, strlen(s));
}

static MO_MUST_USE bool append_shell_quoted(struct mo_buffer *buf, const char *s) {
    if (!mo_buffer_append_byte(buf, '\''))
        return false;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == '\'') {
            if (!append_str(buf, "'\\''"))
                return false;
        } else if (!mo_buffer_append_byte(buf, (uint8_t)*p)) {
            return false;
        }
    }
    return mo_buffer_append_byte(buf, '\'');
}

static MO_MUST_USE bool write_atomic_file(const char *dir, const char *name, const char *dest_path,
                                          const uint8_t *bytes, size_t len, mode_t mode,
                                          int *err_out) {
    struct stat existing;
    if (lstat(dest_path, &existing) == 0 && !S_ISREG(existing.st_mode)) {
        *err_out = S_ISDIR(existing.st_mode) ? EISDIR : EEXIST;
        return false;
    }
    static unsigned counter = 0;
    char tmp_path[MO_LIMIT_HOST_PATH_SIZE];
    int fd = -1;
    for (unsigned attempt = 0; attempt < 64; attempt++) {
        int n = snprintf(tmp_path, sizeof tmp_path, "%s/.%s.tmp.%ld.%u", dir, name, (long)getpid(),
                         ++counter);
        if (n <= 0 || (size_t)n >= sizeof tmp_path) {
            *err_out = ENAMETOOLONG;
            return false;
        }
        fd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0)
            break;
        if (errno != EEXIST) {
            *err_out = errno;
            return false;
        }
    }
    if (fd < 0) {
        *err_out = errno != 0 ? errno : EEXIST;
        return false;
    }
    size_t off = 0;
    bool ok = true;
    int err = 0;
    while (off < len) {
        ssize_t w = write(fd, bytes + off, len - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            ok = false;
            err = errno;
            break;
        }
        off += (size_t)w;
    }
    if (ok && fchmod(fd, mode) != 0) {
        ok = false;
        err = errno;
    }
    if (close(fd) != 0 && ok) {
        ok = false;
        err = errno;
    }
    if (ok && rename(tmp_path, dest_path) != 0) {
        ok = false;
        err = errno;
    }
    if (!ok) {
        (void)unlink(tmp_path);
        *err_out = err != 0 ? err : EIO;
    }
    return ok;
}

static MO_MUST_USE bool build_launcher_script(const char *tool_name, const char *fallback_mo,
                                              const char *baked_root, const char *baked_state,
                                              struct mo_buffer *script) {
    if (!append_str(script, "#!/bin/sh\n"
                            "dir=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\n"
                            "if [ -x \"$dir/mo\" ]; then\n"
                            "  mo_bin=\"$dir/mo\"\n"
                            "elif command -v mo >/dev/null 2>&1; then\n"
                            "  mo_bin=mo\n"
                            "else\n"
                            "  mo_bin="))
        return false;
    if (!append_shell_quoted(script,
                             fallback_mo != NULL && fallback_mo[0] != '\0' ? fallback_mo : "mo") ||
        !append_str(script, "\nfi\nroot_arg="))
        return false;
    if (!append_shell_quoted(script, baked_root != NULL ? baked_root : "") ||
        !append_str(script, "\nstate_arg=") ||
        !append_shell_quoted(script, baked_state != NULL ? baked_state : "") ||
        !append_str(script, "\nwhile [ $# -gt 0 ]; do\n"
                            "  case \"$1\" in\n"
                            "    --mo-root)\n"
                            "      [ $# -ge 2 ] || exec \"$mo_bin\" run --root\n"
                            "      root_arg=$2\n"
                            "      shift 2\n"
                            "      ;;\n"
                            "    --mo-state-dir)\n"
                            "      [ $# -ge 2 ] || exec \"$mo_bin\" run --state-dir\n"
                            "      state_arg=$2\n"
                            "      shift 2\n"
                            "      ;;\n"
                            "    *)\n"
                            "      break\n"
                            "      ;;\n"
                            "  esac\n"
                            "done\n"
                            "set -- \"$dir/.mo-libexec/") ||
        !append_str(script, tool_name) ||
        !append_str(script, ".bc\" \"$@\"\n"
                            "if [ -n \"$state_arg\" ]; then\n"
                            "  set -- --state-dir \"$state_arg\" \"$@\"\n"
                            "fi\n"
                            "if [ -n \"$root_arg\" ]; then\n"
                            "  set -- --root \"$root_arg\" \"$@\"\n"
                            "fi\n"
                            "exec \"$mo_bin\" run \"$@\"\n"))
        return false;
    return true;
}

void mo_install_program(const struct mo_cli *cli, const struct mo_sources *sources,
                        const struct mo_buffer *wire, FILE *output, struct mo_diags *diags) {
    char tool_name[MO_LIMIT_INSTALL_NAME + 1];
    if (cli->install_name != NULL) {
        size_t n = strlen(cli->install_name);
        MO_ASSERT(n > 0 && n <= MO_LIMIT_INSTALL_NAME);
        memcpy(tool_name, cli->install_name, n + 1);
    } else if (!derive_tool_name(mo_cli_file(cli, 0), tool_name)) {
        install_error(diags, "could not derive a valid tool name; pass --name NAME",
                      mo_cli_file(cli, 0));
        return;
    }

    char baked_root_buf[MO_LIMIT_HOST_PATH_SIZE];
    char baked_state_buf[MO_LIMIT_HOST_PATH_SIZE];
    const char *baked_root = NULL;
    const char *baked_state = NULL;
    if (cli->root != NULL) {
        if (!resolve_existing_dir(cli->root, baked_root_buf)) {
            install_error(diags, "could not open --root folder", strerror(errno));
            return;
        }
        baked_root = baked_root_buf;
    }
    if (cli->state_dir != NULL) {
        if (!resolve_existing_dir(cli->state_dir, baked_state_buf)) {
            install_error(diags, "could not open --state-dir folder", strerror(errno));
            return;
        }
        baked_state = baked_state_buf;
    }

    char target_bin[MO_LIMIT_HOST_PATH_SIZE];
    if (cli->bin_dir != NULL) {
        size_t len = strlen(cli->bin_dir);
        if (len == 0 || len >= sizeof target_bin) {
            install_error(diags, "install directory path is too long", cli->bin_dir);
            return;
        }
        memcpy(target_bin, cli->bin_dir, len + 1);
    } else {
        const char *home = getenv("HOME");
        if (home == NULL || home[0] == '\0') {
            install_error(diags, "HOME is not set; pass --bin-dir DIR", NULL);
            return;
        }
        int n = snprintf(target_bin, sizeof target_bin, "%s/.local/bin", home);
        if (n <= 0 || (size_t)n >= sizeof target_bin) {
            install_error(diags, "install directory path is too long", home);
            return;
        }
    }

    if (!mkdir_parents(target_bin)) {
        install_error(diags, "could not create install directory", strerror(errno));
        return;
    }
    char real_bin[MO_LIMIT_HOST_PATH_SIZE];
    if (!resolve_existing_dir(target_bin, real_bin)) {
        install_error(diags, "could not resolve install directory", strerror(errno));
        return;
    }

    char libexec_dir[MO_LIMIT_HOST_PATH_SIZE];
    int ln = snprintf(libexec_dir, sizeof libexec_dir, "%s/.mo-libexec", real_bin);
    if (ln <= 0 || (size_t)ln >= sizeof libexec_dir) {
        install_error(diags, "install directory path is too long", real_bin);
        return;
    }
    if (!mkdir_parents(libexec_dir)) {
        install_error(diags, "could not create .mo-libexec directory", strerror(errno));
        return;
    }

    char bc_path[MO_LIMIT_HOST_PATH_SIZE];
    char launcher_path[MO_LIMIT_HOST_PATH_SIZE];
    int bn = snprintf(bc_path, sizeof bc_path, "%s/%s.bc", libexec_dir, tool_name);
    int wn = snprintf(launcher_path, sizeof launcher_path, "%s/%s", real_bin, tool_name);
    if (bn <= 0 || (size_t)bn >= sizeof bc_path || wn <= 0 || (size_t)wn >= sizeof launcher_path) {
        install_error(diags, "installed tool path is too long", tool_name);
        return;
    }

    if (names_a_source(bc_path, sources) || names_a_source(launcher_path, sources)) {
        install_error(diags, "installed tool can't replace a source file", launcher_path);
        return;
    }

    char self_mo[MO_LIMIT_PATH_SIZE];
    const char *fallback_mo = mo_runtime_executable_path(self_mo) ? self_mo : "mo";

    struct mo_buffer script;
    mo_buffer_init(&script, MO_LIMIT_INSTALL_SCRIPT);
    if (!build_launcher_script(tool_name, fallback_mo, baked_root, baked_state, &script)) {
        mo_buffer_free(&script);
        install_error(diags, "launcher script exceeds output budget", tool_name);
        return;
    }

    int write_err = 0;
    if (!write_atomic_file(libexec_dir, tool_name, bc_path, wire->bytes, wire->length, 0644,
                           &write_err)) {
        mo_buffer_free(&script);
        install_error(diags, "bytecode could not be installed", strerror(write_err));
        return;
    }
    if (!write_atomic_file(real_bin, tool_name, launcher_path, script.bytes, script.length, 0755,
                           &write_err)) {
        mo_buffer_free(&script);
        install_error(diags, "launcher could not be installed", strerror(write_err));
        return;
    }
    mo_buffer_free(&script);

    if (fprintf(output, "installed %s\n", launcher_path) < 0 || fflush(output) != 0)
        install_error(diags, "install output could not be written", strerror(errno));
}
