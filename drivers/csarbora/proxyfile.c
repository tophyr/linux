#include <linux/device.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/slab.h>

#define DEVICE_NAME "proxy"

#define PROXY_MAGIC 0xC5
#define PROXY_IOCTL_GET_CONTROL_FD _IOR(PROXY_MAGIC, 0, int)
#define PROXY_IOCTL_SET_LINK _IOW(PROXY_MAGIC, 1, int)
#define PROXY_IOCTL_CLEAR_LINK _IO(PROXY_MAGIC, 2)

static dev_t dev_ = MKDEV(0, 0);

struct proxyfile_data_t {
  struct list_head control_files;
  struct list_head inflight_tasks;
  struct file* linked_file;
};
static struct proxyfile_data_t* proxyfile_data(struct file* proxyfile) {
  return proxyfile->private_data;
}
static void proxyfile_data_resetlink(struct proxyfile_data_t* this, struct file* new_link) {
  if (this->linked_file) {
    fput(this->linked_file);
  }
  this->linked_file = new_link;
}

struct inflight_task_t {
  struct list_head node;
  struct task_struct* task;
};
static void inflight_task_create(struct inflight_task_t* this, struct proxyfile_data_t* proxyfile_data) {
  this->task = current;
  list_add(&this->node, &proxyfile_data->inflight_tasks);
}
static void inflight_task_destroy(struct inflight_task_t* this) {
  list_del(&this->node);
}

static void proxyfile_data_notify_inflight(struct proxyfile_data_t* this) {
  struct inflight_task_t* pos;
  list_for_each_entry(pos, &this->inflight_tasks, node) {
    set_notify_signal(pos->task);
  }
}

struct controlfile_data_t {
  struct list_head node;
  struct file* weak_proxy_file;
};
static struct controlfile_data_t* controlfile_data(struct file* controlfile) {
  return controlfile->private_data;
}

static int proxyfile_control_release(struct inode* node, struct file* controlfile) {
  list_del(&controlfile_data(controlfile)->node);

  return 0;
}

static int proxyfile_release(struct inode* node, struct file* proxyfile) {
  struct controlfile_data_t* pos;
  list_for_each_entry(pos, &proxyfile_data(proxyfile)->control_files, node) {
    pos->weak_proxy_file = NULL;
  }

  return 0;
}

static long proxyfile_control_ioctl_set_link(struct file* controlfile, struct file* proxyfile, unsigned long link_fd) {
  struct file* link_file = fget(link_fd);
  if (IS_ERR(link_file)) {
    return PTR_ERR(link_file);
  }

  if (!link_file) {
    return -EBADF;
  }

  if (link_file->f_op->release == proxyfile_release ||
      link_file->f_op->release == proxyfile_control_release) {
    fput(link_file);
    return -EBADF; // linking a proxyfile to another proxyfile (or controlfd) could result in infinite recursion
  }

  struct proxyfile_data_t* pfd = proxyfile_data(proxyfile);
  proxyfile_data_resetlink(pfd, link_file);
  proxyfile_data_notify_inflight(pfd);

  return 0;
}

static long proxyfile_control_ioctl_clear_link(struct file* controlfile, struct file* proxyfile) {
  struct proxyfile_data_t* pfd = proxyfile_data(proxyfile);
  proxyfile_data_resetlink(pfd, NULL);
  proxyfile_data_notify_inflight(pfd);

  return 0;
}

static long proxyfile_control_ioctl(struct file* controlfile, unsigned int code, unsigned long arg) {
  struct file* weak_proxyfile = controlfile_data(controlfile)->weak_proxy_file;
  if (weak_proxyfile == NULL) {
    return -ESHUTDOWN;
  }

  switch (code) {
    case PROXY_IOCTL_SET_LINK:
      return proxyfile_control_ioctl_set_link(controlfile, weak_proxyfile, arg);
    case PROXY_IOCTL_CLEAR_LINK:
      return proxyfile_control_ioctl_clear_link(controlfile, weak_proxyfile);
    default:
      return -ENOTSUPP;
  }
}

