/*
 *	linux/kernel/softirq.c
 *
 *	Copyright (C) 1992 Linus Torvalds
 *
 * Rewritten. Old one was good in 2.2, but in 2.3 it was immoral. --ANK (990903)
 */

#include <linux/module.h>
#include <linux/kernel_stat.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/notifier.h>
#include <linux/percpu.h>
#include <linux/cpu.h>
#include <linux/kthread.h>
#include <linux/rcupdate.h>

#include <asm/irq.h>
/*
   - No shared variables, all the data are CPU local.
   - If a softirq needs serialization, let it serialize itself
     by its own spinlocks.
   - Even if softirq is serialized, only local cpu is marked for
     execution. Hence, we get something sort of weak cpu binding.
     Though it is still not clear, will it result in better locality
     or will not.

   Examples:
   - NET RX softirq. It is multithreaded and does not require
     any global serialization.
   - NET TX softirq. It kicks software netdevice queues, hence
     it is logically serialized per device, but this serialization
     is invisible to common code.
   - Tasklets: serialized wrt itself.
 */

#ifndef __ARCH_IRQ_STAT
irq_cpustat_t irq_stat[NR_CPUS] ____cacheline_aligned;
EXPORT_SYMBOL(irq_stat);
#endif

static struct softirq_action softirq_vec[32] __cacheline_aligned_in_smp;

static DEFINE_PER_CPU(struct task_struct *, ksoftirqd);

/*
 * we cannot loop indefinitely here to avoid userspace starvation,
 * but we also don't want to introduce a worst case 1/HZ latency
 * to the pending events, so lets the scheduler to balance
 * the softirq load for us.
 */
static inline void wakeup_softirqd(void)
{
	/* Interrupts are disabled: no need to stop preemption */
	struct task_struct *tsk = __get_cpu_var(ksoftirqd);

	if (tsk && tsk->state != TASK_RUNNING)
		wake_up_process(tsk);
}

/*
 * We restart softirq processing MAX_SOFTIRQ_RESTART times,
 * and we fall back to softirqd after that.
 *
 * This number has been established via experimentation.
 * The two things to balance is latency against fairness -
 * we want to handle softirqs as soon as possible, but they
 * should not be able to lock up the box.
 */
#define MAX_SOFTIRQ_RESTART 10

/* 
__do_softirq（）函数读取本地CPU的软中断掩码并执行与每个设置位相关的可延迟函数。
由于正在执行一个软中断函数时可能出现新挂起的软中断，所以为了保证可延迟函数的低延迟性，__do_softirq（）一直运行到执行完所有挂起的软中断。
但是，这种机制可能迫使__do_softirq（）运行很长一段时间，因而大大延迟用户态进程的执行。
因此，__do_softirq（）只做固定次数的循环，然后就返回。如果还有其余挂起的软中断，
那么下一节要描述的内核线程ksoftirqd将会在预期的时间内处理它们。下面简单描述
__do_softirq（）函数执行的操作：
1.
    把循环计数器的值初始化为10。
2.
    把本地CPU（被local_softirq_pending（）选中的）软中断的位掩码复制到局部
    变量pending中。
3.
    调用local_bh_disable（）增加软中断计数器的值。在可延迟函数开始执行之前应
    该禁用它们，这似乎有点违反直觉，但确实极有意义。因为在绝大多数情况下可延
    迟函数是在开中断的状态下运行的，所以在执行__do_softirq（）的过程中可能会
    产生新的中断。当do_IRQ（）执行irg_exit（）宏时，可能有另外一个__do_softirq（）函数的实例开始执行。
	这种情况是应该避免的，因为可延迟函数必须以串行的方式在CPU上运行。
	因此，__do_softirq（）函数的第一个实例禁用可延迟函数，以使每个新的函数实例将会在do_softirg（）函数的第1步就退出。
4.
    清除本地CPU的软中断位图，以便可以激活新的软中断（在第2步，已经把位图
    保存在pending局部变量中）。
5.
    执行1ocal_ira_enable（）来激活本地中断。
6.
    根据局部变量pending每一位的设置，执行对应的软中断处理函数。回忆一下，
    下标为n的软中断函数的地址存放在softira_vec[n]->action变量中。
7.
    执行local_ira_disable（）以禁用本地中断。
8.
    把本地CPU的软中断位掩码复制到局部变量pending中，并且再次递减循环计
    数器。
9.如果pending不为0，那么从最后一次循环开始，至少有一个软中断被激活，而
    且循环计数器仍然是正数，跳转回到第4步。
10.如果还有更多的挂起软中断，则调用wakeup_softirqd（）唤醒内核线程来处理本
    地CPU的软中断（见下一节）。
11.软中断计数器减1，因而重新激活可延迟函数。
 */
