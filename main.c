// SPDX-License-Identifier: GPL-2.0-only

#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include <linux/debugfs.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/list_sort.h>

#include <linux/dcache.h>
#include <linux/fs_struct.h>
#include <linux/limits.h>
#include <linux/slab.h>

#ifdef CONFIG_X86
#include <asm/e820/types.h>
#include <asm/e820/api.h>
#endif

#include "save_load.h"
#include "zns_save_load.h"
#include "check_phfrag.h"
#include "nvmev.h"
#include "conv_ftl.h"
#include "zns_ftl.h"
#include "simple_ftl.h"
#include "kv_ftl.h"
#include "dma.h"

/****************************************************************
 * Memory Layout
 ****************************************************************
 * virtDev
 *  - PCI header
 *    -> BAR at 1MiB area
 *  - PCI capability descriptors
 *
 * +--- memmap_start
 * |
 * v
 * +--------------+------------------------------------------+
 * | <---1MiB---> | <---------- Storage Area --------------> |
 * +--------------+------------------------------------------+
 *
 * 1MiB area for metadata
 *  - BAR : 1 page
 *	- DBS : 1 page
 *	- MSI-x table: 16 bytes/entry * 32
 *
 * Storage area
 *
 ****************************************************************/

/*global variables*/
LIST_HEAD(devices);
//LIST_HEAD(contiguous_lpn_list);
struct nvmev *nvmev = NULL;

int io_using_dma = false;

char *cmd;
/*****************/

static void nvmev_proc_dbs(struct nvmev_dev *nvmev_vdev)
{
	int qid;
	int dbs_idx;
	int new_db;
	int old_db;

	// Admin queue
	new_db = nvmev_vdev->dbs[0];
	if (new_db != nvmev_vdev->old_dbs[0]) {
		nvmev_proc_admin_sq(new_db, nvmev_vdev->old_dbs[0], nvmev_vdev);
		nvmev_vdev->old_dbs[0] = new_db;
	}
	new_db = nvmev_vdev->dbs[1];
	if (new_db != nvmev_vdev->old_dbs[1]) {
		nvmev_proc_admin_cq(new_db, nvmev_vdev->old_dbs[1]);
		nvmev_vdev->old_dbs[1] = new_db;
	}

	// Submission queues
	for (qid = 1; qid <= nvmev_vdev->nr_sq; qid++) {
		if (nvmev_vdev->sqes[qid] == NULL)
			continue;
		dbs_idx = qid * 2;
		new_db = nvmev_vdev->dbs[dbs_idx];
		old_db = nvmev_vdev->old_dbs[dbs_idx];
		if (new_db != old_db) {
			nvmev_vdev->old_dbs[dbs_idx] =
				nvmev_proc_io_sq(qid, new_db, old_db, nvmev_vdev);
		}
	}

	// Completion queues
	for (qid = 1; qid <= nvmev_vdev->nr_cq; qid++) {
		if (nvmev_vdev->cqes[qid] == NULL)
			continue;
		dbs_idx = qid * 2 + 1;
		new_db = nvmev_vdev->dbs[dbs_idx];
		old_db = nvmev_vdev->old_dbs[dbs_idx];
		if (new_db != old_db) {
			nvmev_proc_io_cq(qid, new_db, old_db, nvmev_vdev);
			nvmev_vdev->old_dbs[dbs_idx] = new_db;
		}
	}
}

static int nvmev_dispatcher(void *data)
{
	struct nvmev_dev *nvmev_vdev = (struct nvmev_dev *)data;
	NVMEV_INFO("nvmev_dispatcher started on cpu %d (node %d)\n",
			nvmev_vdev->config.cpu_nr_dispatcher,
			cpu_to_node(nvmev_vdev->config.cpu_nr_dispatcher));

	while (!kthread_should_stop()) {
		nvmev_proc_bars(nvmev_vdev);
		nvmev_proc_dbs(nvmev_vdev);
		cond_resched();
	}

	return 0;
}

static void NVMEV_DISPATCHER_INIT(struct nvmev_dev *nvmev_vdev)
{
	char thread_name[32];
	snprintf(thread_name, sizeof(thread_name), "nvmev_%d_disp", nvmev_vdev->dev_id);

	nvmev_vdev->nvmev_dispatcher = kthread_create(nvmev_dispatcher, nvmev_vdev, thread_name);
	if (nvmev_vdev->config.cpu_nr_dispatcher != -1)
		kthread_bind(nvmev_vdev->nvmev_dispatcher, nvmev_vdev->config.cpu_nr_dispatcher);
	wake_up_process(nvmev_vdev->nvmev_dispatcher);
}

static void NVMEV_DISPATCHER_FINAL(struct nvmev_dev *nvmev_vdev)
{
	if (!IS_ERR_OR_NULL(nvmev_vdev->nvmev_dispatcher)) {
		kthread_stop(nvmev_vdev->nvmev_dispatcher);
		nvmev_vdev->nvmev_dispatcher = NULL;
	}
}

#ifdef CONFIG_X86
static int __validate_configs_arch(struct params *p)
{
	unsigned long resv_start_bytes;
	unsigned long resv_end_bytes;

	resv_start_bytes = p->memmap_start;
	resv_end_bytes = resv_start_bytes + p->memmap_size - 1;

	NVMEV_INFO("memmap_start = %lx , memmap_size = %lx\n",p->memmap_start, p->memmap_size);


	if (e820__mapped_any(resv_start_bytes, resv_end_bytes, E820_TYPE_RAM) ||
			e820__mapped_any(resv_start_bytes, resv_end_bytes, E820_TYPE_RESERVED_KERN)) {
		NVMEV_ERROR("[mem %#010lx-%#010lx] is usable, not reseved region\n",
				(unsigned long)resv_start_bytes, (unsigned long)resv_end_bytes);
		return -EPERM;
	}

	if (!e820__mapped_any(resv_start_bytes, resv_end_bytes, E820_TYPE_RESERVED)) {
		NVMEV_ERROR("[mem %#010lx-%#010lx] is not reseved region\n",
				(unsigned long)resv_start_bytes, (unsigned long)resv_end_bytes);
		return -EPERM;
	}
	return 0;
}
#else
static int __validate_configs_arch(void)
{
	/* TODO: Validate architecture-specific configurations */
	return 0;
}
#endif

static int __validate_configs(struct params *p)
{
	if (!p->memmap_start) {
		NVMEV_ERROR("[memmap_start] should be specified\n");
		return -EINVAL;
	}

	if (!p->memmap_size) {
		NVMEV_ERROR("[memmap_size] should be specified\n");
		return -EINVAL;
	} else if (p->memmap_size <= MB(1)) {
		NVMEV_ERROR("[memmap_size] should be bigger than 1 MiB\n");
		return -EINVAL;
	}

	if (__validate_configs_arch(p)) {
		return -EPERM;
	}

	if (p->nr_io_units == 0 || p->io_unit_shift == 0) {
		NVMEV_ERROR("Need non-zero IO unit size and at least one IO unit\n");
		return -EINVAL;
	}
	if (p->read_time == 0) {
		NVMEV_ERROR("Need non-zero read time\n");
		return -EINVAL;
	}
	if (p->write_time == 0) {
		NVMEV_ERROR("Need non-zero write time\n");
		return -EINVAL;
	}

	return 0;
}

static void __print_perf_configs(struct nvmev_dev *nvmev_vdev)
{
#ifdef CONFIG_NVMEV_VERBOSE
	unsigned long unit_perf_kb = nvmev_vdev->config.nr_io_units
		<< (nvmev_vdev->config.io_unit_shift - 10);
	struct nvmev_config *cfg = &nvmev_vdev->config;

	NVMEV_INFO("=============== Configurations ===============\n");
	NVMEV_INFO("* IO units : %d x %d\n", cfg->nr_io_units, 1 << cfg->io_unit_shift);
	NVMEV_INFO("* I/O times\n");
	NVMEV_INFO("  Read     : %u + %u x + %u ns\n", cfg->read_delay, cfg->read_time,
			cfg->read_trailing);
	NVMEV_INFO("  Write    : %u + %u x + %u ns\n", cfg->write_delay, cfg->write_time,
			cfg->write_trailing);
	NVMEV_INFO("* Bandwidth\n");
	NVMEV_INFO("  Read     : %lu MiB/s\n",
			(1000000000UL / (cfg->read_time + cfg->read_delay + cfg->read_trailing)) *
			unit_perf_kb >>
			10);
	NVMEV_INFO("  Write    : %lu MiB/s\n",
			(1000000000UL / (cfg->write_time + cfg->write_delay + cfg->write_trailing)) *
			unit_perf_kb >>
			10);
#endif
}

