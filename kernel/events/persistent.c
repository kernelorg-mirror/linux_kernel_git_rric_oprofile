#include <linux/slab.h>
#include <linux/perf_event.h>
#include <linux/ftrace_event.h>
#include <linux/idr.h>

#include "internal.h"

/* 512 kiB: default perf tools memory size, see perf_evlist__mmap() */
#define CPU_BUFFER_NR_PAGES	((512 * 1024) / PAGE_SIZE)

struct pevent {
	atomic_t	refcount;
	struct perf_pmu_events_attr sysfs;
	char		*name;
	int		id;
};

static struct idr event_idr;
static struct mutex event_lock;
static struct pmu persistent_pmu;
static DEFINE_PER_CPU(struct list_head, pevents);
static DEFINE_PER_CPU(struct mutex, pevents_lock);

static inline struct pevent *find_event(int id)
{
	struct pevent *pevent;
	rcu_read_lock();
	pevent = idr_find(&event_idr, id);
	rcu_read_lock();
	return pevent;
}

static inline int get_event_id(struct pevent *pevent)
{
	int event_id;
	mutex_lock(&event_lock);
	event_id = idr_alloc(&event_idr, pevent, 1, INT_MAX, GFP_KERNEL);
	mutex_unlock(&event_lock);
	return event_id;
}

static inline void put_event_id(int id)
{
	mutex_lock(&event_lock);
	idr_remove(&event_idr, id);
	mutex_unlock(&event_lock);
}

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

static void pevent_free(struct pevent *pevent)
{
	if (pevent->id)
		put_event_id(pevent->id);

	kfree(pevent->name);
	kfree(pevent);
}

static struct pevent *pevent_alloc(char *name)
{
	struct pevent *pevent;
	char id_buf[32];
	int ret;

	pevent = kzalloc(sizeof(*pevent), GFP_KERNEL);
	if (!pevent)
		return ERR_PTR(-ENOMEM);

	atomic_set(&pevent->refcount, 1);

	ret = get_event_id(pevent);
	if (ret < 0)
		goto fail;
	pevent->id = ret;

	if (!name) {
		snprintf(id_buf, sizeof(id_buf), "%d", pevent->id);
		name = id_buf;
	}

	pevent->name = kstrdup(name, GFP_KERNEL);
	if (!pevent->name) {
		ret = -ENOMEM;
		goto fail;
	}

	return pevent;
fail:
	pevent_free(pevent);
	return ERR_PTR(ret);
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
	event->attr.persistent = 1;
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
		event->attr.persistent = 0;
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

	atomic_inc(&pevent->refcount);
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
	if (event) {
		/* Safe, the caller holds &pevent->refcount too. */
		atomic_dec(&pevent->refcount);
		persistent_event_release(event);
	}
}

static int pevent_sysfs_register(struct pevent *event);
static void pevent_sysfs_unregister(struct pevent *event);

static int __maybe_unused
persistent_open(char *name, struct perf_event_attr *attr, int nr_pages)
{
	struct pevent *pevent;
	int cpu;
	int ret;

	pevent = pevent_alloc(name);
	if (IS_ERR(pevent))
		return PTR_ERR(pevent);

	for_each_possible_cpu(cpu) {
		ret = persistent_event_open(cpu, pevent, attr, nr_pages);
		if (ret)
			goto fail;
	}

	ret = pevent_sysfs_register(pevent);
	if (!ret)
		goto out;
fail:
	for_each_possible_cpu(cpu)
		persistent_event_close(cpu, pevent);

	pr_err("%s: Error adding persistent event: %d\n",
		__func__, ret);
out:
	if (atomic_dec_and_test(&pevent->refcount)) {
		pevent_sysfs_unregister(pevent);
		pevent_free(pevent);
	}

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

static struct mutex sysfs_lock;
static int sysfs_nr_entries;

static struct attribute_group pevents_group = {
	.name = "events",
	.attrs = NULL,		/* dynamically allocated */
};

static const struct attribute_group *persistent_attr_groups[] = {
	&persistent_format_group,
	NULL,			/* placeholder: &pevents_group */
	NULL,
};
#define EVENTS_GROUP_PTR	(&persistent_attr_groups[1])
#define EVENTS_ATTRS_PTR	(&pevents_group.attrs)

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
	struct attribute ***attrs_ptr = EVENTS_ATTRS_PTR;
	struct attribute **attrs;
	int ret = 0;

	sysfs->id	= pevent->id;
	sysfs->attr	= (struct device_attribute)
				__ATTR(, 0444, pevent_sysfs_show, NULL);
	attr->name	= pevent->name;
	sysfs_attr_init(attr);

	mutex_lock(&sysfs_lock);

	/*
	 * Keep old list if no new one is available. Need this for
	 * device_remove_attrs() if unregistering pmu.
	 */
	attrs = __krealloc(*attrs_ptr, (sysfs_nr_entries + 2) * sizeof(*attrs),
			GFP_KERNEL);

	if (!attrs) {
		ret = -ENOMEM;
		goto unlock;
	}

	attrs[sysfs_nr_entries++]	= attr;
	attrs[sysfs_nr_entries]		= NULL;

	if (!*group)
		*group = &pevents_group;

	if (!dev)
		goto out;	/* sysfs not yet initialized */

	if (sysfs_nr_entries == 1)
		ret = sysfs_create_group(&dev->kobj, *group);
	else
		ret = sysfs_add_file_to_group(&dev->kobj, attr, (*group)->name);

	if (ret) {
		/* roll back */
		sysfs_nr_entries--;
		if (!sysfs_nr_entries)
			*group = NULL;
		if (*attrs_ptr != attrs)
			kfree(attrs);
		else
			attrs[sysfs_nr_entries] = NULL;
		goto unlock;
	}
out:
	if (*attrs_ptr != attrs) {
		kfree(*attrs_ptr);
		*attrs_ptr = attrs;
	}
unlock:
	mutex_unlock(&sysfs_lock);

	return ret;
}