asmlinkage void __do_softirq(void)
{
	struct softirq_action *h;
	__u32 pending;
	int max_restart = MAX_SOFTIRQ_RESTART;
	int cpu;

	pending = local_softirq_pending();

	local_bh_disable();
	cpu = smp_processor_id();
restart:
	/* Reset the pending bitmask before enabling irqs */
	local_softirq_pending() = 0;

	local_irq_enable();

	h = softirq_vec;

	do {
		if (pending & 1) {
			h->action(h);
			rcu_bh_qsctr_inc(cpu);
		}
		h++;
		pending >>= 1;
	} while (pending);

	local_irq_disable();

	pending = local_softirq_pending();
	if (pending && --max_restart)
		goto restart;

	if (pending)
		wakeup_softirqd();

	__local_bh_enable();
}

#ifndef __ARCH_HAS_DO_SOFTIRQ

/* 
如果在这样的一个检查点（1ocal_softirg_pending（）不为0）检测到挂起的软中断，
内核就调用do_softirq（）来处理它们。
*/
/* 
1.
	如果in_interrupt（）产生值1，则函数返回。这种情况说明要么在中断上下文中
    调用了do_softirq（）函数，要么当前禁用软中断。
2.
    执行1ocal_irq_save以保存标志的状态值，并禁用本地CPU上的中断。
3.
    调用__do_softirq（）函数（参见下面一节）。
4.
    执行local_ira_restore以恢复在第2步保存的标志（表示本地是关中断还是
    开中断）的状态值并返回。
 */
asmlinkage void do_softirq(void)
{
	__u32 pending;
	unsigned long flags;

	if (in_interrupt())
		return;

	local_irq_save(flags);

	pending = local_softirq_pending();

	if (pending)
		__do_softirq();

	local_irq_restore(flags);
}

EXPORT_SYMBOL(do_softirq);

#endif

void local_bh_enable(void)
{
	WARN_ON(irqs_disabled());
	/*
	 * Keep preemption disabled until we are done with
	 * softirq processing:
 	 */
 	sub_preempt_count(SOFTIRQ_OFFSET - 1);

	if (unlikely(!in_interrupt() && local_softirq_pending()))
		do_softirq();

	dec_preempt_count();
	preempt_check_resched();
}
EXPORT_SYMBOL(local_bh_enable);

#ifdef __ARCH_IRQ_EXIT_IRQS_DISABLED
# define invoke_softirq()	__do_softirq()
#else
# define invoke_softirq()	do_softirq()
#endif

/*
 * Exit an interrupt context. Process softirqs if needed and possible:
 */
void irq_exit(void)
{
	account_system_vtime(current);
	sub_preempt_count(IRQ_EXIT_OFFSET);
	if (!in_interrupt() && local_softirq_pending())
		invoke_softirq();
	preempt_enable_no_resched();
}

/*
 * This function must run with irqs disabled!
 */
inline fastcall void raise_softirq_irqoff(unsigned int nr)
{
	__raise_softirq_irqoff(nr);

	/*
	 * If we're in an interrupt or softirq, we're done
	 * (this also catches softirq-disabled code). We will
	 * actually run the softirq once we return from
	 * the irq or softirq.
	 *
	 * Otherwise we wake up ksoftirqd to make sure we
	 * schedule the softirq soon.
	 */
	if (!in_interrupt())
		wakeup_softirqd();
}

EXPORT_SYMBOL(raise_softirq_irqoff);