static int __get_nr_entries(int dbs_idx, int queue_size, struct nvmev_dev *nvmev_vdev)
{
	int diff = nvmev_vdev->dbs[dbs_idx] - nvmev_vdev->old_dbs[dbs_idx];
	if (diff < 0) {
		diff += queue_size;
	}
	return diff;
}

static struct nvmev_dev *__get_nvmev(const char *dev_name)
{
	struct nvmev_dev *cursor, *next;
	struct nvmev_dev *result = NULL;

	list_for_each_entry_safe(cursor, next, &nvmev->dev_list, list_elem) {
		if (result == NULL && strcmp(dev_name, cursor->dev_name) == 0) {
			result = cursor;
			break;
		}
	}
	if(result == NULL)
		printk("don't have device\n");
	return result;
}

static ssize_t __sysfs_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	/* TODO: Need a function that search "nvnev-vdev" from file name. */
	/* TODO: Print to file from nvmev_config data. */
	ssize_t len = 0;
	const char *dev_name = kobj->name;
	const char *file_name = attr->attr.name;

	struct nvmev_dev *nvmev_vdev = __get_nvmev(dev_name);
	struct nvmev_config *cfg = NULL;
	struct conv_ftl *conv_ftls = (struct conv_ftl *)nvmev_vdev->ns->ftls;
	struct conv_ftl *conv_ftl = &conv_ftls[0];

	struct ssdparams *spp = &conv_ftl->ssd->sp;

	uint64_t i,w,q;

	struct ssd *ssd;
	if (nvmev_vdev == NULL) {
		printk("No dev");
		return 0;
	}

	cfg = &nvmev_vdev->config;

	if (strcmp(file_name, "read_times") == 0) {
		len = sprintf(buf, "%u + %u x + %u", cfg->read_delay, cfg->read_time,
				cfg->read_trailing);
	} else if (strcmp(file_name, "write_times") == 0) {
		len = sprintf(buf, "%u + %u x + %u", cfg->write_delay, cfg->write_time,
				cfg->write_trailing);
	} else if (strcmp(file_name, "io_units") == 0) {
		len = sprintf(buf, "%u x %u", cfg->nr_io_units, cfg->io_unit_shift);
	} else if (strcmp(file_name, "stat") == 0) {
		int i;
		unsigned int nr_in_flight = 0;
		unsigned int nr_dispatch = 0;
		unsigned int nr_dispatched = 0;
		unsigned long long total_io = 0;
		for (i = 1; i <= nvmev_vdev->nr_sq; i++) {
			struct nvmev_submission_queue *sq = nvmev_vdev->sqes[i];
			if (!sq)
				continue;

			len += sprintf(buf, "%2d: %2u %4u %4u %4u %4u %llu\n", i,
					__get_nr_entries(i * 2, sq->queue_size, nvmev_vdev),
					sq->stat.nr_in_flight, sq->stat.max_nr_in_flight,
					sq->stat.nr_dispatch, sq->stat.nr_dispatched,
					sq->stat.total_io);

			nr_in_flight += sq->stat.nr_in_flight;
			nr_dispatch += sq->stat.nr_dispatch;
			nr_dispatched += sq->stat.nr_dispatched;
			total_io += sq->stat.total_io;

			barrier();
			sq->stat.max_nr_in_flight = 0;
		}
		len += sprintf(buf, "total: %u %u %u %llu\n", nr_in_flight, nr_dispatch,
				nr_dispatched, total_io);
	} else if (strcmp(file_name, "debug") == 0) {
		/* Left for later use */
	} else if (strcmp(file_name, "phfrag") == 0){
//		printk("%llu return \n",cfg->count);
		len = sprintf(buf, "%llu\n", cfg->count);
	} else if (strcmp(file_name, "chdie") == 0){
		cfg->ch_die_counter = 0;
		cfg->ch_die_latency = 0;
		cfg->count = 0;
		for(q=0;q < nvmev_vdev->ns->nr_parts; q++){
			ssd = conv_ftls[q].ssd;
			cfg->ch_die_counter += ssd->ch_die_counter;
			cfg->ch_die_latency += ssd->ch_die_latency;
//			ssd->count=0;
		}

		len = sprintf(buf, "%llu %llu\n", cfg->ch_die_counter, cfg->ch_die_latency);
	} else if (!strcmp(file_name, "hugemap_checker")){
		struct nvmev_ns *ns = (struct nvmev_ns *)nvmev_vdev->ns;
		len = sprintf(buf, "%lld %lld %lld %lld\n", ns->stats->sequential_writes,ns->stats->large_jump_writes,ns->stats->lpn_delta_sum,ns->stats->total_writes);
	} else if (!strcmp(file_name, "qing")){
		len = sprintf(buf, "%lld\n", nvmev_vdev->queueing_wait);
	}
	return len;
}

static ssize_t __sysfs_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf,
		size_t count)
{
	/* TODO: Need a function that search "nvnev-vdev" from file name. */
	/* TODO: Scan from file to nvmev_config data. */
	ssize_t len = count;
	unsigned int ret;
	unsigned long long *old_stat;

	const char *dev_name = kobj->name;
	const char *file_name = attr->attr.name;

	struct nvmev_dev *nvmev_vdev = __get_nvmev(dev_name);
	struct nvmev_config *cfg = NULL;

	struct conv_ftl *conv_ftls = (struct conv_ftl *)nvmev_vdev->ns->ftls;
	struct conv_ftl *conv_ftl = &conv_ftls[0];

	struct ssdparams *spp = &conv_ftl->ssd->sp;

	uint64_t i,w,q;

	struct ssd * ssd = conv_ftl->ssd;
	if (nvmev_vdev == NULL)
		goto out;

	cfg = &nvmev_vdev->config;

	if (!strcmp(file_name, "read_times")) {
		ret = sscanf(buf, "%u %u %u", &cfg->read_delay, &cfg->read_time,
				&cfg->read_trailing);
	} else if (!strcmp(file_name, "write_times")) {
		ret = sscanf(buf, "%u %u %u", &cfg->write_delay, &cfg->write_time,
				&cfg->write_trailing);
	} else if (!strcmp(file_name, "io_units")) {
		ret = sscanf(buf, "%d %d", &cfg->nr_io_units, &cfg->io_unit_shift);
		if (ret < 1)
			goto out;

		old_stat = nvmev_vdev->io_unit_stat;
		nvmev_vdev->io_unit_stat =
			kzalloc(sizeof(*nvmev_vdev->io_unit_stat) * cfg->nr_io_units, GFP_KERNEL);

		mdelay(100); /* XXX: Delay the free of old stat so that outstanding
					  * requests accessing the unit_stat are all returned
					  */
		kfree(old_stat);
	} else if (!strcmp(file_name, "stat")) {
		int i;
		for (i = 1; i <= nvmev_vdev->nr_sq; i++) {
			struct nvmev_submission_queue *sq = nvmev_vdev->sqes[i];
			if (!sq)
				continue;

			memset(&sq->stat, 0x00, sizeof(sq->stat));
		}
	} else if (!strcmp(file_name, "debug")) {
		/* Left for later use */
	} else if (!strcmp(file_name, "phfrag")){
		ret =  sscanf(buf, "%llu %llu", &cfg->input_lba, &cfg->lba_len);
		if(cfg->input_lba==0 && cfg->lba_len==0){
			cfg->count=0;
		}	
		else{
			int hi = phfrag_check(nvmev_vdev);
		}
	} else if (!strcmp(file_name, "chdie")){
		ret =  sscanf(buf, "%llu %llu", &cfg->input_lba, &cfg->lba_len);
		if(cfg->input_lba==0 && cfg->lba_len==0){
			for(q=0;q < nvmev_vdev->ns->nr_parts; q++){
				ssd = conv_ftls[q].ssd;
				ssd->ch_die_counter=0;
				ssd->ch_die_latency=0;
				ssd->count=0;
				ssd->checking = true;
			}
			nvmev_vdev->queueing_wait = 0;
		}
		else{
				for(q=0;q < nvmev_vdev->ns->nr_parts; q++){
					ssd = conv_ftls[q].ssd;
					ssd->checking = false;
				}
				//int hi = ch_die_check(nvmev_vdev,true);
			}
	} else if (!strcmp(file_name, "hugemap_checker")){
		struct nvmev_ns *ns = (struct nvmev_ns *)nvmev_vdev->ns;
		ret =  sscanf(buf, "%d", &ns->stats->start);
		if(ns->stats->start == 1){
			ns->stats->total_writes=0;
		}	
	}

out:
	__print_perf_configs(nvmev_vdev);

	return count;
}

