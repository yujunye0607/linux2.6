/*
 *  linux/kernel/fork.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 *  'fork.c' contains the help-routines for the 'fork' system call
 * (see also entry.S and others).
 * Fork is rather simple, once you get the hang of it, but the memory
 * management can be a bitch. See 'mm/memory.c': 'copy_page_range()'
 */

#include <linux/config.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/unistd.h>
#include <linux/smp_lock.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/completion.h>
#include <linux/namespace.h>
#include <linux/personality.h>
#include <linux/mempolicy.h>
#include <linux/sem.h>
#include <linux/file.h>
#include <linux/key.h>
#include <linux/binfmts.h>
#include <linux/mman.h>
#include <linux/fs.h>
#include <linux/cpu.h>
#include <linux/security.h>
#include <linux/swap.h>
#include <linux/syscalls.h>
#include <linux/jiffies.h>
#include <linux/futex.h>
#include <linux/ptrace.h>
#include <linux/mount.h>
#include <linux/audit.h>
#include <linux/profile.h>
#include <linux/rmap.h>
#include <linux/acct.h>

#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/uaccess.h>
#include <asm/mmu_context.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>

/*
 * Protected counters by write_lock_irq(&tasklist_lock)
 */
unsigned long total_forks;	/* Handle normal Linux uptimes. */
int nr_threads; 		/* The idle threads do not count.. */

int max_threads;		/* tunable limit on nr_threads */

DEFINE_PER_CPU(unsigned long, process_counts) = 0;

 __cacheline_aligned DEFINE_RWLOCK(tasklist_lock);  /* outer */

EXPORT_SYMBOL(tasklist_lock);

int nr_processes(void)
{
	int cpu;
	int total = 0;

	for_each_online_cpu(cpu)
		total += per_cpu(process_counts, cpu);

	return total;
}

#ifndef __HAVE_ARCH_TASK_STRUCT_ALLOCATOR
# define alloc_task_struct()	kmem_cache_alloc(task_struct_cachep, GFP_KERNEL)
# define free_task_struct(tsk)	kmem_cache_free(task_struct_cachep, (tsk))
static kmem_cache_t *task_struct_cachep;
#endif

void free_task(struct task_struct *tsk)
{
	free_thread_info(tsk->thread_info);
	free_task_struct(tsk);
}
EXPORT_SYMBOL(free_task);

void __put_task_struct(struct task_struct *tsk)
{
	WARN_ON(!(tsk->exit_state & (EXIT_DEAD | EXIT_ZOMBIE)));
	WARN_ON(atomic_read(&tsk->usage));
	WARN_ON(tsk == current);

	if (unlikely(tsk->audit_context))
		audit_free(tsk);
	security_task_free(tsk);
	free_uid(tsk->user);
	put_group_info(tsk->group_info);

	if (!profile_handoff_task(tsk))
		free_task(tsk);
}

void __init fork_init(unsigned long mempages)
{
#ifndef __HAVE_ARCH_TASK_STRUCT_ALLOCATOR
#ifndef ARCH_MIN_TASKALIGN
#define ARCH_MIN_TASKALIGN	L1_CACHE_BYTES
#endif
	/* create a slab on which task_structs can be allocated */
	task_struct_cachep =
		kmem_cache_create("task_struct", sizeof(struct task_struct),
			ARCH_MIN_TASKALIGN, SLAB_PANIC, NULL, NULL);
#endif

	/*
	 * The default maximum number of threads is set to a safe
	 * value: the thread structures can take up at most half
	 * of memory.
	 */
	max_threads = mempages / (8 * THREAD_SIZE / PAGE_SIZE);

	/*
	 * we need to allow at least 20 threads to boot a system
	 */
	if(max_threads < 20)
		max_threads = 20;

	init_task.signal->rlim[RLIMIT_NPROC].rlim_cur = max_threads/2;
	init_task.signal->rlim[RLIMIT_NPROC].rlim_max = max_threads/2;
}

static struct task_struct *dup_task_struct(struct task_struct *orig)
{
	struct task_struct *tsk;
	struct thread_info *ti;

	prepare_to_copy(orig);

	tsk = alloc_task_struct();
	if (!tsk)
		return NULL;

	ti = alloc_thread_info(tsk);
	if (!ti) {
		free_task_struct(tsk);
		return NULL;
	}

	*ti = *orig->thread_info;
	*tsk = *orig;
	tsk->thread_info = ti;
	ti->task = tsk;

	/* One for us, one for whoever does the "release_task()" (usually parent) */
	atomic_set(&tsk->usage,2);
	return tsk;
}