/* 软中断触发函数 */
void fastcall raise_softirq(unsigned int nr)
{
	unsigned long flags;

	local_irq_save(flags);//CPSID i 指令设置 I 位（禁用 IRQ）并禁用本地CPU上中断
	raise_softirq_irqoff(nr);//在一个已经关闭本地中断的环境下，安全地触发一个软中断
	local_irq_restore(flags);//恢复local_irq_save设置的值并启用本地CPU上中断
}

/* 
处理软中断初始化 
nr 软中断号
action 软中断处理函数
data 软中断数据指针
*/
void open_softirq(int nr, void (*action)(struct softirq_action*), void *data)
{
	softirq_vec[nr].data = data;
	softirq_vec[nr].action = action;
}

EXPORT_SYMBOL(open_softirq);

/* Tasklets */
struct tasklet_head
{
	struct tasklet_struct *list;
};

/* Some compilers disobey section attribute on statics when not
   initialized -- RR */
static DEFINE_PER_CPU(struct tasklet_head, tasklet_vec) = { NULL };
static DEFINE_PER_CPU(struct tasklet_head, tasklet_hi_vec) = { NULL };

void fastcall __tasklet_schedule(struct tasklet_struct *t)
{
	unsigned long flags;

	local_irq_save(flags);
	t->next = __get_cpu_var(tasklet_vec).list;
	__get_cpu_var(tasklet_vec).list = t;
	raise_softirq_irqoff(TASKLET_SOFTIRQ);
	local_irq_restore(flags);
}

EXPORT_SYMBOL(__tasklet_schedule);

void fastcall __tasklet_hi_schedule(struct tasklet_struct *t)
{
	unsigned long flags;

	local_irq_save(flags);
	t->next = __get_cpu_var(tasklet_hi_vec).list;
	__get_cpu_var(tasklet_hi_vec).list = t;
	raise_softirq_irqoff(HI_SOFTIRQ);
	local_irq_restore(flags);
}

EXPORT_SYMBOL(__tasklet_hi_schedule);

/* 
处理tasklet软中断函数
	1.
		禁用本地中断。
	2.
		获得本地CPU的逻辑号n。
	3.
		把tasklet_vec[n]或tasklet_hi_vec[n]指向的链表的地址存人局部变量list。
	4.
		把tasklet_vec[n]或tasklet_hi_vec[n]的值赋为NuLL，因此，已调度的tasklet
		描述符的链表被清空。
	5.
		打开本地中断。
	6.
		对于list指向的链表中的每个tasklet描述符：
		a.在多处理器系统上，检查tasklet的TASKLET_STATE_RUN标志。
		·如果该标志被设置，说明同类型的一个tasklet正在另一个CPU上运行，因
		此，就把任务描述符重新插人到由tasklet_vec[n]或tasklet_hi_vec[n]
		指向的链表中，并再次激活TASKLET_SOFTIRQ或HI_SOFTIRQ软中断。
		这样，当同类型的其他tasklet在其他CPU上运行时，这个tasklet就被延迟。
		如果TASKLET_STATE_RUN标志未被设置，tasklet就没有在其他CPU上运
		行，就需要设置这个标志，以便tasklet函数不能在其他CPU上执行。
		b.通过查看tasklet描述符的count字段，检查tasklet是否被禁止。如果是，就清
		TASKLET_STATE_RUN标志，并把任务描述符重新插入到由tasklet_vec[n]
		或tasklet_hi_vec[n]指向的链表中，然后函数再次激活TASKLET_SOFTIRQ
		或HI_SOFTIRQ软中断。
		c.如果tasklet被激活，清TASKLET_STATE_SCHED标志，并执行tasklet函数。

注意，除非tasklet函数重新激活自己，否则，tasklet的每次激活至多触发tasklet函数的一次执行。
*/
static void tasklet_action(struct softirq_action *a)
{
	struct tasklet_struct *list;

	local_irq_disable();
	list = __get_cpu_var(tasklet_vec).list;
	__get_cpu_var(tasklet_vec).list = NULL;
	local_irq_enable();

	while (list) {
		struct tasklet_struct *t = list;

		list = list->next;

		if (tasklet_trylock(t)) {
			if (!atomic_read(&t->count)) {
				if (!test_and_clear_bit(TASKLET_STATE_SCHED, &t->state))
					BUG();
				t->func(t->data);
				tasklet_unlock(t);
				continue;
			}
			tasklet_unlock(t);
		}

		local_irq_disable();
		t->next = __get_cpu_var(tasklet_vec).list;
		__get_cpu_var(tasklet_vec).list = t;
		__raise_softirq_irqoff(TASKLET_SOFTIRQ);
		local_irq_enable();
	}
}