static struct kobj_attribute read_times_attr =
__ATTR(read_times, 0644, __sysfs_show, __sysfs_store);
static struct kobj_attribute write_times_attr =
__ATTR(write_times, 0644, __sysfs_show, __sysfs_store);
static struct kobj_attribute io_units_attr = __ATTR(io_units, 0644, __sysfs_show, __sysfs_store);
static struct kobj_attribute stat_attr = __ATTR(stat, 0644, __sysfs_show, __sysfs_store);
static struct kobj_attribute debug_attr = __ATTR(debug, 0644, __sysfs_show, __sysfs_store);
static struct kobj_attribute phfrag_checking = __ATTR(phfrag, 0664, __sysfs_show, __sysfs_store);
static struct kobj_attribute chdie_checking = __ATTR(chdie, 0664, __sysfs_show, __sysfs_store);
static struct kobj_attribute hugemap_checking = __ATTR(hugemap_checker, 0664, __sysfs_show, __sysfs_store);
static struct kobj_attribute qing_checking = __ATTR(qing, 0664, __sysfs_show, __sysfs_store);

void NVMEV_STORAGE_INIT(struct nvmev_dev *nvmev_vdev)
{
	int ret = 0;

	NVMEV_INFO("Storage: %#010lx-%#010lx (%lu MiB)\n", nvmev_vdev->config.storage_start,
			nvmev_vdev->config.storage_start + nvmev_vdev->config.storage_size,
			BYTE_TO_MB(nvmev_vdev->config.storage_size));

	nvmev_vdev->io_unit_stat = kzalloc(
			sizeof(*nvmev_vdev->io_unit_stat) * nvmev_vdev->config.nr_io_units, GFP_KERNEL);

	nvmev_vdev->storage_mapped = memremap(nvmev_vdev->config.storage_start,
			nvmev_vdev->config.storage_size, MEMREMAP_WB);



	if (nvmev_vdev->storage_mapped == NULL)
		NVMEV_ERROR("Failed to map storage memory.\n");

	nvmev_vdev->sysfs_root = kobject_create_and_add(nvmev_vdev->dev_name, nvmev->config_root);
	nvmev_vdev->sysfs_read_times = &read_times_attr;
	nvmev_vdev->sysfs_write_times = &write_times_attr;
	nvmev_vdev->sysfs_io_units = &io_units_attr;
	nvmev_vdev->sysfs_stat = &stat_attr;
	nvmev_vdev->sysfs_debug = &debug_attr;
	nvmev_vdev->sysfs_phfrag = &phfrag_checking;
	nvmev_vdev->sysfs_chdie = &chdie_checking;
	nvmev_vdev->sysfs_hugemap = &hugemap_checking;
	nvmev_vdev->sysfs_qing = &qing_checking;

	ret = sysfs_create_file(nvmev_vdev->sysfs_root, &nvmev_vdev->sysfs_read_times->attr);
	ret = sysfs_create_file(nvmev_vdev->sysfs_root, &nvmev_vdev->sysfs_write_times->attr);
	ret = sysfs_create_file(nvmev_vdev->sysfs_root, &nvmev_vdev->sysfs_io_units->attr);
	ret = sysfs_create_file(nvmev_vdev->sysfs_root, &nvmev_vdev->sysfs_stat->attr);
	ret = sysfs_create_file(nvmev_vdev->sysfs_root, &nvmev_vdev->sysfs_debug->attr);
	ret = sysfs_create_file(nvmev_vdev->sysfs_root, &nvmev_vdev->sysfs_phfrag->attr);
	ret = sysfs_create_file(nvmev_vdev->sysfs_root, &nvmev_vdev->sysfs_chdie->attr);
	ret = sysfs_create_file(nvmev_vdev->sysfs_root, &nvmev_vdev->sysfs_hugemap->attr);
	ret = sysfs_create_file(nvmev_vdev->sysfs_root, &nvmev_vdev->sysfs_qing->attr);
}

void NVMEV_STORAGE_FINAL(struct nvmev_dev *nvmev_vdev)
{
	sysfs_remove_file(nvmev_vdev->sysfs_root, &nvmev_vdev->sysfs_read_times->attr);
	sysfs_remove_file(nvmev_vdev->sysfs_root, &nvmev_vdev->sysfs_write_times->attr);
	sysfs_remove_file(nvmev_vdev->sysfs_root, &nvmev_vdev->sysfs_io_units->attr);
	sysfs_remove_file(nvmev_vdev->sysfs_root, &nvmev_vdev->sysfs_stat->attr);
	sysfs_remove_file(nvmev_vdev->sysfs_root, &nvmev_vdev->sysfs_debug->attr);
	sysfs_remove_file(nvmev_vdev->sysfs_root, &nvmev_vdev->sysfs_phfrag->attr);
	sysfs_remove_file(nvmev_vdev->sysfs_root, &nvmev_vdev->sysfs_chdie->attr);
	sysfs_remove_file(nvmev_vdev->sysfs_root, &nvmev_vdev->sysfs_hugemap->attr);
	sysfs_remove_file(nvmev_vdev->sysfs_root, &nvmev_vdev->sysfs_qing->attr);

	kobject_put(nvmev_vdev->sysfs_root);

	if (nvmev_vdev->storage_mapped)
		memunmap(nvmev_vdev->storage_mapped);

	if (nvmev_vdev->io_unit_stat)
		kfree(nvmev_vdev->io_unit_stat);
}

static bool __load_configs(struct nvmev_config *config, struct params *p)
{
	bool first = true;
	unsigned int cpu_nr;
	char *cpu;

	if (__validate_configs(p) < 0) {
		return false;
	}

#if (BASE_SSD == KV_PROTOTYPE)
	p->memmap_size -= KV_MAPPING_TABLE_SIZE; // Reserve space for KV mapping table
#endif

	config->memmap_start = p->memmap_start;
	config->memmap_size = p->memmap_size;
	// storage space starts from 1M offset
	config->storage_start = p->memmap_start + MB(1);
	config->storage_size = p->memmap_size - MB(1);

	config->read_time = p->read_time;
	config->read_delay = p->read_delay;
	config->read_trailing = p->read_trailing;
	config->write_time = p->write_time;
	config->write_delay = p->write_delay;
	config->write_trailing = p->write_trailing;
	config->nr_io_units = p->nr_io_units;
	config->io_unit_shift = p->io_unit_shift;

	config->nr_io_workers = 0;
	config->cpu_nr_dispatcher = -1;

	while ((cpu = strsep(&p->cpus, ",")) != NULL) {
		cpu_nr = (unsigned int)simple_strtol(cpu, NULL, 10);
		if (first) {
			config->cpu_nr_dispatcher = cpu_nr;
		} else {
			config->cpu_nr_io_workers[config->nr_io_workers] = cpu_nr;
			config->nr_io_workers++;
		}
		first = false;
	}

	return true;
}

