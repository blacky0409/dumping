#ifndef _NVMEVIRT_FRAG_H
#define _NVMEVIRT_FRAG_H

#include "nvmev.h"
#include "conv_ftl.h"
#include "ssd.h"
#include "channel_model.h"
#include "pqueue/pqueue_linked.h"

#define CH_MAX_MODEL_SIZE (1 * MB(1))

enum{
	PHFRAG = 0,
	CHDIE = 1,
};

void PHFRAG_INIT(struct nvmev * nvmev);
void PHFRAG_EXIT(void);
bool phfrag_check(struct nvmev_dev * dev);
bool ch_die_check(struct nvmev_dev * dev, bool checkpoint);
#endif
