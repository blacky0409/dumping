#include "check_phfrag.h"
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
#include <linux/list.h>


void PHFRAG_INIT(struct nvmev * nvmev){
	printk("Hello phfrag\n");
}

void PHFRAG_EXIT(void){
	printk("Bye Phfrag\n");
}
static inline unsigned long long __get_wallclock(struct nvmev_dev *nvmev_vdev)
{
	return cpu_clock(nvmev_vdev->config.cpu_nr_dispatcher);
}
static inline uint64_t __get_ioclock(struct ssd *ssd)
{
	return cpu_clock(ssd->cpu_nr_dispatcher);
}
static inline bool valid_ppa(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	int ch = ppa->g.ch;
	int lun = ppa->g.lun;
	int pl = ppa->g.pl;
	int blk = ppa->g.blk;
	int pg = ppa->g.pg;
	//int sec = ppa->g.sec;

	if (ch < 0 || ch >= spp->nchs)
		return false;
	if (lun < 0 || lun >= spp->luns_per_ch)
		return false;
	if (pl < 0 || pl >= spp->pls_per_lun)
		return false;
	if (blk < 0 || blk >= spp->blks_per_pl)
		return false;
	if (pg < 0 || pg >= spp->pgs_per_blk)
		return false;

	return true;
}

static inline bool mapped_ppa(struct ppa *ppa)
{
	return !(ppa->ppa == UNMAPPED_PPA);
}

static bool is_same_flash_page(struct conv_ftl *conv_ftl, struct ppa ppa1, struct ppa ppa2)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	uint32_t ppa1_page = ppa1.g.pg / spp->pgs_per_flashpg;
	uint32_t ppa2_page = ppa2.g.pg / spp->pgs_per_flashpg;

	return (ppa1.h.blk_in_ssd == ppa2.h.blk_in_ssd) && (ppa1_page == ppa2_page);
}

static uint64_t ssd_advance_nand_copy(struct ssd *ssd, struct nand_cmd *ncmd,struct nvmev_config *cfg,bool *first)
{
	uint64_t cmd_stime = (ncmd->stime == 0) ? __get_ioclock(ssd) : ncmd->stime;
	uint64_t nand_stime, nand_etime;
	uint64_t chnl_stime, chnl_etime;
	uint64_t remaining, xfer_size, completed_time;
	struct ssdparams *spp;
	struct nand_lun *lun;
	struct ssd_channel *ch;
	struct ppa *ppa = ncmd->ppa;
	uint32_t cell;
	uint64_t delay_time;
	
	if (ppa->ppa == UNMAPPED_PPA) {
		NVMEV_ERROR("Error ppa 0x%llx\n", ppa->ppa);
		return cmd_stime;
	}

	spp = &ssd->sp;
	ch = &(ssd->ch[ppa->g.ch]);
	lun = &(ch->lun[ppa->g.lun]);
	cell = (ppa->g.pg / spp->pgs_per_flashpg) % (spp->cell_mode + 1);
	remaining = ncmd->xfer_size;

	nand_stime = max(lun->next_lun_avail_time, cmd_stime);
	
	if(!first[ppa->g.ch * spp->luns_per_ch + ppa->g.lun]){
		delay_time = nand_stime - cmd_stime;
		if(delay_time > 0){
			cfg->ch_die_counter ++;
			cfg->ch_die_latency += delay_time;
		}
		first[ppa->g.ch * spp->luns_per_ch + ppa->g.lun] = true;
	}
//	printk(KERN_INFO "ch : %d die : %d cur : %lld  luns %lld\n",ppa->g.ch,ppa->g.lun, cmd_stime, luns[ppa->g.ch * spp->luns_per_ch + ppa->g.lun]);

	if (ncmd->xfer_size == 4096) {
		nand_etime = nand_stime + spp->pg_4kb_rd_lat[cell];
	} else {
		nand_etime = nand_stime + spp->pg_rd_lat[cell];
	}

	chnl_stime = nand_etime;

	while (remaining) {
		xfer_size = min(remaining, (uint64_t)spp->max_ch_xfer_size);
		chnl_etime = chmodel_request(ch->perf_model, chnl_stime, xfer_size,
					     ssd->p_ns->p_dev);

		if (ncmd->interleave_pci_dma) {
			completed_time = ssd_advance_pcie(ssd, chnl_etime, xfer_size);
		} else {
			completed_time = chnl_etime;
		}

		remaining -= xfer_size;
		chnl_stime = completed_time;
	}

	lun->next_lun_avail_time = chnl_etime;

	return completed_time;
}
static bool is_same_oneshot_page(struct conv_ftl *conv_ftl, struct ppa ppa1, struct ppa ppa2)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	uint32_t ppa1_page = ppa1.g.pg / spp->pgs_per_oneshotpg;
	uint32_t ppa2_page = ppa2.g.pg / spp->pgs_per_oneshotpg;

	return (ppa1.h.blk_in_ssd == ppa2.h.blk_in_ssd) && (ppa1_page == ppa2_page);
}