void NVMEV_NAMESPACE_INIT(struct nvmev_dev *nvmev_vdev)
{
	unsigned long long remaining_capacity = nvmev_vdev->config.storage_size;
	void *ns_addr = nvmev_vdev->storage_mapped;
	const int nr_ns = NR_NAMESPACES; // XXX: allow for dynamic nr_ns
	const unsigned int disp_no = nvmev_vdev->config.cpu_nr_dispatcher;
	int i;
	unsigned long long size;

	struct nvmev_ns *ns = kmalloc(sizeof(struct nvmev_ns) * nr_ns, GFP_KERNEL);
	
	struct write_pattern_stats * stats = kmalloc(sizeof(struct write_pattern_stats), GFP_KERNEL);
	
	ns->stats=stats;
	ns->stats->start=0;

	struct ftl_configs *ftl_cfgs = kmalloc(sizeof(struct ftl_configs), GFP_KERNEL);
		
	ftl_cfgs->max_con = nvmev_vdev->max_con;

	for (i = 0; i < nr_ns; i++) {
		if (NS_CAPACITY(i) == 0)
			size = remaining_capacity;
		else
			size = min(NS_CAPACITY(i), remaining_capacity);

		if (nvmev_vdev->ftl == SSD_TYPE_NVM) {
			load_simple_configs(ftl_cfgs);
			simple_init_namespace(&ns[i], i, size, ns_addr, disp_no, ftl_cfgs);
		} else if (nvmev_vdev->ftl == SSD_TYPE_CONV) {
			load_conv_configs(ftl_cfgs);
			conv_init_namespace(&ns[i], i, size, ns_addr, disp_no, ftl_cfgs);
		} else if (nvmev_vdev->ftl == SSD_TYPE_ZNS) {
			load_zns_configs(ftl_cfgs);
			zns_init_namespace(&ns[i], i, size, ns_addr, disp_no, ftl_cfgs);
		} else if (nvmev_vdev->ftl == SSD_TYPE_KV) {
			load_kv_configs(ftl_cfgs);
			kv_init_namespace(&ns[i], i, size, ns_addr, disp_no, ftl_cfgs);
		} else
			BUG_ON(1);

		remaining_capacity -= size;
		ns_addr += size;
		NVMEV_INFO("ns %d/%d: size %lld MiB\n", i, nr_ns, BYTE_TO_MB(ns[i].size));

		ns[i].p_dev = nvmev_vdev;
	}

	nvmev_vdev->ns = ns;
	nvmev_vdev->nr_ns = nr_ns;
	nvmev_vdev->mdts = ftl_cfgs->MDTS;
}

void NVMEV_NAMESPACE_FINAL(struct nvmev_dev *nvmev_vdev)
{
	struct nvmev_ns *ns = nvmev_vdev->ns;
	const int nr_ns = NR_NAMESPACES; // XXX: allow for dynamic nvmev_vdev->nr_ns
	int i;

	for (i = 0; i < nr_ns; i++) {
		if (NS_SSD_TYPE(i) == SSD_TYPE_NVM)
			simple_remove_namespace(&ns[i]);
		else if (NS_SSD_TYPE(i) == SSD_TYPE_CONV)
			conv_remove_namespace(&ns[i]);
		else if (NS_SSD_TYPE(i) == SSD_TYPE_ZNS)
			zns_remove_namespace(&ns[i]);
		else if (NS_SSD_TYPE(i) == SSD_TYPE_KV)
			kv_remove_namespace(&ns[i]);
		else
			BUG_ON(1);
	}

	kfree(ns);
	nvmev_vdev->ns = NULL;
}

static char *parse_to_cmd(char *cmd_line)
{
	char *command;

	if (cmd_line == NULL)
		return NULL;

	command = strsep(&cmd_line, " ");

	return command;
}

static void parse_command(char *cmd_line, struct params *p)
{
	char *arg;
	char *param, *val;

	if (cmd_line == NULL) {
		NVMEV_ERROR("cmd_line is NULL");
		return;
	}

	while ((arg = strsep(&cmd_line, " ")) != NULL) {
		next_arg(arg, &param, &val);
		printk("param is %s\n",param);
		printk("val is %s\n",val);

		if (strcmp(param, "memmap_start") == 0)
			p->memmap_start = memparse(val, NULL);

		else if (strcmp(param, "memmap_size") == 0)
			p->memmap_size = memparse(val, NULL);

		else if (strcmp(param, "cpus") == 0)
			p->cpus = val;

		else if (strcmp(param, "name") == 0){
			p->name = val;
		}
		else if (strcmp(param, "path") == 0){
			p->path = val;
		}
		else if (strcmp(param, "ftl") == 0) {
			if (strcmp(val, "simple") == 0)
				p->ftl = SSD_TYPE_NVM;
			else if (strcmp(val, "conv") == 0)
				p->ftl = SSD_TYPE_CONV;
			else if (strcmp(val, "kv") == 0)
				p->ftl = SSD_TYPE_KV;
			else if (strcmp(val, "zns") == 0)
				p->ftl = SSD_TYPE_ZNS;
		}
		else if (strcmp(param, "degree") == 0) {
			if(kstrtol(val,10,&p->degree_frag)){
				printk(KERN_INFO "Can't conver string to long\n");	
			}
		}	
		else if (strcmp(param, "num_chunk") == 0) {
			if(kstrtol(val,10,&p->num_chunk)){
				printk(KERN_INFO "Can't conver string to long\n");	
			}
		}	
		else if (strcmp(param, "max_con") == 0) {
			if(kstrtoint(val,10,&p->max_con)){
				p->max_con = 0;
				printk(KERN_INFO "Can't conver string to long\n");	
			}
		}	
	}
}

static struct params *PARAM_INIT(void)
{
	struct params *params;

	params = kzalloc(sizeof(struct params), GFP_KERNEL);

	params->memmap_start = 0;
	params->memmap_size = 0;

	params->read_time = 1;
	params->read_delay = 1;
	params->read_trailing = 0;

	params->write_time = 1;
	params->write_delay = 1;
	params->write_trailing = 0;

	params->nr_io_units = 8;
	params->io_unit_shift = 12;

	params->debug = 0;

	params->name = NULL;
	params->cpus = NULL;

	params->ftl = SSD_TYPE_NVM;

	return params;
}

static void __print_base_config(void)
{
	const char *type = "unknown";
	switch (BASE_SSD) {
		case INTEL_OPTANE:
			type = "NVM SSD";
			printk("delete implementation please");
			break;
		case SAMSUNG_970PRO:
			type = "Samsung 970 Pro SSD";
			break;
		case ZNS_PROTOTYPE:
			type = "ZNS SSD Prototype";
			break;
		case KV_PROTOTYPE:
			type = "KVSSD Prototype";
			break;
		case WD_ZN540:
			type = "WD ZN540 ZNS SSD";
			break;
	}

	NVMEV_INFO("Version %x.%x for >> %s <<\n", (NVMEV_VERSION & 0xff00) >> 8,
			(NVMEV_VERSION & 0x00ff), type);
}

