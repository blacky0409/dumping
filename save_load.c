#include "save_load.h"
#include <linux/fs.h>
#include <linux/mman.h>
#include <linux/debugfs.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/uaccess.h>
#include <linux/blkdev.h>
#include <uapi/linux/fs.h>
#include <linux/init.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/sched.h>



void SAVE_LOAD_INIT(struct nvmev * nvmev){
	printk("Hello save and load -fix\n");
}

void SAVE_LOAD_EXIT(void){
	printk("Bye save and load\n");
}
static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
	return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
	return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
	((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
	return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
	((struct line *)a)->pos = pos;
}

void ssd_write_pg(struct nand_page *pg, struct file *file, loff_t *f_pos){
	int	ret = kernel_write(file,&pg->nsecs,sizeof(int),f_pos);
	if(ret < 0){
		printk("Failed to write page nsecs\n");
	}
	ret = kernel_write(file,&pg->status,sizeof(int),f_pos);
	if(ret < 0){
		printk("Failed to write page status\n");
	}
	ret = kernel_write(file,pg->sec,sizeof(nand_sec_status_t)*pg->nsecs,f_pos);
	if(ret < 0){
		printk("Failed to write page sec\n");
	}
}
void ssd_write_blk(struct nand_block *blk, struct file *file, loff_t *f_pos){
	int i;
	int ret = kernel_write(file,&blk->npgs,sizeof(int),f_pos);
//	if(ret < 0){
//		printk("Failed to write block npgs\n");
//	}
	ret = kernel_write(file,&blk->ipc,sizeof(int),f_pos);
//	if(ret < 0){
//		printk("Failed to write block ipc\n");
//	}
	ret = kernel_write(file,&blk->vpc,sizeof(int),f_pos);
//	if(ret < 0){
//		printk("Failed to write block vpc\n");
//	}
	ret = kernel_write(file,&blk->erase_cnt,sizeof(int),f_pos);
//	if(ret < 0){
//		printk("Failed to write block erase_cnt\n");
//	}
	ret = kernel_write(file,&blk->wp,sizeof(int),f_pos);
//	if(ret < 0){
//		printk("Failed to write block wp\n");
//	}
	for(i=0; i < blk->npgs; i++){
		ssd_write_pg(&(blk->pg[i]),file, f_pos);
	}
}
void ssd_write_pl(struct nand_plane *pl, struct file *file, loff_t *f_pos){
	int i;

	for( i=0; i < pl->nblks; i++){
		ssd_write_blk(&(pl->blk[i]),file, f_pos);
	}
}
void ssd_write_lun(struct nand_lun *lun, struct file *file, loff_t *f_pos){
	int i;
	for( i=0; i < lun->npls; i++){
		ssd_write_pl(&(lun->pl[i]),file, f_pos);
	}
}

void ssd_write_ch(struct ssd_channel *ch, struct file *file, loff_t *f_pos){
	int i;
	for(i=0; i < ch->nluns; i++){
		ssd_write_lun(&(ch->lun[i]),file, f_pos);
	}
}

int save_device(struct nvmev_dev *nvmev_vdev,const char *root)
{
	struct nvmev_ns *ns;
	struct conv_ftl *conv_ftl;
	struct conv_ftl *conv_ftls;
	struct ssdparams * spp;

	loff_t f_pos;

	int q,i,ret;
	struct file *file;
	char filename[300];

	struct line * ln;
	struct line *cursor, *next;

	unsigned long size;
	struct path path;
	loff_t curr;


	ns = nvmev_vdev->ns;
	conv_ftl = (struct conv_ftl *)ns->ftls;
	const uint32_t nr_parts = ns->nr_parts;

	struct block_device *bdev;
	struct super_block *esb;
	struct file_system_type *fs_type;
	unsigned int s_size;

	printk("Save Start\n");

	f_pos = 0;

	sprintf(filename,"%s/%s",root,"dumpfile");
	file = filp_open(filename, O_WRONLY | O_CREAT | O_TRUNC |O_LARGEFILE, 0666);

	if(IS_ERR(file)){
		printk("filp_open err!\n");
		return -1;
	}

	for(q=0; q < nr_parts;q++){

		conv_ftls = &conv_ftl[q];

		spp = &conv_ftls->ssd->sp;

		//ssd channel save
		for(i=0; i < spp->nchs;i++){
			ssd_write_ch(&(conv_ftls->ssd->ch[i]),file, &f_pos);
		}

		//write pointer save
		ret = kernel_write(file,&conv_ftls->wp,sizeof(struct write_pointer),&f_pos);
		if(ret < 0){
			printk("Failed to write wp\n");
		}

		ret = kernel_write(file,&conv_ftls->wp.curline->id,sizeof(int),&f_pos);
		if(ret < 0){
			printk("Failed to write wp id\n");
		}

		//gc write pointer save
		ret = kernel_write(file,&conv_ftls->gc_wp,sizeof(struct write_pointer),&f_pos);
		if(ret < 0){
			printk("Failed to write gc_wp\n");
		}

		ret = kernel_write(file,&conv_ftls->gc_wp.curline->id,sizeof(int),&f_pos);
		if(ret < 0){
			printk("Failed to write gc_wp id\n");
		}

		//mapping table save
		ret = kernel_write(file,conv_ftls->maptbl,sizeof(struct ppa) * spp->tt_pgs,&f_pos);
		if(ret < 0){
			printk("Failed to write maptbl\n");
		}

		//reverse maaping table save
		ret = kernel_write(file,conv_ftls->rmap,sizeof(uint64_t) * spp->tt_pgs,&f_pos);
		if(ret < 0){
			printk("Failed to write rmap\n");
		}

		ret = kernel_write(file, conv_ftls->page_counter,sizeof(int64_t) * spp->nchs * spp->luns_per_ch, &f_pos);

		//lines counter save 
		ret = kernel_write(file,&conv_ftls->lm.tt_lines,sizeof(uint32_t),&f_pos);
		if(ret < 0){
			printk("Failed to write tt_lines\n");
		}

		//lines parameter save
		ret = kernel_write(file,&conv_ftls->lm.free_line_cnt,sizeof(uint32_t),&f_pos);
		if(ret < 0){
			printk("Failed to write free_line_cnt\n");
		}

		ret = kernel_write(file,&conv_ftls->lm.victim_line_cnt,sizeof(uint32_t),&f_pos);
		if(ret < 0){
			printk("Failed to write victim_line_cnt\n");
		}

		ret = kernel_write(file,&conv_ftls->lm.full_line_cnt,sizeof(uint32_t),&f_pos);
		if(ret < 0){
			printk("Failed to write full_line_cnt\n");
		}

		//lines save => 각각 저장
		for(i=0;i< conv_ftls->lm.tt_lines;i++){
			ret = kernel_write(file,&conv_ftls->lm.lines[i].id,sizeof(int),&f_pos);
			if(ret < 0){
				printk("Failed to write line id\n");
			}
			ret = kernel_write(file,&conv_ftls->lm.lines[i].ipc,sizeof(int),&f_pos);
			if(ret < 0){
				printk("Failed to write line ipc\n");
			}
			ret = kernel_write(file,&conv_ftls->lm.lines[i].vpc,sizeof(int),&f_pos);
			if(ret < 0){
				printk("Failed to write line vpc\n");
			}
			ret = kernel_write(file,&conv_ftls->lm.lines[i].pos,sizeof(size_t),&f_pos);
			if(ret < 0){
				printk("Failed to write line pos\n");
			}
		}

		//victim line pqueue save
		ret = kernel_write(file,&conv_ftls->lm.victim_line_pq->size,sizeof(size_t),&f_pos);
		if(ret < 0){
			printk("Failed to write pq size\n");
		}
		ret = kernel_write(file,&conv_ftls->lm.victim_line_pq->avail,sizeof(size_t),&f_pos);
		if(ret < 0){
			printk("Failed to write pq avail\n");
		}
		ret = kernel_write(file,&conv_ftls->lm.victim_line_pq->step,sizeof(size_t),&f_pos);
		if(ret < 0){
			printk("Failed to write pq step\n");
		}

		for(i=1;i <= conv_ftls->lm.victim_line_cnt; i ++){
			ln = (struct line *)conv_ftls->lm.victim_line_pq->d[i];
			ret = kernel_write(file,&ln->id,sizeof(int),&f_pos);
			if(ret < 0){
				printk("Failed to write victim array id\n");
			}
		}

		//free line save
		list_for_each_entry_safe(cursor, next, &conv_ftls->lm.free_line_list, entry) {
			ret = kernel_write(file,&cursor->id,sizeof(int) ,&f_pos);
			if(ret < 0){
				printk("Failed to write free line id\n");
			}
		}

		//full line save
		list_for_each_entry_safe(cursor, next, &conv_ftls->lm.full_line_list, entry) {
			ret = kernel_write(file,&cursor->id,sizeof(int) ,&f_pos);
			if(ret < 0){
				printk("Failed to write full line id\n");
			}
		}
		for(i=0; i < spp->tt_pgs; i++){
			if(conv_ftls->rmap[i]!=INVALID_LPN){
				int cur = 0;
				while(cur < PAGE_SIZE){
					ret = kernel_write(file,nvmev_vdev->storage_mapped + (((conv_ftls->rmap[i]*nr_parts + q)*8) << 9) + cur,PAGE_SIZE - cur,&f_pos);
					if(ret < 0){
						printk("Failed to write storage\n");
					}
					cur += ret;
				}
			}
		}
	}
/*
//storage size save
	ret = kernel_write(file,&ns[0].p_dev->config.storage_size,sizeof(unsigned long) ,&f_pos);
	if(ret < 0){
		printk("Failed to write storage size\n");
	}

	//mapped data save
	size = ns[0].p_dev->config.storage_size;

	printk("Storage data saving...\n");
	
	curr = 0;
	while(curr < size){
		ret = kernel_write(file,nvmev_vdev->storage_mapped + curr,size - curr,&f_pos);
		if(ret < 0){
			printk("Failed to write storage\n");
		}

		curr += ret;

	}

*/


	filp_close(file,NULL);
	printk("Save Done..! Finish!\n");
	return 0;
}

void ssd_read_pg(struct nand_page *pg, struct file *file, loff_t *f_pos){
	int	ret = kernel_read(file,&pg->nsecs,sizeof(int),f_pos);
	if(ret < 0){
		printk("Failed to read page nsecs\n");
	}
	ret = kernel_read(file,&pg->status,sizeof(int),f_pos);
	if(ret < 0){
		printk("Failed to read page status\n");
	}
	pg->sec = kmalloc(sizeof(nand_sec_status_t) * pg->nsecs, GFP_KERNEL);

	ret = kernel_read(file,pg->sec,sizeof(nand_sec_status_t)*pg->nsecs,f_pos);
	if(ret < 0){
		printk("Failed to read page sec\n");
	}
}
void ssd_read_blk(struct nand_block *blk, struct file *file, loff_t *f_pos){
	int i;
	int ret = kernel_read(file,&blk->npgs,sizeof(int),f_pos);
	if(ret < 0){
		printk("Failed to read block npgs\n");
	}
//	blk->pg = kmalloc(sizeof(struct nand_page) * blk->npgs,GFP_KERNEL);
	ret = kernel_read(file,&blk->ipc,sizeof(int),f_pos);
	if(ret < 0){
		printk("Failed to read block ipc\n");
	}
	ret = kernel_read(file,&blk->vpc,sizeof(int),f_pos);
	if(ret < 0){
		printk("Failed to read block vpc\n");
	}
	ret = kernel_read(file,&blk->erase_cnt,sizeof(int),f_pos);
	if(ret < 0){
		printk("Failed to read block erase_cnt\n");
	}
	ret = kernel_read(file,&blk->wp,sizeof(int),f_pos);
	if(ret < 0){
		printk("Failed to read block wp\n");
	}

	for(i=0; i < blk->npgs; i++){
		ssd_read_pg(&(blk->pg[i]),file, f_pos);
	}
}
void ssd_read_pl(struct nand_plane *pl, struct file *file, loff_t *f_pos){
	int i;

	for( i=0; i < pl->nblks; i++){
		ssd_read_blk(&(pl->blk[i]),file, f_pos);
	}
}
void ssd_read_lun(struct nand_lun *lun, struct file *file, loff_t *f_pos){
	int i;
	for( i=0; i < lun->npls; i++){
		ssd_read_pl(&(lun->pl[i]),file, f_pos);
	}
}

void ssd_read_ch(struct ssd_channel *ch, struct ssdparams *spp, struct file *file, loff_t *f_pos){
	int i;

	for(i=0; i < ch->nluns; i++){
		ssd_read_lun(&(ch->lun[i]),file, f_pos);
	}
}

void ssd_free_pg(struct nand_page *pg){
	kfree(pg->sec);
}
void ssd_free_blk(struct nand_block *blk){
	int i;
	for(i=0; i < blk->npgs; i++){
		ssd_free_pg(&(blk->pg[i]));
	}
}
void ssd_free_pl(struct nand_plane *pl){
	int i;
	for( i=0; i < pl->nblks; i++){
		ssd_free_blk(&(pl->blk[i]));
	}
}
void ssd_free_lun(struct nand_lun *lun){
	int i;
	for( i=0; i < lun->npls; i++){
		ssd_free_pl(&(lun->pl[i]));
	}
}

void ssd_free_ch(struct ssd_channel *ch){
	int i;
	for(i=0; i < ch->nluns; i++){
		ssd_free_lun(&(ch->lun[i]));
	}
}

int load_device(struct nvmev_dev *nvmev_vdev,const char * root)
{
	struct nvmev_ns *ns;
	struct conv_ftl *conv_ftl;
	struct conv_ftl *conv_ftls;
	struct ssdparams * spp;

	loff_t f_pos;

	struct file *file;
	char filename[300];

	int ret,i,w,q;
	int wp_id,gc_wp_id;
	int id,ipc,vpc,pos;
	loff_t curr;

	int * array;
	int *free_line_id;
	int *full_line_id;

	int checking_wp;
	int checking_gc_wp;

	unsigned long size; 

	struct block_device *bdev;
    unsigned int s_size;
	struct super_block *esb;
    struct file_system_type *fs_type;

	printk("remove the all items\n");
	memset_io(nvmev_vdev->storage_mapped,0,nvmev_vdev->config.storage_size);

	ns = nvmev_vdev->ns;
	conv_ftl = (struct conv_ftl *)ns->ftls;
	const uint32_t nr_parts= ns->nr_parts;

	for (i = 0; i < nr_parts; i++){
		conv_remove_ftl(&conv_ftl[i]);
	}

	printk("Load Start\n");

	if(!nvmev_vdev->storage_mapped)
		printk("Storage is not correctly mapping\n");
	else
		printk("Storage is collect mapping\n");

	f_pos =0;

	sprintf(filename,"%s/%s",root,"dumpfile");
	file = filp_open(filename, O_RDONLY |O_LARGEFILE, 0666);

	if(IS_ERR(file)){
		printk("file don't exist\n");
		return -1;
	}

	for(q=0; q < nr_parts; q++){

		conv_ftls = &conv_ftl[q];

		INIT_LIST_HEAD(&conv_ftls->lm.free_line_list);
		INIT_LIST_HEAD(&conv_ftls->lm.full_line_list);


		//ssd parameter load
		spp = &conv_ftls->ssd->sp;

		for(w=0; w<spp->nchs;w++){
			ssd_free_ch(&(conv_ftls->ssd->ch[w]));
		}

		for(w=0; w<spp->nchs;w++){
			ssd_read_ch(&(conv_ftls->ssd->ch[w]),spp,file, &f_pos);
		}

		//write pointer load
		ret = kernel_read(file,&conv_ftls->wp,sizeof(struct write_pointer),&f_pos);
		if(ret < 0){
			printk("Failed to read wp\n");
		}

		ret = kernel_read(file,&wp_id,sizeof(int),&f_pos);
		if(ret < 0){
			printk("Failed to read wp id\n");
		}

		//gc write pointer load
		ret = kernel_read(file,&conv_ftls->gc_wp,sizeof(struct write_pointer),&f_pos);
		if(ret < 0){
			printk("Failed to read gc_wp\n");
		}
		ret = kernel_read(file,&gc_wp_id,sizeof(int),&f_pos);
		if(ret < 0){
			printk("Failed to read gc_wp id\n");
		}


		conv_ftls->maptbl = vmalloc(sizeof(struct ppa) * spp->tt_pgs);
		//mapping table load 
		ret = kernel_read(file,conv_ftls->maptbl,sizeof(struct ppa) * spp->tt_pgs,&f_pos);
		if(ret < 0){
			printk("Failed to read maptbl\n");
		}


		conv_ftls->rmap = vmalloc(sizeof(uint64_t) * spp->tt_pgs);
		//reverse mapping table load
		ret = kernel_read(file,conv_ftls->rmap,sizeof(uint64_t) * spp->tt_pgs,&f_pos);
		if(ret < 0){
			printk("Failed to read rmap\n");
		}

		ret = kernel_read(file, conv_ftls->page_counter,
    sizeof(int64_t) * spp->nchs * spp->luns_per_ch, &f_pos);
				
		struct line_mgmt *lm = &conv_ftls->lm;
		//conv parameter load
		ret = kernel_read(file,&lm->tt_lines,sizeof(uint32_t),&f_pos);
		if(ret < 0){
			printk("Failed to read tt_lines\n");
		}



		ret = kernel_read(file,&lm->free_line_cnt,sizeof(uint32_t),&f_pos);
		if(ret < 0){
			printk("Failed to read free line cnt\n");
		}

		ret = kernel_read(file,&lm->victim_line_cnt,sizeof(uint32_t),&f_pos);
		if(ret < 0){
			printk("Failed to read victim line cnt\n");
		}

		ret = kernel_read(file,&lm->full_line_cnt,sizeof(uint32_t),&f_pos);
		if(ret < 0){
			printk("Failed to read full line cnt\n");
		}

		conv_ftls->lm.lines= vmalloc(sizeof(struct line) * conv_ftls->lm.tt_lines);
		//lines load
		for(i=0;i< conv_ftls->lm.tt_lines;i++){
			ret = kernel_read(file,&id,sizeof(int),&f_pos);
			if(ret < 0){
				printk("Failed to read line id\n");
			}
			ret = kernel_read(file,&ipc,sizeof(int),&f_pos);
			if(ret < 0){
				printk("Failed to read line ipc\n");
			}
			ret = kernel_read(file,&vpc,sizeof(int),&f_pos);
			if(ret < 0){
				printk("Failed to read line vpc\n");
			}
			ret = kernel_read(file,&pos,sizeof(size_t),&f_pos);
			if(ret < 0){
				printk("Failed to read line pos\n");
			}

			conv_ftls->lm.lines[i].id  = id;
			conv_ftls->lm.lines[i].ipc = ipc;
			conv_ftls->lm.lines[i].vpc = vpc;
			conv_ftls->lm.lines[i].pos = pos;
			INIT_LIST_HEAD(&conv_ftls->lm.lines[i].entry);

		}

		conv_ftls->lm.victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri, victim_line_get_pri,
				victim_line_set_pri, victim_line_get_pos, victim_line_set_pos);
		//victime line pqueue load
		ret = kernel_read(file,&conv_ftls->lm.victim_line_pq->size,sizeof(size_t),&f_pos);
		if(ret < 0){
			printk("Failed to read pq size\n");
		}
		ret = kernel_read(file,&conv_ftls->lm.victim_line_pq->avail,sizeof(size_t),&f_pos);
		if(ret < 0){
			printk("Failed to read pq avail\n");
		}
		ret = kernel_read(file,&conv_ftls->lm.victim_line_pq->step,sizeof(size_t),&f_pos);
		if(ret < 0){
			printk("Failed to read pq step\n");
		}

		array = (int *)kzalloc(sizeof(int) * (conv_ftls->lm.victim_line_cnt),GFP_KERNEL);
		ret = kernel_read(file,array,sizeof(int) * conv_ftls->lm.victim_line_cnt,&f_pos);
		if(ret < 0){
			printk("Failed to read victim id array\n");
		}

		for(i=0;i < conv_ftls->lm.victim_line_cnt; i ++){
			for(w=0; w < conv_ftls->lm.tt_lines;w++){
				if(array[i] == conv_ftls->lm.lines[w].id){
					pqueue_change_data(conv_ftls->lm.victim_line_pq, i+1, &conv_ftls->lm.lines[w]);
            		break;
				}
			}
		}	

		//free line load
		free_line_id = kzalloc(sizeof(int) * conv_ftls->lm.free_line_cnt,GFP_KERNEL);
		ret = kernel_read(file,free_line_id,sizeof(int) * conv_ftls->lm.free_line_cnt,&f_pos);
		if(ret < 0){
			printk("Failed to read free line id array\n");
		}

		for(i=0; i < conv_ftls->lm.free_line_cnt; i ++){
			for(w=0; w < conv_ftls->lm.tt_lines; w++){
				if(conv_ftls->lm.lines[w].id == free_line_id[i])
				{
					list_add_tail(&conv_ftls->lm.lines[w].entry,&conv_ftls->lm.free_line_list);
					break;
				}
			}
		}
		kfree(free_line_id);

		//full line load
		full_line_id = kzalloc(sizeof(int) * conv_ftls->lm.full_line_cnt,GFP_KERNEL);
		ret = kernel_read(file,full_line_id,sizeof(int) * conv_ftls->lm.full_line_cnt,&f_pos);
		if(ret < 0){
			printk("Failed to read full line id array\n");
		}

		for(i=0; i < conv_ftls->lm.full_line_cnt; i ++){
			for(w=0;w < conv_ftls->lm.tt_lines; w++){
				if(conv_ftls->lm.lines[w].id == full_line_id[i]){
					list_add_tail(&conv_ftls->lm.lines[w].entry,&conv_ftls->lm.full_line_list);
					break;
				}

			}
		}
		kfree(full_line_id);

		checking_wp = 0;
		checking_gc_wp=0;
		//write pointer and gc write pointer's curline change to lines data
		for(i=0; i < conv_ftls->lm.tt_lines;i++){
			if( !checking_wp && wp_id == conv_ftls->lm.lines[i].id){
				conv_ftls->wp.curline = &(conv_ftls->lm.lines[i]);
				checking_wp=1; 
			}

			if( !checking_gc_wp && gc_wp_id == conv_ftls->lm.lines[i].id){
				conv_ftls->gc_wp.curline = &(conv_ftls->lm.lines[i]);
				checking_gc_wp=1;
			}

			if(checking_gc_wp && checking_wp)
				break;

		}

		for(i=0; i < spp->tt_pgs; i++){
			if(conv_ftls->rmap[i]!=INVALID_LPN){
				int cur = 0;
				while(cur < PAGE_SIZE){
					ret = kernel_read(file,nvmev_vdev->storage_mapped + (((conv_ftls->rmap[i]*nr_parts+q)*8) << 9) + cur,PAGE_SIZE - cur,&f_pos);
					if(ret < 0){
						printk("Failed to write storage\n");
					}
					cur += ret;
				}
			}
		}

	}
/*
   //storage size load
	ret = kernel_read(file,&size,sizeof(unsigned long),&f_pos);
	if(ret < 0){
		printk("Failed to read storage size\n");
	}

	//storage data load
	printk("Storage data loading..\n");
	curr=0;
	while(curr < size){
		ret = kernel_read(file,nvmev_vdev->storage_mapped + curr, size - curr,&f_pos);
		if(ret < 0){
			printk("Failed to read storage\n");
		}

		curr += ret;
	}
*/

	filp_close(file, NULL);
	printk("Load Done..! Finish!\n");

	return 0;

}


