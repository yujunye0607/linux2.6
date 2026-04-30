/*
 * Generic waiting primitives.
 *
 * (C) 2004 William Irwin, Oracle
 */
#include <linux/config.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/wait.h>
#include <linux/hash.h>

void fastcall add_wait_queue(wait_queue_head_t *q, wait_queue_t *wait)
{
	unsigned long flags;

	wait->flags &= ~WQ_FLAG_EXCLUSIVE;
	spin_lock_irqsave(&q->lock, flags);
	__add_wait_queue(q, wait);
	spin_unlock_irqrestore(&q->lock, flags);
}
EXPORT_SYMBOL(add_wait_queue);

void fastcall add_wait_queue_exclusive(wait_queue_head_t *q, wait_queue_t *wait)
{
	unsigned long flags;

	wait->flags |= WQ_FLAG_EXCLUSIVE;
	spin_lock_irqsave(&q->lock, flags);
	__add_wait_queue_tail(q, wait);
	spin_unlock_irqrestore(&q->lock, flags);
}
EXPORT_SYMBOL(add_wait_queue_exclusive);

void fastcall remove_wait_queue(wait_queue_head_t *q, wait_queue_t *wait)
{
	unsigned long flags;

	spin_lock_irqsave(&q->lock, flags);
	__remove_wait_queue(q, wait);
	spin_unlock_irqrestore(&q->lock, flags);
}
EXPORT_SYMBOL(remove_wait_queue);


/*
 * Note: we use "set_current_state()" _after_ the wait-queue add,
 * because we need a memory barrier there on SMP, so that any
 * wake-function that tests for the wait-queue being active
 * will be guaranteed to see waitqueue addition _or_ subsequent
 * tests in this thread will see the wakeup having taken place.
 *
 * The spin_unlock() itself is semi-permeable and only protects
 * one way (it only protects stuff inside the critical region and
 * stops them from bleeding out - it would still allow subsequent
 * loads to move into the the critical region).
 */
void fastcall
prepare_to_wait(wait_queue_head_t *q, wait_queue_t *wait, int state)
{
	unsigned long flags;

	// 清除排他标志位（清除独占标志，使其成为一个非独占等待项）
	// 对于非独占等待项：wake_up 会一次性唤醒队列中所有的非独占等待项。
	// 对于独占等待项：wake_up 只会唤醒队列中的第一个独占等待项，然后就停止继续唤醒。
	/* 如果没有独占标志（全部非独占）：每次资源可用时，wake_up 会唤醒全部100个进程。
	 * 但最终只有4个能获得资源，其余96个被唤醒后发现自己还是抢不到，只能再次睡眠。这会消耗大量的CPU（即“惊群”）。
	 * 如果有了独占标志：可以让所有等待者都标记为独占。当资源可用时，wake_up 只会唤醒第一个（比如队列头部的）进程。
	 * 这个进程拿到资源后，会在完成工作后再次调用wake_up，唤醒下一个，依次类推。这样，每次只有一个进程被唤醒，大大减轻了系统负担。 */
	wait->flags &= ~WQ_FLAG_EXCLUSIVE;
	spin_lock_irqsave(&q->lock, flags);
	if (list_empty(&wait->task_list))//将"入队"和"设状态"作为一个原子操作完成后，在真正睡眠前又检查了一次条件。如果条件在中间变成真，这次检查就能发现并阻止睡眠
		__add_wait_queue(q, wait);
	/*
	 * don't alter the task state if this is just going to
	 * queue an async wait queue callback
	 */
	/* 
	 * 如果是同步等待项，设置当前状态为指定状态
	#define is_sync_wait(wait)	(!(wait) || ((wait)->task))
	同步等待 (task 不为空)：wait->task 指向一个进程（通常是 current）。
	这意味着，这个等待项代表一个正在运行的进程，它希望自己进入睡眠。当事件发生时，内核需要唤醒这个进程。这就是我们通常所说的“同步”等待。
	异步等待 (task 为空)：wait->task 是 NULL。
	这意味着，这个等待项不代表一个要睡眠的进程，而是代表一个回调函数。
	这种等待项是通过 init_waitqueue_func_entry() 创建的，目的是在事件发生时执行一段自定义代码（异步回调），而不是唤醒一个进程。
	 */
	if (is_sync_wait(wait))
		set_current_state(state);
	spin_unlock_irqrestore(&q->lock, flags);
}
EXPORT_SYMBOL(prepare_to_wait);

void fastcall
prepare_to_wait_exclusive(wait_queue_head_t *q, wait_queue_t *wait, int state)
{
	unsigned long flags;

	wait->flags |= WQ_FLAG_EXCLUSIVE;
	spin_lock_irqsave(&q->lock, flags);
	if (list_empty(&wait->task_list))
	// 排他等待者 不会 “插队”，必须排在 非排他等待者 后面，避免唤醒混乱、饿死、惊群
	// 该函数通常给一些互斥进程使用
		__add_wait_queue_tail(q, wait);
	/*
	 * don't alter the task state if this is just going to
 	 * queue an async wait queue callback
	 */
	if (is_sync_wait(wait))
		set_current_state(state);
	spin_unlock_irqrestore(&q->lock, flags);
}
EXPORT_SYMBOL(prepare_to_wait_exclusive);

