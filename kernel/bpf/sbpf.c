#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/bpf.h>
#include <linux/bpf_verifier.h>
#include <linux/sbpf.h>
#include <linux/stddef.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <asm/tlb.h>

#include "sbpf_mem.h"

int call_sbpf_function(struct bpf_prog *prog, void *arg_ptr, size_t arg_len)
{
	int ret = -EINVAL;

	if (arg_ptr != NULL &&
	    copy_from_user(current->sbpf->sbpf_func.arg, arg_ptr, arg_len)) {
		goto err;
	}

	ret = prog->bpf_func(current->sbpf->sbpf_func.arg, NULL);

err:
	return ret;
}

struct sbpf_alloc_kmem *uaddr_to_kaddr(void *uaddr, size_t len)
{
	struct page **pages;
	size_t nr_pages;
	int page_index;
	void *kaddr = NULL;
	int ret;
	struct sbpf_alloc_kmem *allocated_mem = NULL;
	struct sbpf_alloc_kmem *cur = NULL;

	if (!current->sbpf)
		return NULL;

	uaddr = untagged_addr(uaddr);
	uaddr = (void *)PAGE_ALIGN_DOWN((uint64_t)uaddr);

	list_for_each_entry(cur, &current->sbpf->alloc_kmems, list) {
		if (cur->uaddr == uaddr) {
			return cur;
		}
	}

	len = PAGE_ALIGN(len);
	nr_pages = len / PAGE_SIZE;

	pages = kmalloc_array(nr_pages, sizeof(*pages),
			      GFP_KERNEL | GFP_ATOMIC);

	ret = get_user_pages_remote(current->mm, (unsigned long)uaddr, nr_pages,
				    FOLL_WRITE, pages, NULL, NULL);
	if (ret <= 0 || ret != nr_pages)
		goto err;

	kaddr = vmap(pages, nr_pages, VM_MAP, PAGE_KERNEL);

	allocated_mem = kmalloc(sizeof(struct sbpf_alloc_kmem), GFP_KERNEL);
	if (!allocated_mem)
		goto err;

	allocated_mem->nr_pages = nr_pages;
	allocated_mem->kaddr = kaddr;
	allocated_mem->uaddr = uaddr;
	INIT_LIST_HEAD(&allocated_mem->list);

	list_add_tail(&allocated_mem->list, &current->sbpf->alloc_kmems);

err:
	for (page_index = 0; page_index < ret; page_index++)
		put_page(pages[page_index]);
	kfree(pages);

	return allocated_mem;
}

static void release_sbpf(struct task_struct *tsk, struct sbpf_task *sbpf)
{
	struct sbpf_alloc_folio *alloc_folio, *temp_alloc_folio;
	struct sbpf_alloc_kmem *alloc_kmem, *temp_alloc_kmem;
	struct mmu_gather tlb;

	if (!atomic_dec_and_test(&sbpf->ref)) {
		list_for_each_entry_safe(alloc_kmem, temp_alloc_kmem,
					 &sbpf->alloc_kmems, list) {
			vfree(alloc_kmem->kaddr);

			list_del(&alloc_kmem->list);
			kfree(alloc_kmem);
		}

		list_for_each_entry_safe(alloc_folio, temp_alloc_folio,
					 &sbpf->alloc_folios, list) {
			folio_put(alloc_folio->folio);

			list_del(&alloc_folio->list);
			kfree(alloc_folio);

			dec_mm_counter(tsk->mm, MM_ANONPAGES);
		}

		tlb_gather_mmu(&tlb, tsk->mm);
		free_pgd_range(&tlb, SBPF_USER_VADDR_START, sbpf->max_alloc_end,
			       SBPF_USER_VADDR_START,
			       sbpf->max_alloc_end + (1UL << 39));
		tlb_finish_mmu(&tlb);
	}
}

int copy_sbpf(struct task_struct *tsk)
{
	if (current->sbpf) {
		tsk->sbpf = current->sbpf;
		atomic_inc(&current->sbpf->ref);
	}

	return 0;
}

static void bpf_sbpf_link_release(struct bpf_link *link)
{
	return;
}

static void bpf_sbpf_link_dealloc(struct bpf_link *link)
{
	return;
}

static const struct bpf_link_ops bpf_sbpf_link_ops = {
	.release = bpf_sbpf_link_release,
	.dealloc = bpf_sbpf_link_dealloc,
};

BPF_CALL_2(bpf_get_shared_page, void *, kaddr, size_t, len)
{
	struct sbpf_alloc_kmem *cur;
	off_t offset;

	if (current->sbpf == NULL || kaddr == NULL)
		return 0;

	offset = kaddr - (void *)PAGE_ALIGN_DOWN((uint64_t)kaddr);

	list_for_each_entry(cur, &current->sbpf->alloc_kmems, list) {
		if (cur->kaddr == (kaddr - offset) &&
		    (len + offset) < PAGE_SIZE) {
			return (unsigned long)(cur->kaddr + offset);
		}
	}

	return 0;
}