static int create_device(struct params *p)
{
	__print_base_config();
	struct nvmev_dev *nvmev_vdev;
	nvmev_vdev = VDEV_INIT();

	if (!nvmev_vdev)
		return -EINVAL;

	if (!__load_configs(&nvmev_vdev->config, p)) {
		goto ret_err;
	}


	/* Alloc dev ID from number of device. */
	nvmev_vdev->dev_id = nvmev->nr_dev++;
	
	/* Init degree of physical fragmentation value */
	nvmev_vdev->degree_frag = 0;

	/* Load ftl. */
	nvmev_vdev->ftl = p->ftl;
	/* Load name. */
	if (p->name != NULL)
		strncpy(nvmev_vdev->dev_name, p->name, sizeof(nvmev_vdev->dev_name));

	else
		snprintf(nvmev_vdev->dev_name, sizeof(nvmev_vdev->dev_name), "nvmev_%d",
				nvmev_vdev->dev_id);

	NVMEV_INFO("dev name is %s\n", nvmev_vdev->dev_name);

	/* Put the list of devices for managing. */
	INIT_LIST_HEAD(&nvmev_vdev->list_elem);
	list_add(&nvmev_vdev->list_elem, &nvmev->dev_list);

	NVMEV_STORAGE_INIT(nvmev_vdev);

	NVMEV_NAMESPACE_INIT(nvmev_vdev);

	if (io_using_dma) {
		if (ioat_dma_chan_set("dma7chan0") != 0) {
			io_using_dma = false;
			NVMEV_ERROR("Cannot use DMA engine, Fall back to memcpy\n");
		}
	}

	if (!NVMEV_PCI_INIT(nvmev_vdev)) {
		goto ret_err;
	}

	__print_perf_configs(nvmev_vdev);

	NVMEV_IO_WORKER_INIT(nvmev_vdev);

	NVMEV_DISPATCHER_INIT(nvmev_vdev);

	pci_bus_add_devices(nvmev_vdev->virt_bus);

	NVMEV_INFO("Virtual NVMe device created\n");

	return 0;

ret_err:
	printk("error......\n");

	nvmev->nr_dev--;
	list_del(&nvmev_vdev->list_elem);
	VDEV_FINALIZE(nvmev_vdev);
	return -EIO;
}

static int delete_device(struct nvmev_dev *nvmev_vdev)
{
	int i;

	nvmev->nr_dev--;

	unregister_chrdev(nvmev_vdev->major, nvmev_vdev->dev_name);

	if (nvmev_vdev->virt_bus != NULL) {
		pci_stop_root_bus(nvmev_vdev->virt_bus);
		pci_remove_root_bus(nvmev_vdev->virt_bus);
	}

	NVMEV_DISPATCHER_FINAL(nvmev_vdev);
	NVMEV_IO_WORKER_FINAL(nvmev_vdev);

	NVMEV_NAMESPACE_FINAL(nvmev_vdev);
	NVMEV_STORAGE_FINAL(nvmev_vdev);

	if (io_using_dma) {
		ioat_dma_cleanup();
	}

	for (i = 0; i < nvmev_vdev->nr_sq; i++) {
		kfree(nvmev_vdev->sqes[i]);
	}

	for (i = 0; i < nvmev_vdev->nr_cq; i++) {
		kfree(nvmev_vdev->cqes[i]);
	}

	list_del(&nvmev_vdev->list_elem);

	VDEV_FINALIZE(nvmev_vdev);

	printk("Virtual NVMe device deleted\n");

	return 0;
}

static int delete_all(void)
{
	struct nvmev_dev *cursor, *next;

	list_for_each_entry_safe(cursor, next, &nvmev->dev_list, list_elem)
		delete_device(cursor);

	return 0;
}
static int compare_by_size(void *priv, const struct list_head *nodeA, const struct list_head * nodeB)
{
    lba_chunk_t *dataA;
    lba_chunk_t *dataB;

    dataA = list_entry(nodeA, lba_chunk_t, list);
    dataB = list_entry(nodeB, lba_chunk_t, list);

    if (dataA->size < dataB->size)
        return -1;
    else if (dataA->size > dataB->size)
        return 1;
    else
        return 0;
}

lba_chunk_t * generate_lpn(uint64_t start, uint64_t end, uint64_t size,uint64_t idx,int ch, int lun){
	lba_chunk_t * lpn = vmalloc(sizeof(lba_chunk_t));
	lpn->start_lpn = start;
	lpn->end_lpn = end;
	lpn->size=size;
	lpn->devide_size=0;
	lpn->idx = idx;
	lpn->ch=ch;
	lpn->lun=lun;
	lpn->counter = -1;
	lpn->conti=-1;

	return lpn;
}
struct list_head *create_list_from_table(struct ssdparams *spp,struct nvmev_ns *ns,struct conv_ftl *conv_ftls){
	struct list_head *head;
    head = vmalloc(sizeof(*head));
    if (!head) {
        pr_err("Failed to allocate memory for list head.\n");
        return NULL;
    }

    INIT_LIST_HEAD(head); 

	lba_chunk_t * lpn;
	uint64_t total_lpn_count = spp->tt_pgs * ns->nr_parts;
	uint64_t start_lpn=0;
	uint64_t count = 0;
	uint64_t global_lpn;
	struct conv_ftl *conv_ftl = &conv_ftls[0];
	uint64_t nr_parts = ns->nr_parts;


	for(global_lpn = 0; global_lpn < total_lpn_count; global_lpn++){

		uint64_t idx = global_lpn % ns->nr_parts;
		uint64_t local_lpn = global_lpn / ns->nr_parts;

		conv_ftl = &conv_ftls[idx];
  
		struct ppa ppa = conv_ftl->maptbl[local_lpn]; 

		if(ppa.ppa != UNMAPPED_PPA){
			if(count == 0){
				start_lpn=global_lpn;
			}
			count++;
		}
		else{
			if(count != 0){ 
				lpn = generate_lpn(start_lpn, global_lpn-1,count,0,-1,-1);
				list_add_tail(&lpn->list,head);
				count = 0;
			}	
		}
	}
	if(count != 0){
		lpn = generate_lpn(start_lpn, global_lpn-1,count,0,-1,-1);
		list_add_tail(&lpn->list,head);
	}

    return head;


}
struct list_head *create_list_from_table_split(struct ssdparams *spp,struct nvmev_ns *ns,struct conv_ftl *conv_ftls,uint64_t num_list){
	struct list_head *head;
	uint64_t nr_parts = ns->nr_parts;
	printk(KERN_INFO "num_list : %lld, nr_parts : %lld\n",num_list,nr_parts);
    head = vmalloc(sizeof(*head) * num_list * nr_parts);
    if (!head) {
        pr_err("Failed to allocate memory for list head.\n");
        return NULL;
    }

	uint64_t i;

	for (i = 0; i < nr_parts * num_list; i++)
    	INIT_LIST_HEAD(&head[i]);

	lba_chunk_t * lpn;
	uint64_t total_lpn_count = spp->tt_pgs * ns->nr_parts;
	uint64_t start_lpn=0;
	uint64_t count = 0;
	uint64_t global_lpn;
	uint64_t total_real_count=0;
	uint64_t ppa_lpn = 0;
	struct conv_ftl *conv_ftl = &conv_ftls[0];

	uint64_t split_count = total_lpn_count / num_list;
	printk(KERN_INFO "total : %lld , split : %lld\n",total_lpn_count, split_count);
	int cur_chunk = 0;