static void pevent_sysfs_unregister(struct pevent *pevent)
{
	struct attribute *attr = &pevent->sysfs.attr.attr;
	struct device *dev = persistent_pmu.dev;
	const struct attribute_group **group = EVENTS_GROUP_PTR;
	struct attribute ***attrs_ptr = EVENTS_ATTRS_PTR;
	struct attribute **attrs, **dest;

	mutex_lock(&sysfs_lock);

	for (dest = *attrs_ptr; *dest; dest++) {
		if (*dest == attr)
			break;
	}

	if (!*dest)
		goto unlock;

	sysfs_nr_entries--;

	*dest = (*attrs_ptr)[sysfs_nr_entries];
	(*attrs_ptr)[sysfs_nr_entries] = NULL;

	if (!dev)
		goto out;	/* sysfs not yet initialized */

	if (!sysfs_nr_entries)
		sysfs_remove_group(&dev->kobj, *group);
	else
		sysfs_remove_file_from_group(&dev->kobj, attr, (*group)->name);
out:
	if (!sysfs_nr_entries)
		*group = NULL;

	attrs = __krealloc(*attrs_ptr, (sysfs_nr_entries + 1) * sizeof(*attrs),
			GFP_KERNEL);

	if (!attrs && *attrs_ptr != attrs) {
		kfree(*attrs_ptr);
		*attrs_ptr = attrs;
	}
unlock:
	mutex_unlock(&sysfs_lock);
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

	idr_init(&event_idr);
	mutex_init(&event_lock);
	mutex_init(&sysfs_lock);
	perf_pmu_register(&persistent_pmu, "persistent", PERF_TYPE_PERSISTENT);

	for_each_possible_cpu(cpu) {
		INIT_LIST_HEAD(&per_cpu(pevents, cpu));
		mutex_init(&per_cpu(pevents_lock, cpu));
	}
}

/*
 * Detach an event from a process. The event will remain in the system
 * after closing the event's fd, it becomes persistent.
 */
int perf_event_detach(struct perf_event *event)
{
	struct pevent *pevent;
	int cpu;
	int ret;

	if (!try_get_event(event))
		return -ENOENT;

	/* task events not yet supported: */
	cpu = event->cpu;
	if ((unsigned)cpu >= nr_cpu_ids) {
		ret = -EINVAL;
		goto fail_rb;
	}

	/*
	 * Avoid grabbing an id, later checked again in pevent_add()
	 * with mmap_mutex held.
	 */
	if (event->pevent_id) {
		ret = -EEXIST;
		goto fail_rb;
	}

	mutex_lock(&event->mmap_mutex);
	if (event->rb)
		ret = -EBUSY;
	else
		ret = perf_alloc_rb(event, CPU_BUFFER_NR_PAGES, 0);
	mutex_unlock(&event->mmap_mutex);

	if (ret)
		goto fail_rb;

	pevent = pevent_alloc(NULL);
	if (IS_ERR(pevent)) {
		ret = PTR_ERR(pevent);
		goto fail_pevent;
	}

	ret = pevent_add(pevent, event);
	if (ret)
		goto fail_add;

	ret = pevent_sysfs_register(pevent);
	if (ret)
		goto fail_sysfs;

	atomic_inc(&event->mmap_count);

	return pevent->id;
fail_sysfs:
	pevent_del(pevent, cpu);
fail_add:
	pevent_free(pevent);
fail_pevent:
	mutex_lock(&event->mmap_mutex);
	if (event->rb)
		perf_free_rb(event);
	mutex_unlock(&event->mmap_mutex);
fail_rb:
	put_event(event);
	return ret;
}

/*
 * Attach an event to a process. The event will be removed after all
 * users disconnected from it, it's no longer persistent in the
 * system.
 */
int perf_event_attach(struct perf_event *event)
{
	int cpu = event->cpu;
	struct pevent *pevent;

	if ((unsigned)cpu >= nr_cpu_ids)
		return -EINVAL;

	pevent = find_event(event->pevent_id);
	if (!pevent)
		return -EINVAL;

	event = pevent_del(pevent, cpu);
	if (!event)
		return -EINVAL;

	if (atomic_dec_and_test(&pevent->refcount)) {
		pevent_sysfs_unregister(pevent);
		pevent_free(pevent);
	}

	persistent_event_release(event);

	return 0;
}
