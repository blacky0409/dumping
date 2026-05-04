/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _NVMEVIRT_ZNS_SAVE_LOAD_H
#define _NVMEVIRT_ZNS_SAVE_LOAD_H

#include <linux/types.h>
#include "nvmev.h"
#include "ssd.h"
#include "zns_ftl.h"

/*
 * ============================================================
 * ZNS Snapshot 저장/복원 대상 및 근거
 * ============================================================
 *
 * [저장 O]
 *  1. zone_descs[]
 *       - zone마다 state / wp / zslba / zone_capacity / za(zrwav 포함) / zai 보관.
 *       - nvme_zns.h 확인 결과 내부 포인터 없음 → 통째 직렬화 안전.
 *  2. res_infos[]
 *       - ACTIVE / OPEN / ZRWA 리소스 acquired_cnt.
 *       - zone_descs 만으론 재계산 불가 (e.g. CLOSED zone 은 ACTIVE 만 소비).
 *  3. zwra_buffer[i].remaining   (zrwa_buffer_size > 0 일 때만)
 *       - struct buffer 는 spinlock 포함 → 통째 저장 불가.
 *       - size / lock 은 init 값 그대로, remaining 만 덮어쓰면 충분.
 *  4. zone_write_buffer[i].remaining  (zone_wb_size > 0 일 때만)
 *       - 위와 동일 이유.
 *  5. storage 데이터
 *       - EMPTY / OFFLINE zone 제외, 나머지는 zslba ~ (wp-1) 구간.
 *       - FULL zone 은 wp == zslba + zone_capacity 이므로 zone 전체가 저장됨.
 *       - READ_ONLY zone 도 wp 까지 저장.
 *
 * [저장 X]
 *  - NAND 계층 (ch / lun / plane / blk / page / sec)
 *      ZNS 는 __lpn_to_ppa() 수식으로 LPN→PPA 결정 (maptbl 없음).
 *      zns_read_write.c 어디에도 page->status / sec[] 를 읽는 경로 없음.
 *      lun->next_lun_avail_time 은 복원 후 새 I/O 기준으로 자동 갱신.
 *  - report_buffer
 *      zone_descs 의 단순 캐시. load 후 zone_descs 로 memcpy 재구성.
 *  - znsparams / ssdparams
 *      zns_init_namespace() 에서 이미 설정. 헤더에서 일치 검증만 하면 됨.
 *
 * ============================================================
 * 저장 파일 레이아웃  (<root>/zns_dumpfile)
 * ============================================================
 *
 *  [zns_dump_header]                          -- 고정 헤더
 *  [zone_descs : sizeof(zone_descriptor)*N]   -- N = nr_zones
 *  [res_infos  : sizeof(zone_resource_info)*RES_TYPE_COUNT]
 *  [zwra_remaining : size_t * N]              -- zrwa_buf_en==1 일 때만
 *  [zone_wb_remaining : size_t * N]           -- zone_wb_en==1 일 때만
 *  -- storage 데이터 섹션 (EMPTY/OFFLINE zone 제외, 반복) --
 *  [zid : uint32_t] [byte_len : uint64_t] [data : byte_len bytes]
 *  ...
 *  [ZNS_ZONE_SENTINEL : uint32_t]             -- 섹션 끝 마커
 */

#define ZNS_DUMP_FILENAME   "zns_dumpfile"
#define ZNS_DUMP_MAGIC      0x5A4E5346U   /* 'Z' 'N' 'S' 'F' */
#define ZNS_ZONE_SENTINEL   0xFFFFFFFFU   /* storage 섹션 끝 마커 */

struct zns_dump_header {
	uint32_t magic;        /* ZNS_DUMP_MAGIC                         */
	uint32_t nr_zones;     /* znsparams.nr_zones                     */
	uint32_t zone_size;    /* znsparams.zone_size (bytes)            */
	uint32_t nchs;         /* ssdparams.nchs          (검증용)       */
	uint32_t luns_per_ch;  /* ssdparams.luns_per_ch   (검증용)       */
	uint8_t  zrwa_buf_en;  /* zwra_buffer 섹션 포함 여부 (0 or 1)   */
	uint8_t  zone_wb_en;   /* zone_write_buffer 섹션 포함 여부       */
	uint8_t  _pad[2];
};

int save_zns_device(struct nvmev_dev *nvmev_vdev, const char *root);
int load_zns_device(struct nvmev_dev *nvmev_vdev, const char *root);

#endif /* _NVMEVIRT_ZNS_SAVE_LOAD_H */
