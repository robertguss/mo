#include "base/binary.h"
#include "base/utf8.h"
#include "tools/upgrade.h"
#include <stdio.h>
#include <string.h>

struct schema_node;
struct schema_field {
    const char *name;
    struct schema_node *type;
};
struct schema_case {
    const char *name;
    bool unnamed;
    uint32_t count;
    struct schema_field *fields;
};
struct schema_node {
    uint8_t tag;
    const char *name;
    uint32_t count, groups;
    struct schema_node **parameters;
    struct schema_case *cases;
};
struct schema_input {
    struct mo_binary bytes;
    struct mo_arena arena;
    struct schema_node **nodes;
    uint32_t count, capacity;
};
static bool number(struct schema_input *p, unsigned width, uint32_t *out) {
    uint64_t n;
    if (!mo_binary_uint(&p->bytes, width, &n) || n > UINT32_MAX)
        return false;
    *out = (uint32_t)n;
    return true;
}
static const char *text(struct schema_input *p) {
    uint32_t length;
    const uint8_t *bytes;
    if (!number(p, 4, &length) || length > MO_LIMIT_STRING_SIZE ||
        !mo_binary_read(&p->bytes, length, &bytes) || memchr(bytes, 0, length) != NULL ||
        !mo_utf8_valid(bytes, length))
        return NULL;
    char *copy = mo_arena_allocate(&p->arena, (size_t)length + 1, 1);
    if (copy == NULL)
        return NULL;
    memcpy(copy, bytes, length);
    copy[length] = 0;
    return copy;
}
static struct schema_node *read_node(struct schema_input *p, unsigned depth) {
    uint32_t tag;
    if (depth >= MO_LIMIT_SAVED_DEPTH || !number(p, 1, &tag))
        return NULL;
    if (tag == 255) {
        uint32_t index;
        return number(p, 4, &index) && index < p->count ? p->nodes[index] : NULL;
    }
    /* Source structs use choice layout. A legacy record layout cannot be
     * reproduced by a source declaration without changing its saved shape. */
    if (tag > 18 || tag == 17 || p->count == p->capacity)
        return NULL;
    struct schema_node *node =
        mo_arena_allocate(&p->arena, sizeof *node, _Alignof(struct schema_node));
    if (node == NULL)
        return NULL;
    *node = (struct schema_node){.tag = (uint8_t)tag};
    p->nodes[p->count++] = node;
    if (tag >= 17 && (node->name = text(p)) == NULL)
        return NULL;
    if (!number(p, 4, &node->count) || node->count > MO_LIMIT_TYPES)
        return NULL;
    node->parameters = mo_arena_allocate_array(&p->arena, node->count, sizeof *node->parameters,
                                               _Alignof(struct schema_node *));
    if (node->count != 0 && node->parameters == NULL)
        return NULL;
    for (uint32_t i = 0; i < node->count; i++)
        if ((node->parameters[i] = read_node(p, depth + 1)) == NULL)
            return NULL;
    if (!number(p, 4, &node->groups) || node->groups > MO_LIMIT_FIELDS)
        return NULL;
    node->cases = mo_arena_allocate_array(&p->arena, node->groups, sizeof *node->cases,
                                          _Alignof(struct schema_case));
    if (node->groups != 0 && node->cases == NULL)
        return NULL;
    for (uint32_t i = 0; i < node->groups; i++) {
        struct schema_case *c = &node->cases[i];
        *c = (struct schema_case){0};
        uint32_t unnamed = 0;
        if (tag == 18 && ((c->name = text(p)) == NULL || !number(p, 1, &unnamed) || unnamed > 1))
            return NULL;
        c->unnamed = unnamed != 0;
        if (!number(p, 4, &c->count) || c->count > MO_LIMIT_FIELDS)
            return NULL;
        c->fields = mo_arena_allocate_array(&p->arena, c->count, sizeof *c->fields,
                                            _Alignof(struct schema_field));
        if (c->count != 0 && c->fields == NULL)
            return NULL;
        for (uint32_t j = 0; j < c->count; j++)
            if ((c->fields[j].name = text(p)) == NULL ||
                (c->fields[j].type = read_node(p, depth + 1)) == NULL)
                return NULL;
    }
    return node;
}
static bool put(struct mo_buffer *out, const char *s) {
    return mo_buffer_append(out, s, strlen(s));
}
static size_t owner_length(const char *name) {
    size_t owner = 0;
    if (name != NULL)
        for (size_t i = 0; name[i] != 0 && name[i] != '('; i++)
            if (name[i] == '.')
                owner = i;
    return owner;
}
static bool same_owner(const char *a, const char *b) {
    size_t n = owner_length(a);
    return n == owner_length(b) && (n == 0 || memcmp(a, b, n) == 0);
}
/* The serialized nominal name contains concrete arguments. Reuse that source spelling,
 * translating qualified user names into the generated namespace. */
