int my_open(struct inode *inode, struct file *file);
int my_close(struct inode *inode, struct file *file);
ssize_t my_read(struct file *file, char __user *buf, size_t len, loff_t *ppos);