#ifdef CONFIG_MMU
static inline int dup_mmap(struct mm_struct * mm, struct mm_struct * oldmm)
{
	struct vm_area_struct * mpnt, *tmp, **pprev;
	struct rb_node **rb_link, *rb_parent;
	int retval;
	unsigned long charge;
	struct mempolicy *pol;

	down_write(&oldmm->mmap_sem);
	flush_cache_mm(current->mm);
	mm->locked_vm = 0;
	mm->mmap = NULL;
	mm->mmap_cache = NULL;
	mm->free_area_cache = oldmm->mmap_base;
	mm->map_count = 0;
	mm->rss = 0;
	mm->anon_rss = 0;
	cpus_clear(mm->cpu_vm_mask);
	mm->mm_rb = RB_ROOT;
	rb_link = &mm->mm_rb.rb_node;
	rb_parent = NULL;
	pprev = &mm->mmap;

	for (mpnt = current->mm->mmap ; mpnt ; mpnt = mpnt->vm_next) {
		struct file *file;

		if (mpnt->vm_flags & VM_DONTCOPY) {
			__vm_stat_account(mm, mpnt->vm_flags, mpnt->vm_file,
							-vma_pages(mpnt));
			continue;
		}
		charge = 0;
		if (mpnt->vm_flags & VM_ACCOUNT) {
			unsigned int len = (mpnt->vm_end - mpnt->vm_start) >> PAGE_SHIFT;
			if (security_vm_enough_memory(len))
				goto fail_nomem;
			charge = len;
		}
		tmp = kmem_cache_alloc(vm_area_cachep, SLAB_KERNEL);
		if (!tmp)
			goto fail_nomem;
		*tmp = *mpnt;
		pol = mpol_copy(vma_policy(mpnt));
		retval = PTR_ERR(pol);
		if (IS_ERR(pol))
			goto fail_nomem_policy;
		vma_set_policy(tmp, pol);
		tmp->vm_flags &= ~VM_LOCKED;
		tmp->vm_mm = mm;
		tmp->vm_next = NULL;
		anon_vma_link(tmp);
		file = tmp->vm_file;
		if (file) {
			struct inode *inode = file->f_dentry->d_inode;
			get_file(file);
			if (tmp->vm_flags & VM_DENYWRITE)
				atomic_dec(&inode->i_writecount);
      
			/* insert tmp into the share list, just after mpnt */
			spin_lock(&file->f_mapping->i_mmap_lock);
			tmp->vm_truncate_count = mpnt->vm_truncate_count;
			flush_dcache_mmap_lock(file->f_mapping);
			vma_prio_tree_add(tmp, mpnt);
			flush_dcache_mmap_unlock(file->f_mapping);
			spin_unlock(&file->f_mapping->i_mmap_lock);
		}

		/*
		 * Link in the new vma and copy the page table entries:
		 * link in first so that swapoff can see swap entries,
		 * and try_to_unmap_one's find_vma find the new vma.
		 */
		spin_lock(&mm->page_table_lock);
		*pprev = tmp;
		pprev = &tmp->vm_next;

		__vma_link_rb(mm, tmp, rb_link, rb_parent);
		rb_link = &tmp->vm_rb.rb_right;
		rb_parent = &tmp->vm_rb;

		mm->map_count++;
		retval = copy_page_range(mm, current->mm, tmp);
		spin_unlock(&mm->page_table_lock);

		if (tmp->vm_ops && tmp->vm_ops->open)
			tmp->vm_ops->open(tmp);

		if (retval)
			goto out;
	}
	retval = 0;

out:
	flush_tlb_mm(current->mm);
	up_write(&oldmm->mmap_sem);
	return retval;
fail_nomem_policy:
	kmem_cache_free(vm_area_cachep, tmp);
fail_nomem:
	retval = -ENOMEM;
	vm_unacct_memory(charge);
	goto out;
}

static inline int mm_alloc_pgd(struct mm_struct * mm)
{
	mm->pgd = pgd_alloc(mm);
	if (unlikely(!mm->pgd))
		return -ENOMEM;
	return 0;
}

static inline void mm_free_pgd(struct mm_struct * mm)
{
	pgd_free(mm->pgd);
}
#else
#define dup_mmap(mm, oldmm)	(0)
#define mm_alloc_pgd(mm)	(0)
#define mm_free_pgd(mm)
#endif /* CONFIG_MMU */

 __cacheline_aligned_in_smp DEFINE_SPINLOCK(mmlist_lock);

#define allocate_mm()	(kmem_cache_alloc(mm_cachep, SLAB_KERNEL))
#define free_mm(mm)	(kmem_cache_free(mm_cachep, (mm)))

#include <linux/init_task.h>

static struct mm_struct * mm_init(struct mm_struct * mm)
{
	atomic_set(&mm->mm_users, 1);
	atomic_set(&mm->mm_count, 1);
	init_rwsem(&mm->mmap_sem);
	INIT_LIST_HEAD(&mm->mmlist);
	mm->core_waiters = 0;
	mm->nr_ptes = 0;
	spin_lock_init(&mm->page_table_lock);
	rwlock_init(&mm->ioctx_list_lock);
	mm->ioctx_list = NULL;
	mm->default_kioctx = (struct kioctx)INIT_KIOCTX(mm->default_kioctx, *mm);
	mm->free_area_cache = TASK_UNMAPPED_BASE;

	if (likely(!mm_alloc_pgd(mm))) {
		mm->def_flags = 0;
		return mm;
	}
	free_mm(mm);
	return NULL;
}

/*
 * Allocate and initialize an mm_struct.
 */
struct mm_struct * mm_alloc(void)
{
	struct mm_struct * mm;

	mm = allocate_mm();
	if (mm) {
		memset(mm, 0, sizeof(*mm));
		mm = mm_init(mm);
	}
	return mm;
}

/*
 * Called when the last reference to the mm
 * is dropped: either by a lazy thread or by
 * mmput. Free the page directory and the mm.
 */
void fastcall __mmdrop(struct mm_struct *mm)
{
	BUG_ON(mm == &init_mm);
	mm_free_pgd(mm);
	destroy_context(mm);
	free_mm(mm);
}

/*
 * Decrement the use count and release all resources for an mm.
 */
void mmput(struct mm_struct *mm)
{
	if (atomic_dec_and_test(&mm->mm_users)) {
		exit_aio(mm);
		exit_mmap(mm);
		if (!list_empty(&mm->mmlist)) {
			spin_lock(&mmlist_lock);
			list_del(&mm->mmlist);
			spin_unlock(&mmlist_lock);
		}
		put_swap_token(mm);
		mmdrop(mm);
	}
}
EXPORT_SYMBOL_GPL(mmput);

/**
 * get_task_mm - acquire a reference to the task's mm
 *
 * Returns %NULL if the task has no mm.  Checks PF_BORROWED_MM (meaning
 * this kernel workthread has transiently adopted a user mm with use_mm,
 * to do its AIO) is not set and if so returns a reference to it, after
 * bumping up the use count.  User must release the mm via mmput()
 * after use.  Typically used by /proc and ptrace.
 */
struct mm_struct *get_task_mm(struct task_struct *task)
{
	struct mm_struct *mm;

	task_lock(task);
	mm = task->mm;
	if (mm) {
		if (task->flags & PF_BORROWED_MM)
			mm = NULL;
		else
			atomic_inc(&mm->mm_users);
	}
	task_unlock(task);
	return mm;
}
EXPORT_SYMBOL_GPL(get_task_mm);

/* Please note the differences between mmput and mm_release.
 * mmput is called whenever we stop holding onto a mm_struct,
 * error success whatever.
 *
 * mm_release is called after a mm_struct has been removed
 * from the current process.
 *
 * This difference is important for error handling, when we
 * only half set up a mm_struct for a new process and need to restore
 * the old one.  Because we mmput the new mm_struct before
 * restoring the old one. . .
 * Eric Biederman 10 January 1998
 */
