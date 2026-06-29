#include <linux/poll.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/sched.h>

#include "file-funcs.h"
#include "mmap-funcs.h"

MODULE_AUTHOR("Luca Abeni");
MODULE_DESCRIPTION("Test for misc devices");
MODULE_LICENSE("GPL");

static struct file_operations my_fops = {
  .owner   = THIS_MODULE,
  .read    = my_read,
  .open    = my_open,
  .release = my_close,
  .mmap    = my_mmap,
#if 0
  .write =        my_write,
#endif
};

static struct miscdevice test_device = {
  MISC_DYNAMIC_MINOR, "mmap-test", &my_fops
};


static int testmodule_init(void)
{
  int res;

  res = misc_register(&test_device);
  printk("Misc Register returned %d\n", res);
  my_mmap_init();

  return 0;
}

static void testmodule_exit(void)
{
  misc_deregister(&test_device);
}

module_init(testmodule_init);
module_exit(testmodule_exit);
