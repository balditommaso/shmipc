#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pfn.h>
#include <linux/vmalloc.h>
#include <asm/page.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include "ipc_dev.h"
#include "shmipc_uapi.h"

 
static unsigned int max_peers = 8;
module_param(max_peers, uint, 0444);
MODULE_PARM_DESC(max_peers, "Maximum number of IPC peers");
MODULE_AUTHOR("Tommaso Baldi");
MODULE_DESCRIPTION("IPC based on shared memory");
MODULE_LICENSE("GPL");

 
static int shmipc_open(struct inode *inode, struct file *file);
static int shmipc_release(struct inode *inode, struct file *file);
static int shmipc_mmap(struct file *file, struct vm_area_struct *vma);
static long shmipc_ioctl(struct file *file, unsigned int cmd, unsigned long arg);


static struct file_operations shmipc_fops = {
    .owner          = THIS_MODULE,
    .open           = shmipc_open,
    .release        = shmipc_release,
    .mmap           = shmipc_mmap,
    .unlocked_ioctl = shmipc_ioctl,
};

static struct miscdevice ipc_device = {
    MISC_DYNAMIC_MINOR, "shmipc", &shmipc_fops
};


struct shmipc_peer {
    unsigned int id;
    struct file *file;
};


int shmipc_open(struct inode *inode, struct file *file)
{
    struct shmipc_peer *peer;
    unsigned int id;
    pid_t curr_pid = task_tgid_nr(current);

    id = ipc_dev_alloc_id(curr_pid);
    if (id < 0) {
        printk("Error during slot allocation of pid %u\n", curr_pid);
        return id;
    }

    peer = kzalloc(sizeof(*peer), GFP_KERNEL);
    if (!peer) {
        ipc_dev_free_id(id);
        return -ENOMEM;
    }
        
    peer->id = id;
    peer->file = file;
    file->private_data = peer;

    printk("peer %u connected\n", peer->id);
    return 0;
}


int shmipc_release(struct inode *inode, struct file *file)
{
    struct shmipc_peer *peer = file->private_data;

    printk("shmipc: peer %u disconnected\n", peer->id);
    ipc_dev_free_id(peer->id);
    kfree(peer);

    return 0;
}


/*
 * Control-plane for shmipc. Currently exposes a single command,
 * IPC_GET_LAYOUT, which lets a peer discover:
 *
 *   - its own peer ID (which output section it may write to)
 *   - every section size, so it can compute offsets into the
 *     mapping without hardcoding kernel-side constants
 *   - total_size, the exact `length` it must pass to mmap()
 *     (shmipc_mmap() rejects any other length)
 *
 * This is intentionally a read-only query: it has no side effects
 * and does not touch dev.lock, since it doesn't mutate any shared
 * state. Only the per-fd peer->id (already fixed at open() time)
 * is read.
 */
static long shmipc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct shmipc_peer *peer = file->private_data;
    struct shmipc_layout layout;

    switch (cmd) {
    case IPC_GET_LAYOUT:
        layout.peer_id = peer->id;
        layout.max_peers = dev.max_peers;
        layout.state_size = dev.state_size;
        layout.rw_size = dev.rw_size;
        layout.out_size = dev.out_size;
        layout.total_size = dev.total_size;

        if (copy_to_user((void __user *)arg, &layout, sizeof(layout)))
            return -EFAULT;

        return 0;
    
    default:
        return -ENOTTY;
    }
}


/*
 * Remap a [offset, offset+len) sub-range of dev.base, at the matching
 * offset within the VMA, using the given protection. vma->vm_page_prot
 * is temporarily overridden for the duration of this call only; the
 * caller is responsible for restoring it afterwards.
 *
 * offset/len are byte offsets/lengths within dev.base and MUST be
 * page-aligned (guaranteed by ipc_dev_init() for every section).
 */
static int shmipc_remap_segment(struct vm_area_struct *vma,
                                unsigned long offset,
                                unsigned long len,
                                pgprot_t prot)
{
    pgprot_t saved_prot = vma->vm_page_prot;
    unsigned long addr = vma->vm_start + offset;
    unsigned long pos;
    int ret = 0;

    vma->vm_page_prot = prot;
    for (pos = 0; pos < len; pos += PAGE_SIZE) {
        unsigned long pfn = vmalloc_to_pfn(dev.base + offset + pos);
        ret = remap_pfn_range(vma, addr + pos, pfn, PAGE_SIZE, vma->vm_page_prot);
        if (ret)
            break;
    }
    vma -> vm_page_prot = saved_prot;
    return ret;
}


/*
 * Map the whole shared region for the calling peer, applying
 * per-section protection:
 *
 *   - State Table:                 read-only for everyone
 *   - R/W Section:                 read-write for everyone
 *   - Output section[peer->id]:    read-write for the owner
 *   - Output section[other id]:    read-only for everyone else
 *
 * The mapping always covers dev.total_size; partial mmap() requests
 * (vm_pgoff != 0, or a length shorter than total_size) are rejected
 * to keep the section layout/offsets unambiguous.
 */
static int shmipc_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct shmipc_peer *peer = file->private_data;
    unsigned long size = vma->vm_end - vma->vm_start;
    pgprot_t ro_prot, rw_prot;
    unsigned int i;
    int ret;

    if (vma->vm_pgoff != 0 || size != dev.total_size)
        return -EINVAL;
    
    rw_prot = vma->vm_page_prot;
    ro_prot = pgprot_nx(vm_get_page_prot(vma->vm_flags & ~(VM_WRITE | VM_MAYWRITE)));

    ret = shmipc_remap_segment(vma, 0, dev.state_size, ro_prot);
    if (ret)
        return ret;

    if (dev.rw_size) {
        ret = shmipc_remap_segment(vma, ipc_dev_rw_offset(), dev.rw_size, rw_prot);
        if (ret)
            return ret;
    }

    for (i = 0; i < dev.max_peers; i++) {
        pgprot_t prot = (i == peer->id) ? rw_prot : ro_prot;
        ret = shmipc_remap_segment(vma, ipc_dev_out_offset(i), dev.out_size, prot);
        if (ret)
            return ret;
    }

    return 0;
}


static int __init shmipc_init(void)
{
    int ret;

    printk("Loading shmipc module...\n");

    ret = ipc_dev_init(max_peers);
    if (ret)
        return ret;

    ret = misc_register(&ipc_device);
    if (ret) {
        ipc_dev_destroy();
        printk("shmipc: failed to register misc device (%d)\n", ret);
        return ret;
    }

    printk("shmipc: registered /dev/%s (max peers: %d)\n", ipc_device.name, max_peers);

    return 0;
}


static void __exit shmipc_exit(void)
{
    misc_deregister(&ipc_device);
    ipc_dev_destroy();
    printk("shmipc: module unloaded\n");
}


module_init(shmipc_init);
module_exit(shmipc_exit);