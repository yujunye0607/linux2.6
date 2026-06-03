#ifndef __CHARDEV_H__
#define __CHARDEV_H__

#include <linux/ioctl.h>

#define DEVICE_NAME "mychardev"
#define CLASS_NAME  "mychar"

#define BUFFER_SIZE 1024

// IOCTL 命令定义
#define IOCTL_MAGIC 'k'

// 清空缓冲区
#define IOCTL_CLEAR_BUFFER _IO(IOCTL_MAGIC, 0)
// 设置缓冲区大小
#define IOCTL_SET_BUFFER_SIZE _IOW(IOCTL_MAGIC, 1, int)
// 获取缓冲区大小
#define IOCTL_GET_BUFFER_SIZE _IOR(IOCTL_MAGIC, 2, int)
// 获取当前读写位置
#define IOCTL_GET_POSITION _IOR(IOCTL_MAGIC, 3, loff_t)

// 最大命令数
#define IOCTL_MAX_NR 3

#endif