const struct bpf_func_proto bpf_get_shared_page_proto = {
	.func = bpf_get_shared_page,
	.gpl_only = false,
	.ret_type = RET_PTR_TO_DYNPTR_MEM_OR_NULL,
	.arg1_type = ARG_ANYTHING,
	.arg2_type = ARG_CONST_ALLOC_SIZE_OR_ZERO,
};

BPF_CALL_2(bpf_uaddr_to_kaddr, void *, uaddr, size_t, len)
{
	struct sbpf_alloc_kmem *allocated_page;

	allocated_page = uaddr_to_kaddr(uaddr, len);
	if (allocated_page != NULL && allocated_page->kaddr) {
		return (unsigned long)(allocated_page->kaddr +
				       (uaddr - allocated_page->uaddr));
	}

	return 0;
}

const struct bpf_func_proto bpf_uaddr_to_kaddr_proto = {
	.func = bpf_uaddr_to_kaddr,
	.gpl_only = false,
	.ret_type = RET_PTR_TO_DYNPTR_MEM_OR_NULL,
	.arg1_type = ARG_ANYTHING,
	.arg2_type = ARG_CONST_ALLOC_SIZE_OR_ZERO,
};

static const struct bpf_func_proto *
bpf_sbpf_func_proto(enum bpf_func_id func_id, const struct bpf_prog *prog)
{
	switch (func_id) {
	case BPF_FUNC_uaddr_to_kaddr:
		return &bpf_uaddr_to_kaddr_proto;
	case BPF_FUNC_get_shared_page:
		return &bpf_get_shared_page_proto;
	case BPF_FUNC_set_page_table:
		return &bpf_set_page_table_proto;
	default:
		return bpf_base_func_proto(func_id);
	}
}

int bpf_sbpf_link_attach(const union bpf_attr *attr, struct bpf_prog *prog)
{
	struct bpf_link_primer link_primer;
	struct bpf_sbpf_link *link;
	struct sbpf_alloc_kmem *aux_page;
	off_t offset;
	int err;

	if (attr->link_create.attach_type == BPF_SBPF_PAGE_FAULT) {
		if (attr->link_create.sbpf.aux_ptr) {
			aux_page = uaddr_to_kaddr(
				attr->link_create.sbpf.aux_ptr, PAGE_SIZE);
			offset = attr->link_create.sbpf.aux_ptr -
				 aux_page->uaddr;
			current->sbpf->mm.aux = aux_page->kaddr + offset;
		} else {
			current->sbpf->mm.aux = kmalloc(PAGE_SIZE, GFP_KERNEL);
		}
		current->sbpf->mm.prog = prog;
	} else if (attr->link_create.attach_type == BPF_SBPF_FUNCTION) {
		current->sbpf->sbpf_func.prog = prog;
		current->sbpf->sbpf_func.arg =
			kmalloc(PAGE_SIZE, GFP_KERNEL | __GFP_ZERO);
	}

	link = kzalloc(sizeof(*link), GFP_USER);
	if (!link) {
		err = -ENOMEM;
		goto out_sbpf;
	}
	bpf_link_init(&link->link, BPF_LINK_TYPE_SBPF, &bpf_sbpf_link_ops,
		      prog);
	link->type = attr->link_create.attach_type;
	err = bpf_link_prime(&link->link, &link_primer);
	if (err) {
		kfree(link);
		goto out_sbpf;
	}

	return bpf_link_settle(&link_primer);

out_sbpf:

	return err;
}

bool verify_sbpf(int off, int size, enum bpf_access_type type,
		 const struct bpf_prog *prog, struct bpf_insn_access_aux *info)
{
	if (off < 0 || off + size > PAGE_SIZE)
		return false;

	return true;
}

const struct bpf_verifier_ops sbpf_verifier_ops = {
	.get_func_proto = bpf_sbpf_func_proto,
	.is_valid_access = verify_sbpf,
};

const struct bpf_prog_ops sbpf_prog_ops = {

};

int bpf_prog_load_sbpf(struct bpf_prog *prog)
{
	if (current->sbpf)
		return 0;

	current->sbpf = kmalloc(sizeof(struct sbpf_task), GFP_KERNEL);
	if (current->sbpf == NULL)
		return -ENOMEM;

	INIT_LIST_HEAD(&current->sbpf->alloc_kmems);
	INIT_LIST_HEAD(&current->sbpf->alloc_folios);

	return 0;
}

void exit_sbpf(struct task_struct *tsk)
{
	if (!tsk->sbpf)
		return;

	release_sbpf(tsk, tsk->sbpf);
}