void fastcall finish_wait(wait_queue_head_t *q, wait_queue_t *wait)
{
	unsigned long flags;

	__set_current_state(TASK_RUNNING);
	/*
	 * We can check for list emptiness outside the lock
	 * IFF:
	 *  - we use the "careful" check that verifies both
	 *    the next and prev pointers, so that there cannot
	 *    be any half-pending updates in progress on other
	 *    CPU's that we haven't seen yet (and that might
	 *    still change the stack area.
	 * and
	 *  - all other users take the lock (ie we can only
	 *    have _one_ other CPU that looks at or modifies
	 *    the list).
	 */
	if (!list_empty_careful(&wait->task_list)) {
		spin_lock_irqsave(&q->lock, flags);
		list_del_init(&wait->task_list);
		spin_unlock_irqrestore(&q->lock, flags);
	}
}
EXPORT_SYMBOL(finish_wait);

int autoremove_wake_function(wait_queue_t *wait, unsigned mode, int sync, void *key)
{
	int ret = default_wake_function(wait, mode, sync, key);

	if (ret)
		list_del_init(&wait->task_list);
	return ret;
}
EXPORT_SYMBOL(autoremove_wake_function);

int wake_bit_function(wait_queue_t *wait, unsigned mode, int sync, void *arg)
{
	struct wait_bit_key *key = arg;
	struct wait_bit_queue *wait_bit
		= container_of(wait, struct wait_bit_queue, wait);

	if (wait_bit->key.flags != key->flags ||
			wait_bit->key.bit_nr != key->bit_nr ||
			test_bit(key->bit_nr, key->flags))
		return 0;
	else
		return autoremove_wake_function(wait, mode, sync, key);
}
EXPORT_SYMBOL(wake_bit_function);

/*
 * To allow interruptible waiting and asynchronous (i.e. nonblocking)
 * waiting, the actions of __wait_on_bit() and __wait_on_bit_lock() are
 * permitted return codes. Nonzero return codes halt waiting and return.
 */
int __sched fastcall
__wait_on_bit(wait_queue_head_t *wq, struct wait_bit_queue *q,
			int (*action)(void *), unsigned mode)
{
	int ret = 0;

	do {
		prepare_to_wait(wq, &q->wait, mode);
		if (test_bit(q->key.bit_nr, q->key.flags))
			ret = (*action)(q->key.flags);
	} while (test_bit(q->key.bit_nr, q->key.flags) && !ret);
	finish_wait(wq, &q->wait);
	return ret;
}
EXPORT_SYMBOL(__wait_on_bit);

int __sched fastcall out_of_line_wait_on_bit(void *word, int bit,
					int (*action)(void *), unsigned mode)
{
	wait_queue_head_t *wq = bit_waitqueue(word, bit);
	DEFINE_WAIT_BIT(wait, word, bit);

	return __wait_on_bit(wq, &wait, action, mode);
}
EXPORT_SYMBOL(out_of_line_wait_on_bit);

int __sched fastcall
__wait_on_bit_lock(wait_queue_head_t *wq, struct wait_bit_queue *q,
			int (*action)(void *), unsigned mode)
{
	int ret = 0;

	do {
		prepare_to_wait_exclusive(wq, &q->wait, mode);
		if (test_bit(q->key.bit_nr, q->key.flags)) {
			if ((ret = (*action)(q->key.flags)))
				break;
		}
	} while (test_and_set_bit(q->key.bit_nr, q->key.flags));
	finish_wait(wq, &q->wait);
	return ret;
}
EXPORT_SYMBOL(__wait_on_bit_lock);

int __sched fastcall out_of_line_wait_on_bit_lock(void *word, int bit,
					int (*action)(void *), unsigned mode)
{
	wait_queue_head_t *wq = bit_waitqueue(word, bit);
	DEFINE_WAIT_BIT(wait, word, bit);

	return __wait_on_bit_lock(wq, &wait, action, mode);
}
EXPORT_SYMBOL(out_of_line_wait_on_bit_lock);

void fastcall __wake_up_bit(wait_queue_head_t *wq, void *word, int bit)
{
	struct wait_bit_key key = __WAIT_BIT_KEY_INITIALIZER(word, bit);
	if (waitqueue_active(wq))
		__wake_up(wq, TASK_INTERRUPTIBLE|TASK_UNINTERRUPTIBLE, 1, &key);
}
EXPORT_SYMBOL(__wake_up_bit);

/**
 * wake_up_bit - wake up a waiter on a bit
 * @word: the word being waited on, a kernel virtual address
 * @bit: the bit of the word being waited on
 *
 * There is a standard hashed waitqueue table for generic use. This
 * is the part of the hashtable's accessor API that wakes up waiters
 * on a bit. For instance, if one were to have waiters on a bitflag,
 * one would call wake_up_bit() after clearing the bit.
 *
 * In order for this to function properly, as it uses waitqueue_active()
 * internally, some kind of memory barrier must be done prior to calling
 * this. Typically, this will be smp_mb__after_clear_bit(), but in some
 * cases where bitflags are manipulated non-atomically under a lock, one
 * may need to use a less regular barrier, such fs/inode.c's smp_mb(),
 * because spin_unlock() does not guarantee a memory barrier.
 */
void fastcall wake_up_bit(void *word, int bit)
{
	__wake_up_bit(bit_waitqueue(word, bit), word, bit);
}
EXPORT_SYMBOL(wake_up_bit);

fastcall wait_queue_head_t *bit_waitqueue(void *word, int bit)
{
	const int shift = BITS_PER_LONG == 32 ? 5 : 6;
	const struct zone *zone = page_zone(virt_to_page(word));
	unsigned long val = (unsigned long)word << shift | bit;

	return &zone->wait_table[hash_long(val, zone->wait_table_bits)];
}
EXPORT_SYMBOL(bit_waitqueue);