void mm_release(struct task_struct *tsk, struct mm_struct *mm)
{
	struct completion *vfork_done = tsk->vfork_done;

	/* Get rid of any cached register state */
	deactivate_mm(tsk, mm);

	/* notify parent sleeping on vfork() */
	if (vfork_done) {
		tsk->vfork_done = NULL;
		complete(vfork_done);
	}
	if (tsk->clear_child_tid && atomic_read(&mm->mm_users) > 1) {
		u32 __user * tidptr = tsk->clear_child_tid;
		tsk->clear_child_tid = NULL;

		/*
		 * We don't check the error code - if userspace has
		 * not set up a proper pointer then tough luck.
		 */
		put_user(0, tidptr);
		sys_futex(tidptr, FUTEX_WAKE, 1, NULL, NULL, 0);
	}
}

static int copy_mm(unsigned long clone_flags, struct task_struct * tsk)
{
	struct mm_struct * mm, *oldmm;
	int retval;

	tsk->min_flt = tsk->maj_flt = 0;
	tsk->nvcsw = tsk->nivcsw = 0;

	tsk->mm = NULL;
	tsk->active_mm = NULL;

	/*
	 * Are we cloning a kernel thread?
	 *
	 * We need to steal a active VM for that..
	 */
	oldmm = current->mm;
	if (!oldmm)
		return 0;

	if (clone_flags & CLONE_VM) {
		atomic_inc(&oldmm->mm_users);
		mm = oldmm;
		/*
		 * There are cases where the PTL is held to ensure no
		 * new threads start up in user mode using an mm, which
		 * allows optimizing out ipis; the tlb_gather_mmu code
		 * is an example.
		 */
		spin_unlock_wait(&oldmm->page_table_lock);
		goto good_mm;
	}

	retval = -ENOMEM;
	mm = allocate_mm();
	if (!mm)
		goto fail_nomem;

	/* Copy the current MM stuff.. */
	memcpy(mm, oldmm, sizeof(*mm));
	if (!mm_init(mm))
		goto fail_nomem;

	if (init_new_context(tsk,mm))
		goto fail_nocontext;

	retval = dup_mmap(mm, oldmm);
	if (retval)
		goto free_pt;

	mm->hiwater_rss = mm->rss;
	mm->hiwater_vm = mm->total_vm;

good_mm:
	tsk->mm = mm;
	tsk->active_mm = mm;
	return 0;

free_pt:
	mmput(mm);
fail_nomem:
	return retval;

fail_nocontext:
	/*
	 * If init_new_context() failed, we cannot use mmput() to free the mm
	 * because it calls destroy_context()
	 */
	mm_free_pgd(mm);
	free_mm(mm);
	return retval;
}

static inline struct fs_struct *__copy_fs_struct(struct fs_struct *old)
{
	struct fs_struct *fs = kmem_cache_alloc(fs_cachep, GFP_KERNEL);
	/* We don't need to lock fs - think why ;-) */
	if (fs) {
		atomic_set(&fs->count, 1);
		rwlock_init(&fs->lock);
		fs->umask = old->umask;
		read_lock(&old->lock);
		fs->rootmnt = mntget(old->rootmnt);
		fs->root = dget(old->root);
		fs->pwdmnt = mntget(old->pwdmnt);
		fs->pwd = dget(old->pwd);
		if (old->altroot) {
			fs->altrootmnt = mntget(old->altrootmnt);
			fs->altroot = dget(old->altroot);
		} else {
			fs->altrootmnt = NULL;
			fs->altroot = NULL;
		}
		read_unlock(&old->lock);
	}
	return fs;
}

struct fs_struct *copy_fs_struct(struct fs_struct *old)
{
	return __copy_fs_struct(old);
}

EXPORT_SYMBOL_GPL(copy_fs_struct);

static inline int copy_fs(unsigned long clone_flags, struct task_struct * tsk)
{
	if (clone_flags & CLONE_FS) {
		atomic_inc(&current->fs->count);
		return 0;
	}
	tsk->fs = __copy_fs_struct(current->fs);
	if (!tsk->fs)
		return -ENOMEM;
	return 0;
}

static int count_open_files(struct files_struct *files, int size)
{
	int i;

	/* Find the last open fd */
	for (i = size/(8*sizeof(long)); i > 0; ) {
		if (files->open_fds->fds_bits[--i])
			break;
	}
	i = (i+1) * 8 * sizeof(long);
	return i;
}

