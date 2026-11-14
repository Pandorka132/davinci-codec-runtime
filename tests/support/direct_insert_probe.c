#include "codec_records.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NODE_BLACK = 1, NODE_RED = 0 };

typedef struct Node {
    struct Node *left;
    struct Node *right;
    struct Node *parent;
    uint8_t color;
    uint8_t padding[3];
    uint32_t fourcc;
    uint32_t codec_id;
    uint8_t tail_padding[4];
} Node;

typedef struct ResolveMap {
    Node *begin;
    Node *root;
    size_t size;
} ResolveMap;

_Static_assert(sizeof(Node) == 0x28, "Resolve node size");
_Static_assert(offsetof(Node, parent) == 0x10, "Resolve node parent offset");
_Static_assert(offsetof(Node, fourcc) == 0x1c, "Resolve FourCC offset");
_Static_assert(offsetof(Node, codec_id) == 0x20, "Resolve codec ID offset");
_Static_assert(sizeof(ResolveMap) == 0x18, "Resolve map size");
_Static_assert(offsetof(ResolveMap, begin) == 0x00, "Resolve begin offset");
_Static_assert(offsetof(ResolveMap, root) == 0x08, "Resolve root/sentinel offset");
_Static_assert(offsetof(ResolveMap, size) == 0x10, "Resolve map size offset");

static long allocation_budget = -1;
#ifdef FOURCC_TREE_PROBE_INSTRUMENT
unsigned fourcc_probe_left_rotations;
unsigned fourcc_probe_right_rotations;
#endif

static Node *allocate_node(void) {
    if (allocation_budget == 0) return NULL;
    if (allocation_budget > 0) --allocation_budget;
    Node *node = (Node *)malloc(sizeof(Node));
    if (node) memset(node, 0xa5, sizeof(Node));
    return node;
}

static Node *sentinel(ResolveMap *map) {
    return (Node *)&map->root;
}

static void rotate_left(ResolveMap *map, Node *x) {
#ifdef FOURCC_TREE_PROBE_INSTRUMENT
    ++fourcc_probe_left_rotations;
#endif
    Node *y = x->right;
    if (!y) abort();
    x->right = y->left;
    if (y->left) y->left->parent = x;
    y->parent = x->parent;
    if (x->parent == sentinel(map)) map->root = y;
    else if (x == x->parent->left) x->parent->left = y;
    else x->parent->right = y;
    y->left = x;
    x->parent = y;
}

static void rotate_right(ResolveMap *map, Node *x) {
#ifdef FOURCC_TREE_PROBE_INSTRUMENT
    ++fourcc_probe_right_rotations;
#endif
    Node *y = x->left;
    if (!y) abort();
    x->left = y->right;
    if (y->right) y->right->parent = x;
    y->parent = x->parent;
    if (x->parent == sentinel(map)) map->root = y;
    else if (x == x->parent->right) x->parent->right = y;
    else x->parent->left = y;
    y->right = x;
    x->parent = y;
}

static void rebalance_after_insert(ResolveMap *map, Node *node) {
    node->color = node == map->root ? NODE_BLACK : NODE_RED;
    while (node != map->root && node->parent->color == NODE_RED) {
        Node *parent = node->parent;
        Node *grandparent = parent->parent;
        if (parent == grandparent->left) {
            Node *uncle = grandparent->right;
            if (uncle && uncle->color == NODE_RED) {
                parent->color = NODE_BLACK;
                uncle->color = NODE_BLACK;
                grandparent->color = NODE_RED;
                node = grandparent;
            } else {
                if (node == parent->right) {
                    node = parent;
                    rotate_left(map, node);
                    parent = node->parent;
                    grandparent = parent->parent;
                }
                parent->color = NODE_BLACK;
                grandparent->color = NODE_RED;
                rotate_right(map, grandparent);
            }
        } else {
            Node *uncle = grandparent->left;
            if (uncle && uncle->color == NODE_RED) {
                parent->color = NODE_BLACK;
                uncle->color = NODE_BLACK;
                grandparent->color = NODE_RED;
                node = grandparent;
            } else {
                if (node == parent->left) {
                    node = parent;
                    rotate_right(map, node);
                    parent = node->parent;
                    grandparent = parent->parent;
                }
                parent->color = NODE_BLACK;
                grandparent->color = NODE_RED;
                rotate_left(map, grandparent);
            }
        }
    }
    if (map->root) map->root->color = NODE_BLACK;
}

static int tree_insert(ResolveMap *map, uint32_t fourcc, uint32_t codec_id) {
    Node **link = &map->root;
    Node *parent = sentinel(map);
    while (*link) {
        parent = *link;
        if (fourcc < parent->fourcc) link = &parent->left;
        else if (fourcc > parent->fourcc) link = &parent->right;
        else return 0;
    }

    Node *node = allocate_node();
    if (!node) return -1;
    node->left = NULL;
    node->right = NULL;
    node->fourcc = fourcc;
    node->codec_id = codec_id;
    node->parent = parent;
    *link = node;
    if (!map->begin || fourcc < map->begin->fourcc) map->begin = node;
    rebalance_after_insert(map, node);
    ++map->size;
    return 1;
}

static Node *tree_find(const ResolveMap *map, uint32_t fourcc) {
    Node *node = map->root;
    while (node) {
        if (fourcc < node->fourcc) node = node->left;
        else if (fourcc > node->fourcc) node = node->right;
        else return node;
    }
    return NULL;
}

