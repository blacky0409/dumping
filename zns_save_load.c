// SPDX-License-Identifier: GPL-2.0-only

/*
 * zns_save_load.c  —  ZNS SSD snapshot save / load
 */

#include "zns_save_load.h"

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>
#include <linux/string.h>

/* -----------------------------------------------------------------------
 * 내부 헬퍼
 * --------------------------------------------------------------------- */

static int __zns_kwrite(struct file *f, const void *buf, size_t sz,
			loff_t *pos, const char *tag)
{
	int ret = kernel_write(f, buf, sz, pos);
	if (ret < 0)
		printk(KERN_ERR "[zns_save] write '%s' failed (ret=%d)\n", tag, ret);
	return ret;
}

static int __zns_kread(struct file *f, void *buf, size_t sz,
		       loff_t *pos, const char *tag)
{
	int ret = kernel_read(f, buf, sz, pos);
	if (ret < 0)
		printk(KERN_ERR "[zns_load] read '%s' failed (ret=%d)\n", tag, ret);
	return ret;
}

#define ZNS_WRITE(f, buf, sz, pos, tag)  __zns_kwrite(f, buf, sz, pos, tag)
#define ZNS_READ(f, buf, sz, pos, tag)   __zns_kread(f, buf, sz, pos, tag)

/* -----------------------------------------------------------------------
 * save_zns_device
 *
 * 저장 순서:
 *   1. zns_dump_header
 *   2. zone_descs[]          (nr_zones 개, 통째)
 *   3. res_infos[]           (RES_TYPE_COUNT 개, 통째)
 *   4. zwra_buffer[].remaining       (zrwa_buf_en 일 때만)
 *   5. zone_write_buffer[].remaining (zone_wb_en 일 때만)
 *   6. storage 데이터        (EMPTY/OFFLINE 제외, zslba~wp-1 구간)
 *   7. ZNS_ZONE_SENTINEL     (storage 섹션 끝 마커)
 * --------------------------------------------------------------------- */
