#include <linux/slab.h>
#include <linux/file.h>
#include <linux/perf_event.h>
#include <linux/anon_inodes.h>

#include "internal.h"

/* 512 kiB: default perf tools memory size, see perf_evlist__mmap() */
#define CPU_BUFFER_NR_PAGES	((512 * 1024) / PAGE_SIZE)

static DEFINE_PER_CPU(struct list_head, pers_events);

static struct perf_event *
add_persistent_event_on_cpu(unsigned int cpu, struct perf_event_attr *attr,
			    unsigned nr_pages)
{
	struct perf_event *event = ERR_PTR(-ENOMEM);
	struct pers_event_desc *desc;
	struct ring_buffer *buf;

	desc = kzalloc(sizeof(*desc), GFP_KERNEL);
	if (!desc)
		goto out;

	buf = rb_alloc(nr_pages, 0, cpu, 0);
	if (!buf)
		goto err_rb;

	event = perf_event_create_kernel_counter(attr, cpu, NULL, NULL, NULL);
	if (IS_ERR(event))
		goto err_event;

	rcu_assign_pointer(event->rb, buf);

	desc->event = event;
	desc->attr  = attr;

	INIT_LIST_HEAD(&desc->plist);
	list_add_tail(&desc->plist, &per_cpu(pers_events, cpu));

	/* All workie, enable event now */
	perf_event_enable(event);

	goto out;

 err_event:
	rb_put(buf);

 err_rb:
	kfree(desc);

 out:
	return event;
}

static void del_persistent_event(int cpu, struct perf_event_attr *attr)
{
	struct pers_event_desc *desc, *tmp;
	struct perf_event *event = NULL;

	list_for_each_entry_safe(desc, tmp, &per_cpu(pers_events, cpu), plist) {
		if (desc->attr->config == attr->config) {
			event = desc->event;
			break;
		}
	}

	if (!event)
		return;

	list_del(&desc->plist);

	perf_event_disable(event);
	if (event->rb) {
		rb_put(event->rb);
		rcu_assign_pointer(event->rb, NULL);
	}

	perf_event_release_kernel(event);
	put_unused_fd(desc->fd);
	kfree(desc->attr);
	kfree(desc);
}

static int __alloc_persistent_event_fd(struct pers_event_desc *desc)
{
	struct file *event_file = NULL;
	int event_fd = -1;

	event_fd = get_unused_fd();
	if (event_fd < 0)
		goto out;

	event_file = anon_inode_getfile("[pers_event]", &perf_fops,
					desc->event, O_RDONLY);
	if (IS_ERR(event_file))
		goto err_event_file;

	desc->fd = event_fd;
	fd_install(event_fd, event_file);

	goto out;


 err_event_file:
	put_unused_fd(event_fd);

 out:
	return event_fd;
}

/*
 * Create and enable the persistent version of the perf event described by
 * @attr.
 *
 * @attr: perf event descriptor
 * @nr_pages: size in pages
 */
int perf_add_persistent_event(struct perf_event_attr *attr, unsigned nr_pages)
{
	struct perf_event *event;
	int i;

	for_each_possible_cpu(i) {
		event = add_persistent_event_on_cpu(i, attr, nr_pages);
		if (IS_ERR(event))
			goto unwind;
	}
	return 0;

unwind:
	pr_err("%s: Error adding persistent event on cpu %d: %ld\n",
		__func__, i, PTR_ERR(event));

	while (--i >= 0)
		del_persistent_event(i, attr);

	return PTR_ERR(event);
}

int perf_add_persistent_event_by_id(int id)
{
	struct perf_event_attr *attr;

	attr = kzalloc(sizeof(*attr), GFP_KERNEL);
	if (!attr)
		return -ENOMEM;

	attr->sample_period	= 1;
	attr->wakeup_events	= 1;
	attr->sample_type	= PERF_SAMPLE_RAW;
	attr->persistent	= 1;
	attr->config		= id;
	attr->type		= PERF_TYPE_TRACEPOINT;
	attr->size		= sizeof(*attr);

	return perf_add_persistent_event(attr, CPU_BUFFER_NR_PAGES);
}

int perf_get_persistent_event_fd(unsigned cpu, struct perf_event_attr *attr)
{
	struct pers_event_desc *desc;

	if (cpu >= (unsigned)nr_cpu_ids)
		return -EINVAL;

	list_for_each_entry(desc, &per_cpu(pers_events, cpu), plist)
		if (desc->attr->config == attr->config)
			return __alloc_persistent_event_fd(desc);

	return -ENODEV;
}


void __init persistent_events_init(void)
{
	int i;

	for_each_possible_cpu(i)
		INIT_LIST_HEAD(&per_cpu(pers_events, i));
}
