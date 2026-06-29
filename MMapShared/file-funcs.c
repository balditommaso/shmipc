#include <linux/poll.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/sched.h>

#include "file-funcs.h"

int my_open(struct inode *inode, struct file *file)
{
  int *count;

  count = kmalloc(sizeof(int), GFP_USER);
  if (count == NULL) {
    return -1;
  }

  *count = 0;
  file->private_data = count;
  
  printk("Device open!\n");

  return 0;
}

int my_close(struct inode *inode, struct file *file)
{
  kfree(file->private_data);
  printk("Device close!\n");

  return 0;
}

ssize_t my_read(struct file *file, char __user *buf, size_t len, loff_t *ppos)
{
  int res, err, *count;

  count = file->private_data;
  *count = *count + 1;
  if (*count == 10) {
    set_current_state(TASK_INTERRUPTIBLE);
    schedule_timeout(5000);

    return 0;
  }

  if (len > 9) {
    res = 9;
  } else {
    res = len;
  }
  err = copy_to_user(buf, "hi there!", res);
  if (err) {
    return -EFAULT;
  }
  printk("Read %lu (%d)\n", len, *count);

  return res;
}