int save_zns_device(struct nvmev_dev *nvmev_vdev, const char *root)
{
	struct nvmev_ns  *ns      = nvmev_vdev->ns;
	struct zns_ftl   *zns_ftl = (struct zns_ftl *)ns->ftls;
	struct ssd       *ssd     = zns_ftl->ssd;
	struct ssdparams *spp     = &ssd->sp;
	struct znsparams *zpp     = &zns_ftl->zp;

	struct file *file;
	char  filename[300];
	loff_t f_pos = 0;
	int    i;

	struct zns_dump_header hdr;
	uint32_t sentinel = ZNS_ZONE_SENTINEL;

	printk(KERN_INFO "[zns_save] start — nr_zones=%u zone_size=%u B\n",
	       zpp->nr_zones, zpp->zone_size);

	/* ---- 파일 열기 ---- */
	snprintf(filename, sizeof(filename), "%s/%s", root, ZNS_DUMP_FILENAME);
	file = filp_open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_LARGEFILE, 0666);
	if (IS_ERR(file)) {
		printk(KERN_ERR "[zns_save] filp_open failed: %s\n", filename);
		return PTR_ERR(file);
	}

	/* ---- 1. 헤더 ---- */
	hdr.magic       = ZNS_DUMP_MAGIC;
	hdr.nr_zones    = zpp->nr_zones;
	hdr.zone_size   = zpp->zone_size;
	hdr.nchs        = spp->nchs;
	hdr.luns_per_ch = spp->luns_per_ch;
	hdr.zrwa_buf_en = (zpp->zrwa_buffer_size > 0) ? 1 : 0;
	hdr.zone_wb_en  = (zpp->zone_wb_size     > 0) ? 1 : 0;
	hdr._pad[0] = hdr._pad[1] = 0;

	ZNS_WRITE(file, &hdr, sizeof(hdr), &f_pos, "header");

	/* ---- 2. zone_descs[] ----
	 *
	 * struct zone_descriptor (nvme_zns.h) 확인 결과:
	 *   type(4b) / state(4b) / za(8b) / zai(8b) / reserved(32b)
	 *   zone_capacity(64b) / zslba(64b) / wp(64b) / rsvd[8](256b)
	 * → 내부 포인터 없음. 배열 통째로 직렬화 안전.
	 */
	ZNS_WRITE(file, zns_ftl->zone_descs,
		  sizeof(struct zone_descriptor) * zpp->nr_zones,
		  &f_pos, "zone_descs");

	/* ---- 3. res_infos[] ----
	 *
	 * struct zone_resource_info { u32 acquired_cnt; u32 total_cnt; }
	 * zone_descs 만으로는 acquired_cnt 를 재계산할 수 없다.
	 * (예: CLOSED zone 은 ACTIVE_ZONE 만 소비, OPEN 은 소비하지 않음)
	 */
	ZNS_WRITE(file, zns_ftl->res_infos,
		  sizeof(struct zone_resource_info) * RES_TYPE_COUNT,
		  &f_pos, "res_infos");

	/* ---- 4. zwra_buffer[].remaining (옵션) ----
	 *
	 * struct buffer 는 spinlock_t 포함 → 통째 직렬화 불가.
	 * remaining 만 저장하고, 복원 시 buffer_init() 된 구조체에 덮어씀.
	 */
	if (hdr.zrwa_buf_en) {
		for (i = 0; i < (int)zpp->nr_zones; i++) {
			size_t rem = zns_ftl->zwra_buffer[i].remaining;
			ZNS_WRITE(file, &rem, sizeof(size_t),
				  &f_pos, "zwra_buf_remaining");
		}
	}

	/* ---- 5. zone_write_buffer[].remaining (옵션) ---- */
	if (hdr.zone_wb_en) {
		for (i = 0; i < (int)zpp->nr_zones; i++) {
			size_t rem = zns_ftl->zone_write_buffer[i].remaining;
			ZNS_WRITE(file, &rem, sizeof(size_t),
				  &f_pos, "zone_wb_remaining");
		}
	}

	/* ---- 6. storage 데이터 ----
	 *
	 * 저장 대상 zone:
	 *   - ZONE_STATE_EMPTY   → skip (데이터 없음)
	 *   - ZONE_STATE_OFFLINE → skip (접근 불가 zone)
	 *   - 나머지 (OPENED_IMPL/EXPL, CLOSED, FULL, READ_ONLY)
	 *       → zslba ~ (wp - 1) 구간 저장
	 *       → FULL zone 은 wp == zslba + zone_capacity 이므로 전체 zone 저장됨
	 *
	 * 형식: [zid : u32] [byte_len : u64] [data : byte_len bytes]
	 */
	for (i = 0; i < (int)zpp->nr_zones; i++) {
		struct zone_descriptor *zd = &zns_ftl->zone_descs[i];
		uint64_t written_lbas;
		uint64_t byte_len;
		uint8_t *src;
		int cur, written;

		if (zd->state == ZONE_STATE_EMPTY ||
		    zd->state == ZONE_STATE_OFFLINE)
			continue;

		/* wp - zslba == 이 zone 에 실제로 쓰여진 LBA 수 */
		written_lbas = zd->wp - zd->zslba;
		byte_len     = written_lbas * (uint64_t)spp->secsz;

		if (byte_len == 0)
			continue;

		ZNS_WRITE(file, &i,        sizeof(uint32_t), &f_pos, "zid");
		ZNS_WRITE(file, &byte_len, sizeof(uint64_t), &f_pos, "byte_len");

		src = (uint8_t *)get_storage_addr_from_zid(zns_ftl, i);
		cur = 0;
		while (cur < (int)byte_len) {
			written = kernel_write(file, src + cur,
					       (size_t)(byte_len - cur), &f_pos);
			if (written < 0) {
				printk(KERN_ERR "[zns_save] storage write error zone=%d\n", i);
				goto close_and_return;
			}
			cur += written;
		}
	}

	/* ---- 7. sentinel ---- */
	ZNS_WRITE(file, &sentinel, sizeof(uint32_t), &f_pos, "sentinel");

	printk(KERN_INFO "[zns_save] done — %lld bytes → %s\n",
	       (long long)f_pos, filename);

close_and_return:
	filp_close(file, NULL);
	return 0;
}

/* -----------------------------------------------------------------------
 * load_zns_device
 *
 * 복원 순서:
 *   1. 헤더 읽기 + 파라미터 검증
 *   2. storage 초기화 (memset_io)
 *   3. zone_descs[] 복원
 *   4. res_infos[] 복원
 *   5. zwra_buffer[].remaining 복원 (옵션)
 *   6. zone_write_buffer[].remaining 복원 (옵션)
 *   7. storage 데이터 복원
 *   8. report_buffer 재구성 (zone_descs memcpy)
 * --------------------------------------------------------------------- */