static int copy_files(unsigned long clone_flags, struct task_struct * tsk)
{
	struct files_struct *oldf, *newf;
	struct file **old_fds, **new_fds;
	int open_files, size, i, error = 0, expand;

	/*
	 * A background process may not have any files ...
	 */
	oldf = current->files;
	if (!oldf)
		goto out;

	if (clone_flags & CLONE_FILES) {
		atomic_inc(&oldf->count);
		goto out;
	}

	/*
	 * Note: we may be using current for both targets (See exec.c)
	 * This works because we cache current->files (old) as oldf. Don't
	 * break this.
	 */
	tsk->files = NULL;
	error = -ENOMEM;
	newf = kmem_cache_alloc(files_cachep, SLAB_KERNEL);
	if (!newf) 
		goto out;

	atomic_set(&newf->count, 1);

	spin_lock_init(&newf->file_lock);
	newf->next_fd	    = 0;
	newf->max_fds	    = NR_OPEN_DEFAULT;
	newf->max_fdset	    = __FD_SETSIZE;
	newf->close_on_exec = &newf->close_on_exec_init;
	newf->open_fds	    = &newf->open_fds_init;
	newf->fd	    = &newf->fd_array[0];

	spin_lock(&oldf->file_lock);

	open_files = count_open_files(oldf, oldf->max_fdset);
	expand = 0;

	/*
	 * Check whether we need to allocate a larger fd array or fd set.
	 * Note: we're not a clone task, so the open count won't  change.
	 */
	if (open_files > newf->max_fdset) {
		newf->max_fdset = 0;
		expand = 1;
	}
	if (open_files > newf->max_fds) {
		newf->max_fds = 0;
		expand = 1;
	}

	/* if the old fdset gets grown now, we'll only copy up to "size" fds */
	if (expand) {
		spin_unlock(&oldf->file_lock);
		spin_lock(&newf->file_lock);
		error = expand_files(newf, open_files-1);
		spin_unlock(&newf->file_lock);
		if (error < 0)
			goto out_release;
		spin_lock(&oldf->file_lock);
	}

	old_fds = oldf->fd;
	new_fds = newf->fd;

	memcpy(newf->open_fds->fds_bits, oldf->open_fds->fds_bits, open_files/8);
	memcpy(newf->close_on_exec->fds_bits, oldf->close_on_exec->fds_bits, open_files/8);

	for (i = open_files; i != 0; i--) {
		struct file *f = *old_fds++;
		if (f) {
			get_file(f);
		} else {
			/*
			 * The fd may be claimed in the fd bitmap but not yet
			 * instantiated in the files array if a sibling thread
			 * is partway through open().  So make sure that this
			 * fd is available to the new process.
			 */
			FD_CLR(open_files - i, newf->open_fds);
		}
		*new_fds++ = f;
	}
	spin_unlock(&oldf->file_lock);

	/* compute the remainder to be cleared */
	size = (newf->max_fds - open_files) * sizeof(struct file *);

	/* This is long word aligned thus could use a optimized version */ 
	memset(new_fds, 0, size); 

	if (newf->max_fdset > open_files) {
		int left = (newf->max_fdset-open_files)/8;
		int start = open_files / (8 * sizeof(unsigned long));

		memset(&newf->open_fds->fds_bits[start], 0, left);
		memset(&newf->close_on_exec->fds_bits[start], 0, left);
	}

	tsk->files = newf;
	error = 0;
out:
	return error;

out_release:
	free_fdset (newf->close_on_exec, newf->max_fdset);
	free_fdset (newf->open_fds, newf->max_fdset);
	free_fd_array(newf->fd, newf->max_fds);
	kmem_cache_free(files_cachep, newf);
	goto out;
}

/*
 *	Helper to unshare the files of the current task.
 *	We don't want to expose copy_files internals to
 *	the exec layer of the kernel.
 */

int unshare_files(void)
{
	struct files_struct *files  = current->files;
	int rc;

	if(!files)
		BUG();

	/* This can race but the race causes us to copy when we don't
	   need to and drop the copy */
	if(atomic_read(&files->count) == 1)
	{
		atomic_inc(&files->count);
		return 0;
	}
	rc = copy_files(0, current);
	if(rc)
		current->files = files;
	return rc;
}

EXPORT_SYMBOL(unshare_files);

static inline int copy_sighand(unsigned long clone_flags, struct task_struct * tsk)
{
	struct sighand_struct *sig;

	if (clone_flags & (CLONE_SIGHAND | CLONE_THREAD)) {
		atomic_inc(&current->sighand->count);
		return 0;
	}
	sig = kmem_cache_alloc(sighand_cachep, GFP_KERNEL);
	tsk->sighand = sig;
	if (!sig)
		return -ENOMEM;
	spin_lock_init(&sig->siglock);
	atomic_set(&sig->count, 1);
	memcpy(sig->action, current->sighand->action, sizeof(sig->action));
	return 0;
}

static inline int copy_signal(unsigned long clone_flags, struct task_struct * tsk)
{
	struct signal_struct *sig;

	if (clone_flags & CLONE_THREAD) {
		atomic_inc(&current->signal->count);
		atomic_inc(&current->signal->live);
		return 0;
	}
	sig = kmem_cache_alloc(signal_cachep, GFP_KERNEL);
	tsk->signal = sig;
	if (!sig)
		return -ENOMEM;
	atomic_set(&sig->count, 1);
	atomic_set(&sig->live, 1);
	init_waitqueue_head(&sig->wait_chldexit);
	sig->flags = 0;
	sig->group_exit_code = 0;
	sig->group_exit_task = NULL;
	sig->group_stop_count = 0;
	sig->curr_target = NULL;
	init_sigpending(&sig->shared_pending);
	INIT_LIST_HEAD(&sig->posix_timers);

	sig->tty = current->signal->tty;
	sig->pgrp = process_group(current);
	sig->session = current->signal->session;
	sig->leader = 0;	/* session leadership doesn't inherit */
	sig->tty_old_pgrp = 0;

	sig->utime = sig->stime = sig->cutime = sig->cstime = cputime_zero;
	sig->nvcsw = sig->nivcsw = sig->cnvcsw = sig->cnivcsw = 0;
	sig->min_flt = sig->maj_flt = sig->cmin_flt = sig->cmaj_flt = 0;

	task_lock(current->group_leader);
	memcpy(sig->rlim, current->signal->rlim, sizeof sig->rlim);
	task_unlock(current->group_leader);

	return 0;
}

static inline void copy_flags(unsigned long clone_flags, struct task_struct *p)
{
	unsigned long new_flags = p->flags;

	new_flags &= ~PF_SUPERPRIV;
	new_flags |= PF_FORKNOEXEC;
	if (!(clone_flags & CLONE_PTRACE))
		p->ptrace = 0;
	p->flags = new_flags;
}

asmlinkage long sys_set_tid_address(int __user *tidptr)
{
	current->clear_child_tid = tidptr;

	return current->pid;
}

/*
 * This creates a new process as a copy of the old one,
 * but does not actually start it yet.
 *
 * It copies the registers, and all the appropriate
 * parts of the process environment (as per the clone
 * flags). The actual kick-off is left to the caller.
 */
