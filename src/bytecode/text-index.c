#include "bytecode/text-index.h"

#include <string.h>

static MO_MUST_USE int compare(struct mo_bytecode_string a, struct mo_bytecode_string b) {
    uint32_t n = a.length < b.length ? a.length : b.length;
    int order = n == 0 ? 0 : memcmp(a.bytes, b.bytes, n);
    return order != 0 ? order : (a.length > b.length) - (a.length < b.length);
}

struct mo_text_index *mo_text_find(struct mo_text_index *root, struct mo_bytecode_string key) {
    while (root != NULL) {
        int order = compare(key, root->key);
        if (order == 0)
            return root;
        root = order < 0 ? root->left : root->right;
    }
    return NULL;
}

static MO_MUST_USE unsigned height(const struct mo_text_index *n) {
    return n == NULL ? 0 : n->height;
}
static void update(struct mo_text_index *n) {
    unsigned a = height(n->left), b = height(n->right);
    n->height = 1 + (a > b ? a : b);
}
static MO_MUST_USE struct mo_text_index *rotate(struct mo_text_index *n, bool left) {
    struct mo_text_index *p = left ? n->right : n->left;
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
static MO_MUST_USE struct mo_text_index *insert(struct mo_text_index *root, struct mo_text_index *n,
                                                unsigned depth) {
    /* An AVL tree containing at most the file's bounded entries is shorter than 64. */
    MO_ASSERT(depth < 64);
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

struct mo_text_index *mo_text_insert(struct mo_text_reader *r, struct mo_text_index **root,
                                     struct mo_bytecode_string key, uint32_t value) {
    struct mo_text_index *n = mo_text_find(*root, key);
    if (n != NULL)
        return n;
    n = mo_arena_allocate(&r->arena, sizeof *n, _Alignof(struct mo_text_index));
    if (n == NULL) {
        mo_text_refuse(r, 0, "no memory");
        return NULL;
    }
    *n = (struct mo_text_index){.key = key, .value = value, .height = 1};
    *root = insert(*root, n, 0);
    return n;
}