static bool is_next_correct(struct conv_ftl *conv_ftl, struct ppa *pre, struct ppa *next, bool* page_counter){
	
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	
	/*if (pre->g.blk != next->g.blk) {
        if (next->g.ch == 0 && next->g.lun == 0 && next->g.pg == 0) {
            return 1;
        } else {
            return 0;
        }
    }*/

	// equal oneshot pg?
//	if(pre->g.ch == next->g.ch && pre->g.lun == next->g.lun && pre->g.blk == next->g.blk){
//		uint64_t idx = pre->g.pg % spp->pgs_per_oneshotpg;
//		if((pre->g.pg - idx) <= next->g.pg && next->g.pg < (pre->g.pg + spp->pgs_per_oneshotpg)){
//			return 1;
//		}
//	}
	if(is_same_oneshot_page(conv_ftl,*(pre),*(next))){
		return 1;
	}
	else{	
		//next ch and lun??
		int predict_next_ch = pre->g.ch + 1;
		int predict_next_die = pre->g.lun;
		
		if(predict_next_ch >= spp->nchs){
			predict_next_ch = 0;
			predict_next_die ++;
		
			if(predict_next_die >= spp->luns_per_ch){
				predict_next_die = 0;
			}
		}
		if(next->g.ch == predict_next_ch && next->g.lun == predict_next_die){
			return 1;
		}
	}
	return 0;
}

bool phfrag_check(struct nvmev_dev * dev){
	struct nvmev_config *cfg = &dev->config;
	struct nvmev_ns *ns = dev->ns;
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	struct conv_ftl *conv_ftl = &conv_ftls[0];

	struct ssdparams *spp = &conv_ftl->ssd->sp;

	uint64_t lba = cfg->input_lba;
	uint64_t nr_lba = cfg->lba_len;
	uint64_t start_lpn = lba / spp->secs_per_pg;
	uint64_t end_lpn = (lba + nr_lba -1) /spp->secs_per_pg;

	uint64_t lpn;
	uint32_t nr_parts = ns->nr_parts;
	uint32_t i;
	uint64_t count = 0;
	struct ppa prev_ppa;
	uint64_t total_ppa = 0;
	bool page_counter = false;


	NVMEV_ASSERT(conv_ftls);
	uint64_t w=0;
	
	if((end_lpn / nr_parts) >= spp->tt_pgs){
		NVMEV_ERROR("lpn passed FTL range in phfrag");		
		return false;
	}		

	uint64_t local_lpn;
	struct ppa cur_ppa;
	for(i=0; (i<nr_parts) &&(start_lpn <= end_lpn); i++, start_lpn++){
		uint64_t index = start_lpn % nr_parts;
		conv_ftl = &conv_ftls[index];
		
		prev_ppa = conv_ftl->maptbl[start_lpn / nr_parts];

		for(lpn = start_lpn; lpn <= end_lpn; lpn += nr_parts){
			uint64_t local_lpn;
			struct ppa cur_ppa;

			local_lpn = lpn / nr_parts;
		
			cur_ppa = conv_ftl->maptbl[local_lpn];
		
			if(!is_next_correct(conv_ftl,&prev_ppa, &cur_ppa,&page_counter))
			{
				count++;
			}

			total_ppa+=1;
			prev_ppa = cur_ppa;	
	
		}
	}
	cfg->count += count;
	
	return true;	

}  

