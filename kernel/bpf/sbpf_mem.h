#ifndef __SBPF_MEM_H__
#define __SBPF_MEM_H__

#include <linux/types.h>
#include <linux/radix-tree.h>

#define USE_RADIX_TREE 1
#define TRI_SIZE 512

struct sbpf_mm_struct {
	struct sbpf_mm_struct *parent;
#ifdef USE_RADIX_TREE
	struct radix_tree_root shadow_pages;
#else
	struct trie_node *shadow_pages;
#endif
};

struct trie_node {
	union {
		struct trie_node *trie_node[TRI_SIZE];
		uint64_t data[TRI_SIZE];
	};
};

extern const struct bpf_func_proto bpf_set_page_table_proto;
void trie_init(struct trie_node **node);
int trie_remove(struct trie_node *root, uint64_t caddr);
int trie_insert(struct trie_node *root, uint64_t caddr, uint64_t data);
uint64_t trie_search(struct trie_node *root, uint64_t caddr);
void **trie_search_node(struct trie_node *root, uint64_t caddr);
int trie_free(struct trie_node *root);

#endif