int dump_ftl_state (struct nvmev_dev *nvmev_vdev,const char *root)
{
	struct nvmev_ns *ns;
	struct conv_ftl *conv_ftl;
	struct ssdparams * spp;


	struct file *file;
	char filename[300];

	ns = nvmev_vdev->ns;
	conv_ftl = (struct conv_ftl *)ns->ftls;
	const uint32_t nr_parts= ns->nr_parts;


    loff_t pos = 0;
    int q, i, ret = 0;
    struct conv_ftl *ftl;
    struct dump_header hdr;

	sprintf(filename,"%s/%s",root,"ftl.bin");
    file = filp_open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_LARGEFILE, 0644);
    if (IS_ERR(file)) {
        printk(KERN_ERR "ftl_verify: cannot open %s\n", filename);
        return PTR_ERR(file);
    }

    ftl = &conv_ftl[0];
    spp = &ftl->ssd->sp;

    hdr.nr_parts    = nr_parts;
    hdr.tt_pgs      = spp->tt_pgs;
    hdr.tt_lines    = ftl->lm.tt_lines;
    hdr.nchs        = spp->nchs;
    hdr.luns_per_ch = spp->luns_per_ch;
    kernel_write(file, &hdr, sizeof(hdr), &pos);

    for (q = 0; q < nr_parts; q++) {
        ftl = &conv_ftl[q];
        spp = &ftl->ssd->sp;

        /* 1. maptbl */
        kernel_write(file, ftl->maptbl,
                     sizeof(struct ppa) * spp->tt_pgs, &pos);

        /* 2. rmap */
        kernel_write(file, ftl->rmap,
                     sizeof(uint64_t) * spp->tt_pgs, &pos);

        /* 3. lines: id, ipc, vpc만 저장 (pos는 pqueue 재구성용이라 제외) */
        for (i = 0; i < ftl->lm.tt_lines; i++) {
            int id  = ftl->lm.lines[i].id;
            int ipc = ftl->lm.lines[i].ipc;
            int vpc = ftl->lm.lines[i].vpc;
            kernel_write(file, &id,  sizeof(int), &pos);
            kernel_write(file, &ipc, sizeof(int), &pos);
            kernel_write(file, &vpc, sizeof(int), &pos);
        }

        /* 4. line management counters */
        kernel_write(file, &ftl->lm.free_line_cnt,   sizeof(uint32_t), &pos);
        kernel_write(file, &ftl->lm.victim_line_cnt, sizeof(uint32_t), &pos);
        kernel_write(file, &ftl->lm.full_line_cnt,   sizeof(uint32_t), &pos);

        /* 5. write pointer */
        {
            int wp_vals[4] = {
                ftl->wp.curline->id,
                ftl->wp.ch,
                ftl->wp.lun,
                ftl->wp.pg
            };
            kernel_write(file, wp_vals, sizeof(wp_vals), &pos);
        }

        /* 6. gc write pointer */
        {
            int gc_vals[4] = {
                ftl->gc_wp.curline->id,
                ftl->gc_wp.ch,
                ftl->gc_wp.lun,
                ftl->gc_wp.pg
            };
            kernel_write(file, gc_vals, sizeof(gc_vals), &pos);
        }

        /* 7. page_counter */
        kernel_write(file, ftl->page_counter,
                     sizeof(int64_t) * spp->nchs * spp->luns_per_ch, &pos);
    }

    filp_close(file, NULL);
    printk(KERN_INFO "ftl_verify: dump written to %s (%lld bytes)\n",
           filename, pos);
    return ret;
}