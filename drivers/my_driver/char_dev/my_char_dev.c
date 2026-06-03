#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include "include/my_char_dev.h"

#define MY_CHARDEV_MAJOR 230
static int my_chardev_major=MY_CHARDEV_MAJOR;
module_param(my_chardev_major,int,S_IRUGO);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("A character device driver with llseek and ioctl");
MODULE_VERSION("1.0");

// 设备私有数据结构
struct chardev_data {
    char *buffer;           // 数据缓冲区
    loff_t buffer_size;     // 缓冲区大小
    loff_t read_pos;        // 当前读位置
    loff_t write_pos;       // 当前写位置
    struct mutex lock;      // 互斥锁
    struct cdev cdev;       // 字符设备对象，指向内核cdev结构体的指针 是cdev类型 不是指向自己 
    dev_t dev_num;          // 设备号
};

// 全局变量
static struct chardev_data *g_dev;
static struct class *g_class = NULL;
static int major = 0;
static int minor = 0;

// 函数声明
static loff_t chardev_llseek(struct file *filp, loff_t offset, int whence);
static long chardev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
static ssize_t chardev_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos);
static ssize_t chardev_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos);
static int chardev_open(struct inode *inode, struct file *filp);
static int chardev_release(struct inode *inode, struct file *filp);

// 文件操作结构体
static struct file_operations chardev_fops = {
    .owner = THIS_MODULE,
    .open = chardev_open,
    .release = chardev_release,
    .read = chardev_read,
    .write = chardev_write,
    .llseek = chardev_llseek,
    .unlocked_ioctl = chardev_ioctl,
};

/**
 * chardev_llseek - 修改文件读写位置
 * @filp: 文件指针
 * @offset: 偏移量
 * @whence: 起始位置 (SEEK_SET, SEEK_CUR, SEEK_END)
 */
static loff_t chardev_llseek(struct file *filp, loff_t offset, int whence)
{
    struct chardev_data *dev = filp->private_data;
    loff_t new_pos;
    loff_t max_size = dev->buffer_size;
    
    mutex_lock(&dev->lock);
    
    switch (whence) {
    case SEEK_SET:
        new_pos = offset;
        break;
    case SEEK_CUR:
        new_pos = filp->f_pos + offset;
        break;
    case SEEK_END:
        new_pos = max_size + offset;
        break;
    default:
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }
    
    // 检查边界
    if (new_pos < 0 || new_pos > max_size) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }
    
    filp->f_pos = new_pos;
    dev->read_pos = new_pos;
    dev->write_pos = new_pos;
    
    mutex_unlock(&dev->lock);
    
    printk(KERN_DEBUG "llseek: new position=%lld\n", new_pos);
    return new_pos;
}

/**
 * chardev_ioctl - 设备控制函数
 * @filp: 文件指针
 * @cmd: 命令码
 * @arg: 参数
 */