static void tasklet_hi_action(struct softirq_action *a)
{
	struct tasklet_struct *list;

	local_irq_disable();
	list = __get_cpu_var(tasklet_hi_vec).list;
	__get_cpu_var(tasklet_hi_vec).list = NULL;
	local_irq_enable();

	while (list) {
		struct tasklet_struct *t = list;

		list = list->next;

		if (tasklet_trylock(t)) {
			if (!atomic_read(&t->count)) {
				if (!test_and_clear_bit(TASKLET_STATE_SCHED, &t->state))
					BUG();
				t->func(t->data);
				tasklet_unlock(t);
				continue;
			}
			tasklet_unlock(t);
		}

		local_irq_disable();
		t->next = __get_cpu_var(tasklet_hi_vec).list;
		__get_cpu_var(tasklet_hi_vec).list = t;
		__raise_softirq_irqoff(HI_SOFTIRQ);
		local_irq_enable();
	}
}


void tasklet_init(struct tasklet_struct *t,
		  void (*func)(unsigned long), unsigned long data)
{
	t->next = NULL;
	t->state = 0;
	atomic_set(&t->count, 0);
	t->func = func;
	t->data = data;
}

EXPORT_SYMBOL(tasklet_init);

void tasklet_kill(struct tasklet_struct *t)
{
	if (in_interrupt())
		printk("Attempt to kill tasklet from interrupt\n");

	while (test_and_set_bit(TASKLET_STATE_SCHED, &t->state)) {
		do
			yield();
		while (test_bit(TASKLET_STATE_SCHED, &t->state));
	}
	tasklet_unlock_wait(t);
	clear_bit(TASKLET_STATE_SCHED, &t->state);
}

EXPORT_SYMBOL(tasklet_kill);

void __init softirq_init(void)
{
	open_softirq(TASKLET_SOFTIRQ, tasklet_action, NULL);
	open_softirq(HI_SOFTIRQ, tasklet_hi_action, NULL);
}

/* 在最近的内核版本中，每个CPU都有自己的ksoftirqd/n内核线程（这里，n为CPU的逻辑号）。
 * 每个ksoftirqd/n内核线程都运行ksoftirqd（）函数，该函数实际上执行下列的循环：
	for(;;) {
	set_current_state(TASK_INTERRUPTIBLE );
	schedule();

		while (local_softirq_pending()) {
			preempt_disable();
			do_softirq();
			preempt_enable();
			cond_resched();
		}
	}
	当内核线程被唤醒时，就检查1ocal_softirq_pending（）中的软中断位掩码并在必要时
	调用do_softirq（）。如果没有挂起的软中断，函数把当前进程状态置为
	TASK_INTERRUPTIBLE，随后，如果当前进程需要（当前thread_info的TIF_NEED_RESCHED
	标志被设置）就调用cond_resched（）函数来实现进程切换。
 */