static long proxyfile_ioctl_get_control_fd(struct file* proxyfile, int __user* out_controlfd) {
  int err;

  struct inode* inodep = alloc_anon_inode(proxyfile->f_inode->i_sb);
  if (IS_ERR(inodep)) {
    err = PTR_ERR(inodep);
    goto out;
  }

  struct controlfile_data_t* controlfile_data = kmalloc(sizeof(struct controlfile_data_t), GFP_KERNEL_ACCOUNT);
  if (IS_ERR(controlfile_data)) {
    err = PTR_ERR(controlfile_data);
    goto cleanup_inodep;
  }
  controlfile_data->weak_proxy_file = proxyfile;
  list_add(&controlfile_data->node, &proxyfile_data(proxyfile)->control_files);

  static struct file_operations control_ops = {
    .unlocked_ioctl = proxyfile_control_ioctl,
    .release = proxyfile_control_release,
  };
  struct file* control_file = alloc_file_pseudo(inodep, proxyfile->f_path.mnt, "proxy", 0, &control_ops);
  if (IS_ERR(control_file)) {
    err = PTR_ERR(control_file);
    goto cleanup_controlfile_data;
  }
  control_file->private_data = controlfile_data;

  int control_fd = get_unused_fd_flags(0);
  if (control_fd < 0) {
    err = control_fd;
    goto cleanup_control_file;
  }

  if (copy_to_user(out_controlfd, &control_fd, sizeof(control_fd))) {
    err = -EFAULT;
    goto cleanup_control_fd;
  }

  fd_install(control_fd, control_file);
  err = 0;

out:
  return err;

cleanup_control_fd:
  put_unused_fd(control_fd);
cleanup_control_file:
  fput(control_file);
cleanup_controlfile_data:
  list_del(&controlfile_data->node);
  kfree(controlfile_data);
cleanup_inodep:
  iput(inodep);
  goto out;
}

#define PROXYFILE_FWD(op, proxyfile, ...) ({                              \
  struct inflight_task_t inflight_task __cleanup(inflight_task_destroy);  \
  inflight_task_create(&inflight_task, proxyfile_data(proxyfile));        \
  proxyfile_data(proxyfile)->linked_file                                  \
    ? vfs_##op(proxyfile_data(proxyfile)->linked_file, __VA_ARGS__)       \
    : -ENOTCONN;                                                          \
})

static long proxyfile_ioctl(struct file* proxyfile, unsigned int code, unsigned long arg) {
  switch (code) {
    case PROXY_IOCTL_GET_CONTROL_FD:
      return proxyfile_ioctl_get_control_fd(proxyfile, (int __user*)arg);
    default:
      return PROXYFILE_FWD(ioctl, proxyfile, code, arg);
  }
}

static ssize_t proxyfile_read(struct file* proxyfile, char __user* userbuf, size_t userbufsz, loff_t* offset) {
  return PROXYFILE_FWD(read, proxyfile, userbuf, userbufsz, offset);
}

static int proxyfile_open(struct inode* node, struct file* proxyfile) {
  struct proxyfile_data_t* proxyfile_data = kzalloc(sizeof(struct proxyfile_data_t), GFP_KERNEL_ACCOUNT);
  if (IS_ERR(proxyfile_data)) {
    return PTR_ERR(proxyfile_data);
  }

  INIT_LIST_HEAD(&proxyfile_data->control_files);
  INIT_LIST_HEAD(&proxyfile_data->inflight_tasks);

  proxyfile->private_data = proxyfile_data;

  return 0;
}

static int __init proxyfile_init(void)
{
  struct class* classp;
  struct device* devp;
  int ret;

  static struct file_operations fops = {
    .read = proxyfile_read,
    // .readv = proxyfile_readv,
    // .write = proxyfile_write,
    // .writev = proxyfile_writev,
    .unlocked_ioctl = proxyfile_ioctl,
    // .poll = proxyfile_poll,
    .open = proxyfile_open,
    .release = proxyfile_release,
  };

  ret = register_chrdev(0, DEVICE_NAME, &fops);
  if (ret < 0) {
    printk(KERN_WARNING "Registering proxyfile device failed with %d\n", ret);
    goto fail;
  }
  dev_ = MKDEV(ret, 0);
  ret = 0;

  classp = class_create(DEVICE_NAME);
  if (IS_ERR(classp)) {
    printk(KERN_WARNING "failed to create device class for " DEVICE_NAME "\n");
    ret = PTR_ERR(classp);
    goto fail_cleanup_chrdev;
  }

  devp = device_create(classp, NULL, dev_, NULL, DEVICE_NAME);
  if (IS_ERR(devp)) {
    printk(KERN_WARNING "failed to create device for " DEVICE_NAME "\n");
    ret = PTR_ERR(devp);
    goto fail_cleanup_class;
  }

  printk(KERN_INFO "Created proxyfile device with major number %d\n", MAJOR(dev_));

out:
  return ret;

fail_cleanup_class:
  class_destroy(classp);

fail_cleanup_chrdev:
  unregister_chrdev(MAJOR(dev_), DEVICE_NAME);
  dev_ = MKDEV(0, 0);

fail:
  goto out;
}

static void __exit proxyfile_exit(void)
{
  if (MAJOR(dev_) != 0) {
    return unregister_chrdev(MAJOR(dev_), DEVICE_NAME);
  }
}

module_init(proxyfile_init);
module_exit(proxyfile_exit);
MODULE_DESCRIPTION("Proxy File Driver");
MODULE_LICENSE("GPL v2");