static int validate_subtree(const ResolveMap *map, const Node *node, const Node *parent,
                            uint64_t min_key, uint64_t max_key, size_t *visited) {
    if (!node) return 1;
    if (++*visited > map->size || node->parent != parent) return -1;
    if ((uint64_t)node->fourcc <= min_key || (uint64_t)node->fourcc >= max_key) return -1;
    if (node->color != NODE_BLACK && node->color != NODE_RED) return -1;
    if (node->color == NODE_RED &&
        ((node->left && node->left->color == NODE_RED) ||
         (node->right && node->right->color == NODE_RED))) return -1;
    int left_height = validate_subtree(map, node->left, node, min_key, node->fourcc, visited);
    int right_height = validate_subtree(map, node->right, node, node->fourcc, max_key, visited);
    if (left_height < 0 || right_height < 0 || left_height != right_height) return -1;
    return left_height + (node->color == NODE_BLACK ? 1 : 0);
}

static int validate_map(const ResolveMap *map) {
    if (!map->root) return map->begin == NULL && map->size == 0;
    if (map->root->parent != (const Node *)&map->root || map->root->color != NODE_BLACK) return 0;
    size_t visited = 0;
    if (validate_subtree(map, map->root, (const Node *)&map->root, 0, UINT64_MAX, &visited) < 0) return 0;
    Node *minimum = map->root;
    while (minimum->left) minimum = minimum->left;
    return visited == map->size && map->begin == minimum;
}

static void destroy_subtree(Node *node) {
    if (!node) return;
    destroy_subtree(node->left);
    destroy_subtree(node->right);
    free(node);
}

static void destroy_map(ResolveMap *map) {
    destroy_subtree(map->root);
    memset(map, 0, sizeof(*map));
}

static int build_map(ResolveMap *map, const CodecRecord *records, size_t count) {
    memset(map, 0, sizeof(*map));
    for (size_t i = 0; i < count; ++i) {
        if (tree_insert(map, records[i].fourcc, records[i].codec_id) != 1) return -1;
        if (!validate_map(map)) return -1;
    }
    return 0;
}

static int verify_records(const ResolveMap *map, const CodecRecord *records, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        Node *node = tree_find(map, records[i].fourcc);
        if (!node || node->codec_id != records[i].codec_id) return 0;
    }
    return 1;
}

int main(void) {
    CodecRecord expanded[72];
    memcpy(expanded, source_records, sizeof(source_records));
    expanded[71] = (CodecRecord){0x61763031u, 226u};

    ResolveMap map = {0};
    if (build_map(&map, source_records, 71) != 0 || map.size != 71 ||
        tree_find(&map, 0x61763031u) || !validate_map(&map)) {
        fprintf(stderr, "initial 71-entry map failed\\n");
        destroy_map(&map);
        return 1;
    }

    allocation_budget = -1;
    if (tree_insert(&map, 0x61763031u, 226u) != 1 || map.size != 72 ||
        !validate_map(&map) || !verify_records(&map, expanded, 72)) {
        fprintf(stderr, "single-node AV1 insertion failed\\n");
        destroy_map(&map);
        return 2;
    }

    Node *av1 = tree_find(&map, 0x61763031u);
    if (!av1 || av1->codec_id != 226u || (av1->color != NODE_RED && av1 != map.root)) {
        fprintf(stderr, "AV1 key/value or insertion color invalid\\n");
        destroy_map(&map);
        return 3;
    }

    if (tree_insert(&map, 0x61763031u, 999u) != 0 || map.size != 72 ||
        tree_find(&map, 0x61763031u)->codec_id != 226u || !validate_map(&map)) {
        fprintf(stderr, "duplicate FourCC did not preserve existing entry\\n");
        destroy_map(&map);
        return 4;
    }

    const size_t before_failure = map.size;
    allocation_budget = 0;
    const int allocation_result = tree_insert(&map, 0x7a7a7a7au, 999u);
    allocation_budget = -1;
    if (allocation_result != -1 || map.size != before_failure ||
        tree_find(&map, 0x7a7a7a7au) || !validate_map(&map)) {
        fprintf(stderr, "allocation failure changed the live map\\n");
        destroy_map(&map);
        return 5;
    }

    destroy_map(&map);

    ResolveMap minimum_map = {0};
    if (build_map(&minimum_map, source_records, 71) != 0 ||
        tree_insert(&minimum_map, 0x01020304u, 17u) != 1 ||
        minimum_map.begin != tree_find(&minimum_map, 0x01020304u) ||
        !validate_map(&minimum_map)) {
        fprintf(stderr, "begin pointer was not updated for a new minimum\n");
        destroy_map(&minimum_map);
        return 6;
    }
    destroy_map(&minimum_map);

    puts("PASS: single av01 -> 226 insertion into existing 71-node tree");
    puts("PASS: all original mappings, begin/root/parent links, size and red-black invariants");
    puts("PASS: duplicate rejection without overwrite");
    puts("PASS: allocation failure leaves the original tree unchanged");
    puts("PASS: begin pointer advances when a newly inserted key is the new minimum");
    puts("PASS: allocator memory prefilled with 0xa5; required node fields initialized by insertion/rebalance");
    puts("LIMIT: structural simulation only; Resolve allocator and rebalance function were not called");
    return 0;
}