int load_zns_device(struct nvmev_dev *nvmev_vdev, const char *root)
{
	struct nvmev_ns  *ns      = nvmev_vdev->ns;
	struct zns_ftl   *zns_ftl = (struct zns_ftl *)ns->ftls;
	struct ssd       *ssd     = zns_ftl->ssd;
	struct ssdparams *spp     = &ssd->sp;
	struct znsparams *zpp     = &zns_ftl->zp;

	struct file *file;
	char  filename[300];
	loff_t f_pos = 0;
	int    i, ret;

	struct zns_dump_header hdr;

	printk(KERN_INFO "[zns_load] start\n");

	/* ---- 파일 열기 ---- */
	snprintf(filename, sizeof(filename), "%s/%s", root, ZNS_DUMP_FILENAME);
	file = filp_open(filename, O_RDONLY | O_LARGEFILE, 0666);
	if (IS_ERR(file)) {
		printk(KERN_ERR "[zns_load] cannot open: %s\n", filename);
		return PTR_ERR(file);
	}

	/* ---- 1. 헤더 검증 ---- */
	ZNS_READ(file, &hdr, sizeof(hdr), &f_pos, "header");

	if (hdr.magic != ZNS_DUMP_MAGIC) {
		printk(KERN_ERR "[zns_load] magic mismatch 0x%x != 0x%x\n",
		       hdr.magic, ZNS_DUMP_MAGIC);
		filp_close(file, NULL);
		return -EINVAL;
	}
	if (hdr.nr_zones  != zpp->nr_zones ||
	    hdr.zone_size != zpp->zone_size) {
		printk(KERN_ERR
		       "[zns_load] param mismatch: file(zones=%u size=%u) != dev(zones=%u size=%u)\n",
		       hdr.nr_zones, hdr.zone_size, zpp->nr_zones, zpp->zone_size);
		filp_close(file, NULL);
		return -EINVAL;
	}

	/* ---- 2. storage 전체 초기화 ----
	 *
	 * load 전 storage 를 완전히 비운다.
	 * 이후 storage 데이터 섹션에서 유효 zone 만 복원된다.
	 */
	memset_io(nvmev_vdev->storage_mapped, 0,
		  nvmev_vdev->config.storage_size);

	/* ---- 3. zone_descs[] 복원 ---- */
	ZNS_READ(file, zns_ftl->zone_descs,
		 sizeof(struct zone_descriptor) * zpp->nr_zones,
		 &f_pos, "zone_descs");

	/* ---- 4. res_infos[] 복원 ---- */
	ZNS_READ(file, zns_ftl->res_infos,
		 sizeof(struct zone_resource_info) * RES_TYPE_COUNT,
		 &f_pos, "res_infos");

	/* ---- 5. zwra_buffer[].remaining 복원 (옵션) ----
	 *
	 * buffer_init() 은 zns_init_namespace() 에서 이미 호출됨.
	 * size / spinlock 은 그대로 두고 remaining 만 교체.
	 */
	if (hdr.zrwa_buf_en) {
		for (i = 0; i < (int)zpp->nr_zones; i++) {
			size_t rem = 0;
			ZNS_READ(file, &rem, sizeof(size_t),
				 &f_pos, "zwra_buf_remaining");
			zns_ftl->zwra_buffer[i].remaining = rem;
		}
	}

	/* ---- 6. zone_write_buffer[].remaining 복원 (옵션) ---- */
	if (hdr.zone_wb_en) {
		for (i = 0; i < (int)zpp->nr_zones; i++) {
			size_t rem = 0;
			ZNS_READ(file, &rem, sizeof(size_t),
				 &f_pos, "zone_wb_remaining");
			zns_ftl->zone_write_buffer[i].remaining = rem;
		}
	}

	/* ---- 7. storage 데이터 복원 ----
	 *
	 * sentinel 이 나올 때까지 [zid][byte_len][data] 청크를 반복.
	 */
	while (1) {
		uint32_t zid      = 0;
		uint64_t byte_len = 0;
		uint8_t *dst;
		int cur, rd;

		ret = ZNS_READ(file, &zid, sizeof(uint32_t),
			       &f_pos, "zid/sentinel");
		if (ret <= 0)
			break;

		if (zid == ZNS_ZONE_SENTINEL)
			break;

		if (zid >= zpp->nr_zones) {
			printk(KERN_ERR "[zns_load] invalid zid=%u (nr_zones=%u)\n",
			       zid, zpp->nr_zones);
			filp_close(file, NULL);
			return -EINVAL;
		}

		ZNS_READ(file, &byte_len, sizeof(uint64_t), &f_pos, "byte_len");

		dst = (uint8_t *)get_storage_addr_from_zid(zns_ftl, zid);
		cur = 0;
		while (cur < (int)byte_len) {
			rd = kernel_read(file, dst + cur,
					 (size_t)(byte_len - cur), &f_pos);
			if (rd < 0) {
				printk(KERN_ERR
				       "[zns_load] storage read error zone=%u\n", zid);
				filp_close(file, NULL);
				return rd;
			}
			cur += rd;
		}
	}

	/* ---- 8. report_buffer 재구성 ----
	 *
	 * report_buffer 는 zone_descs 의 캐시.
	 * zone_descs 복원 후 memcpy 로 동기화하면 충분.
	 * (zns_zmgmt_recv → __fill_zone_report 에서 zone_descs 를 직접 복사하므로
	 *  report_buffer 자체를 미리 채워두는 것은 엄밀히는 불필요하지만,
	 *  중간에 직접 report_buffer 를 읽는 경로가 생길 경우를 대비해 동기화.)
	 */
	memcpy(zns_ftl->report_buffer->zd,
	       zns_ftl->zone_descs,
	       sizeof(struct zone_descriptor) * zpp->nr_zones);
	zns_ftl->report_buffer->nr_zones = zpp->nr_zones;

	filp_close(file, NULL);
	printk(KERN_INFO "[zns_load] done\n");
	return 0;
}
