#ifndef _NVMEVIRT_TOY_H
#define _NVMEVIRT_TOY_H

#include "nvmev.h"
#include "conv_ftl.h"
#include "ssd.h"
#include "pqueue/pqueue.h"

struct dump_header {
    uint32_t nr_parts;
    uint64_t tt_pgs;
    uint32_t tt_lines;
    uint32_t nchs;
    uint32_t luns_per_ch;
};

void SAVE_LOAD_INIT(struct nvmev * nvmev);
int save_device(struct nvmev_dev *nvmev_vdev, const char * root);
int load_device(struct nvmev_dev *nvmev_vdev, const char * root);
int dump_ftl_state (struct nvmev_dev *nvmev_vdev,const char *root);
void SAVE_LOAD_EXIT(void);

#endif
