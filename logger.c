#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uio.h>
#include <linux/version.h>

struct logger_channel {
	struct mutex mutex;
	const char *tag;
	char *buffer;
	size_t start;
	size_t end;
	pid_t pid;
};

static void logger_emit(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 10, 0)
	vprintk_emit(3, LOGLEVEL_INFO, NULL, 0, fmt, args);
#else
	vprintk_emit(3, LOGLEVEL_INFO, NULL, fmt, args);
#endif

	va_end(args);
}

static ssize_t logger_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct logger_channel *channel = iocb->ki_filp->private_data;
	size_t len = iov_iter_count(from);
	char *buffer;
	size_t start;
	size_t end;

	mutex_lock(&channel->mutex);

	if (channel->buffer && channel->pid != task_pid_nr(current)) {
		logger_emit("%s[%d]: %.*s\n", channel->tag, channel->pid,
			    channel->end - channel->start,
			    &channel->buffer[channel->start]);

		kfree(channel->buffer);
		channel->buffer = NULL;
		channel->start = 0;
		channel->end = 0;
	}

	buffer = kmalloc(channel->end - channel->start + len, GFP_KERNEL);
	if (!buffer) {
		mutex_unlock(&channel->mutex);
		return -ENOMEM;
	}

	if (channel->buffer)
		memcpy(buffer, &channel->buffer[channel->start],
		       channel->end - channel->start);

	if (!copy_from_iter_full(&buffer[channel->end], len, from)) {
		kfree(buffer);
		mutex_unlock(&channel->mutex);
		return -EFAULT;
	}

	start = 0;
	for (end = channel->end; end < channel->end + len; end++) {
		if (buffer[end] != '\n')
			continue;

		if (start != end)
			logger_emit("%s[%d]: %.*s\n", channel->tag,
				    task_pid_nr(current), end - start,
				    &buffer[start]);

		start = end + 1;
	}

	kfree(channel->buffer);

	if (start != end) {
		channel->buffer = buffer;
		channel->start = start;
		channel->end = end;
		channel->pid = task_pid_nr(current);
	} else {
		kfree(buffer);
		channel->buffer = NULL;
		channel->start = 0;
		channel->end = 0;
	}

	mutex_unlock(&channel->mutex);

	return len;
}

static int logger_open(struct inode *inode, struct file *filp)
{
	struct logger_channel *channel;

	channel = kzalloc(sizeof(*channel), GFP_KERNEL);
	if (!channel)
		return -ENOMEM;

	mutex_init(&channel->mutex);
	channel->tag = current->comm;

	filp->private_data = channel;

	return 0;
}

static int logger_release(struct inode *inode, struct file *filp)
{
	struct logger_channel *channel = filp->private_data;

	if (channel->buffer) {
		logger_emit("%s[%d]: %.*s\n", channel->tag, channel->pid,
			    channel->end - channel->start,
			    &channel->buffer[channel->start]);

		kfree(channel->buffer);
	}

	mutex_destroy(&channel->mutex);

	kfree(filp->private_data);

	return 0;
}

static const struct file_operations logger_fops = {
	.owner = THIS_MODULE,
	.write_iter = logger_write_iter,
	.open = logger_open,
	.release = logger_release,
};

static struct miscdevice logger_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "logger",
	.fops = &logger_fops,
	.mode = S_IWUGO,
};

static int logger_init(void)
{
	return misc_register(&logger_misc);
}

static void logger_exit(void)
{
	misc_deregister(&logger_misc);
}

module_init(logger_init);
module_exit(logger_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Tian Yuanhao <tianyuanhao@aliyun.com>");
MODULE_DESCRIPTION("Kernel logger");