static task_t *copy_process(unsigned long clone_flags,
				 unsigned long stack_start,
				 struct pt_regs *regs,
				 unsigned long stack_size,
				 int __user *parent_tidptr,
				 int __user *child_tidptr,
				 int pid)
{
	int retval;
	struct task_struct *p = NULL;

	/* 1.检查参数clone_flags所传递标志的一致性。尤其是，在下列情况下，它返回错
    误代号：
    a．CLONE_NEWNS 和CLONE_FS 标志都被设置。
    b.CLONE_THREAD标志被设置，但CLONE_SIGHAND标志被清O（同一线程组中的轻量级进程必须共享信号）。
    c.CLONE_SIGHAND标志被设置，但CLONE_VM被清O（共享信号处理程序的轻量级进程也必须共享内存描述符） */
	if ((clone_flags & (CLONE_NEWNS|CLONE_FS)) == (CLONE_NEWNS|CLONE_FS))
		return ERR_PTR(-EINVAL);

	/*
	 * Thread groups must share signals as well, and detached threads
	 * can only be started up within the thread group.
	 */
	if ((clone_flags & CLONE_THREAD) && !(clone_flags & CLONE_SIGHAND))
		return ERR_PTR(-EINVAL);

	/*
	 * Shared signal handlers imply shared VM. By way of the above,
	 * thread groups also imply shared VM. Blocking this case allows
	 * for various simplifications in other code.
	 */
	if ((clone_flags & CLONE_SIGHAND) && !(clone_flags & CLONE_VM))
		return ERR_PTR(-EINVAL);
	/* 通过调用security_task_create（）以及稍后调用的security_task_alloc（）执行所有附加的安全检查。
	Linux2.6提供扩展安全性的钩子函数，与传统Unix相比，它具有更加强壮的安全模型。详情参见第二十章。*/
	retval = security_task_create(clone_flags);
	if (retval)
		goto fork_out;

	retval = -ENOMEM;
	/* 调用dup_task_struct（）为子进程获取进程描述符。该函数执行如下操作：
	a.如果需要，则在当前进程中调用__unlazy_fpu（），把FPU、MMX和SSE/SSE2寄存器的内容保存到父进程的thread_info结构中。
	  稍后，dup_task_struct（）将把这些值复制到子进程的thread_info结构中。
	b.执行alloc_task_struct（）宏，为新进程获取进程描述符（task_struct结构），并将描述符地址保存在tsk局部变量中。
	c.执行alloc_thread_info宏以获取一块空闲内存区，用来存放新进程的thread_info结构和内核栈，并将这块内存区字段的地址存在局部变量ti中。
	  正如在本章前面“标识一个进程”一节中所述：这块内存区字段的大小是8KB或4KB。
	d.将current进程描述符的内容复制到tsk所指向的task_struct结构中，然后把tsk->thread_info置为ti。
	e.把current进程的thread_info描述符的内容复制到ti所指向的结构中，然后把ti->task置为tsk。
	f.把新进程描述符的使用计数器（tsk->usage）置为2，用来表示进程描述符正在被使用而且其相应的进程处于活动状态（进程状态即不是EXIT_ZOMBIE，
      也不是EXIT_DEAD)。
	g.返回新进程的进程描述符指针（tsk）。*/
	p = dup_task_struct(current);
	if (!p)
		goto fork_out;

	retval = -EAGAIN;
	/* 4.检查存放在current->Signal->rlim[RLIMIT_NPROC].rlim_cur变量中的值是否
    小于或等于用户所拥有的进程数。如果是，则返回错误码，除非进程没有root权
    限。该函数从每用户数据结构user_struct中获取用户所拥有的进程数。通过进
    程描述符user字段的指针可以找到这个数据结构。
 */
	if (atomic_read(&p->user->processes) >=
			p->signal->rlim[RLIMIT_NPROC].rlim_cur) {
		if (!capable(CAP_SYS_ADMIN) && !capable(CAP_SYS_RESOURCE) &&
				p->user != &root_user)
			goto bad_fork_free;
	}
	/* 5.递增user_struct结构的使用计数器（tsk->user->__count字段）和用户所拥有的进程的计数器（tsk->user->processes）。 */
	atomic_inc(&p->user->__count);
	atomic_inc(&p->user->processes);
	get_group_info(p->group_info);

	/*
	 * If multiple threads are within copy_process(), then this check
	 * triggers too late. This doesn't hurt, the check is only there
	 * to stop root fork bombs.
	 */
	/* 6.检查系统中的进程数量（存放在nr_threads变量中）是否超过max_threads变量的值。
		 这个变量的缺省值取决于系统内存容量的大小。总的原则是：所有thread_info描述符和内核栈所占用的空间不能超过物理内存大小的1/8。
		 不过，系统管理员可以通过写/proc/sys/kernel/threads-max文件来改变这个值。 */
	if (nr_threads >= max_threads)
		goto bad_fork_cleanup_count;
	/* 7.如果实现新进程的执行域和可执行格式的内核函数（参见第二十章）都包含在内核
    模块中，则递增它们的使用计数器（参见附录二）。 */
	if (!try_module_get(p->thread_info->exec_domain->module))
		goto bad_fork_cleanup_count;

	if (p->binfmt && !try_module_get(p->binfmt->module))
		goto bad_fork_cleanup_put_domain;

	/* 8.设置与进程状态相关的几个关键字段：
    a.把tsk->did_exec字段初始化为O：它记录了进程发出的execve（）系统调用的次数。
    b.更新从父进程复制到tsk->flags字段中的一些标志：首先清除PF_SUPERPRIV标志，该标志表示进程是否使用了某种超级用户权限。
	  然后设置PF_FORKNOEXEC标志，它表示子进程还没有发出execve（）系统调用。 */
	p->did_exec = 0;
	copy_flags(clone_flags, p);
	/* 9.把新进程的PID存人tsk->pid字段。 */
	p->pid = pid;
	retval = -EFAULT;
	/* 10.如果设置了 CLONE_PARENT_SETTID，就把子进程的PID复制到参数parent_tidptr指向的用户态变量中。
	   用于线程库（如 NPTL）获取新线程的 ID */
	if (clone_flags & CLONE_PARENT_SETTID)
		if (put_user(p->pid, parent_tidptr))
			goto bad_fork_cleanup;

	/* 11.初始化子进程描述符中的list_head数据结构和自旋锁，并为与挂起信号、定时器及时间统计表相关的几个字段赋初值。*/
	p->proc_dentry = NULL;
	//  链表初始化
	INIT_LIST_HEAD(&p->children);
	INIT_LIST_HEAD(&p->sibling);
	p->vfork_done = NULL;
	spin_lock_init(&p->alloc_lock);
	spin_lock_init(&p->proc_lock);

	//  信号相关初始化
	clear_tsk_thread_flag(p, TIF_SIGPENDING);
	init_sigpending(&p->pending);

	//  定时器相关初始化
	p->it_real_value = 0;
	p->it_real_incr = 0;
	p->it_virt_value = cputime_zero;
	p->it_virt_incr = cputime_zero;
	p->it_prof_value = cputime_zero;
	p->it_prof_incr = cputime_zero;
	init_timer(&p->real_timer);
	p->real_timer.data = (unsigned long) p;

	//   I/O 统计清零
	p->utime = cputime_zero;
	p->stime = cputime_zero;
	p->rchar = 0;		/* I/O counter: bytes read */
	p->wchar = 0;		/* I/O counter: bytes written */
	p->syscr = 0;		/* I/O counter: read syscalls */
	p->syscw = 0;		/* I/O counter: write syscalls */
	acct_clear_integrals(p);
	
	/*把大内核锁计数器tsk->lock_depth初始化为-1（参见第五章“大内核锁”一节)。 */
	p->lock_depth = -1;		/* -1 = no lock */
	do_posix_clock_monotonic_gettime(&p->start_time);
	p->security = NULL;
	p->io_context = NULL;
	p->io_wait = NULL;
	p->audit_context = NULL;
#ifdef CONFIG_NUMA
 	p->mempolicy = mpol_copy(p->mempolicy);
 	if (IS_ERR(p->mempolicy)) {
 		retval = PTR_ERR(p->mempolicy);
 		p->mempolicy = NULL;
 		goto bad_fork_cleanup;
 	}
#endif

	//  线程组相关初始化
	/*  */
	p->tgid = p->pid;
	if (clone_flags & CLONE_THREAD)
		p->tgid = current->tgid;

	// 核心资源复制
	/* 12.调用copy_semundo（）等来创建新的数据结构，并把父进程相应数据结构的值复制到新数据结构中
	除非clone_flags参数指出它们有不同的值。 */
	if ((retval = security_task_alloc(p)))
		goto bad_fork_cleanup_policy;
	if ((retval = audit_alloc(p)))
		goto bad_fork_cleanup_security;
	/* copy all the process information */
	if ((retval = copy_semundo(clone_flags, p)))
		goto bad_fork_cleanup_audit;
	if ((retval = copy_files(clone_flags, p)))
		goto bad_fork_cleanup_semundo;
	if ((retval = copy_fs(clone_flags, p)))
		goto bad_fork_cleanup_files;
	if ((retval = copy_sighand(clone_flags, p)))
		goto bad_fork_cleanup_fs;
	if ((retval = copy_signal(clone_flags, p)))
		goto bad_fork_cleanup_sighand;
	if ((retval = copy_mm(clone_flags, p)))
		goto bad_fork_cleanup_signal;
	if ((retval = copy_keys(clone_flags, p)))
		goto bad_fork_cleanup_mm;
	if ((retval = copy_namespace(clone_flags, p)))
		goto bad_fork_cleanup_keys;
		//  复制线程上下文
		/*
		13.调用copy_thread（），用发出clone（）系统调用时CPU寄存器的值（正如第十章所述，这些值已经被保存在父进程的内核栈中）来初始化子进程的内核栈。
		不过，copy_thread（）把eax寄存器对应字段的值[这是fork（)和clone（）系统调用在子进程中的返回值】字段强行置为0。
		子进程描述符的thread.esp字段初始化为子进程内核栈的基地址，汇编语言函数（ret_from_fork（））的地址存放在thread.eip字段中。
		如果父进程使用I/O权限位图，则子进程获取该位图的一个拷贝。
		最后，如果CLONE_SETTLS标志被设置，则子进程获取由clone（）系统调用的参数tls指向的用户态数据结构所表示的TLS段（注9）。*/
	retval = copy_thread(0, clone_flags, stack_start, stack_size, p, regs);
	if (retval)
		goto bad_fork_cleanup_namespace;

		// CLONE_CHILD 相关设置
		/* 14.如果clone_flagS参数的值被置为CLONE_CHILD_SETTID或CLONE_CHILD_CLEARTID，
    		  就把child_tidotr参数的值分别复制到tsk->set_chid_tid或tsk->clear_child_tid字段。
			  这些标志说明：必须改变子进程用户态地址空间的child_tidptr所指向的变量的值，不过实际的写操作要稍后再执行。*/
	p->set_child_tid = (clone_flags & CLONE_CHILD_SETTID) ? child_tidptr : NULL;
	/*
	 * Clear TID on mm_release()?
	 */
	p->clear_child_tid = (clone_flags & CLONE_CHILD_CLEARTID) ? child_tidptr: NULL;

	/*
	 * Syscall tracing should be turned off in the child regardless
	 * of CLONE_PTRACE.
	 */
	/* 15.清除子进程thread_info结构的TIF_SYSCALL_TRACE标志，以使ret_from_fork（）函数不会把系统调用结束的消息通知给调试进程
	（参见第十章“进入和退出系统调用”一节）。
	（因为对子进程的跟踪是由tsk->ptrace中的PTRACE_SYSCALL标志来控制的，所以子进程的系统调用跟踪不会被禁用。）*/
	clear_tsk_thread_flag(p, TIF_SYSCALL_TRACE);

	/* Our parent execution domain becomes current domain
	   These must match for thread signalling to apply */
	   
	/* 16.用clone_flags参数低位的信号数字编码初始化tsk->exit_signal字段，如果CLONE_THREAD标志被置位，就把tsk->exit_signal字段初始化为-1。
		  只有当线程组的最后一个成员（通常是线程组的领头）“死亡”，才会产生一个信号，以通知线程组的领头进程的父进程。 */
	p->parent_exec_id = p->self_exec_id;

	/* ok, now we should be set up.. */
	p->exit_signal = (clone_flags & CLONE_THREAD) ? -1 : (clone_flags & CSIGNAL);
	p->pdeath_signal = 0;
	p->exit_state = 0;

	/* Perform scheduler related setup */
	/* 17.调用sched_fork（）完成对新进程调度程序数据结构的初始化。
		  该函数把新进程的状态设置为TASK_RUNNING，并把thread_info结构的preempt_count字段设置为1，从而禁止内核抢占（参见第五章“内核抢占”一节）。
		  此外，为了保证公平的进程调度，该函数在父子进程之间共享父进程的时间片（参见第七章“scheduler_tick（）函数”一节）。 */
	sched_fork(p);

	/*
	 * Ok, make it visible to the rest of the system.
	 * We dont wake it up yet.
	 */
	p->group_leader = p;
	INIT_LIST_HEAD(&p->ptrace_children);
	INIT_LIST_HEAD(&p->ptrace_list);

	/* Need tasklist lock for parent etc handling! */
	write_lock_irq(&tasklist_lock);

	/*
	 * The task hasn't been attached yet, so cpus_allowed mask cannot
	 * have changed. The cpus_allowed mask of the parent may have
	 * changed after it was copied first time, and it may then move to
	 * another CPU - so we re-copy it here and set the child's CPU to
	 * the parent's CPU. This avoids alot of nasty races.
	 */
	/* 18.把新进程的thread_info结构的cpu字段设置为由smp_processor_id（）所返回的本地CPU号。 */
	p->cpus_allowed = current->cpus_allowed;
	set_task_cpu(p, smp_processor_id());

	/*
	 * Check for pending SIGKILL! The new thread should not be allowed
	 * to slip out of an OOM kill. (or normal SIGKILL.)
	 */
	if (sigismember(&current->pending.signal, SIGKILL)) {
		write_unlock_irq(&tasklist_lock);
		retval = -EINTR;
		goto bad_fork_cleanup_namespace;
	}

	/* CLONE_PARENT re-uses the old parent */
	/* 19.初始化表示亲子关系的字段。
		  尤其是，如果CLONE_PARENT或CLONE_THREAD被设置，就用current->real_parent的值初始化tsk->real_parent和tsk->parent，
    	  因此，子进程的父进程似乎是当前进程的父进程。
		  否则，把tsk->real_parent和tsk->parent置为当前进程。 */
	if (clone_flags & (CLONE_PARENT|CLONE_THREAD))
		p->real_parent = current->real_parent;
	else
		p->real_parent = current;
	p->parent = p->real_parent;

	if (clone_flags & CLONE_THREAD) {
		spin_lock(&current->sighand->siglock);
		/*
		 * Important: if an exit-all has been started then
		 * do not create this new thread - the whole thread
		 * group is supposed to exit anyway.
		 */
		if (current->signal->flags & SIGNAL_GROUP_EXIT) {
			spin_unlock(&current->sighand->siglock);
			write_unlock_irq(&tasklist_lock);
			retval = -EAGAIN;
			goto bad_fork_cleanup_namespace;
		}
		p->group_leader = current->group_leader;

		if (current->signal->group_stop_count > 0) {
			/*
			 * There is an all-stop in progress for the group.
			 * We ourselves will stop as soon as we check signals.
			 * Make the new thread part of that group stop too.
			 */
			current->signal->group_stop_count++;
			set_tsk_thread_flag(p, TIF_SIGPENDING);
		}

		spin_unlock(&current->sighand->siglock);
	}
	/* 	20.执行SET_LINKS宏，把新进程描述符插人进程链表。 */

	SET_LINKS(p);
	/* 21.如果子进程必须被跟踪（tsk->ptrace字段的PT_PTRACED标志被设置），
		  就把current->parent赋给tsk->parent，并将子进程插人调试程序的跟踪链表中。 */
	if (unlikely(p->ptrace & PT_PTRACED))
		__ptrace_link(p, current->parent);

	/* 22.调用attach_pid（）把新进程描述符的PID插人pidhash[PIDTYPE_PID]散列表。 */
	attach_pid(p, PIDTYPE_PID, p->pid);
	attach_pid(p, PIDTYPE_TGID, p->tgid);
	/* 23.如果子进程是线程组的领头进程（CLONE_THREAD标志被清0）：
    	a.把tsk->tgid的初值置为tsk->pid。
    	b.把tsk->group_leader的初值置为tsk。
    	c.调用两次attach_pid（），把子进程分别插人PIDTYPE_PGID和PIDTYPE_SID类型的PID散列表。 */
	if (thread_group_leader(p)) {
		attach_pid(p, PIDTYPE_PGID, process_group(p));
		attach_pid(p, PIDTYPE_SID, p->signal->session);
		if (p->pid)
			__get_cpu_var(process_counts)++;
	}

/* 26．现在，新进程已经被加入进程集合：递增nr_threads变量的值。 */
/* 27.递增total_forks变量以记录被创建的进程的数量。 */
	nr_threads++;
	total_forks++;
	write_unlock_irq(&tasklist_lock);
	retval = 0;
/* 28.终止并返回子进程描述符指针（tsk）。 */
fork_out:
	if (retval)
		return ERR_PTR(retval);
	return p;

bad_fork_cleanup_namespace:
	exit_namespace(p);
bad_fork_cleanup_keys:
	exit_keys(p);
bad_fork_cleanup_mm:
	if (p->mm)
		mmput(p->mm);
bad_fork_cleanup_signal:
	exit_signal(p);
bad_fork_cleanup_sighand:
	exit_sighand(p);
bad_fork_cleanup_fs:
	exit_fs(p); /* blocking */
bad_fork_cleanup_files:
	exit_files(p); /* blocking */
bad_fork_cleanup_semundo:
	exit_sem(p);
bad_fork_cleanup_audit:
	audit_free(p);
bad_fork_cleanup_security:
	security_task_free(p);
bad_fork_cleanup_policy:
#ifdef CONFIG_NUMA
	mpol_free(p->mempolicy);
#endif
bad_fork_cleanup:
	if (p->binfmt)
		module_put(p->binfmt->module);
bad_fork_cleanup_put_domain:
	module_put(p->thread_info->exec_domain->module);
bad_fork_cleanup_count:
	put_group_info(p->group_info);
	atomic_dec(&p->user->processes);
	free_uid(p->user);
bad_fork_free:
	free_task(p);
	goto fork_out;
}