bool ch_die_check(struct nvmev_dev * dev,bool checkpoint){
	struct nvmev_config *cfg = &dev->config;
	struct nvmev_ns *ns = dev->ns;

	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	struct conv_ftl *conv_ftl = &conv_ftls[0];

	struct ssdparams *spp = &conv_ftl->ssd->sp;

	uint64_t lba = cfg->input_lba;
	uint64_t nr_lba = cfg->lba_len;
	/*confirm*/
	uint64_t start_lpn = lba/spp->secs_per_pg;
	uint64_t end_lpn = (lba + nr_lba -1) / spp->secs_per_pg;
	//////////////////////////////////////////
	uint64_t lpn;
	uint64_t nsecs_start = __get_wallclock(dev);

	uint64_t nsecs_completed, nsecs_latest = nsecs_start;
	uint32_t xfer_size, i;
	uint32_t nr_parts = ns->nr_parts;



	struct ppa prev_ppa;
	struct nand_cmd srd = {
		.stime = nsecs_start,
		.type = USER_IO,
		.interleave_pci_dma = true,
	};

	NVMEV_ASSERT(conv_ftls);
	if ((end_lpn / nr_parts) >= spp->tt_pgs) {
		NVMEV_ERROR("%s: lpn passed FTL range (start_lpn=%lld > tt_pgs=%ld)\n", __func__,
			    start_lpn, spp->tt_pgs);
		return false;
	}

	if (LBA_TO_BYTE(nr_lba) <= (KB(4) * nr_parts)) {
		srd.stime += spp->fw_4kb_rd_lat;
	} else {
		srd.stime += spp->fw_rd_lat;
	}

	for (i = 0; (i < nr_parts) && (start_lpn <= end_lpn); i++, start_lpn++) {
		conv_ftl = &conv_ftls[start_lpn % nr_parts];
		xfer_size = 0;
		prev_ppa = conv_ftl->maptbl[start_lpn / nr_parts];

		for (lpn = start_lpn; lpn <= end_lpn; lpn += nr_parts) {
			uint64_t local_lpn;
			struct ppa cur_ppa;

			local_lpn = lpn / nr_parts;
			cur_ppa = conv_ftl->maptbl[local_lpn];
			if (!mapped_ppa(&cur_ppa) || !valid_ppa(conv_ftl, &cur_ppa)) {
				continue;
			}

			if (mapped_ppa(&prev_ppa) &&
			    is_same_flash_page(conv_ftl, cur_ppa, prev_ppa)) {
				xfer_size += spp->pgsz;
				continue;
			}

			if (xfer_size > 0) {
		//		  srd.stime = __get_wallclock(dev);
                  srd.xfer_size = xfer_size;
                  srd.ppa = &prev_ppa;

                  nsecs_completed = ssd_advance_nand_copy(conv_ftl->ssd, &srd, cfg, cfg->first_io);
                  nsecs_latest = max(nsecs_completed, nsecs_latest);
            }

			xfer_size = spp->pgsz;
			prev_ppa = cur_ppa;
		}

		if (xfer_size > 0) {
		//		  srd.stime = __get_wallclock(dev);
                     srd.xfer_size = xfer_size;
                     srd.ppa = &prev_ppa;

                     nsecs_completed = ssd_advance_nand_copy(conv_ftl->ssd, &srd, cfg,cfg->first_io);
                     nsecs_latest = max(nsecs_completed, nsecs_latest);
				}
	}

	return true;
}  

