#include <linux/poll.h>
#include <linux/mm_types.h>
#include <linux/mm.h>
#include <linux/slab.h>

#include "mmap-funcs.h"

#define MAX_PAGES 16

static void my_vma_open(struct vm_area_struct *vma);
static void my_vma_close(struct vm_area_struct *vma);
static vm_fault_t my_vma_nopage(struct vm_fault *vmf);

struct my_data {
  struct mutex s;
  struct page *pages[MAX_PAGES];
  unsigned int usage_count;
};

static struct my_data private;

static struct vm_operations_struct simple_remap_vm_ops = {
  .open  = my_vma_open,
  .close = my_vma_close,
  .fault = my_vma_nopage,
};

static void fill_page(struct page *page)
{
  char *p = page_to_virt(page);
  unsigned int i;
  char string[32] = "This is just a test string";

  for (i = 0; i < 1 << PAGE_SHIFT; i++) {
    p[i] = string[i % 32];
  }
}

static void my_vma_open(struct vm_area_struct *vma)
{
  int size = vma->vm_end - vma->vm_start;
  unsigned int i;
  struct my_data *p;

  printk("VMA open, virt %lx --- %lx (%d - %d pages), Offset %lx\n",
		  vma->vm_start, vma->vm_end, size, size >> PAGE_SHIFT, vma->vm_pgoff << PAGE_SHIFT);

  if (size > MAX_PAGES << PAGE_SHIFT) {
    printk("Cannot allocate more than %d pages!", MAX_PAGES);

    return;
  }
  p = &private;
  mutex_lock(&p->s);
  p->usage_count += 1;
  for (i = 0; i < size >> PAGE_SHIFT; i++) {
    if (p->pages[i] == NULL) {
      p->pages[i] = alloc_page(GFP_KERNEL);
      if (p->pages[i] == NULL) {
        printk("Cannot allocate the %d^th page!", i);
        /*TODO: Free the already allocated pages... */
        mutex_unlock(&p->s);

        return;
      }
      fill_page(p->pages[i]);
    }
  }
  vma->vm_private_data = p;
  mutex_unlock(&p->s);
}

static void my_vma_close(struct vm_area_struct *vma)
{
  unsigned int i;
  struct my_data *p = vma->vm_private_data;

  printk("VMA close.\n");

  mutex_lock(&p->s);
  p->usage_count--;
  if (p->usage_count == 0) {
    for (i = 0; i < MAX_PAGES; i++) {
     if (p->pages[i]) {
       __free_page(p->pages[i]);
       p->pages[i] = NULL;
     }
    }
  }
  mutex_unlock(&p->s);
}

static vm_fault_t my_vma_nopage(struct vm_fault *vmf)
{
  struct vm_area_struct *vma = vmf->vma;
  struct my_data *p = vma->vm_private_data;
  struct page *page;
  int p_offs;

  p_offs = ((vmf->address - vma->vm_start) >> PAGE_SHIFT) + vma->vm_pgoff;
  if (p_offs >= MAX_PAGES) {
    printk("Fault is out of range");

    return VM_FAULT_NOPAGE;
  }
  
  page = p->pages[p_offs];
  if (page == NULL) {
    printk("Unmapped page!!!");

    return VM_FAULT_NOPAGE;
  }
  get_page(page);
  vmf->page = page;

  return 0;
}


int my_mmap(struct file *filp, struct vm_area_struct *vma)
{
  printk("My mmap %lx - %lx (offs %lx)!!!", vma->vm_start, vma->vm_end, vma->vm_pgoff);

  vma->vm_ops = &simple_remap_vm_ops;
  my_vma_open(vma);

  return 0;
}

void my_mmap_init(void)
{
  mutex_init(&private.s);
}
