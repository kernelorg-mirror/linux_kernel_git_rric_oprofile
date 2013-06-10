#include <linux/slab.h>
#include <linux/perf_event.h>
#include <linux/ftrace_event.h>

#include "internal.h"

/* 512 kiB: default perf tools memory size, see perf_evlist__mmap() */
#define CPU_BUFFER_NR_PAGES	((512 * 1024) / PAGE_SIZE)

struct pevent {
	struct perf_pmu_events_attr sysfs;
	char		*name;
	int		id;
};

static struct pmu persistent_pmu;
static DEFINE_PER_CPU(struct list_head, pevents);
static DEFINE_PER_CPU(struct mutex, pevents_lock);

/* Must be protected with pevents_lock. */
static struct perf_event *__pevent_find(int cpu, int id)
{
	struct perf_event *event;

	list_for_each_entry(event, &per_cpu(pevents, cpu), pevent_entry) {
		if (event->pevent_id == id)
			return event;
	}

	return NULL;
}

static int pevent_add(struct pevent *pevent, struct perf_event *event)
{
	int ret = -EEXIST;
	int cpu = event->cpu;

	mutex_lock(&per_cpu(pevents_lock, cpu));

	if (__pevent_find(cpu, pevent->id))
		goto unlock;

	if (event->pevent_id)
		goto unlock;

	ret = 0;
	event->pevent_id = pevent->id;
	list_add_tail(&event->pevent_entry, &per_cpu(pevents, cpu));
unlock:
	mutex_unlock(&per_cpu(pevents_lock, cpu));

	return ret;
}

static struct perf_event *pevent_del(struct pevent *pevent, int cpu)
{
	struct perf_event *event;

	mutex_lock(&per_cpu(pevents_lock, cpu));

	event = __pevent_find(cpu, pevent->id);
	if (event) {
		list_del(&event->pevent_entry);
		event->pevent_id = 0;
	}

	mutex_unlock(&per_cpu(pevents_lock, cpu));

	return event;
}

static void persistent_event_release(struct perf_event *event)
{
	/*
	 * Safe since we hold &event->mmap_count. The ringbuffer is
	 * released with put_event() if there are no other references.
	 * In this case there are also no other mmaps.
	 */
	atomic_dec(&event->rb->mmap_count);
	atomic_dec(&event->mmap_count);
	put_event(event);
}

static int persistent_event_open(int cpu, struct pevent *pevent,
				struct perf_event_attr *attr, int nr_pages)
{
	struct perf_event *event;
	int ret;

	event = perf_event_create_kernel_counter(attr, cpu, NULL, NULL, NULL);
	if (IS_ERR(event))
		return PTR_ERR(event);

	if (nr_pages < 0)
		nr_pages = CPU_BUFFER_NR_PAGES;

	ret = perf_alloc_rb(event, nr_pages, 0);
	if (ret)
		goto fail;

	ret = pevent_add(pevent, event);
	if (ret)
		goto fail;

	atomic_inc(&event->mmap_count);

	/* All workie, enable event now */
	perf_event_enable(event);

	return ret;
fail:
	perf_event_release_kernel(event);
	return ret;
}

static void persistent_event_close(int cpu, struct pevent *pevent)
{
	struct perf_event *event = pevent_del(pevent, cpu);
	if (event)
		persistent_event_release(event);
}

static int pevent_sysfs_register(struct pevent *event);

static int __maybe_unused
persistent_open(char *name, struct perf_event_attr *attr, int nr_pages)
{
	struct pevent *pevent;
	char id_buf[32];
	int cpu;
	int ret = 0;

	pevent = kzalloc(sizeof(*pevent), GFP_KERNEL);
	if (!pevent)
		return -ENOMEM;

	pevent->id = attr->config;

	if (!name) {
		snprintf(id_buf, sizeof(id_buf), "%d", pevent->id);
		name = id_buf;
	}

	pevent->name = kstrdup(name, GFP_KERNEL);
	if (!pevent->name) {
		ret = -ENOMEM;
		goto fail;
	}

	pevent->sysfs.id = pevent->id;

	for_each_possible_cpu(cpu) {
		ret = persistent_event_open(cpu, pevent, attr, nr_pages);
		if (ret)
			goto fail;
	}

	ret = pevent_sysfs_register(pevent);
	if (ret)
		goto fail;

	return 0;
fail:
	for_each_possible_cpu(cpu)
		persistent_event_close(cpu, pevent);
	kfree(pevent->name);
	kfree(pevent);

	pr_err("%s: Error adding persistent event: %d\n",
		__func__, ret);

	return ret;
}

