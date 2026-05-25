#ifndef __ASM_SPINLOCK_H
#define __ASM_SPINLOCK_H

#if __LINUX_ARM_ARCH__ < 6
#error SMP not supported on pre-ARMv6 CPUs
#endif

/*
 * ARMv6 Spin-locking.
 *
 * We (exclusively) read the old value, and decrement it.  If it
 * hits zero, we may have won the lock, so we try (exclusively)
 * storing it.
 *
 * Unlocked value: 0
 * Locked value: 1
 * 
 * slock
 	该字段表示自旋锁的状态：值为1表示“未加锁”状态，而任何负数和0都表示“加
 	锁”状态。
 * break_lock
 	表示进程正在忙等自旋锁（只在内核支持SMP和内核抢占的情况下使用该标志）。
 */
typedef struct {
	volatile unsigned int lock;
#ifdef CONFIG_PREEMPT
	unsigned int break_lock;
#endif
} spinlock_t;

#define SPIN_LOCK_UNLOCKED	(spinlock_t) { 0 }

#define spin_lock_init(x)	do { *(x) = SPIN_LOCK_UNLOCKED; } while (0)
#define spin_is_locked(x)	((x)->lock != 0)
#define spin_unlock_wait(x)	do { barrier(); } while (spin_is_locked(x))
#define _raw_spin_lock_flags(lock, flags) _raw_spin_lock(lock)

static inline void _raw_spin_lock(spinlock_t *lock)
{
	unsigned long tmp;

	__asm__ __volatile__(
"1:	ldrex	%0, [%1]\n"
"	teq	%0, #0\n"
"	strexeq	%0, %2, [%1]\n"
"	teqeq	%0, #0\n"
"	bne	1b"
	: "=&r" (tmp)
	: "r" (&lock->lock), "r" (1)
	: "cc", "memory");
}

static inline int _raw_spin_trylock(spinlock_t *lock)
{
	unsigned long tmp;

	__asm__ __volatile__(
"	ldrex	%0, [%1]\n"
"	teq	%0, #0\n"
"	strexeq	%0, %2, [%1]"
	: "=&r" (tmp)
	: "r" (&lock->lock), "r" (1)
	: "cc", "memory");

	return tmp == 0;
}

static inline void _raw_spin_unlock(spinlock_t *lock)
{
	__asm__ __volatile__(
"	str	%1, [%0]"
	:
	: "r" (&lock->lock), "r" (0)
	: "cc", "memory");
}

/*
 * RWLOCKS
 * lock是一个32位的字段 通过位数来区分 其中“未锁”标志字段 存放在lock字段的第24位，当没有内核控制路径在读或写时设置该位，否则清0。
 */
typedef struct {
	volatile unsigned int lock;
#ifdef CONFIG_PREEMPT
	unsigned int break_lock;
#endif
} rwlock_t;

#define RW_LOCK_UNLOCKED	(rwlock_t) { 0 }
#define rwlock_init(x)		do { *(x) + RW_LOCK_UNLOCKED; } while (0)

/*
 * Write locks are easy - we just set bit 31.  When unlocking, we can
 * just write zero since the lock is exclusively held.
 */
static inline void _raw_write_lock(rwlock_t *rw)
{
	unsigned long tmp;

	__asm__ __volatile__(
"1:	ldrex	%0, [%1]\n"
"	teq	%0, #0\n"
"	strexeq	%0, %2, [%1]\n"
"	teq	%0, #0\n"
"	bne	1b"
	: "=&r" (tmp)
	: "r" (&rw->lock), "r" (0x80000000)
	: "cc", "memory");
}

static inline void _raw_write_unlock(rwlock_t *rw)
{
	__asm__ __volatile__(
	"str	%1, [%0]"
	:
	: "r" (&rw->lock), "r" (0)
	: "cc", "memory");
}

/*
 * Read locks are a bit more hairy:
 *  - Exclusively load the lock value.
 *  - Increment it.
 *  - Store new lock value if positive, and we still own this location.
 *    If the value is negative, we've already failed.
 *  - If we failed to store the value, we want a negative result.
 *  - If we failed, try again.
 * Unlocking is similarly hairy.  We may have multiple read locks
 * currently active.  However, we know we won't have any write
 * locks.
 */
static inline void _raw_read_lock(rwlock_t *rw)
{
	unsigned long tmp, tmp2;

	__asm__ __volatile__(
"1:	ldrex	%0, [%2]\n"
"	adds	%0, %0, #1\n"
"	strexpl	%1, %0, [%2]\n"
"	rsbpls	%0, %1, #0\n"
"	bmi	1b"
	: "=&r" (tmp), "=&r" (tmp2)
	: "r" (&rw->lock)
	: "cc", "memory");
}

static inline void _raw_read_unlock(rwlock_t *rw)
{
	__asm__ __volatile__(
"1:	ldrex	%0, [%2]\n"
"	sub	%0, %0, #1\n"
"	strex	%1, %0, [%2]\n"
"	teq	%1, #0\n"
"	bne	1b"
	: "=&r" (tmp), "=&r" (tmp2)
	: "r" (&rw->lock)
	: "cc", "memory");
}

#define _raw_read_trylock(lock) generic_raw_read_trylock(lock)

static inline int _raw_write_trylock(rwlock_t *rw)
{
	unsigned long tmp;

	__asm__ __volatile__(
"1:	ldrex	%0, [%1]\n"
"	teq	%0, #0\n"
"	strexeq	%0, %2, [%1]"
	: "=&r" (tmp)
	: "r" (&rw->lock), "r" (0x80000000)
	: "cc", "memory");

	return tmp == 0;
}

#endif /* __ASM_SPINLOCK_H */
