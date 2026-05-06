#ifndef _LINUX_LINKAGE_H
#define _LINUX_LINKAGE_H

#include <linux/config.h>
#include <asm/linkage.h>

#ifdef __cplusplus
#define CPP_ASMLINKAGE extern "C"
#else
#define CPP_ASMLINKAGE
#endif

/* asmlinkage 是 Linux 内核中一个非常重要的宏，它的作用是告诉编译器：这个函数的参数不是通过寄存器传递的，而是全部通过堆栈（stack）来传递。 */
#ifndef asmlinkage
#define asmlinkage CPP_ASMLINKAGE
#endif

#ifndef prevent_tail_call
# define prevent_tail_call(ret) do { } while (0)
#endif

#ifndef __ALIGN
#define __ALIGN		.align 4,0x90
#define __ALIGN_STR	".align 4,0x90"
#endif

#ifdef __ASSEMBLY__

#define ALIGN __ALIGN
#define ALIGN_STR __ALIGN_STR

#define ENTRY(name) \
  .globl name; \
  ALIGN; \
  name:

#endif

#define NORET_TYPE    /**/
#define ATTRIB_NORET  __attribute__((noreturn))
#define NORET_AND     noreturn,

#ifndef FASTCALL
/* 空宏 从而在所有现代平台上禁用这个过时的优化*/
#define FASTCALL(x)	x
#define fastcall
#endif

#endif