	for(global_lpn = 0; global_lpn < total_lpn_count; global_lpn++){
		total_real_count++;
		if(total_real_count >= split_count){
			total_real_count=0;
	/*		if(count!=0){
				uint64_t dump_s;
				uint64_t dump_e;
				for(i=0; i<nr_parts;i++){
					int cur_idx = i * num_list + cur_chunk;
					uint64_t first = start_lpn;
   					uint64_t last  = global_lpn - 1;

    				uint64_t start_mod = first % nr_parts;
    				uint64_t last_mod  = last  % nr_parts;

    				uint64_t s_diff = (i + nr_parts - start_mod) % nr_parts;
   				 	dump_s = first + s_diff;

    				uint64_t e_diff = (last_mod + nr_parts - i) % nr_parts;
    				dump_e = last - e_diff;

					lpn = generate_lpn(dump_s, dump_e,count,i,-1,-1);
					//printk(KERN_INFO "start : %lld, end: %lld, idx : %i, cur_chunk: %d, input area: %d\n",dump_s,dump_e,i,cur_chunk,cur_idx);
					list_add_tail(&lpn->list,&head[cur_idx]);
				}
				count = 0;
			}*/
			cur_chunk++;
			if(cur_chunk == num_list){
				cur_chunk--;
			}
		}
		uint64_t idx = global_lpn % ns->nr_parts;
		uint64_t local_lpn = global_lpn / ns->nr_parts;

		conv_ftl = &conv_ftls[idx];
  
		struct ppa ppa = conv_ftl->maptbl[local_lpn]; 

		if(ppa.ppa != UNMAPPED_PPA){
			if(count == 0){
				start_lpn=global_lpn;
			}
			count++;
		}
		else{
			if(count != 0){ 
				uint64_t dump_s;
				uint64_t dump_e;
				for(i=0; i<nr_parts;i++){
					int cur_idx = i * num_list + cur_chunk;
					uint64_t first = start_lpn;
   					uint64_t last  = global_lpn - 1;

    				uint64_t start_mod = first % nr_parts;
    				uint64_t last_mod  = last  % nr_parts;

    				uint64_t s_diff = (i + nr_parts - start_mod) % nr_parts;
   				 	dump_s = first + s_diff;

    				uint64_t e_diff = (last_mod + nr_parts - i) % nr_parts;
    				dump_e = last - e_diff;

					lpn = generate_lpn(dump_s, dump_e,count,i,-1,-1);
					//printk(KERN_INFO "start : %lld, end: %lld, idx : %i, cur_chunk: %d, input area: %d\n",dump_s,dump_e,i,cur_chunk,cur_idx);
					list_add_tail(&lpn->list,&head[cur_idx]);
				}
				count = 0;
			}	
		}
	}
	if(count != 0){
		uint64_t dump_s;
		uint64_t dump_e;
		for(i=0; i<nr_parts;i++){
			int cur_idx = i * num_list + cur_chunk;
			uint64_t first = start_lpn;
   			uint64_t last  = global_lpn - 1;

    		uint64_t start_mod = first % nr_parts;
   			uint64_t last_mod  = last  % nr_parts;

    		uint64_t s_diff = (i + nr_parts - start_mod) % nr_parts;
   		 	dump_s = first + s_diff;

    		uint64_t e_diff = (last_mod + nr_parts - i) % nr_parts;
    		dump_e = last - e_diff;
			lpn = generate_lpn(dump_s, dump_e,count,i,-1,-1);
			//printk(KERN_INFO "start : %lld, end: %lld, idx : %i, cur_chunk: %d, input area: %d\n",dump_s,dump_e,i,cur_chunk,cur_idx);
			list_add_tail(&lpn->list,&head[cur_idx]);
		}
	}

    return head;


}

static bool any_active_nonempty(struct list_head *arr, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (!list_empty(&arr[i]))
            return true;
    }
    return false;
}

