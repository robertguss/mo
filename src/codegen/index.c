#include "codegen/index.h"

#include <string.h>

#include "base/limits.h"

static MO_MUST_USE int compare(mo_core_string a, mo_core_string b) {
    size_t n = a.length < b.length ? a.length : b.length;
    int order = n == 0 ? 0 : memcmp(a.bytes, b.bytes, n);
    return order != 0 ? order : (a.length > b.length) - (a.length < b.length);
}

struct mo_codegen_index *mo_codegen_find(struct mo_codegen_index *root, mo_core_string key) {
    while (root != NULL) {
        int order = compare(key, root->key);
        if (order == 0)
            return root;
        root = order < 0 ? root->left : root->right;
    }
    return NULL;
}

static MO_MUST_USE unsigned height(const struct mo_codegen_index *n) {
    return n == NULL ? 0 : n->height;
}
static void update(struct mo_codegen_index *n) {
    unsigned a = height(n->left), b = height(n->right);
    n->height = 1 + (a > b ? a : b);
}
static MO_MUST_USE struct mo_codegen_index *rotate(struct mo_codegen_index *n, bool left) {
    struct mo_codegen_index *p = left ? n->right : n->left;
    MO_ASSERT(p != NULL);
    if (left) {
        n->right = p->left;
        p->left = n;
    } else {
        n->left = p->right;
        p->right = n;
    }
    update(n);
    update(p);
    return p;
}
static MO_MUST_USE struct mo_codegen_index *insert(struct mo_codegen_index *root,
                                                   struct mo_codegen_index *n, unsigned depth) {
    MO_ASSERT(depth < MO_LIMIT_CORE_DEPTH);
    if (root == NULL)
        return n;
    if (compare(n->key, root->key) < 0)
        root->left = insert(root->left, n, depth + 1);
    else
        root->right = insert(root->right, n, depth + 1);
    update(root);
    if (height(root->left) > height(root->right) + 1) {
        if (height(root->left->right) > height(root->left->left))
            root->left = rotate(root->left, true);
        return rotate(root, false);
    }
    if (height(root->right) > height(root->left) + 1) {
        if (height(root->right->left) > height(root->right->right))
            root->right = rotate(root->right, false);
        return rotate(root, true);
    }
    return root;
}

struct mo_codegen_index *mo_codegen_insert(struct mo_arena *arena, struct mo_codegen_index **root,
                                           mo_core_string key, uint32_t value) {
    struct mo_codegen_index *n = mo_codegen_find(*root, key);
    if (n != NULL)
        return n;
    n = mo_arena_allocate(arena, sizeof *n, _Alignof(struct mo_codegen_index));
    if (n == NULL)
        return NULL;
    *n = (struct mo_codegen_index){.key = key, .value = value, .height = 1};
    *root = insert(*root, n, 0);
    return n;
}
