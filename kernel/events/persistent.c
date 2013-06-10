#include <linux/slab.h>
#include <linux/file.h>
#include <linux/perf_event.h>
#include <linux/anon_inodes.h>

#include "internal.h"

/* 512 kiB: default perf tools memory size, see perf_evlist__mmap() */
#define CPU_BUFFER_NR_PAGES	((512 * 1024) / PAGE_SIZE)

struct pers_event_desc {
	struct perf_event *event;
	struct list_head plist;
	int fd;
};

struct pers_event {
	char				*name;
	struct perf_event_attr		attr;
	struct perf_pmu_events_attr	sysfs;
};

static struct pmu persistent_pmu;
static DEFINE_PER_CPU(struct list_head, pers_events);
static DEFINE_PER_CPU(struct mutex, pers_events_lock);

static struct pers_event_desc
*get_persistent_event(int cpu, struct perf_event_attr *attr)
{
	struct pers_event_desc *desc;

	list_for_each_entry(desc, &per_cpu(pers_events, cpu), plist) {
		if (desc->event->attr.config == attr->config)
			return desc;
	}

	return NULL;
}

static struct perf_event *
add_persistent_event_on_cpu(unsigned int cpu, struct perf_event_attr *attr,
			    unsigned nr_pages)
{
	struct perf_event *event;
	struct pers_event_desc *desc;
	struct ring_buffer *buf;

	mutex_lock(&per_cpu(pers_events_lock, cpu));

	desc = get_persistent_event(cpu, attr);
	if (desc) {
		event = ERR_PTR(-EEXIST);
		goto out;
	}

	desc = kzalloc(sizeof(*desc), GFP_KERNEL);
	if (!desc) {
		event = ERR_PTR(-ENOMEM);
		goto out;
	}

	event = perf_event_create_kernel_counter(attr, cpu, NULL, NULL, NULL);
	if (IS_ERR(event))
		goto err_event;

	buf = rb_alloc(nr_pages, 0, cpu, 0);
	if (!buf)
		goto err_rb;

	rcu_assign_pointer(event->rb, buf);

	desc->event = event;

	INIT_LIST_HEAD(&desc->plist);
	list_add_tail(&desc->plist, &per_cpu(pers_events, cpu));

	/* All workie, enable event now */
	perf_event_enable(event);

	goto out;
err_rb:
	perf_event_release_kernel(event);
	event = ERR_PTR(-ENOMEM);
err_event:
	kfree(desc);
out:
	mutex_unlock(&per_cpu(pers_events_lock, cpu));
	return event;
}

static void del_persistent_event(int cpu, struct perf_event_attr *attr)
{
	struct pers_event_desc *desc;
	struct perf_event *event;

	mutex_lock(&per_cpu(pers_events_lock, cpu));

	desc = get_persistent_event(cpu, attr);
	if (!desc)
		goto out;
	event = desc->event;

	list_del(&desc->plist);

	perf_event_disable(event);
	perf_event_release_kernel(event);
	put_unused_fd(desc->fd);
	kfree(desc);
out:
	mutex_unlock(&per_cpu(pers_events_lock, cpu));
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

static int pers_event_sysfs_register(struct pers_event *event);

int perf_add_persistent_event_by_id(char* name, int id)
{
	struct pers_event	*event;
	struct perf_event_attr	*attr;
	int ret = -ENOMEM;

	event = kzalloc(sizeof(*event), GFP_KERNEL);
	if (!event)
		return -ENOMEM;
	event->name = kstrdup(name, GFP_KERNEL);
	if (!event->name)
		goto fail;

	event->sysfs.id		= id;

	attr = &event->attr;
	attr->sample_period	= 1;
	attr->wakeup_events	= 1;
	attr->sample_type	= PERF_SAMPLE_RAW;
	attr->persistent	= 1;
	attr->config		= id;
	attr->type		= PERF_TYPE_TRACEPOINT;
	attr->size		= sizeof(*attr);

	ret = perf_add_persistent_event(attr, CPU_BUFFER_NR_PAGES);
	if (ret)
		goto fail;

	pers_event_sysfs_register(event);

	return 0;
fail:
	kfree(event->name);
	kfree(event);

	return ret;
}

int perf_get_persistent_event_fd(unsigned cpu, struct perf_event_attr *attr)
{
	struct pers_event_desc *desc;
	int event_fd = -ENODEV;

	if (cpu >= (unsigned)nr_cpu_ids)
		return -EINVAL;

	mutex_lock(&per_cpu(pers_events_lock, cpu));

	desc = get_persistent_event(cpu, attr);
	if (!desc)
		goto out;

	event_fd = anon_inode_getfd("[pers_event]", &perf_fops,
				desc->event, O_RDONLY);
	if (event_fd >= 0)
		desc->fd = event_fd;
out:
	mutex_unlock(&per_cpu(pers_events_lock, cpu));

	return event_fd;
}

PMU_FORMAT_ATTR(persistent, "attr5:23");

static struct attribute *persistent_format_attrs[] = {
	&format_attr_persistent.attr,
	NULL,
};

static struct attribute_group persistent_format_group = {
	.name = "format",
	.attrs = persistent_format_attrs,
};

#define MAX_EVENTS 16

static struct attribute *persistent_events_attr[MAX_EVENTS + 1] = { };

static struct attribute_group persistent_events_group = {
	.name = "events",
	.attrs = persistent_events_attr,
};

static const struct attribute_group *persistent_attr_groups[] = {
	&persistent_format_group,
	NULL, /* placeholder: &persistent_events_group */
	NULL,
};
#define EVENTS_GROUP	(persistent_attr_groups[1])

static ssize_t pers_event_sysfs_show(struct device *dev,
				struct device_attribute *__attr, char *page)
{
	struct perf_pmu_events_attr *attr =
		container_of(__attr, struct perf_pmu_events_attr, attr);
	return sprintf(page, "persistent,config=%lld",
		(unsigned long long)attr->id);
}

static int pers_event_sysfs_register(struct pers_event *event)
{
	struct device_attribute *attr = &event->sysfs.attr;
	int idx;

	*attr = (struct device_attribute)__ATTR(, 0444, pers_event_sysfs_show,
						NULL);
	attr->attr.name = event->name;

	/* add sysfs attr to events: */
	for (idx = 0; idx < MAX_EVENTS; idx++) {
		if (!cmpxchg(persistent_events_attr + idx, NULL, &attr->attr))
			break;
	}

	if (idx >= MAX_EVENTS)
		return -ENOSPC;
	if (!idx)
		EVENTS_GROUP = &persistent_events_group;
	if (!persistent_pmu.dev)
		return 0;	/* sysfs not yet initialized */
	if (idx)
		return sysfs_update_group(&persistent_pmu.dev->kobj,
					EVENTS_GROUP);
	return sysfs_create_group(&persistent_pmu.dev->kobj, EVENTS_GROUP);
}

static int persistent_pmu_init(struct perf_event *event)
{
	if (persistent_pmu.type != event->attr.type)
		return -ENOENT;

	/* Not a persistent event. */
	return -EFAULT;
}

static struct pmu persistent_pmu = {
	.event_init	= persistent_pmu_init,
	.attr_groups	= persistent_attr_groups,
};

void __init persistent_events_init(void)
{
	int cpu;

	perf_pmu_register(&persistent_pmu, "persistent", -1);

	for_each_possible_cpu(cpu) {
		INIT_LIST_HEAD(&per_cpu(pers_events, cpu));
		mutex_init(&per_cpu(pers_events_lock, cpu));
	}
}