static bool nominal_text(struct mo_buffer *out, const char *name, const char *root,
                         const char *module) {
    size_t at = 0;
    while (name[at] != 0) {
        if (name[at] == '(' || name[at] == ')' || name[at] == ',' || name[at] == ' ') {
            if (!mo_buffer_append_byte(out, (uint8_t)name[at++]))
                return false;
            continue;
        }
        size_t start = at, dot = SIZE_MAX;
        while (name[at] != 0 && name[at] != '(' && name[at] != ')' && name[at] != ',' &&
               name[at] != ' ') {
            if (name[at] == '.')
                dot = at;
            at++;
        }
        if (dot == SIZE_MAX) {
            if (!mo_buffer_append(out, name + start, at - start))
                return false;
        } else {
            size_t owner = dot - start;
            bool local = owner == owner_length(module) && memcmp(name + start, module, owner) == 0;
            bool main = owner == owner_length(root) && memcmp(name + start, root, owner) == 0;
            if ((!local &&
                 (!put(out, "UpgradeOld.") ||
                  (!main && (!mo_buffer_append(out, name + start, owner) || !put(out, "."))))) ||
                !mo_buffer_append(out, name + dot + 1, at - dot - 1))
                return false;
        }
    }
    return true;
}
static bool module_text(struct mo_buffer *out, const char *name, const char *root);
static bool generated_name(struct mo_buffer *out, const char *name) {
    const char *short_name = name + owner_length(name) + 1;
    if (strchr(short_name, '(') == NULL)
        return put(out, short_name);
    static const char hex[] = "0123456789abcdef";
    if (!put(out, "Saved_"))
        return false;
    for (size_t i = 0; name[i] != 0; i++) {
        uint8_t c = (uint8_t)name[i];
        if (!mo_buffer_append_byte(out, (uint8_t)hex[c >> 4]) ||
            !mo_buffer_append_byte(out, (uint8_t)hex[c & 15]))
            return false;
    }
    return true;
}
static bool uses_module(const struct schema_node *node, const char *module, unsigned depth) {
    if (depth >= MO_LIMIT_SAVED_DEPTH)
        return false;
    if (node->name != NULL && owner_length(node->name) != 0)
        return same_owner(node->name, module);
    for (uint32_t i = 0; i < node->count; i++)
        if (uses_module(node->parameters[i], module, depth + 1))
            return true;
    for (uint32_t c = 0; c < node->groups; c++)
        for (uint32_t i = 0; i < node->cases[c].count; i++)
            if (uses_module(node->cases[c].fields[i].type, module, depth + 1))
                return true;
    return false;
}
static const char *const builtin[] = {"Unit",   "Bool",     "Byte",    "Int",   "Float", "Char",
                                      "Time",   "Duration", "Decimal", "Range", "Text",  "Bytes",
                                      "BigInt", "",         "List",    "Set",   "Map"};