#ifdef CONFIG_EVENT_TRACING

int perf_add_persistent_tp(struct ftrace_event_call *tp)
{
	struct perf_event_attr attr;

	memset(&attr, 0, sizeof(attr));
	attr.sample_period	= 1;
	attr.wakeup_events	= 1;
	attr.sample_type	= PERF_SAMPLE_RAW;
	attr.persistent		= 1;
	attr.config		= tp->event.type;
	attr.type		= PERF_TYPE_TRACEPOINT;
	attr.size		= sizeof(attr);

	return persistent_open(tp->name, &attr, -1);
}

#endif /* CONFIG_EVENT_TRACING */

int perf_get_persistent_event_fd(int cpu, int id)
{
	struct perf_event *event;
	int event_fd = 0;

	if ((unsigned)cpu >= nr_cpu_ids)
		return -EINVAL;

	/* Must be root for persistent events */
	if (perf_paranoid_cpu() && !capable(CAP_SYS_ADMIN))
		return -EACCES;

	mutex_lock(&per_cpu(pevents_lock, cpu));
	event = __pevent_find(cpu, id);
	if (!event || !try_get_event(event))
		event_fd = -ENOENT;
	mutex_unlock(&per_cpu(pevents_lock, cpu));

	if (event_fd)
		return event_fd;

	event_fd = perf_get_fd(event);
	if (event_fd < 0)
		put_event(event);

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

static struct attribute *pevents_attr[MAX_EVENTS + 1] = { };

static struct attribute_group pevents_group = {
	.name = "events",
	.attrs = pevents_attr,
};

static const struct attribute_group *persistent_attr_groups[] = {
	&persistent_format_group,
	NULL,			/* placeholder: &pevents_group */
	NULL,
};
#define EVENTS_GROUP_PTR	(&persistent_attr_groups[1])

static ssize_t pevent_sysfs_show(struct device *dev,
				struct device_attribute *__attr, char *page)
{
	struct perf_pmu_events_attr *attr =
		container_of(__attr, struct perf_pmu_events_attr, attr);
	return sprintf(page, "persistent,config=%lld",
		(unsigned long long)attr->id);
}

static int pevent_sysfs_register(struct pevent *pevent)
{
	struct perf_pmu_events_attr *sysfs = &pevent->sysfs;
	struct attribute *attr = &sysfs->attr.attr;
	struct device *dev = persistent_pmu.dev;
	const struct attribute_group **group = EVENTS_GROUP_PTR;
	int idx;

	sysfs->id	= pevent->id;
	sysfs->attr	= (struct device_attribute)
				__ATTR(, 0444, pevent_sysfs_show, NULL);
	attr->name	= pevent->name;
	sysfs_attr_init(attr);

	/* add sysfs attr to events: */
	for (idx = 0; idx < MAX_EVENTS; idx++) {
		if (!cmpxchg(pevents_attr + idx, NULL, attr))
			break;
	}

	if (idx >= MAX_EVENTS)
		return -ENOSPC;
	if (!idx)
		*group = &pevents_group;
	if (!dev)
		return 0;	/* sysfs not yet initialized */
	if (idx)
		return sysfs_add_file_to_group(&dev->kobj, attr, (*group)->name);
	return sysfs_create_group(&persistent_pmu.dev->kobj, *group);
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

void __init perf_register_persistent(void)
{
	int cpu;

	perf_pmu_register(&persistent_pmu, "persistent", PERF_TYPE_PERSISTENT);

	for_each_possible_cpu(cpu) {
		INIT_LIST_HEAD(&per_cpu(pevents, cpu));
		mutex_init(&per_cpu(pevents_lock, cpu));
	}
}