struct pt_regs * __devinit __attribute__((weak)) idle_regs(struct pt_regs *regs)
{
	memset(regs, 0, sizeof(struct pt_regs));
	return regs;
}

task_t * __devinit fork_idle(int cpu)
{
	task_t *task;
	struct pt_regs regs;

	task = copy_process(CLONE_VM, 0, idle_regs(&regs), 0, NULL, NULL, 0);
	if (!task)
		return ERR_PTR(-ENOMEM);
	init_idle(task, cpu);
	unhash_process(task);
	return task;
}

static inline int fork_traceflag (unsigned clone_flags)
{
	if (clone_flags & CLONE_UNTRACED)
		return 0;
	else if (clone_flags & CLONE_VFORK) {
		if (current->ptrace & PT_TRACE_VFORK)
			return PTRACE_EVENT_VFORK;
	} else if ((clone_flags & CSIGNAL) != SIGCHLD) {
		if (current->ptrace & PT_TRACE_CLONE)
			return PTRACE_EVENT_CLONE;
	} else if (current->ptrace & PT_TRACE_FORK)
		return PTRACE_EVENT_FORK;

	return 0;
}

/*
 *  Ok, this is the main fork-routine.
 *
 * It copies the process, and if successful kick-starts
 * it and waits for it to finish using the VM if required.
 */