static void clean_ftl(struct nvmev_dev * dev){
	printk(KERN_INFO "device's physical fragmentation cleaning...\n");
	struct nvmev_config *cfg = &dev->config;
	struct nvmev_ns *ns = dev->ns;
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	struct conv_ftl *conv_ftl = &conv_ftls[0];

	struct ssdparams *spp = &conv_ftl->ssd->sp;


	const uint32_t nr_parts = ns->nr_parts;

	uint32_t i;
	int w;	

	for(i =0 ; i < nr_parts; i++){
		conv_ftls[i].ssd->sp.max_con = dev->max_con;
	}

	int max_con = dev->max_con;


	long num_lists = dev->num_list;

	struct list_head *active_lba_chunks_list;


	printk(KERN_INFO "1\n");
	NVMEV_DISPATCHER_FINAL(dev);
	NVMEV_IO_WORKER_FINAL(dev);	
	printk(KERN_INFO "2\n");

	struct list_head *lists;
	lists = vmalloc(nr_parts * num_lists * sizeof(struct list_head));
    if (!lists) {
      	pr_err("Failed to allocate memory for list head.\n");
		return;
    }

	for (i = 0; i < nr_parts * num_lists; i++)
    	INIT_LIST_HEAD(&lists[i]);
	
	// 1. maptbl을 읽으면서 연속적인 length를 먼저 구함.
	uint64_t spoint = 0;
	active_lba_chunks_list= create_list_from_table_split(spp,ns,conv_ftls,num_lists);

	for(i=0; i < num_lists * nr_parts; i ++){
		list_sort(NULL, &active_lba_chunks_list[i],compare_by_size);
	}
	printk(KERN_INFO "start active : %s\n",(any_active_nonempty(active_lba_chunks_list,num_lists * nr_parts)) ? "have data":"empty");

	printk(KERN_INFO "1\n");
	uint64_t desired_degree = dev->degree_frag;
    NVMEV_NAMESPACE_FINAL(dev);
    NVMEV_NAMESPACE_INIT(dev);
	ns = dev->ns;
	conv_ftls = (struct conv_ftl *)ns->ftls;

	printk(KERN_INFO "2\n");
	// 2. 연속적인 LBA를 physical 영역에 degree에 따라 불규칙적으로 배열
	// current ns is only 1. later i need fix it => load, checking part also! 
	struct rewrite_pointer *r;
	bool restart = true;
	lba_chunk_t * regenerate;

	int count = 0;
	int cur_chunk = 0;

	int cur_index = 0; // nr_parts

	printk(KERN_INFO "start!!!\n");
	do{
    	lba_chunk_t *lpn, *tmp;

		if((!restart && count >= 16 * spp->pgs_per_oneshotpg)|| list_empty(&active_lba_chunks_list[cur_index * num_lists + cur_chunk])){
			count=0;
			cur_chunk++;
			if(cur_chunk >= num_lists)
				cur_chunk=0;

			bool have_chunk = false;

			for(i=0; i < num_lists;i++){
				if(!list_empty(&active_lba_chunks_list[cur_index * num_lists + i])){
					have_chunk = true;
					break;
				}
			}

			if(!have_chunk){
				cur_chunk=0;
				count=0;
				cur_index++;
				if(cur_index >= nr_parts)
					cur_index=0;
			}
		}

		/*restart = false;
   		list_for_each_entry_safe(lpn, tmp, &active_lba_chunks_list[cur_index * num_lists + cur_chunk], list) {
			NVMEV_ASSERT(lpn->idx == cur_index);
			if(count >= 2 * spp->pgs_per_oneshotpg){
				break;
			}
			r = phfrag_cleaner(ns, lpn->start_lpn, lpn->end_lpn,desired_degree ,lpn->idx, false,false,&count,num_lists,cur_chunk,&lpn->conti);
			if(r!=NULL){
				lpn->start_lpn = r->startpoint;
				vfree(r);
			}
			else{
				list_del(&lpn->list);
				vfree(lpn);
			}
		}

	} while (restart || any_active_nonempty(active_lba_chunks_list, num_lists * nr_parts));*/
	restart = false;
    	list_for_each_entry_safe(lpn, tmp, &active_lba_chunks_list[cur_index * num_lists + cur_chunk], list) {
			NVMEV_ASSERT(lpn->idx == cur_index);
			if(count >= 16 * spp->pgs_per_oneshotpg){
				break;
			}
			r = phfrag_cleaner(ns, lpn->start_lpn, lpn->end_lpn,desired_degree,lists, lpn->idx, false,false,&count,num_lists,cur_chunk,&lpn->conti);
			 if(r!=NULL){
				if(r->preempt){
					lpn->start_lpn = r->startpoint;
					lpn->ch = r->ch;
					lpn->lun = r->lun;


					lba_chunk_t *prem_lpn,*prem_tmp,*preempt_lpn;
					int prem_counter=0;
    				list_for_each_entry_safe(preempt_lpn, prem_tmp, &lists[cur_index * num_lists + cur_chunk], list) {
						if(prem_counter == r->preempt_counter){
							prem_lpn = preempt_lpn;
							break;
						}
						prem_counter++;
					}
					vfree(r);
					NVMEV_ASSERT(prem_lpn->idx == cur_index);
					r = phfrag_cleaner(ns, prem_lpn->start_lpn, prem_lpn->end_lpn,desired_degree,lists, prem_lpn->idx, true,true,&count,num_lists,cur_chunk,&prem_lpn->conti);	
					if(r != NULL){
						prem_lpn->start_lpn = r->startpoint;
						prem_lpn->ch = r->ch;
						prem_lpn->lun = r->lun;
						if(r->ch == -1 || r->lun== -1){
							prem_lpn->conti = -1;
							list_del(&prem_lpn->list);
							list_add_tail(&prem_lpn->list,&active_lba_chunks_list[cur_index * num_lists + cur_chunk]);
						}
						vfree(r);
					}
					else{
						list_del(&prem_lpn->list);
						vfree(prem_lpn);
					}
					
					restart=true;
					break;
				}
				else{
					if(r->ch == -1 || r->lun ==-1){
						lpn->conti = -1;
						lpn->start_lpn = r->startpoint;
					}
					else{
						list_del(&lpn->list);
					
						lpn->start_lpn = r->startpoint;
						lpn->ch = r->ch;
						lpn->lun = r->lun;

						list_add_tail(&lpn->list,&lists[lpn->idx * num_lists + cur_chunk]);
					}
				}
				vfree(r);
			}
			else{
				list_del(&lpn->list);
				vfree(lpn);
			}
		}

		if(!restart && list_empty(&active_lba_chunks_list[cur_index * num_lists + cur_chunk]) && !list_empty(&lists[cur_index * num_lists + cur_chunk])){
			lpn = list_first_entry(&lists[cur_index *num_lists + cur_chunk], lba_chunk_t,list);
			list_del(&lpn->list);
			lpn->conti=-1;
			list_add_tail(&lpn->list,&active_lba_chunks_list[cur_index * num_lists + cur_chunk]);
		}

	} while (restart || any_active_nonempty(active_lba_chunks_list, num_lists * nr_parts));

    printk(KERN_INFO "device's physical fragmentation clean done!-first\n");
	printk(KERN_INFO "active : %s\n",(any_active_nonempty(active_lba_chunks_list,num_lists * nr_parts)) ? "have data":"empty");
	vfree(active_lba_chunks_list);
    NVMEV_IO_WORKER_INIT(dev);
    NVMEV_DISPATCHER_INIT(dev);
    printk(KERN_INFO "device's physical fragmentation clean done!\n");
}
	/*
	printk(KERN_INFO "device's physical fragmentation cleaning...\n");
	struct nvmev_config *cfg = &dev->config;
	struct nvmev_ns *ns = dev->ns;
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	struct conv_ftl *conv_ftl = &conv_ftls[0];

	struct ssdparams *spp = &conv_ftl->ssd->sp;

	uint32_t i;
	int w;
	lba_chunk_t * lpn;
	struct convparams cpp = conv_ftl->cp;

	int pq_size=0;

	struct list_head *active_lba_chunks_list;
	struct list_head *active_lba_chunks_list_ver2;
    active_lba_chunks_list_ver2 = kmalloc(sizeof(*active_lba_chunks_list_ver2), GFP_KERNEL);
    if (!active_lba_chunks_list_ver2) {
        pr_err("Failed to allocate memory for list head.\n");
    }

    INIT_LIST_HEAD(active_lba_chunks_list_ver2); 

	printk(KERN_INFO "1\n");
	NVMEV_DISPATCHER_FINAL(dev);
	NVMEV_IO_WORKER_FINAL(dev);
	printk(KERN_INFO "2\n");

	// 1. maptbl을 읽으면서 연속적인 length를 먼저 구함.
	active_lba_chunks_list= create_list_from_table(spp,ns,conv_ftls);

	printk(KERN_INFO "3\n");
    NVMEV_NAMESPACE_FINAL(dev);
	printk(KERN_INFO "4\n");
    NVMEV_NAMESPACE_INIT(dev);
	ns = dev->ns;
	conv_ftls = (struct conv_ftl *)ns->ftls;
    NVMEV_IO_WORKER_INIT(dev);
    NVMEV_DISPATCHER_INIT(dev);
    printk(KERN_INFO "5\n");

	bool origin_list=true;
	bool process_from_front=true;

//fragmentation 정도 조절 0~100사이의 숫자
	uint64_t desired_degree = dev->degree_frag;
 	if (desired_degree == 0) {
    	lba_chunk_t *lpn, *tmp;

    	list_for_each_entry_safe(lpn, tmp, active_lba_chunks_list, list) {
			phfrag_cleaner(ns, lpn->start_lpn, lpn->end_lpn);

			list_del(&lpn->list);
            vfree(lpn);
		}
        goto cleanup_and_exit;
    }

    while (!list_empty(active_lba_chunks_list) || !list_empty(active_lba_chunks_list_ver2)) {
 		lba_chunk_t *current_active_chunk;

		if(origin_list && list_empty(active_lba_chunks_list)){
			origin_list=false;
		}
		else if(!origin_list && list_empty(active_lba_chunks_list_ver2)){
			origin_list=true;
		}

		if(origin_list){
			if(process_from_front){
        		current_active_chunk = list_first_entry(active_lba_chunks_list, lba_chunk_t, list);
			} else{
        		current_active_chunk = list_last_entry(active_lba_chunks_list, lba_chunk_t, list);
			}
		}
		else{
			if(process_from_front){
        		current_active_chunk = list_first_entry(active_lba_chunks_list_ver2, lba_chunk_t, list);
			} else{
        		current_active_chunk = list_last_entry(active_lba_chunks_list_ver2, lba_chunk_t, list);
        	}
		}
        list_del(&current_active_chunk->list);

		if(current_active_chunk->devide_size==0){
			if(desired_degree >= 100){
				current_active_chunk->devide_size = current_active_chunk->size;
			}
			else{
				current_active_chunk->devide_size = 1 + (desired_degree * desired_degree * (current_active_chunk->size - 1)) / (100 * 100);
			}
		}

		uint64_t base_size = current_active_chunk->size / current_active_chunk->devide_size;
       	uint64_t cur_size = current_active_chunk->end_lpn - current_active_chunk->start_lpn + 1;

		if(cur_size > base_size){
			cur_size = base_size;
		}

        phfrag_cleaner(ns, current_active_chunk->start_lpn, current_active_chunk->start_lpn + cur_size - 1);

		current_active_chunk->start_lpn += cur_size;


        if (current_active_chunk->start_lpn > current_active_chunk->end_lpn) {
            vfree(current_active_chunk);
        } else {
			if(!origin_list){
                list_add_tail(&current_active_chunk->list, active_lba_chunks_list);
			}
			else{
                list_add_tail(&current_active_chunk->list, active_lba_chunks_list_ver2);
			}
        }

    	process_from_front = !process_from_front;
    }

    printk(KERN_INFO "Final processed\n");

    lba_chunk_t *remaining_chunk, *tmp;
    list_for_each_entry_safe(remaining_chunk, tmp, active_lba_chunks_list, list) {
        list_del(&remaining_chunk->list);
        vfree(remaining_chunk);
    }
    lba_chunk_t *remaining_chunk2, *tmp2;
    list_for_each_entry_safe(remaining_chunk2, tmp2, active_lba_chunks_list_ver2, list) {
        list_del(&remaining_chunk2->list);
        vfree(remaining_chunk2);
    }

cleanup_and_exit:
    printk(KERN_INFO "6\n");
    printk(KERN_INFO "device's physical fragmentation clean done!\n");
}
*/

static void check_physical_fragmentation(struct nvmev_dev * dev,int32_t type){
	struct nvmev_config *cfg = &dev->config;
	struct nvmev_ns *ns = dev->ns;
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	struct conv_ftl *conv_ftl = &conv_ftls[0];

	struct ssdparams *spp = &conv_ftl->ssd->sp;

	uint32_t i;
	int w;

	uint64_t *luns=NULL;
	if(type == CHDIE){
		cfg->ch_die_counter=0;
		cfg->ch_die_latency=0;
		luns =  vzalloc(sizeof(uint64_t) * spp->nchs * spp->luns_per_ch);
	}
	else{
		cfg->count=0;
	}	

	struct list_head *chunks_list;
	printk(KERN_INFO "1\n");

	// 1. maptbl을 읽으면서 연속적인 length를 먼저 구함.
	uint64_t spoint = 0;
	chunks_list= create_list_from_table(spp,ns,conv_ftls);

	printk(KERN_INFO "2\n");

	// 2. check process
    lba_chunk_t *lpn, *tmp;
#define small_chunk_size 40
	uint64_t latency = 0;
    list_for_each_entry_safe(lpn, tmp, chunks_list, list) {
//		printk(KERN_INFO "size : %lld\n",(lpn->end_lpn - lpn->start_lpn +  1));
		for(; lpn->start_lpn <= lpn->end_lpn;){
			cfg->input_lba=lpn->start_lpn;
			cfg->lba_len= min((lpn->end_lpn - lpn->start_lpn + 1),small_chunk_size);
//			if(type == CHDIE){
//				ch_die_check(dev,luns,latency);
//			}
//			else
				phfrag_check(dev);

			lpn->start_lpn += cfg->lba_len;
			latency++;
		
		}
		list_del(&lpn->list);
		vfree(lpn);
	}

	kfree(chunks_list);
	if(luns)
		vfree(luns);
	printk(KERN_INFO "3\n");
	printk(KERN_INFO "finish check process\n");

}