static long chardev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct chardev_data *dev = filp->private_data;
    int ret = 0;
    int new_size;
    char *new_buffer;
    
    // 检查命令类型是否正确
    if (_IOC_TYPE(cmd) != IOCTL_MAGIC) {
        printk(KERN_WARNING "Invalid ioctl type: %c\n", _IOC_TYPE(cmd));
        return -ENOTTY;
    }
    
    // 检查命令号范围
    if (_IOC_NR(cmd) > IOCTL_MAX_NR) {
        printk(KERN_WARNING "Invalid ioctl number: %d\n", _IOC_NR(cmd));
        return -ENOTTY;
    }
    
    mutex_lock(&dev->lock);
    
    switch (cmd) {
    case IOCTL_CLEAR_BUFFER:
        // 清空缓冲区
        memset(dev->buffer, 0, dev->buffer_size);
        dev->read_pos = 0;
        dev->write_pos = 0;
        filp->f_pos = 0;
        printk(KERN_INFO "Buffer cleared\n");
        break;
        
    case IOCTL_SET_BUFFER_SIZE:
        // 设置缓冲区大小
        ret = copy_from_user(&new_size, (int __user *)arg, sizeof(int));
        if (ret) {
            ret = -EFAULT;
            break;
        }
        
        if (new_size <= 0 || new_size > 1024*1024) {  // 限制最大 1MB
            printk(KERN_WARNING "Invalid buffer size: %d\n", new_size);
            ret = -EINVAL;
            break;
        }
        
        // 分配新缓冲区
        new_buffer = kmalloc(new_size, GFP_KERNEL);
        if (!new_buffer) {
            ret = -ENOMEM;
            break;
        }
        
        // 复制旧数据
        if (dev->buffer && dev->write_pos > 0) {
            size_t copy_size = min_t(size_t, dev->write_pos, new_size);
            memcpy(new_buffer, dev->buffer, copy_size);
            dev->read_pos = min(dev->read_pos, (loff_t)new_size);
            dev->write_pos = min(dev->write_pos, (loff_t)new_size);
            filp->f_pos = min(filp->f_pos, (loff_t)new_size);
        } else {
            dev->read_pos = 0;
            dev->write_pos = 0;
            filp->f_pos = 0;
        }
        
        // 替换缓冲区
        kfree(dev->buffer);
        dev->buffer = new_buffer;
        dev->buffer_size = new_size;
        
        printk(KERN_INFO "Buffer size changed to %d bytes\n", new_size);
        break;
        
    case IOCTL_GET_BUFFER_SIZE:
        // 获取缓冲区大小
        ret = copy_to_user((int __user *)arg, &dev->buffer_size, sizeof(int));
        if (ret)
            ret = -EFAULT;
        break;
        
    case IOCTL_GET_POSITION:
        // 获取当前读写位置
        ret = copy_to_user((loff_t __user *)arg, &dev->read_pos, sizeof(loff_t));
        if (ret)
            ret = -EFAULT;
        break;
        
    default:
        ret = -ENOTTY;
        break;
    }
    
    mutex_unlock(&dev->lock);
    return ret;
}

/**
 * chardev_read - 从设备读取数据
 */
static ssize_t chardev_read(struct file *filp, char __user *buf, 
                            size_t count, loff_t *f_pos)
{
    struct chardev_data *dev = filp->private_data;
    ssize_t ret = 0;
    size_t available;
    size_t to_read;
    
    mutex_lock(&dev->lock);
    
    // 计算可读数据量
    if (*f_pos >= dev->write_pos) {
        ret = 0;  // EOF
        goto out;
    }
    
    available = dev->write_pos - *f_pos;
    to_read = min(count, available);
    
    // 复制数据到用户空间
    if (copy_to_user(buf, dev->buffer + *f_pos, to_read)) {
        ret = -EFAULT;
        goto out;
    }
    
    *f_pos += to_read;
    dev->read_pos = *f_pos;
    ret = to_read;
    
    printk(KERN_DEBUG "Read %zu bytes at position %lld\n", to_read, *f_pos);
    
out:
    mutex_unlock(&dev->lock);
    return ret;
}

/**
 * chardev_write - 向设备写入数据
 */
static ssize_t chardev_write(struct file *filp, const char __user *buf,
                             size_t count, loff_t *f_pos)
{
    struct chardev_data *dev = filp->private_data;
    ssize_t ret = 0;
    size_t available;
    size_t to_write;
    
    mutex_lock(&dev->lock);
    
    // 检查是否有足够空间
    if (*f_pos >= dev->buffer_size) {
        ret = -ENOSPC;
        goto out;
    }
    
    available = dev->buffer_size - *f_pos;
    to_write = min(count, available);
    
    // 从用户空间复制数据
    if (copy_from_user(dev->buffer + *f_pos, buf, to_write)) {
        ret = -EFAULT;
        goto out;
    }
    
    *f_pos += to_write;
    if (*f_pos > dev->write_pos)
        dev->write_pos = *f_pos;
    dev->read_pos = *f_pos;
    ret = to_write;
    
    printk(KERN_DEBUG "Wrote %zu bytes at position %lld\n", to_write, *f_pos);
    
out:
    mutex_unlock(&dev->lock);
    return ret;
}

/**
 * chardev_open - 打开设备
 */
static int chardev_open(struct inode *inode, struct file *filp)
{
    /* container_of() 获取结构体指针 目的是从inode->i_cdev成员指针反向获取到chardev_data类型的整个结构体的指针，cdev是chardev_data中的名称*/
    struct chardev_data *dev = container_of(inode->i_cdev, 
                                            struct chardev_data, cdev);
    /* 将设备数据指针赋值给filp->private_data，从而可以直接访问设备数据结构体的成员变量 */
    filp->private_data = dev;
    filp->f_pos = dev->read_pos;
    
    mutex_lock(&dev->lock);
    printk(KERN_INFO "Device opened, read_pos=%lld, write_pos=%lld\n",
           dev->read_pos, dev->write_pos);
    mutex_unlock(&dev->lock);
    
    return 0;
}