/* 无论是 fork、vfork 还是 clone，在内核中最终都汇聚到同一个核心函数（在旧版本中叫 do_fork，新版本中是 kernel_clone），
 * 但它们在调用时传入了不同的 clone_flags 标志位。这些标志位决定了新进程与父进程之间共享或复制哪些资源 */
long do_fork(unsigned long clone_flags,
	      unsigned long stack_start,
	      struct pt_regs *regs,
	      unsigned long stack_size,
	      int __user *parent_tidptr,
	      int __user *child_tidptr)
{
	struct task_struct *p;
	int trace = 0;
	long pid = alloc_pidmap();

	if (pid < 0)
		return -EAGAIN;
	/* 如果当前进程（父进程）正被调试器（如 strace、gdb）跟踪：
	调用 fork_traceflag() 确定跟踪类型。
	设置 CLONE_PTRACE 标志，让子进程也被父进程的调试器跟踪。 */
	if (unlikely(current->ptrace)) {
		trace = fork_traceflag (clone_flags);
		if (trace)
			clone_flags |= CLONE_PTRACE;
	}

	p = copy_process(clone_flags, stack_start, regs, stack_size, parent_tidptr, child_tidptr, pid);
	/*
	 * Do this prior waking up the new thread - the thread pointer
	 * might get invalid after that point, if the thread exits quickly.
	 */
	if (!IS_ERR(p)) {
		struct completion vfork;
		/* 如果是 vfork()（clone_flags 包含 CLONE_VFORK）：
		在子进程的 task_struct 中保存 vfork_done 指针。
		初始化一个 completion 同步对象。
		作用是：子进程执行期间，父进程会等待这个 completion 对象，直到子进程 exec 或 exit 才唤醒父进程*/
		if (clone_flags & CLONE_VFORK) {
			p->vfork_done = &vfork;
			init_completion(&vfork);
		}
		/* 如果需要子进程被跟踪（PT_PTRACED），或者要求子进程创建后立即停止（CLONE_STOPPED）：
		向子进程挂起信号集中加入 SIGSTOP。
		设置 TIF_SIGPENDING 标志，让子进程在返回用户态时处理信号（即自己停下）。 */
		if ((p->ptrace & PT_PTRACED) || (clone_flags & CLONE_STOPPED)) {
			/*
			 * We'll start up with an immediate SIGSTOP.
			 */
			sigaddset(&p->pending.signal, SIGSTOP);
			set_tsk_thread_flag(p, TIF_SIGPENDING);
		}

		if (!(clone_flags & CLONE_STOPPED))
			wake_up_new_task(p, clone_flags);
		else
			p->state = TASK_STOPPED;
		/* 如果父进程正在被跟踪（trace 非零），发送 SIGTRAP 给调试器，告知新进程已创建 */
		if (unlikely (trace)) {
			current->ptrace_message = pid;
			ptrace_notify ((trace << 8) | SIGTRAP);
		}
		/* 关键：父进程在这里等待 vfork 的 completion 对象。
		子进程在 exec 或 exit 时会调用 complete(vfork_done)，唤醒父进程。
		如果是 vfork 且父进程被跟踪，还会额外通知调试器。 */
		if (clone_flags & CLONE_VFORK) {
			wait_for_completion(&vfork);
			if (unlikely (current->ptrace & PT_TRACE_VFORK_DONE))
				ptrace_notify ((PTRACE_EVENT_VFORK_DONE << 8) | SIGTRAP);
		}
	} else {
		free_pidmap(pid);
		pid = PTR_ERR(p);
	}
	return pid;
}