static bool node_text(struct mo_buffer *out, const struct schema_node *node, const char *root,
                      const char *module, unsigned depth) {
    if (depth >= MO_LIMIT_SAVED_DEPTH)
        return false;
    if (node->tag >= 17) {
        if (owner_length(node->name) != 0)
            return (same_owner(node->name, module) ||
                    (module_text(out, node->name, root) && put(out, "."))) &&
                   generated_name(out, node->name);
        if (strncmp(node->name, "Option(", 7) == 0 && node->groups == 2 &&
            node->cases[1].count == 1)
            return put(out, "Option(") &&
                   node_text(out, node->cases[1].fields[0].type, root, module, depth + 1) &&
                   put(out, ")");
        return nominal_text(out, node->name, root, module);
    }
    if (!put(out, builtin[node->tag]))
        return false;
    if (node->count != 0 || node->tag == 13) {
        if (!put(out, "("))
            return false;
        for (uint32_t i = 0; i < node->count; i++)
            if ((i != 0 && !put(out, ", ")) ||
                !node_text(out, node->parameters[i], root, module, depth + 1))
                return false;
        if (!put(out, ")"))
            return false;
    }
    return true;
}
static bool module_text(struct mo_buffer *out, const char *name, const char *root) {
    return put(out, "UpgradeOld") &&
           (same_owner(name, root) ||
            (put(out, ".") && mo_buffer_append(out, name, owner_length(name))));
}
bool mo_upgrade_schema_sources(const uint8_t *saved, size_t length, struct mo_sources *sources) {
    if (length < 9 || length > MO_LIMIT_SAVED_BYTES || memcmp(saved, "MOSV\1", 5) != 0)
        return false;
    struct schema_input p = {.bytes = {saved, length, 5}};
    mo_arena_init(&p.arena, MO_CLI_CHECK_SIZE);
    uint32_t size;
    const uint8_t *schema;
    bool ok = number(&p, 4, &size) && mo_binary_read(&p.bytes, size, &schema);
    if (!ok) {
        mo_arena_free(&p.arena);
        return false;
    }
    p.bytes = (struct mo_binary){schema, size, 0};
    p.capacity = size < MO_LIMIT_TYPES ? size : MO_LIMIT_TYPES;
    p.nodes = mo_arena_allocate_array(&p.arena, p.capacity, sizeof *p.nodes,
                                      _Alignof(struct schema_node *));
    struct schema_node *root = p.nodes == NULL ? NULL : read_node(&p, 0);
    ok = root != NULL && p.bytes.position == p.bytes.length;
    const char *root_name = root != NULL && root->name != NULL ? root->name : "Main.State";
    struct mo_buffer out, path;
    mo_buffer_init(&out, MO_LIMIT_FILE_SIZE);
    mo_buffer_init(&path, MO_LIMIT_PATH_SIZE);
    for (uint32_t m = 0; ok && m <= p.count; m++) {
        const char *module = m == p.count ? root_name : p.nodes[m]->name;
        if (module == NULL || (m != p.count && owner_length(module) == 0))
            continue;
        bool seen = false;
        for (uint32_t i = 0; i < m; i++)
            if (p.nodes[i]->name != NULL && same_owner(p.nodes[i]->name, module))
                seen = true;
        if (seen)
            continue;
        out.length = 0;
        path.length = 0;
        ok = put(&out, "module ") && module_text(&out, module, root_name) && put(&out, "\n");
        bool exposed = false;
        for (uint32_t i = 0; ok && i < p.count; i++) {
            const char *name = p.nodes[i]->name;
            if (name == NULL || owner_length(name) == 0 || !same_owner(name, module))
                continue;
            ok = put(&out, exposed ? ", " : "expose ") && generated_name(&out, name) &&
                 put(&out, "(..)");
            exposed = true;
        }
        if (exposed)
            ok = ok && put(&out, "\n");
        for (uint32_t i = 0; ok && i < p.count; i++) {
            const char *name = p.nodes[i]->name;
            if (name == NULL || owner_length(name) == 0 || same_owner(name, module))
                continue;
            bool duplicate = false;
            for (uint32_t j = 0; j < i; j++)
                duplicate |= p.nodes[j]->name != NULL && same_owner(name, p.nodes[j]->name);
            bool needed = false;
            for (uint32_t j = 0; j < p.count; j++) {
                const struct schema_node *node = p.nodes[j];
                if (node->name == NULL || !same_owner(node->name, module))
                    continue;
                for (uint32_t c = 0; c < node->groups; c++)
                    for (uint32_t f = 0; f < node->cases[c].count; f++)
                        needed |= uses_module(node->cases[c].fields[f].type, name, 0);
            }
            if (!duplicate && needed)
                ok = put(&out, "use ") && module_text(&out, name, root_name) && put(&out, "\n");
        }
        for (uint32_t i = 0; ok && i < p.count; i++) {
            const struct schema_node *node = p.nodes[i];
            if (node->name == NULL || owner_length(node->name) == 0 ||
                !same_owner(node->name, module))
                continue;
            const char *short_name = node->name + owner_length(node->name) + 1;
            bool record = strchr(short_name, '(') == NULL &&
                          (node->tag == 17 || (node->groups == 1 && !node->cases[0].unnamed &&
                                               strcmp(node->cases[0].name, short_name) == 0));
            ok = put(&out, record ? "struct " : "enum ") && generated_name(&out, node->name) &&
                 put(&out, "\n");
            for (uint32_t c = 0; ok && c < node->groups; c++) {
                const struct schema_case *group = &node->cases[c];
                if (!record)
                    ok = put(&out, group->name) && (group->count == 0 || put(&out, "("));
                for (uint32_t f = 0; ok && f < group->count; f++) {
                    const struct schema_field *field = &group->fields[f];
                    ok = (record || f == 0 || put(&out, ", ")) &&
                         (group->unnamed || (put(&out, field->name) && put(&out, ": "))) &&
                         node_text(&out, field->type, root_name, module, 0) &&
                         (!record || put(&out, "\n"));
                }
                if (!record)
                    ok = ok && (group->count == 0 || put(&out, ")")) && put(&out, "\n");
            }
            ok = ok && put(&out, "end\n");
        }
        if (ok && same_owner(module, root_name))
            ok = put(&out, "fn upgrade_input(value: ") &&
                 node_text(&out, root, root_name, module, 0) && put(&out, ") : ") &&
                 node_text(&out, root, root_name, module, 0) && put(&out, "\nvalue\nend\n");
        ok = ok && module_text(&path, module, root_name) && put(&path, ".mo") &&
             mo_buffer_append_byte(&path, 0);
        uint32_t file;
        struct mo_source_error error;
        ok = ok && mo_sources_add_bytes(sources, (const char *)path.bytes, out.bytes, out.length,
                                        &file, &error);
    }
    mo_buffer_free(&out);
    mo_buffer_free(&path);
    mo_arena_free(&p.arena);
    return ok;
}