static int ksoftirqd(void * __bind_cpu)
{
	set_user_nice(current, 19);
	current->flags |= PF_NOFREEZE;

	set_current_state(TASK_INTERRUPTIBLE);

	while (!kthread_should_stop()) {
		if (!local_softirq_pending())
			schedule();

		__set_current_state(TASK_RUNNING);

		while (local_softirq_pending()) {
			/* Preempt disable stops cpu going offline.
			   If already offline, we'll be on wrong CPU:
			   don't process */
			preempt_disable();
			if (cpu_is_offline((long)__bind_cpu))
				goto wait_to_die;
			do_softirq();
			preempt_enable();
			cond_resched();
		}

		set_current_state(TASK_INTERRUPTIBLE);
	}
	__set_current_state(TASK_RUNNING);
	return 0;

wait_to_die:
	preempt_enable();
	/* Wait for kthread_stop */
	set_current_state(TASK_INTERRUPTIBLE);
	while (!kthread_should_stop()) {
		schedule();
		set_current_state(TASK_INTERRUPTIBLE);
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

#ifdef CONFIG_HOTPLUG_CPU
/*
 * tasklet_kill_immediate is called to remove a tasklet which can already be
 * scheduled for execution on @cpu.
 *
 * Unlike tasklet_kill, this function removes the tasklet
 * _immediately_, even if the tasklet is in TASKLET_STATE_SCHED state.
 *
 * When this function is called, @cpu must be in the CPU_DEAD state.
 */
void tasklet_kill_immediate(struct tasklet_struct *t, unsigned int cpu)
{
	struct tasklet_struct **i;

	BUG_ON(cpu_online(cpu));
	BUG_ON(test_bit(TASKLET_STATE_RUN, &t->state));

	if (!test_bit(TASKLET_STATE_SCHED, &t->state))
		return;

	/* CPU is dead, so no lock needed. */
	for (i = &per_cpu(tasklet_vec, cpu).list; *i; i = &(*i)->next) {
		if (*i == t) {
			*i = t->next;
			return;
		}
	}
	BUG();
}

static void takeover_tasklets(unsigned int cpu)
{
	struct tasklet_struct **i;

	/* CPU is dead, so no lock needed. */
	local_irq_disable();

	/* Find end, append list for that CPU. */
	for (i = &__get_cpu_var(tasklet_vec).list; *i; i = &(*i)->next);
	*i = per_cpu(tasklet_vec, cpu).list;
	per_cpu(tasklet_vec, cpu).list = NULL;
	raise_softirq_irqoff(TASKLET_SOFTIRQ);

	for (i = &__get_cpu_var(tasklet_hi_vec).list; *i; i = &(*i)->next);
	*i = per_cpu(tasklet_hi_vec, cpu).list;
	per_cpu(tasklet_hi_vec, cpu).list = NULL;
	raise_softirq_irqoff(HI_SOFTIRQ);

	local_irq_enable();
}
#endif /* CONFIG_HOTPLUG_CPU */

static int __devinit cpu_callback(struct notifier_block *nfb,
				  unsigned long action,
				  void *hcpu)
{
	int hotcpu = (unsigned long)hcpu;
	struct task_struct *p;

	switch (action) {
	case CPU_UP_PREPARE:
		BUG_ON(per_cpu(tasklet_vec, hotcpu).list);
		BUG_ON(per_cpu(tasklet_hi_vec, hotcpu).list);
		p = kthread_create(ksoftirqd, hcpu, "ksoftirqd/%d", hotcpu);
		if (IS_ERR(p)) {
			printk("ksoftirqd for %i failed\n", hotcpu);
			return NOTIFY_BAD;
		}
		kthread_bind(p, hotcpu);
  		per_cpu(ksoftirqd, hotcpu) = p;
 		break;
	case CPU_ONLINE:
		wake_up_process(per_cpu(ksoftirqd, hotcpu));
		break;
#ifdef CONFIG_HOTPLUG_CPU
	case CPU_UP_CANCELED:
		/* Unbind so it can run.  Fall thru. */
		kthread_bind(per_cpu(ksoftirqd, hotcpu), smp_processor_id());
	case CPU_DEAD:
		p = per_cpu(ksoftirqd, hotcpu);
		per_cpu(ksoftirqd, hotcpu) = NULL;
		kthread_stop(p);
		takeover_tasklets(hotcpu);
		break;
#endif /* CONFIG_HOTPLUG_CPU */
 	}
	return NOTIFY_OK;
}

static struct notifier_block __devinitdata cpu_nfb = {
	.notifier_call = cpu_callback
};

__init int spawn_ksoftirqd(void)
{
	void *cpu = (void *)(long)smp_processor_id();
	cpu_callback(&cpu_nfb, CPU_UP_PREPARE, cpu);
	cpu_callback(&cpu_nfb, CPU_ONLINE, cpu);
	register_cpu_notifier(&cpu_nfb);
	return 0;
}