/* SLAB cache for signal_struct structures (tsk->signal) */
kmem_cache_t *signal_cachep;

/* SLAB cache for sighand_struct structures (tsk->sighand) */
kmem_cache_t *sighand_cachep;

/* SLAB cache for files_struct structures (tsk->files) */
kmem_cache_t *files_cachep;

/* SLAB cache for fs_struct structures (tsk->fs) */
kmem_cache_t *fs_cachep;

/* SLAB cache for vm_area_struct structures */
kmem_cache_t *vm_area_cachep;

/* SLAB cache for mm_struct structures (tsk->mm) */
kmem_cache_t *mm_cachep;

void __init proc_caches_init(void)
{
	sighand_cachep = kmem_cache_create("sighand_cache",
			sizeof(struct sighand_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC, NULL, NULL);
	signal_cachep = kmem_cache_create("signal_cache",
			sizeof(struct signal_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC, NULL, NULL);
	files_cachep = kmem_cache_create("files_cache", 
			sizeof(struct files_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC, NULL, NULL);
	fs_cachep = kmem_cache_create("fs_cache", 
			sizeof(struct fs_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC, NULL, NULL);
	vm_area_cachep = kmem_cache_create("vm_area_struct",
			sizeof(struct vm_area_struct), 0,
			SLAB_PANIC, NULL, NULL);
	mm_cachep = kmem_cache_create("mm_struct",
			sizeof(struct mm_struct), 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC, NULL, NULL);
}