/**
 * chardev_release - 关闭设备
 */
static int chardev_release(struct inode *inode, struct file *filp)
{
    printk(KERN_INFO "Device closed\n");
    return 0;
}

/**
 * chardev_init - 模块初始化
 */
static int __init chardev_init(void)
{
    int ret;
    
    printk(KERN_INFO "Initializing character device\n");

    // 分配设备数据
    g_dev = kzalloc(sizeof(struct chardev_data), GFP_KERNEL);
    if (!g_dev) {
        printk(KERN_ERR "Failed to allocate device data\n");
        return -ENOMEM;
    }
    // 构造设备号
    g_dev->dev_num = MKDEV(my_chardev_major, 0);
    // 初始化互斥锁
    mutex_init(&g_dev->lock);
    
    // 分配缓冲区
    g_dev->buffer_size = BUFFER_SIZE;
    g_dev->buffer = kzalloc(g_dev->buffer_size, GFP_KERNEL);//GFP = Get Free Pages（获取空闲页）
    if (!g_dev->buffer) {
        printk(KERN_ERR "Failed to allocate buffer\n");
        ret = -ENOMEM;
        goto err_free_dev;
    }
    
    g_dev->read_pos = 0;
    g_dev->write_pos = 0;
    
    // 动态分配设备号
    if (my_chardev_major) {
        ret = register_chrdev_region(g_dev->dev_num, 1, DEVICE_NAME);//静态注册设备号
    } else {
        ret = alloc_chrdev_region(&g_dev->dev_num, 0, 1, DEVICE_NAME);//动态分配设备号 注意传入g_dev->dev_num的地址
    }
    if (ret < 0) {
        printk(KERN_ERR "Failed to allocate device number\n");
        goto err_free_buffer;
    }
    
    major = MAJOR(g_dev->dev_num);
    minor = MINOR(g_dev->dev_num);
    printk(KERN_INFO "Allocated major=%d, minor=%d\n", major, minor);
    
    // 初始化 cdev
    cdev_init(&g_dev->cdev, &chardev_fops);
    g_dev->cdev.owner = THIS_MODULE;
    
    // 添加 cdev 到内核的 cdev_map 哈希表，这一步不会创建dev节点
    ret = cdev_add(&g_dev->cdev, g_dev->dev_num, 1);
    if (ret < 0) {
        printk(KERN_ERR "Failed to add cdev\n");
        goto err_unregister_dev;
    }
    
    // 创建 device class
    g_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(g_class)) {
        printk(KERN_ERR "Failed to create class\n");
        ret = PTR_ERR(g_class);
        goto err_cdev_del;
    }
    
    // 创建设备节点
    if (device_create(g_class, NULL, g_dev->dev_num, NULL, 
                      DEVICE_NAME) == NULL) {
        printk(KERN_ERR "Failed to create device\n");
        ret = -ENOMEM;
        goto err_class_destroy;
    }
    
    printk(KERN_INFO "Device initialized successfully\n");
    printk(KERN_INFO "You can use: mknod /dev/%s c %d %d\n", 
           DEVICE_NAME, major, minor);
    
    return 0;

err_class_destroy:
    class_destroy(g_class);
err_cdev_del:
    cdev_del(&g_dev->cdev);
err_unregister_dev:
    unregister_chrdev_region(g_dev->dev_num, 1);
err_free_buffer:
    kfree(g_dev->buffer);
err_free_dev:
    kfree(g_dev);
    return ret;
}

/**
 * chardev_exit - 模块退出
 */
static void __exit chardev_exit(void)
{
    printk(KERN_INFO "Exiting character device\n");
    
    // 销毁设备节点和 class
    device_destroy(g_class, g_dev->dev_num);
    class_destroy(g_class);
    
    // 删除 cdev
    cdev_del(&g_dev->cdev);
    
    // 释放设备号
    unregister_chrdev_region(g_dev->dev_num, 1);
    
    // 释放缓冲区
    kfree(g_dev->buffer);
    
    // 释放设备数据
    kfree(g_dev);
    
    printk(KERN_INFO "Device cleanup complete\n");
}

module_init(chardev_init);
module_exit(chardev_exit);