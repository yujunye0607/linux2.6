#ifndef _LINUX_CDEV_H
#define _LINUX_CDEV_H
#ifdef __KERNEL__

/* 字符设备结构体 */

struct cdev {
	struct kobject kobj;
	struct module *owner; //所属模块
	struct file_operations *ops; // 字符设备操作函数指针
	struct list_head list; // 字符设备链表头
	dev_t dev; // 字符设备号
	unsigned int count; // 字符设备数量
};

void cdev_init(struct cdev *, struct file_operations *);

struct cdev *cdev_alloc(void);

void cdev_put(struct cdev *p);

int cdev_add(struct cdev *, dev_t, unsigned);

void cdev_del(struct cdev *);

void cd_forget(struct inode *);

#endif
#endif