static ssize_t __config_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return 0;
}

static ssize_t __config_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char __user *buf, size_t count)
{
	ssize_t len = count;
	const char *filename = attr->attr.name;
	size_t nr_copied;
	char input[128];

	struct nvmev_dev *nvmev_vdev;
	struct params *p;
	struct path pwd;
	char *cwd = NULL;
	char *buffer = NULL;

	strncpy(input, buf, min(count, sizeof(input)));

	p = PARAM_INIT();
	cmd = parse_to_cmd(input);
	parse_command(input + (strlen(cmd)+ 1), p);

	printk("name is %s\n",p->name);
	printk("cmd is %s\n",cmd);
	if (strcmp(cmd, "create") == 0) {
		printk("return = %d\n", create_device(p));
	} else if (strcmp(cmd, "delete") == 0) {
		if (p->name != NULL) {
			printk("target device: %s\n", p->name);

			nvmev_vdev = __get_nvmev(p->name);
			delete_device(nvmev_vdev);
		} else {
			printk("Do not have target device\n");
		}
	}
	else if(strcmp(cmd,"save") == 0){
		if (p->name != NULL) {
			if(p->path == NULL){
				get_fs_pwd(current->fs, &pwd);
				buffer = kmalloc(PATH_MAX, GFP_ATOMIC | __GFP_NOWARN | __GFP_ZERO);
				if (buffer) {
					cwd = d_path(&pwd, buffer, PATH_MAX);
					kfree(buffer);
					p->path = cwd;
				}
			}	
			printk("target device: %s\n", p->name);
			printk("path: %s\n", p->path);

			nvmev_vdev = __get_nvmev(p->name);
			save_device(nvmev_vdev, p->path);
		} else {
			printk("Do not have target device\n");
		}
	}
	else if(strcmp(cmd,"load") == 0){
		if (p->name != NULL) {
			if(p->path == NULL){
				get_fs_pwd(current->fs, &pwd);
				buffer = kmalloc(PATH_MAX, GFP_ATOMIC | __GFP_NOWARN | __GFP_ZERO);
				if (buffer) {
					cwd = d_path(&pwd, buffer, PATH_MAX);
					kfree(buffer);
					p->path=cwd;
				}
			}	
			printk("target device: %s\n", p->name);
			printk("path: %s\n", p->path);

			nvmev_vdev = __get_nvmev(p->name);
			load_device(nvmev_vdev,p->path);
		} else {
			printk("Do not have target device\n");
		}
	}
	else if(strcmp(cmd,"clean") == 0){
		if (p->name != NULL) {
			printk("target device: %s\n", p->name);

			nvmev_vdev = __get_nvmev(p->name);

			nvmev_vdev->degree_frag = p->degree_frag;
			nvmev_vdev->max_con = p->max_con;
			nvmev_vdev->num_list = p->num_chunk;
			printk("change degree of physical fragmentation to %ld\n", nvmev_vdev->degree_frag);

			clean_ftl(nvmev_vdev);
		} else {
			printk("Do not have target device\n");
		}
	} 
	else if(strcmp(cmd,"phfrag") == 0){
		if (p->name != NULL) {
			printk("target device: %s\n", p->name);

			nvmev_vdev = __get_nvmev(p->name);

			printk("count all of file's phfrag count\n");

			check_physical_fragmentation(nvmev_vdev,PHFRAG);
		} else {
			printk("Do not have target device\n");
		}
	} 
	else if(strcmp(cmd,"chdie") == 0){
		if (p->name != NULL) {
			printk("target device: %s\n", p->name);

			nvmev_vdev = __get_nvmev(p->name);

			printk("count directly physical fragmentation\n");

			check_physical_fragmentation(nvmev_vdev,CHDIE);
		} else {
			printk("Do not have target device\n");
		}
	} 
	else if(strcmp(cmd,"compftl") == 0){
		if (p->name != NULL) {
			if(p->path == NULL){
				get_fs_pwd(current->fs, &pwd);
				buffer = kmalloc(PATH_MAX, GFP_ATOMIC | __GFP_NOWARN | __GFP_ZERO);
				if (buffer) {
					cwd = d_path(&pwd, buffer, PATH_MAX);
					kfree(buffer);
					p->path = cwd;
				}
			}	
			printk("target device: %s\n", p->name);
			printk("path: %s\n", p->path);

			nvmev_vdev = __get_nvmev(p->name);
			dump_ftl_state(nvmev_vdev, p->path);
		} else {
			printk("Do not have target device\n");
		}
	}
	else if (strcmp(cmd, "zns_save") == 0) {
     if (p->name != NULL) {
        if (p->path == NULL) {
             get_fs_pwd(current->fs, &pwd);
             buffer = kmalloc(PATH_MAX, GFP_ATOMIC | __GFP_NOWARN | __GFP_ZERO);
              if (buffer) {
                 cwd = d_path(&pwd, buffer, PATH_MAX);
                 kfree(buffer);
                 p->path = cwd;
             }
         }
         printk("target device: %s\n", p->name);
         printk("path: %s\n", p->path);
         nvmev_vdev = __get_nvmev(p->name);
         save_zns_device(nvmev_vdev, p->path);
      } else {
          printk("Do not have target device\n");
      }
  	}
	else if (strcmp(cmd, "zns_load") == 0) {
    	if (p->name != NULL) {
       		if (p->path == NULL) {
             	get_fs_pwd(current->fs, &pwd);
             	buffer = kmalloc(PATH_MAX, GFP_ATOMIC | __GFP_NOWARN | __GFP_ZERO);
             	if (buffer) {
                 	cwd = d_path(&pwd, buffer, PATH_MAX);
                 	kfree(buffer);
                 	p->path = cwd;
             	}
         	}
         	printk("target device: %s\n", p->name);
         	printk("path: %s\n", p->path);
         	nvmev_vdev = __get_nvmev(p->name);
         	load_zns_device(nvmev_vdev, p->path);
      	} else {
          	printk("Do not have target device\n");
      	}
  	}
	else {
		NVMEV_ERROR("Doesn't not command.");
	}

	return len;
}

static struct kobj_attribute config_attr = __ATTR(config, 0664, __config_show, __config_store);

static int NVMeV_init(void)
{
	int ret = 0;

	nvmev = kzalloc(sizeof(struct nvmev), GFP_KERNEL);

	INIT_LIST_HEAD(&nvmev->dev_list);
	nvmev->nr_dev = 0;

	nvmev->config_root = kobject_create_and_add("nvmevirt", NULL);
	nvmev->config_attr = &config_attr;

	if (sysfs_create_file(nvmev->config_root, &nvmev->config_attr->attr)) {
		printk("Cannot create sysfs file...\n");
		return -EIO;
	}

	SAVE_LOAD_INIT(nvmev);

	NVMEV_INFO("Successfully load Virtual NVMe device module\n");
	return 0;
}

static void NVMeV_exit(void)
{
	delete_all();

	//	SAVE_LOAD_EXIT();

	sysfs_remove_file(nvmev->config_root, &nvmev->config_attr->attr);
	kobject_put(nvmev->config_root);

	kfree(nvmev);

	NVMEV_INFO("Virtual NVMe device closed\n");
}

MODULE_LICENSE("GPL v2");
module_init(NVMeV_init);
module_exit(NVMeV_exit);
