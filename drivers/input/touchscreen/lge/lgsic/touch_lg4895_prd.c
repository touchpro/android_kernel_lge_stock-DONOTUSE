/* production_test.c
 *
 * Copyright (C) 2015 LGE.
 *
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#define TS_MODULE "[prd]"
#define GET_MAX(x,y,z)	((z)>(((x)>(y))?(x):(y))?	\
			(z):(((x)>(y))?(x):(y)))
#define EVEN		(0)
#define ODD		(1)

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/syscalls.h>
#include <linux/file.h>
#include <linux/workqueue.h>
#include <linux/interrupt.h>
#include <linux/firmware.h>
#include <soc/qcom/lge/board_lge.h>

/*
 *  Include to touch core Header File
 */
#include <touch_hwif.h>
#include <touch_core.h>

/*
 *  Include to Local Header File
 */
#include "touch_lg4895.h"
#include "touch_lg4895_prd.h"

static char line[50000];
static char W_Buf[BUF_SIZE];
static u16 M2_Rawdata_buf[ROW_SIZE*COL_SIZE];
static u16 M2_Rawdata_even_buf[ROW_SIZE*COL_SIZE];
static u16 M2_Rawdata_odd_buf[ROW_SIZE*COL_SIZE];
static u16 M2_Jitter_buf[ROW_SIZE*COL_SIZE];
static u16 M1_Rawdata_buf[ROW_SIZE*M1_COL_SIZE];
static u16 LowerImage[ROW_SIZE][COL_SIZE];
static u16 UpperImage[ROW_SIZE][COL_SIZE];
static int jitter_max_value[JITTER_TEST_CNT][4];
static int jitter_max;
static int jitter_avg_max;
static int total_jmax, total_avg_jmax;
static atomic_t block_prd;
static int read_memory(struct device *dev, u16 w_addr,
	u32 w_data, u16 w_size, u16 r_addr, u8 *r_buff, u32 r_size)
{
        u16 nToRead = r_size;

	if ( NULL == r_buff ) {
		TOUCH_E("%s : fail\n", __func__);
		return -ENOMEM;
	}

       if(lg4895_reg_write( dev , w_addr , &w_data, sizeof(u32))<0){
                TOUCH_E("reg addr 0x%x ReadMemory->write fail\n", w_addr);
                return -1;
        }

        if(lg4895_reg_read(dev , r_addr , r_buff , nToRead)<0){
                TOUCH_E("reg addr 0x%x ReadMemory->read fail\n", r_addr);
                return -1;
        }
        return 1;
}

static void log_file_size_check(struct device *dev)
{
	char *fname = NULL;
	struct file *file;
	loff_t file_size = 0;
	int i = 0;
	char buf1[128] = {0};
	char buf2[128] = {0};
	mm_segment_t old_fs = get_fs();
	int ret = 0;
	int boot_mode = 0;

	set_fs(KERNEL_DS);

	boot_mode = touch_boot_mode_check(dev);

	switch (boot_mode) {
	case NORMAL_BOOT:
		fname = "/sdcard/touch_self_test.txt";
		break;
	case MINIOS_AAT:
		fname = "/data/touch/touch_self_test.txt";
		break;
	case MINIOS_MFTS_FOLDER:
	case MINIOS_MFTS_FLAT:
	case MINIOS_MFTS_CURVED:
		fname = "/data/touch/touch_self_mfts.txt";
		break;
	default:
		TOUCH_I("%s : not support mode\n", __func__);
		break;
	}

	if (fname) {
		file = filp_open(fname, O_RDONLY, 0666);
		sys_chmod(fname, 0666);
	} else {
		TOUCH_E("%s : fname is NULL, can not open FILE\n",
				__func__);
		goto error;
	}

	if (IS_ERR(file)) {
		TOUCH_I("%s : ERR(%ld) Open file error [%s]\n",
				__func__, PTR_ERR(file), fname);
		goto error;
	}

	file_size = vfs_llseek(file, 0, SEEK_END);
	TOUCH_I("%s : [%s] file_size = %lld\n",
			__func__, fname, file_size);

	filp_close(file, 0);

	if (file_size > MAX_LOG_FILE_SIZE) {
		TOUCH_I("%s : [%s] file_size(%lld) > MAX_LOG_FILE_SIZE(%d)\n",
				__func__, fname, file_size, MAX_LOG_FILE_SIZE);

		for (i = MAX_LOG_FILE_COUNT - 1; i >= 0; i--) {
			if (i == 0)
				sprintf(buf1, "%s", fname);
			else
				sprintf(buf1, "%s.%d", fname, i);

			ret = sys_access(buf1, 0);

			if (ret == 0) {
				TOUCH_I("%s : file [%s] exist\n",
						__func__, buf1);

				if (i == (MAX_LOG_FILE_COUNT - 1)) {
					if (sys_unlink(buf1) < 0) {
						TOUCH_E("%s : failed to remove file [%s]\n",
								__func__, buf1);
						goto error;
					}

					TOUCH_I("%s : remove file [%s]\n",
							__func__, buf1);
				} else {
					sprintf(buf2, "%s.%d",
							fname,
							(i + 1));

					if (sys_rename(buf1, buf2) < 0) {
						TOUCH_E("%s : failed to rename file [%s] -> [%s]\n",
								__func__, buf1, buf2);
						goto error;
					}

					TOUCH_I("%s : rename file [%s] -> [%s]\n",
							__func__, buf1, buf2);
				}
			} else {
				TOUCH_I("%s : file [%s] does not exist (ret = %d)\n",
						__func__, buf1, ret);
			}
		}
	}

error:
	set_fs(old_fs);
	return;
}
static void write_file(struct device *dev, char *data, int write_time)
{
	int fd = 0;
	char *fname = NULL;
	char time_string[64] = {0};
	struct timespec my_time;
	struct tm my_date;
	mm_segment_t old_fs = get_fs();
	int boot_mode = 0;

	set_fs(KERNEL_DS);

	boot_mode = touch_boot_mode_check(dev);

	switch (boot_mode) {
	case NORMAL_BOOT:
		fname = "/sdcard/touch_self_test.txt";
		break;
	case MINIOS_AAT:
		fname = "/data/touch/touch_self_test.txt";
		break;
	case MINIOS_MFTS_FOLDER:
	case MINIOS_MFTS_FLAT:
	case MINIOS_MFTS_CURVED:
		fname = "/data/touch/touch_self_mfts.txt";
		break;
	default:
		TOUCH_I("%s : not support mode\n", __func__);
		break;
	}

	if (fname) {
		fd = sys_open(fname, O_WRONLY|O_CREAT|O_APPEND, 0666);
		sys_chmod(fname, 0666);
	} else {
		TOUCH_E("%s : fname is NULL, can not open FILE\n", __func__);
		set_fs(old_fs);
		return;
	}

	if (fd >= 0) {
		if (write_time == TIME_INFO_WRITE) {
			my_time = __current_kernel_time();
			time_to_tm(my_time.tv_sec,
					sys_tz.tz_minuteswest * 60 * (-1),
					&my_date);
			snprintf(time_string, 64,
				"\n[%02d-%02d %02d:%02d:%02d.%03lu]\n",
				my_date.tm_mon + 1,
				my_date.tm_mday, my_date.tm_hour,
				my_date.tm_min, my_date.tm_sec,
				(unsigned long) my_time.tv_nsec / 1000000);
			sys_write(fd, time_string, strlen(time_string));
		}
		sys_write(fd, data, strlen(data));
		sys_close(fd);
	} else {
		TOUCH_I("File open failed\n");
	}
	set_fs(old_fs);
}

static int write_test_mode(struct device *dev, u8 type)
{
	u32 testmode = 0;
	u8 disp_mode = 0x3;
	int retry = 20;
	u32 rdata = 0x01;
	int waiting_time = 400;

	switch (type) {
	case OPEN_NODE_TEST:
		testmode = (disp_mode << 8) + type;
		waiting_time = 100;
		break;
	case SHORT_NODE_TEST:
		testmode = (disp_mode << 8) + type;
		waiting_time = 100;
		break;
	case U3_M2_RAWDATA_TEST:
		testmode = (disp_mode << 8) + type;
		waiting_time = 100;
		break;
	case U0_M1_RAWDATA_TEST:
		type = 0x6;
		testmode = type;
		break;
	case U0_M2_RAWDATA_TEST:
		type = 0x5;
		testmode = type;
		break;
	case JITTER_TEST:
		testmode = BLU_JITTER_TEST_CMD|LINE_FILTER_OPTION;
		waiting_time = 200;
		break;
	}

	/* TestType Set */
	lg4895_reg_write(dev, tc_tsp_test_ctl,
			(u8 *)&testmode,
			sizeof(testmode));
	TOUCH_I("write testmode = 0x%x\n", testmode);
	touch_msleep(waiting_time);

	/* Check Test Result - wait until 0 is written */
	do {
		touch_msleep(100);
		lg4895_reg_read(dev, tc_tsp_test_sts,
				(u8 *)&rdata,
				sizeof(rdata));
		TOUCH_I("rdata = 0x%x\n", rdata);
	} while ((rdata != 0xAA) && retry--);

	if (rdata != 0xAA) {
		TOUCH_I("ProductionTest Type [%d] Time out\n", type);
		goto error;
	}
	return 1;
error:
	TOUCH_E("[%s] fail\n", __func__);
	return 0;
}

static int prd_os_result_read(struct device *dev,
	u16 *buf, int type, u32 data_size)
{
	int ret = 0;
	u32 diff_data_offset = 0;

	if ( NULL == buf ) {
		TOUCH_E("buf is null exception\n");
		ret = 1;
		goto error;
	}

	ret = lg4895_reg_read(dev, prod_open3_short_offset,
		&diff_data_offset, sizeof(u32));
	TOUCH_I("%s : %x\n", __func__, diff_data_offset);

	if (ret) {
		return ret;
	} else {
		switch (type) {
			case OPEN_NODE_TEST:
				diff_data_offset &= 0xFFFF;
				break;
			case SHORT_NODE_TEST:
				diff_data_offset = (diff_data_offset >> 16);
				break;
		}
	}
	ret = lg4895_reg_write(dev, tc_tsp_test_data_offset, 
		&diff_data_offset, sizeof(u32));
	if (ret) {
		return ret;
	} else {
		ret = lg4895_reg_read(dev, tc_tsp_data_access_addr,
		buf, sizeof(u16)*data_size);
		if (ret)
			return ret;
	}

error:
	return ret;
}

static void prnit_prd_open_short_result(u16 *open_buf, u16 *short_buf)
{
	int i=0, j=0;
	int ret=0;

	TOUCH_I("%s : O/S test is fail\n", __func__);
	if (NULL == open_buf || NULL == short_buf)
		goto error;

	/* open test result */
	TOUCH_I("open test result\n");
	ret += snprintf(W_Buf + ret, BUF_SIZE - ret, " OPEN TEST RESULT\n ");
	for (i = 0; i < COL_SIZE; i++)
		ret += snprintf(W_Buf + ret, BUF_SIZE - ret, " [%2d] ", i);

	for (i = 0; i < ROW_SIZE; i++) {
		char log_buf[LOG_BUF_SIZE] = {0, };
		int log_ret = 0;

		ret += snprintf(W_Buf + ret, BUF_SIZE - ret,  "\n[%2d], ", i);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret,  "[%2d]  ", i);
		for (j = 0; j < COL_SIZE; j++) {
			ret += snprintf(W_Buf + ret, BUF_SIZE - ret,
					"%5d, ", open_buf[i*COL_SIZE+j]);
			log_ret += snprintf(log_buf + log_ret,
					LOG_BUF_SIZE - log_ret,
					"%5d ", open_buf[i*COL_SIZE+j]);
		}
		TOUCH_I("%s\n", log_buf);
	}
	ret += snprintf(W_Buf + ret, BUF_SIZE - ret, "\n");

	TOUCH_I("short test result\n");
	ret += snprintf(W_Buf + ret, BUF_SIZE - ret, " SHORT TEST RESULT\n ");
	ret += snprintf(W_Buf + ret, BUF_SIZE - ret, "\n");
	/* short test result */
	for ( i=0; i<RAWDATA_SIZE*RAWDATA_SIZE; i++) {
			char log_buf[LOG_BUF_SIZE] = {0, };
			int log_ret = 0;

			ret += snprintf(W_Buf + ret, BUF_SIZE - ret,  "\n[%2d], ", i);
			log_ret += snprintf(log_buf + log_ret,
					LOG_BUF_SIZE - log_ret,  "[%2d]  ", i);
			for (j = 0; j < ROW_SIZE; j++) {
				ret += snprintf(W_Buf + ret, BUF_SIZE - ret,
						"%5d, ", short_buf[i*ROW_SIZE+j]);
				log_ret += snprintf(log_buf + log_ret,
						LOG_BUF_SIZE - log_ret,
						"%5d ", short_buf[i*ROW_SIZE+j]);
			}
			TOUCH_I("%s\n", log_buf);
		}
		ret += snprintf(W_Buf + ret, BUF_SIZE - ret, "\n");
	return ;
error:
	TOUCH_E("mem fail\n");
	return ;
}
static int prd_open_short_test(struct device *dev)
{
	int type = 0;
	int ret = 0;
	int write_test_mode_result = 0;
	u32 open_result = 0;
	u32 short_result = 0;
	u32 openshort_all_result = 0;
	u16 *open_buf = NULL;
	u16 *short_buf = NULL;
	u32 data_size = 0;

	open_buf = kzalloc(sizeof(u16)*ROW_SIZE*COL_SIZE,
					GFP_KERNEL);
	short_buf = kzalloc(sizeof(u16)*ROW_SIZE*RAWDATA_SIZE*RAWDATA_SIZE,
					GFP_KERNEL);

	/* Test Type Write */
	write_file(dev, "[OPEN_SHORT_ALL_TEST]\n", TIME_INFO_SKIP);

	/* 1. open_test */
	type = OPEN_NODE_TEST;
	write_test_mode_result = write_test_mode(dev, type);
	if (write_test_mode_result == 0) {
		TOUCH_E("write_test_mode fail\n");
		return 0x3;
	}

	lg4895_reg_read(dev, tc_tsp_test_pf_result,
			(u8 *)&open_result, sizeof(open_result));
	TOUCH_I("open_result = %d\n", open_result);

	if (open_result) {
		data_size = ROW_SIZE*COL_SIZE;
		ret = prd_os_result_read(dev, open_buf, type, data_size);
		openshort_all_result |= 0x1;
	}

	/* 2. short_test */
	type = SHORT_NODE_TEST;
	write_test_mode_result = write_test_mode(dev, type);
	if (write_test_mode_result == 0) {
		TOUCH_E("write_test_mode fail\n");
		return 0x3;
	}

	lg4895_reg_read(dev, tc_tsp_test_pf_result,
			(u8 *)&short_result, sizeof(short_result));
	TOUCH_I("short_result = %d\n", short_result);

	if (short_result) {
		data_size = ROW_SIZE*RAWDATA_SIZE*RAWDATA_SIZE;
		ret = prd_os_result_read(dev, short_buf, type, data_size);
		openshort_all_result |= 0x2;
	}

	/* fail case */
	if (openshort_all_result != 0) {
		prnit_prd_open_short_result(open_buf, short_buf);

	} else
		ret = snprintf(W_Buf + ret, BUF_SIZE - ret,
				"OPEN_SHORT_ALL_TEST : Pass\n");

	write_file(dev, W_Buf, TIME_INFO_SKIP);

	kfree(open_buf);
	kfree(short_buf);

	return openshort_all_result;
}
static void prd_read_rawdata(struct device *dev, u8 type)
{
	u32 raw_offset_info = 0;
	u16 raw_offset = 0;
	u32 m2_raw_data_odd_offset = 0;
	u32 m2_raw_data_even_offset = 0;
	int ret = 0;

	int __m1_frame_size = ROW_SIZE*M1_COL_SIZE*RAWDATA_SIZE;
	int __m2_frame_size = ROW_SIZE*COL_SIZE*RAWDATA_SIZE;
	int i = 0;
	static u16 M1_Rawdata_temp[ROW_SIZE*M1_COL_SIZE];

	if (__m1_frame_size % 4)
		__m1_frame_size = (((__m1_frame_size >> 2) + 1) << 2);
	if (__m2_frame_size % 4)
		__m2_frame_size = (((__m2_frame_size >> 2) + 1) << 2);

	lg4895_reg_read(dev, tc_tsp_test_off_info,
			(u8 *)&raw_offset_info, sizeof(u32));

	switch (type) {
		case U0_M1_RAWDATA_TEST:
			raw_offset = raw_offset_info & 0xFFFF;
			lg4895_reg_write(dev, tc_tsp_test_data_offset,
					(u8 *)&raw_offset,
					sizeof(u32));
			memset(M1_Rawdata_buf, 0, sizeof(M1_Rawdata_buf));
			memset(M1_Rawdata_temp, 0, sizeof(M1_Rawdata_buf));
			lg4895_reg_read(dev, tc_tsp_data_access_addr,
					(u8 *)&M1_Rawdata_temp,
					__m1_frame_size);

			for(i = 0; i < ROW_SIZE; i++)
			{
				M1_Rawdata_buf[i*2] = M1_Rawdata_temp[ROW_SIZE+i];
				M1_Rawdata_buf[i*2+1] = M1_Rawdata_temp[i];
			}
			break;
		case U3_M2_RAWDATA_TEST:
			m2_raw_data_odd_offset = 0;
			m2_raw_data_even_offset = 0;

			ret += lg4895_reg_read(dev, prod_open1_2_offset,
					&raw_offset_info, sizeof(u32));

			m2_raw_data_odd_offset = raw_offset_info & 0xFFFF;
			m2_raw_data_even_offset = raw_offset_info >> 16;

			TOUCH_I("m2_raw_data_odd_offset : %d, m2_raw_data_even_offset : %d\n",
				m2_raw_data_odd_offset, m2_raw_data_even_offset);

			ret += lg4895_reg_write(dev, tc_tsp_test_data_offset,
					&m2_raw_data_odd_offset, sizeof(u32));
			memset(M2_Rawdata_odd_buf, 0x0, sizeof(M2_Rawdata_odd_buf));
			ret += lg4895_reg_read(dev, tc_tsp_data_access_addr,
					(u8 *)&M2_Rawdata_odd_buf, __m2_frame_size);

			ret += lg4895_reg_write(dev, tc_tsp_test_data_offset,
					&m2_raw_data_even_offset, sizeof(u32));
			memset(M2_Rawdata_even_buf, 0x0, sizeof(M2_Rawdata_even_buf));
			ret += lg4895_reg_read(dev, tc_tsp_data_access_addr,
					(u8 *)&M2_Rawdata_even_buf, __m2_frame_size);
			if (ret)
				goto error;
			break;
		case U0_M2_RAWDATA_TEST:
			raw_offset = (raw_offset_info >> 16) & 0xFFFF;
			lg4895_reg_write(dev, tc_tsp_test_data_offset,
					(u8 *)&raw_offset,
					sizeof(u32));
			memset(M2_Rawdata_buf, 0, sizeof(M2_Rawdata_buf));
			lg4895_reg_read(dev, tc_tsp_data_access_addr,
					(u8 *)&M2_Rawdata_buf,
					__m2_frame_size);
			break;
	}
	return ;
error:
	TOUCH_E("type %d test fail \n", type);
}

static int sdcard_spec_file_read(struct device *dev)
{
	int ret = 0;
	int fd = 0;
	char *path[2] = { "/mnt/sdcard/k5_limit.txt",
		"/mnt/sdcard/k5_limit_mfts.txt"
	};
	int path_idx = 0;

	mm_segment_t old_fs = get_fs();

	if(touch_boot_mode_check(dev) >= MINIOS_MFTS_FOLDER)
		path_idx = 1;
	else
		path_idx = 0;
	set_fs(KERNEL_DS);
	fd = sys_open(path[path_idx], O_RDONLY, 0);
	if (fd >= 0) {
		sys_read(fd, line, sizeof(line));
		sys_close(fd);
		TOUCH_I("%s file existing\n", path[path_idx]);
		ret = 1;
	}
	set_fs(old_fs);

	return ret;
}

static int spec_file_read(struct device *dev)
{
	int ret = 0;
	struct touch_core_data *ts = to_touch_core(dev);
	const struct firmware *fwlimit = NULL;
	const char *path[2] = { ts->panel_spec,
		ts->panel_spec_mfts
	};
	int path_idx = 0;

	if(touch_boot_mode_check(dev) >= MINIOS_MFTS_FOLDER)
		path_idx = 1;
	else
		path_idx = 0;

	if (ts->panel_spec == NULL || ts->panel_spec_mfts == NULL) {
		TOUCH_I("panel_spec_file name is null\n");
		ret = -1;
		goto error;
	}

	if (request_firmware(&fwlimit, path[path_idx], dev) < 0) {
		TOUCH_I("request ihex is failed in normal mode\n");
		ret = -1;
		goto error;
	}

	if (fwlimit->data == NULL) {
		ret = -1;
		TOUCH_I("fwlimit->data is NULL\n");
		goto error;
	}

	strlcpy(line, fwlimit->data, sizeof(line));

error:
	if (fwlimit)
		release_firmware(fwlimit);

	return ret;
}

static int sic_get_limit(struct device *dev, char *breakpoint, u16 (*buf)[COL_SIZE])
{
	int p = 0;
	int q = 0;
	int r = 0;
	int cipher = 1;
	int ret = 0;
	char *found;
	int boot_mode = 0;
	int file_exist = 0;
	int tx_num = 0;
	int rx_num = 0;


	if (breakpoint == NULL) {
		ret = -1;
		goto error;
	}

	boot_mode = touch_boot_mode_check(dev);
	if (boot_mode > MINIOS_MFTS_CURVED
			|| boot_mode < NORMAL_BOOT) {
		ret = -1;
		goto error;
	}

	file_exist = sdcard_spec_file_read(dev);
	if (!file_exist) {
		ret = spec_file_read(dev);
		if (ret == -1)
			goto error;
	}

	if (line == NULL) {
		ret =  -1;
		goto error;
	}

	found = strnstr(line, breakpoint, sizeof(line));
	if (found != NULL) {
		q = found - line;
	} else {
		TOUCH_I(
				"failed to find breakpoint. The panel_spec_file is wrong\n");
		ret = -1;
		goto error;
	}

	memset(buf, 0, ROW_SIZE * COL_SIZE * 2);

	while (1) {
		if (line[q] == ',') {
			cipher = 1;
			for (p = 1; (line[q - p] >= '0') &&
					(line[q - p] <= '9'); p++) {
				buf[tx_num][rx_num] += ((line[q - p] - '0') * cipher);
				cipher *= 10;
			}
			r++;
			if (r % (int)COL_SIZE == 0) {
				rx_num = 0;
				tx_num++;
			} else {
				rx_num++;
			}
		}
		q++;
		if (r == (int)ROW_SIZE * (int)COL_SIZE) {
			TOUCH_I("panel_spec_file scanning is success\n");
			break;
		}
	}

error:
	return ret;
}
static int prd_print_dual_rawdata(struct device *dev, char *buf)
{
	int i=0,j=0;
	int min = 9999;
	int max = 0;
	int ret = 0;
	int baseline_cnt=0;
	const int col_size = COL_SIZE;
	u16 *rawdata_buf = M2_Rawdata_odd_buf;

	TOUCH_I("this f/w use %s \n", __func__);

	ret += snprintf(buf + ret, BUF_SIZE - ret, "\n");
	for (baseline_cnt=0; baseline_cnt<2; baseline_cnt++) {
		TOUCH_I("rawdata_buf : %p\n", rawdata_buf);
		for (i = 0; i < col_size; i++)
			ret += snprintf(buf + ret, BUF_SIZE - ret, " [%2d] ", i);

		for (i = 0; i < ROW_SIZE; i++) {
			char log_buf[LOG_BUF_SIZE] = {0, };
			int log_ret = 0;

			ret += snprintf(buf + ret, BUF_SIZE - ret,  "\n[%2d], ", i);
			log_ret += snprintf(log_buf + log_ret,
					LOG_BUF_SIZE - log_ret,  "[%2d]  ", i);
			for (j = 0; j < col_size; j++) {
				ret += snprintf(buf + ret, BUF_SIZE - ret,
						"%5d, ", rawdata_buf[i*col_size+j]);
				log_ret += snprintf(log_buf + log_ret,
						LOG_BUF_SIZE - log_ret,
						"%5d ", rawdata_buf[i*col_size+j]);
				if (rawdata_buf[i*col_size+j] != 0 &&
						rawdata_buf[i*col_size+j] < min)
					min = rawdata_buf[i*col_size+j];
				if (rawdata_buf[i*col_size+j] > max)
					max = rawdata_buf[i*col_size+j];
			}
			TOUCH_I("%s\n", log_buf);
		}
		ret += snprintf(buf + ret, BUF_SIZE - ret, "\n");

		ret += snprintf(buf + ret, BUF_SIZE - ret,
				"\ndata min : %d , max : %d\n", min, max);
		TOUCH_I("data min : %d , max : %d\n", min, max);

		ret += snprintf(buf + ret, BUF_SIZE - ret, "\n");
		rawdata_buf = M2_Rawdata_even_buf;
	}
	return ret;

}

static int prd_print_rawdata(struct device *dev, char *buf, u8 type)
{
	int i = 0, j = 0;
	int ret = 0;
	int min = 9999;
	int max = 0;
	u16 *rawdata_buf = NULL;
	int col_size = 0;

	/* print a frame data */
	ret = snprintf(buf, PAGE_SIZE, "\n   : ");

	switch (type) {
		case U0_M1_RAWDATA_TEST:
			col_size = M1_COL_SIZE;
			rawdata_buf = M1_Rawdata_buf;
			break;
		case U3_M2_RAWDATA_TEST:
			ret = prd_print_dual_rawdata(dev, buf);
			return ret;
			break;
		case U0_M2_RAWDATA_TEST:
			col_size = COL_SIZE;
			rawdata_buf = M2_Rawdata_buf;
			break;
		case JITTER_TEST:
			col_size = COL_SIZE;
			rawdata_buf = M2_Jitter_buf;
			jitter_max = jitter_avg_max = 0;
			break;
	}

	for (i = 0; i < col_size; i++)
		ret += snprintf(buf + ret, BUF_SIZE - ret, " [%2d] ", i);

	for (i = 0; i < ROW_SIZE; i++) {
		char log_buf[LOG_BUF_SIZE] = {0, };
		int log_ret = 0;
		int avg_left = 0, avg_right = 0;

		ret += snprintf(buf + ret, BUF_SIZE - ret,  "\n[%2d], ", i);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret,  "[%2d]  ", i);
		for (j = 0; j < col_size; j++) {
			ret += snprintf(buf + ret, BUF_SIZE - ret,
					"%5d, ", rawdata_buf[i*col_size+j]);
			log_ret += snprintf(log_buf + log_ret,
					LOG_BUF_SIZE - log_ret,
					"%5d ", rawdata_buf[i*col_size+j]);
			if (rawdata_buf[i*col_size+j] != 0 &&
					rawdata_buf[i*col_size+j] < min)
				min = rawdata_buf[i*col_size+j];
			if (rawdata_buf[i*col_size+j] > max)
				max = rawdata_buf[i*col_size+j];
			if ((col_size/2) <= j)
				avg_left += rawdata_buf[i*col_size+j];
			else if ((col_size/2) > j)
				avg_right += rawdata_buf[i*col_size+j];
		}
		if ( JITTER_TEST == type ) {
			avg_left /= (col_size/2);
			avg_right /= (col_size/2);
			jitter_avg_max = GET_MAX(avg_left, avg_right, jitter_avg_max);
		}
		TOUCH_I("%s\n", log_buf);
	}
	jitter_max = max;

	ret += snprintf(buf + ret, BUF_SIZE - ret, "\n");

	ret += snprintf(buf + ret, BUF_SIZE - ret,
			"\ndata min : %d , max : %d\n", min, max);

	ret += snprintf(buf + ret, BUF_SIZE - ret,
			"\ndata avg: %d\n", jitter_avg_max);

	ret += snprintf(buf + ret, BUF_SIZE - ret, "\n");
	return ret;
}

static void write_csv_file(struct device *dev, char *data)
{
	int fd_csv = 0;
	char fname_csv[64] = {0,};
	mm_segment_t const old_fs = get_fs();
	static u32 csv_cnt = 0;

	if ( BLOCKED == atomic_read(&block_prd)) {
		TOUCH_I("csv captuer disable\n");
		return ;
	}

	TOUCH_I("enter\n");

	set_fs(KERNEL_DS);

	snprintf(fname_csv, 64, "/sdcard/touch_self_test_csv%d.csv", ++csv_cnt);

	if ( likely(NULL != fname_csv) ) {
		TOUCH_I("csv captuer enable : %s\n", fname_csv);
		fd_csv = sys_open(fname_csv, O_WRONLY|O_CREAT|O_APPEND, 0666);
		sys_chmod(fname_csv, 0666);
	} else {
		TOUCH_E("%s : fname is NULL, can not open FILE\n", __func__);
		set_fs(old_fs);
		return;
	}

	if (0 <= fd_csv) {
		sys_write(fd_csv, data, strlen(data));
		sys_close(fd_csv);
	} else {
		TOUCH_I("File open failed\n");
	}

	set_fs(old_fs);
}

/* Rawdata compare result
Pass : reurn 0
Fail : return 1
 */
static int prd_compare_rawdata(struct device *dev, u8 type)
{
	/* spec reading */
	char lower_str[64] = {0, };
	char upper_str[64] = {0, };
	u16 *rawdata_buf = NULL;
	u8 baseline_cnt = 1;
	u8 dual_baseline = NOT_SUPPORT;
	int col_size = 0;
	int i, j;
	int ret = 0;
	int cnt = 0;
	int result = 0;

	switch (type) {
		case U0_M1_RAWDATA_TEST:
			snprintf(lower_str, sizeof(lower_str),
					"DOZE2_M1_Lower");
			snprintf(upper_str, sizeof(upper_str),
					"DOZE2_M1_Upper");
			col_size = M1_COL_SIZE;
			rawdata_buf = M1_Rawdata_buf;
			break;
		case U0_M2_RAWDATA_TEST:
			snprintf(lower_str, sizeof(lower_str),
					"DOZE2_M2_Lower");
			snprintf(upper_str, sizeof(upper_str),
					"DOZE2_M2_Upper");
			col_size = COL_SIZE;
			rawdata_buf = M2_Rawdata_buf;
			break;
		case U3_M2_RAWDATA_TEST:
			snprintf(lower_str, sizeof(lower_str),
					"DOZE1_M2_Lower");
			snprintf(upper_str, sizeof(upper_str),
					"DOZE1_M2_Upper");
			col_size = COL_SIZE;
			rawdata_buf = M2_Rawdata_odd_buf;
			dual_baseline = SUPPORT;
			baseline_cnt = 2;
			break;
		case JITTER_TEST:
			snprintf(lower_str, sizeof(lower_str),
					"DOZE1_M2_Jitter_Lower");
			snprintf(upper_str, sizeof(upper_str),
					"DOZE1_M2_Jitter_Upper");
			col_size = COL_SIZE;
			rawdata_buf = M2_Jitter_buf;
			break;
	}

	if (sic_get_limit(dev, lower_str, LowerImage) ||
			sic_get_limit(dev, upper_str, UpperImage)) {
		TOUCH_E("can't read spec in the file.\n");
		result = 1;
		goto error;
	} else {
		TOUCH_I("size of Image : Lower[%d], Upper[%d]\n",
			sizeof(LowerImage), sizeof(UpperImage));
	}
	for ( cnt=1; cnt<=baseline_cnt; cnt++) {
		for (i = 0; i < ROW_SIZE; i++) {
			for (j = 0; j < col_size; j++) {
				if ((rawdata_buf[i*col_size+j] < LowerImage[i][j]) ||
						(rawdata_buf[i*col_size+j] > UpperImage[i][j])) {
					if ((type != U0_M1_RAWDATA_TEST) &&
							(i <= 1 && j <= 4)) {
						if (rawdata_buf[i*col_size+j] != 0) {
							result = 1;
							ret += snprintf(W_Buf + ret, BUF_SIZE - ret,
									"F [%d][%d] = %d\n", i, j, rawdata_buf[i*col_size+j]);
						}
					} else {
						result = 1;
						ret += snprintf(W_Buf + ret, BUF_SIZE - ret,
								"F [%d][%d] = %d\n", i, j, rawdata_buf[i*col_size+j]);
					}
				}
			}
		}
		if ( dual_baseline )
			rawdata_buf = M2_Rawdata_even_buf;
	}

error:
	return result;
}

static void BLU_jitter_enable(struct device *dev)
{
	struct lg4895_data *d = to_lg4895_data(dev);
	struct touch_core_data *ts = to_touch_core(dev);

	TOUCH_TRACE();

	TOUCH_I("%s : work is %s\n", __func__,
			delayed_work_pending(&d->BLU_jitter_work)?"pendding":"idle");
	TOUCH_I("%s : sync [%d]\n", __func__,
			queue_delayed_work(ts->wq, &d->BLU_jitter_work,	msecs_to_jiffies(10)));
}

static int prd_jitter_test(struct device *dev, u8 type,
		int* max)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int write_test_mode_result = 0;
	int jitter_result = 0;
	u8 const BLU_jitter_test = SUPPORT;
	u16 *buf = NULL;
	int i = 0;
	int j = 0;
	int ret = 0;
	u32 frame_cmd = 0x0;
	u32 offset = 0;

	TOUCH_TRACE();

	/* Test Type Write */
	write_file(dev, "[JITTER_TEST]\n", TIME_INFO_SKIP);

	memset(&jitter_max_value[0][0], 0x0, sizeof(int)*JITTER_TEST_CNT*4);
	total_jmax = total_avg_jmax = 0;
	buf = M2_Jitter_buf;
	if ( NULL != buf )
		memset(buf, 0x0, sizeof(u16)*ROW_SIZE*COL_SIZE);

	if ( lg4895_reg_read(dev, prod_open1_2_offset, &offset, sizeof(u32)) )
		TOUCH_E("%s : read command fail \n", __func__);

	for ( i=0; i<JITTER_TEST_CNT; i++) {
		if(BLU_jitter_test) {
			BLU_jitter_enable(dev);
		}

		/* jitter_test */
		write_test_mode_result = write_test_mode(dev, type);
		if (write_test_mode_result == 0) {
			TOUCH_E("write_test_mode fail\n");
			return 0x3;
		}

		for (j=0; j<2; j++) {
			if (EVEN == j)
				frame_cmd = (offset&0xFFFF);
			else if (ODD == j)
				frame_cmd = (offset>>16);

			if (read_memory(dev, SERIAL_DATA_OFFSET, frame_cmd, sizeof(u32),
						DATA_I2CBASE_ADDR, (u8 *)buf, sizeof(u8)*(ROW_SIZE)*(COL_SIZE)*2) < 0)
				TOUCH_E("read memory error!!\n");

			/* data print result */
			if (NULL != W_Buf)
				memset(W_Buf, 0x0, sizeof(char)*BUF_SIZE);
			jitter_result = prd_print_rawdata(dev, W_Buf, type);
			write_file(dev, W_Buf, TIME_INFO_SKIP);

			/* data compare result(pass : 0 fail : 1) */
			if (NULL != W_Buf)
				memset(W_Buf, 0x0, sizeof(char)*BUF_SIZE);
			jitter_result = prd_compare_rawdata(dev, type);
			write_file(dev, W_Buf, TIME_INFO_SKIP);

			/* data gettering */
			if (EVEN == j) {
				jitter_max_value[i][EVEN*2+JITTER_VA] = jitter_max;
				jitter_max_value[i][EVEN*2+JITTER_AVG_VA] = jitter_avg_max;
			}
			else if (ODD == j) {
				jitter_max_value[i][ODD*2+JITTER_VA] = jitter_max;
				jitter_max_value[i][ODD*2+JITTER_AVG_VA] = jitter_avg_max;
			}

			/* data compare for the avg */
			if ( ts->jitter_avg_spec < jitter_avg_max ) {
				TOUCH_E("the avg value %d is over spec\n", jitter_avg_max);
				++jitter_result;
			}

			if (jitter_result)
				ret += jitter_result;
		}
		max[JITTER_VA] = GET_MAX(jitter_max_value[i][EVEN*2], jitter_max_value[i][ODD*2],
				max[JITTER_VA]);
		max[JITTER_AVG_VA] = GET_MAX(jitter_max_value[i][EVEN*2+1],
				jitter_max_value[i][ODD*2+1], max[JITTER_AVG_VA]);
	}

	return ret;
}
static void tune_display(struct device *dev, char *tc_tune_code,
		int offset, int type)
{
	char log_buf[tc_tune_code_size] = {0,};
	int ret = 0;
	int i = 0;
	char temp[tc_tune_code_size] = {0,};

	switch (type) {
		case 1:
			ret = snprintf(log_buf, tc_tune_code_size,
					"GOFT tune_code_read : ");
			if ((tc_tune_code[offset] >> 4) == 1) {
				temp[offset] = tc_tune_code[offset] - (0x1 << 4);
				ret += snprintf(log_buf + ret,
						tc_tune_code_size - ret,
						"-%d  ", temp[offset]);
			} else {
				ret += snprintf(log_buf + ret,
						tc_tune_code_size - ret,
						" %d  ", tc_tune_code[offset]);
			}
			TOUCH_I("%s\n", log_buf);
			ret += snprintf(log_buf + ret, tc_tune_code_size - ret, "\n");
			write_file(dev, log_buf, TIME_INFO_SKIP);
			break;
		case 2:
			ret = snprintf(log_buf, tc_tune_code_size,
					"LOFT tune_code_read : ");
			for (i = 0; i < tc_total_ch_size; i++) {
				if ((tc_tune_code[offset+i]) >> 5 == 1) {
					temp[offset+i] =
						tc_tune_code[offset+i] - (0x1 << 5);
					ret += snprintf(log_buf + ret,
							tc_tune_code_size - ret,
							"-%d  ", temp[offset+i]);
				} else {
					ret += snprintf(log_buf + ret,
							tc_tune_code_size - ret,
							" %d  ",
							tc_tune_code[offset+i]);
				}
			}
			TOUCH_I("%s\n", log_buf);
			ret += snprintf(log_buf + ret, tc_tune_code_size - ret, "\n");
			write_file(dev, log_buf, TIME_INFO_SKIP);
			break;
	}
}

static void read_tune_code(struct device *dev, u8 type)
{
	u8 tune_code_read_buf[276] = {0,};

	u32 tune_code_offset;
	u32 offset;

	lg4895_reg_read(dev, PROD_os_result_tune_code_offset,
			(u8 *)&tune_code_offset, sizeof(u32));
	offset = (tune_code_offset >> 16) & 0xFFFF;

	lg4895_reg_write(dev, tc_tsp_test_data_offset,
			(u8 *)&offset,
			sizeof(u32));

	lg4895_reg_read(dev, tc_tsp_data_access_addr,
			(u8 *)&tune_code_read_buf[0], tc_tune_code_size);

	write_file(dev, "\n[Read Tune Code]\n", TIME_INFO_SKIP);
	switch (type) {
		case U0_M1_RAWDATA_TEST:
			tune_display(dev, tune_code_read_buf,
					TSP_TUNE_CODE_L_GOFT_OFFSET, 1);
			tune_display(dev, tune_code_read_buf,
					TSP_TUNE_CODE_R_GOFT_OFFSET, 1);
			tune_display(dev, tune_code_read_buf,
					TSP_TUNE_CODE_L_M1_OFT_OFFSET, 2);
			tune_display(dev, tune_code_read_buf,
					TSP_TUNE_CODE_R_M1_OFT_OFFSET, 2);
			break;
		case U3_M2_RAWDATA_TEST:
		case U0_M2_RAWDATA_TEST:
			tune_display(dev, tune_code_read_buf,
					TSP_TUNE_CODE_L_GOFT_OFFSET + 1, 1);
			tune_display(dev, tune_code_read_buf,
					TSP_TUNE_CODE_R_GOFT_OFFSET + 1, 1);
			tune_display(dev, tune_code_read_buf,
					TSP_TUNE_CODE_L_G1_OFT_OFFSET, 2);
			tune_display(dev, tune_code_read_buf,
					TSP_TUNE_CODE_L_G2_OFT_OFFSET, 2);
			tune_display(dev, tune_code_read_buf,
					TSP_TUNE_CODE_L_G3_OFT_OFFSET, 2);
			tune_display(dev, tune_code_read_buf,
					TSP_TUNE_CODE_R_G1_OFT_OFFSET, 2);
			tune_display(dev, tune_code_read_buf,
					TSP_TUNE_CODE_R_G2_OFT_OFFSET, 2);
			tune_display(dev, tune_code_read_buf,
					TSP_TUNE_CODE_R_G3_OFT_OFFSET, 2);
			break;
	}
	write_file(dev, "\n", TIME_INFO_SKIP);

}

static int prd_rawdata_test(struct device *dev, u8 type)
{
	char test_type[32] = {0, };
	int result = 0;
	int write_test_mode_result = 0;

	switch (type) {
		case U3_M2_RAWDATA_TEST:
			snprintf(test_type, sizeof(test_type),
					"[U3_M2_RAWDATA_TEST]");
			break;
		case U0_M1_RAWDATA_TEST:
			snprintf(test_type, sizeof(test_type),
					"[U0_M1_RAWDATA_TEST]");
			break;
		case U0_M2_RAWDATA_TEST:
			snprintf(test_type, sizeof(test_type),
					"[U0_M2_RAWDATA_TEST]");
			break;
		default:
			TOUCH_I("Test Type not defined\n");
			return 1;
	}

	/* Test Type Write */
	write_file(dev, test_type, TIME_INFO_SKIP);

	write_test_mode_result = write_test_mode(dev, type);
	if (write_test_mode_result == 0) {
		TOUCH_E("production test couldn't be done\n");
		return 2;
	}

	prd_read_rawdata(dev, type);

	result = prd_print_rawdata(dev, W_Buf, type);
	write_file(dev, W_Buf, TIME_INFO_SKIP);

	write_csv_file(dev, W_Buf);

	memset(W_Buf, 0, BUF_SIZE);
	/* rawdata compare result(pass : 0 fail : 1) */
	result = prd_compare_rawdata(dev, type);
	write_file(dev, W_Buf, TIME_INFO_SKIP);

	/* To Do - tune code result check */
	/*result = */read_tune_code(dev, type);

	return result;
}

static void ic_run_info_print(struct device *dev)
{
	unsigned char buffer[LOG_BUF_SIZE] = {0,};
	int ret = 0;
	u32 rdata[4] = {0};

	lg4895_reg_read(dev, info_lot_num, (u8 *)&rdata, sizeof(rdata));

	ret = snprintf(buffer, LOG_BUF_SIZE,
			"\n===== Production Info =====\n");
	ret += snprintf(buffer + ret, LOG_BUF_SIZE - ret,
			"lot : %d\n", rdata[0]);
	ret += snprintf(buffer + ret, LOG_BUF_SIZE - ret,
			"serial : 0x%X\n", rdata[1]);
	ret += snprintf(buffer + ret, LOG_BUF_SIZE - ret,
			"date : 0x%X 0x%X\n",
			rdata[2], rdata[3]);
	ret += snprintf(buffer + ret, LOG_BUF_SIZE - ret,
			"date : %04d.%02d.%02d %02d:%02d:%02d Site%d\n\n",
			rdata[2] & 0xFFFF, (rdata[2] >> 16 & 0xFF),
			(rdata[2] >> 24 & 0xFF), rdata[3] & 0xFF,
			(rdata[3] >> 8 & 0xFF),
			(rdata[3] >> 16 & 0xFF),
			(rdata[3] >> 24 & 0xFF));

	write_file(dev, buffer, TIME_INFO_SKIP);
}

static void firmware_version_log(struct device *dev)
{
	struct lg4895_data *d = to_lg4895_data(dev);
	int ret = 0;
	unsigned char buffer[LOG_BUF_SIZE] = {0,};
	int boot_mode = 0;

	boot_mode = touch_boot_mode_check(dev);
	if (boot_mode >= MINIOS_MFTS_FOLDER)
		ret = lg4895_ic_info(dev);

	ret = snprintf(buffer, LOG_BUF_SIZE,
			"======== Firmware Info ========\n");
	if (d->ic_info.version.build) {
		ret += snprintf(buffer + ret, LOG_BUF_SIZE - ret,
				"version : v%d.%02d.%d\n",
				d->ic_info.version.major, d->ic_info.version.minor, d->ic_info.version.build);
	} else {
		ret += snprintf(buffer + ret, LOG_BUF_SIZE - ret,
				"version : v%d.%02d\n",
				d->ic_info.version.major, d->ic_info.version.minor);
	}
	ret += snprintf(buffer + ret, LOG_BUF_SIZE - ret,
			"revision : %d\n", d->ic_info.revision);

	ret += snprintf(buffer + ret, LOG_BUF_SIZE - ret,
			"fpc : %d, cg : %d, wfr : %d\n",
			d->ic_info.fpc, d->ic_info.cg, d->ic_info.wfr);

	ret += snprintf(buffer + ret, LOG_BUF_SIZE - ret,
			"product id : %s\n", d->ic_info.product_id);

	write_file(dev, buffer, TIME_INFO_SKIP);
}

static int ic_exception_check(struct device *dev, char *buf)
{
#if 0
	struct lg4895_data *d = to_lg4895_data(dev);
	int boot_mode = 0;
	int ret = 0;

	boot_mode = touch_boot_mode_check(dev);
	/* MINIOS mode, MFTS mode check */
	if ((lge_get_boot_mode() > LGE_BOOT_MODE_NORMAL) || boot_mode > 0) {
		if (d->fw.revision < 2 ||
				d->fw.revision > 3 ||
				d->fw.version[0] != 1) {
			TOUCH_I("ic_revision : %d, fw_version : v%d.%02d\n",
					d->fw.revision,
					d->fw.version[0], d->fw.version[1]);

			ret = snprintf(buf, PAGE_SIZE,
					"========RESULT=======\n");

			if (d->fw.version[0] != 1) {
				ret += snprintf(buf + ret, PAGE_SIZE - ret,
						"version[v%d.%02d] : Fail\n",
						d->fw.version[0], d->fw.version[1]);
			} else {
				ret += snprintf(buf + ret, PAGE_SIZE - ret,
						"version[v%d.%02d] : Pass\n",
						d->fw.version[0], d->fw.version[1]);
			}

			if (d->fw.revision < 2 || d->fw.revision > 3) {
				ret += snprintf(buf + ret, PAGE_SIZE - ret,
						"revision[%d] : Fail\n",
						d->fw.revision);
			} else {
				ret += snprintf(buf + ret, PAGE_SIZE - ret,
						"revision[%d] : Pass\n",
						d->fw.revision);
			}
			write_file(dev, buf, TIME_INFO_SKIP);

			ret += snprintf(buf + ret, PAGE_SIZE - ret,
					"Raw data : Fail\n");
			ret += snprintf(buf + ret, PAGE_SIZE - ret,
					"Channel Status : Fail\n");
			write_file(dev, "", TIME_INFO_WRITE);
		}
	}
	return ret;
#endif
	return 0;
}
static ssize_t store_block_prd(struct device *dev,
		const char *buf, size_t count)
{
	u8 value = 0;
	u8 const zero = '0';

	if (value == UNBLOCKED || value == zero)
		value = 0x00;
	else
		value = 0x01;

	atomic_set(&block_prd, value);

	TOUCH_I("%s : %s\n", __func__,
			value ? "BLOCKED" : "UNBLOCKED");

	return count;
}

static ssize_t show_sd(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct lg4895_data *d = to_lg4895_data(dev);
	int openshort_ret = 0;
	int jitter_ret = 0;
	int rawdata_ret = 0;
	u32 test_mode_ctrl_cmd = 0;
	u32 test_mode_ctrl_cmd_chk = -1;
	int ret = 0;
	int i = 0;
	int max[2] = {0, };
	int const boot_mode = atomic_read(&ts->state.mfts);
	int const fpcb_version_otp = lge_get_lg4895_revision();
	int fpcb_ret = 0;
	int revision_lot_ret = 0;
	//int find_max=0, find_time=0;

	TOUCH_I("%s boot_mode : %d\n", TS_MODULE, boot_mode);

	/* file create , time log */
	write_file(dev, "\nShow_sd Test Start", TIME_INFO_SKIP);
	write_file(dev, "\n", TIME_INFO_WRITE);
	TOUCH_I("Show_sd Test Start\n");

	/* LCD mode check */
	if (d->lcd_mode != LCD_MODE_U3) {
		ret = snprintf(buf + ret, PAGE_SIZE - ret,
				"LCD mode is not U3. Test Result : Fail\n");
		TOUCH_I("LCD mode is not U3. Test Result : Fail\n");
		write_file(dev, buf, TIME_INFO_SKIP);
		write_file(dev, "Show_sd Test End\n", TIME_INFO_WRITE);
		return ret;
	}

	/* ic rev check - MINIOS mode, MFTS mode check */
	ret = ic_exception_check(dev, buf);
	if (ret > 0)
		return ret;

	firmware_version_log(dev);
	ic_run_info_print(dev);

	mutex_lock(&ts->lock);

	test_mode_ctrl_cmd = cmd_test_enter;
	lg4895_reg_write(dev, tc_test_mode_ctl,
			(u32 *)&test_mode_ctrl_cmd,
			sizeof(u32));
	TOUCH_I("write tc_test_mode_ctl= %x\n", cmd_test_enter);
	touch_msleep(30);
	lg4895_reg_read(dev, tc_test_mode_ctl,
			(u32 *)&test_mode_ctrl_cmd_chk,
			sizeof(u32));
	TOUCH_I("read tc_test_mode_ctl= %x\n", test_mode_ctrl_cmd_chk);
	touch_interrupt_control(ts->dev, INTERRUPT_DISABLE);

	lg4895_tc_driving(dev, LCD_MODE_STOP);

	/* Revision check for h/w revision for fpcb */
	/* 1,2 : not re-work, after 3 : re-worked*/

	if ( fpcb_version_otp > 2 && d->ic_info.fpc == 0x2 ) {
		TOUCH_I("The sample is fixed by fpcb[%d][%d]\n", fpcb_version_otp, d->ic_info.fpc);
		write_file(dev, "The sample is fixed by fpcb\n\n",
			TIME_INFO_SKIP);
	} else {
		TOUCH_I("The sample is not fixed by fpcb[%d][%d]\n", fpcb_version_otp, d->ic_info.fpc);
		write_file(dev, "The sample is not fixed by fpcb\n\n",
			TIME_INFO_SKIP);
		fpcb_ret = 1;
	}

	/* revison check for h/w revision for siw */
	/* OxFF : not PT sample */
	if ( d->ic_info.revision == 0xFF || ((d->ic_info.lot[2] & 0xFFFF) == 0xFFF)) {
		TOUCH_I("The sample do not production test[%X][%X]\n", d->ic_info.revision, (d->ic_info.lot[2]&0xFFFF));
		write_file(dev, "The sample do not production test\n\n",
			TIME_INFO_SKIP);
		revision_lot_ret = 1;
	}

	if ( fpcb_ret || revision_lot_ret )
		goto finish;
	/*
	   DOZE1_M2_RAWDATA_TEST
	   rawdata - pass : 0, fail : 1
	   rawdata tunecode - pass : 0, fail : 2
	 */
	rawdata_ret = prd_rawdata_test(dev, U3_M2_RAWDATA_TEST);

	/*
	   DOZE1_M2_JITTER_TEST
	   jitterdata - pass : 0, fail : 1
	   jitterdata - no_global_tuen : 1, no_local_tuen : 2
	 */

	jitter_ret = prd_jitter_test(dev, JITTER_TEST, max);

	/*
	   OPEN_SHORT_ALL_TEST
	   open - pass : 0, fail : 1
	   short - pass : 0, fail : 2
	 */
	openshort_ret = prd_open_short_test(dev);

	ret = snprintf(buf, PAGE_SIZE,
			"\n========RESULT=======\n");
	TOUCH_I("========RESULT=======\n");

	if ( rawdata_ret || jitter_ret ) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Raw Data : Fail[%d][%d]\n", rawdata_ret, jitter_ret?1:0);
		TOUCH_I("Raw Data : Fail[%d][%d]\n", rawdata_ret, jitter_ret?1:0);
	} else {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Raw Data : Pass\n");
		TOUCH_I("Raw Data : Pass\n");
	}

	if (openshort_ret == 0) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Channel Status : Pass\n");
		TOUCH_I("Channel Status : Pass\n");
	} else {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Channel Status : Fail (%d/%d)\n",
				((openshort_ret & 0x1) == 0x1) ? 0 : 1,
				((openshort_ret & 0x2) == 0x2) ? 0 : 1);
		TOUCH_I("Channel Status : Fail (%d/%d)\n",
				((openshort_ret & 0x1) == 0x1) ? 0 : 1,
				((openshort_ret & 0x2) == 0x2) ? 0 : 1);
	}
	/* Jitter Test result */
	ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"BLU Jitter Spec : %d\n", ts->jitter_spec);
	TOUCH_I("BLU Jitter Spec : %d\n", ts->jitter_spec);
	ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"BLU Jitter Avg Spec : %d\n", ts->jitter_avg_spec);
	TOUCH_I("BLU Jitter Avg Spec : %d\n", ts->jitter_avg_spec);
	if (0 == jitter_ret) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Jitter Result : Pass (%3d)(%3d)\n", *max, *(max+JITTER_AVG_VA));
		TOUCH_I("Jitter Result : Pass (%3d)(%3d)\n", *max, *(max+JITTER_AVG_VA));
	} else {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Jitter Result : Fail (%3d)(%3d)\n", *max, *(max+JITTER_AVG_VA));
		TOUCH_I("Jitter Result : Fail (%3d)(%3d)\n", *max, *(max+JITTER_AVG_VA));
	}
	for ( i=0; i<JITTER_TEST_CNT; i++) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"%2dtrial : %d\n", i+1,
				GET_MAX(0, jitter_max_value[i][EVEN*2], jitter_max_value[i][ODD*2]));
		TOUCH_I("%2dtrial : [E/m]:%3d [E/av]:%3d [O/m]:%3d [O/av]%3d\n", i+1,
				jitter_max_value[i][EVEN*2], jitter_max_value[i][EVEN*2+JITTER_AVG_VA],
				jitter_max_value[i][ODD*2], jitter_max_value[i][ODD*2+JITTER_AVG_VA]);
	}
#if 0
	/* rank funtion for find max times */
	find_max = find_time = 0;
	for ( i=0; i<JITTER_TEST_CNT; i++) {
		if ( find_max < GET_MAX(jitter_max_value[i][EVEN*2], jitter_max_value[i][ODD*2], find_max)) {
			find_max = GET_MAX(jitter_max_value[i][EVEN*2], jitter_max_value[i][ODD*2], find_max);
			find_time = i+1;
		}
	}
	ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"BLU find max times : %d\n", find_time);
	TOUCH_I("BLU find max times : %d\n", find_time);
#endif //1
finish:
	if ( fpcb_ret || revision_lot_ret ) {
		ret = snprintf(buf, PAGE_SIZE,
			"\n========RESULT=======\n");
		TOUCH_I("========RESULT=======\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Raw Data : Fail[%s%s]\n",
				fpcb_ret?"not re-worked":"",
				revision_lot_ret?", PT Failed":"");
		TOUCH_I("Raw Data : Fail[%s%s]\n",
			fpcb_ret?"not re-worked":"",
			revision_lot_ret?", PT Failed":"");
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Channel Status : N/A\n");
		TOUCH_I("Channel Status : N/A\n");
	}

	TOUCH_I("=====================\n");
	write_file(dev, buf, TIME_INFO_SKIP);

	ts->driver->power(dev, POWER_OFF);
	ts->driver->power(dev, POWER_ON);
	touch_msleep(90);
	ts->driver->init(dev);
	touch_interrupt_control(ts->dev, INTERRUPT_ENABLE);
	test_mode_ctrl_cmd = cmd_test_exit;
	lg4895_reg_write(dev, tc_test_mode_ctl,
			(u32 *)&test_mode_ctrl_cmd,
			sizeof(u32));
	touch_msleep(30);
	TOUCH_I("write tc_test_mode_ctl= %x\n", cmd_test_exit);
	lg4895_reg_read(dev, tc_test_mode_ctl,
			(u32 *)&test_mode_ctrl_cmd_chk,
			sizeof(u32));
	TOUCH_I("read tc_test_mode_ctl= %x\n", test_mode_ctrl_cmd_chk);
	mutex_unlock(&ts->lock);

	write_file(dev, "Show_sd Test End\n", TIME_INFO_WRITE);
	log_file_size_check(dev);
	TOUCH_I("Show_sd Test End\n");
	return ret;
}

static int stop_firmware(struct device *dev, u32 wdata)
{
	u32 read_val;
	u32 check_data=0;
	int try_cnt=0;
	int ret = 0;

	/* STOP F/W to check */
	lg4895_reg_write(dev, ADDR_CMD_REG_SIC_IMAGECTRL_TYPE, &wdata, sizeof(u32));
	lg4895_reg_read(dev, ADDR_CMD_REG_SIC_IMAGECTRL_TYPE, &check_data,
			sizeof(u32));
	lg4895_reg_read(dev, ADDR_CMD_REG_SIC_IMAGECTRL_TYPE, &check_data,
			sizeof(u32));

	try_cnt = 1000;
	do
	{
		--try_cnt;
		if (try_cnt == 0) {
			TOUCH_E("[ERR]get_data->try_cnt == 0\n");
			ret = 1;
			goto error;
		}
		lg4895_reg_read(dev, ADDR_CMD_REG_SIC_GETTER_READYSTATUS,
				&read_val, sizeof(u32));
		TOUCH_I("read_val = [%x] , RS_IMAGE = [%x]\n",read_val,
				(u32)RS_IMAGE);
		touch_msleep(10);
	} while(read_val != (u32)RS_IMAGE);

error:
	return ret;
}

static void start_firmware(struct device *dev)
{
	u32 const cmd = IT_NONE;
	u32 check_data = 0;

	/* Release F/W to operate */
	lg4895_reg_write(dev,ADDR_CMD_REG_SIC_IMAGECTRL_TYPE, (void *)&cmd,
			sizeof(u32));
	lg4895_reg_read(dev, ADDR_CMD_REG_SIC_IMAGECTRL_TYPE, &check_data,
			sizeof(u32));
	lg4895_reg_read(dev, ADDR_CMD_REG_SIC_IMAGECTRL_TYPE, &check_data,
			sizeof(u32));
	TOUCH_I("check_data : %x\n", check_data);
}

static ssize_t get_data(struct device *dev, int16_t *buf, u32 wdata)
{
	int i;
	int ret = 0;
	int r, c = 0;

	TOUCH_I("======== get data ========\n");
	TOUCH_I("wdata = %d\n", wdata);
	if( CMD_DELTADATA == wdata ) {
		int16_t *delta_buf = NULL;
		delta_buf = kzalloc(sizeof(int16_t) *
				((COL_SIZE+2) * (ROW_SIZE+2)), GFP_KERNEL);
		if ( stop_firmware(dev, IT_DELTA_IMAGE) )
			goto error_delta;
		if (read_memory(dev, SERIAL_DATA_OFFSET,
					DATA_SRAM_DELTA_BASE_ADDR, sizeof(u32),
					DATA_I2CBASE_ADDR, (u8 *)delta_buf,
					(ROW_SIZE+2)*(COL_SIZE+2)*2) < 0) {
			TOUCH_E("read memory error!!\n");
			ret = 1;
			goto error_delta;
		} else {
			for ( i=0; i<COL_SIZE*ROW_SIZE; i++) {
				r = i/COL_SIZE;
				c = i%COL_SIZE;
				buf[i] =
					delta_buf[(r+1)*(COL_SIZE+2)+(c+1)];
			}
			TOUCH_I("read_memory ok\n");
		}
error_delta:
		start_firmware(dev);
		if ( delta_buf != NULL )
			kfree(delta_buf);
		return ret;
	} else if( CMD_LABELDATA == wdata ) { /* label */
		u8 *label_buf = NULL;
		label_buf = kzalloc(sizeof(u8) * ((COL_SIZE+2) * (ROW_SIZE+2)), GFP_KERNEL);
		if ( stop_firmware(dev, IT_LABEL_IMAGE) )
			goto error_label;
		if (read_memory(dev, SERIAL_DATA_OFFSET, DATA_LABLE_BASE_ADDR, sizeof(u32),
					DATA_I2CBASE_ADDR, (u8 *)label_buf,
					sizeof(u8)*(ROW_SIZE+2)*(COL_SIZE+2)) < 0) {
			TOUCH_E("read memory error!!\n");
			ret = 1;
			goto error_label;
		} else {
			for ( i=0; i<COL_SIZE*ROW_SIZE; i++) {
				r = i/COL_SIZE;
				c = i%COL_SIZE;
				buf[i] = (int16_t)label_buf[(r+1)*(COL_SIZE+2)+(c+1)];
			}
			TOUCH_I("read_memory ok\n");
		}
error_label:
		start_firmware(dev);
		if ( label_buf != NULL )
			kfree(label_buf);
		return ret;
	} else if( CMD_BASEDATA == wdata ) { /* base */
		if ( stop_firmware(dev, IT_BASELINE_IMAGE) )
			goto error_base;
		if (read_memory(dev, SERIAL_DATA_OFFSET, DATA_BASE_ADDR, sizeof(u32),
					DATA_I2CBASE_ADDR, (u8 *)buf, sizeof(u8)*(ROW_SIZE)*(COL_SIZE)*2) < 0) {
			TOUCH_E("read memory error!!\n");
			ret = 1;
			goto error_base;
		}
error_base:
		start_firmware(dev);
		return ret;
	} else if( CMD_RAWDATA == wdata ||
			CMD_ALGORITHM == wdata ) { /* with algorithm */
		if ( stop_firmware(dev, IT_ALGORITHM_RAW_IMAGE) )
			goto error_raw_algorithm;
		if (read_memory(dev, SERIAL_DATA_OFFSET, DATA_RAWSIW_BASE_ADDR, sizeof(u32),
					DATA_I2CBASE_ADDR, (u8 *)buf, sizeof(u8)*(ROW_SIZE)*(COL_SIZE)*2) < 0) {
			TOUCH_E("read memory error!!\n");
			ret = 1;
			goto error_raw_algorithm;
		}
error_raw_algorithm:
		start_firmware(dev);
		return ret;
	}
	else if( CMD_TCMDATA == wdata ){
		u8 const tcm_col = COL_SIZE/2;
		u16 const read_addr = 0x303;
		u8 tc_mem_sel_value = 0;
		u8 cmd = 0;
		if (lg4895_reg_write(dev, 0x007C, &cmd, sizeof(u32)) < 0)
			TOUCH_E("TCM_OFFSET addr write fail\n");
		else{
			if (read_memory(dev, 0x0457, (u32)tc_mem_sel_value, sizeof(u32),
						read_addr, (u8 *)buf, sizeof(u32)*tcm_col*(ROW_SIZE)) < 0) {
				TOUCH_E("read memory error!!\n");
				ret = 1;
				goto error_tcm;
			}
		}
error_tcm:
		return ret;
	}
	return 1;
}


static ssize_t show_fdata(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct lg4895_data *d = to_lg4895_data(dev);
	int ret = 0;
	int ret2 = 0;
	u8 type = U3_M2_RAWDATA_TEST;

	/* LCD off */
	if (d->lcd_mode != LCD_MODE_U3) {
		ret = snprintf(buf + ret, PAGE_SIZE - ret,
				"LCD Off. Test Result : Fail\n");
		return ret;
	}

	mutex_lock(&ts->lock);
	touch_interrupt_control(ts->dev, INTERRUPT_DISABLE);

	ret2 = write_test_mode(dev, type);
	if (ret2 == 0) {
		TOUCH_E("write_test_mode fail\n");
		ts->driver->power(dev, POWER_OFF);
		ts->driver->power(dev, POWER_ON);
		touch_msleep(ts->caps.hw_reset_delay);
		ts->driver->init(dev);
		touch_interrupt_control(ts->dev, INTERRUPT_ENABLE);
		return ret;
	}

	prd_read_rawdata(dev, type);
	ret = prd_print_rawdata(dev, buf, type);

	ts->driver->power(dev, POWER_OFF);
	ts->driver->power(dev, POWER_ON);
	touch_msleep(90);
	ts->driver->init(dev);
	touch_interrupt_control(ts->dev, INTERRUPT_ENABLE);
	mutex_unlock(&ts->lock);

	return ret;
}

static int print_log(struct device *dev, char *buf, int16_t *data,
		ssize_t (*f)(struct device*, int16_t*, u32), u8 cmd)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int i,j = 0;
	int ret = 0;

	mutex_lock(&ts->lock);
	if ( f(dev, data, cmd) ) {
		TOUCH_E("Test fail (Check if LCD is OFF)\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Test fail (Check if LCD is OFF)\n");
		mutex_unlock(&ts->lock);
		return ret;
	}
	for (i = 0 ; i < ROW_SIZE; i++) {
		char log_buf[LOG_BUF_SIZE] = {0,};
		int log_ret = 0;
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "[%2d] ", i);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret, "[%2d]  ", i);
		for (j = 0 ; j < COL_SIZE ; j++) {
			ret += snprintf(buf + ret, PAGE_SIZE - ret,
					"%5d ", data[i * COL_SIZE + j]);
			log_ret += snprintf(log_buf + log_ret,
					LOG_BUF_SIZE - log_ret,
					"%5d ", data[i * COL_SIZE + j]);
		}
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "\n");
		TOUCH_I("%s\n", log_buf);
	}
	mutex_unlock(&ts->lock);
	return ret;
}
static ssize_t show_rawdata(struct device *dev, char *buf)
{
	int ret = 0;
	void *data = NULL;

	data = kzalloc(sizeof(u16) * (COL_SIZE*ROW_SIZE), GFP_KERNEL);

	if (data == NULL) {
		TOUCH_E("mem_error\n");
		goto error;
	}
	TOUCH_I("======== raw data ========\n");
	ret = print_log(dev, buf, (int16_t*)data, get_data, CMD_RAWDATA);
error:
	if ( NULL != data )
		kfree(data);
	return ret;
}

static ssize_t show_delta(struct device *dev, char *buf)
{
	int ret = 0;
	void *data = NULL;

	data = kzalloc(sizeof(int16_t) * ((COL_SIZE+2) * (ROW_SIZE+2)), GFP_KERNEL);
	if ( NULL == data ) {
		TOUCH_E("delta mem_error\n");
		goto error;
	}

	TOUCH_I("======== delta data ========\n");

	ret = print_log(dev, buf, (int16_t*)data, get_data, CMD_DELTADATA);


error:
	if ( NULL != data )
		kfree(data);

	return ret;
}
static ssize_t show_labeldata(struct device *dev, char *buf)
{
	int ret = 0;
	void *data = NULL;

	data = kzalloc(sizeof(u16) * ((COL_SIZE+2) * (ROW_SIZE+2)), GFP_KERNEL);

	if ( NULL == data ) {
		TOUCH_E("mem_error\n");
		goto error;
	}

	TOUCH_I("======== label data ========\n");

	ret = print_log(dev, buf, (int16_t*)data, get_data, CMD_LABELDATA);
error:
	if ( NULL != data )
		kfree(data);

	return ret;
}

static ssize_t show_baseline(struct device *dev, char *buf)
{
	int ret = 0;
	void *data = NULL;

	data = kzalloc(sizeof(u16) * ((COL_SIZE) * (ROW_SIZE)), GFP_KERNEL);

	if ( NULL == data ) {
		TOUCH_E("mem_error\n");
		goto error;
	}

	TOUCH_I("======== base data ========\n");

	ret= print_log(dev, buf, (int16_t*)data, get_data, CMD_BASEDATA);
error:
	if ( NULL != data )
		kfree(data);

	return ret;
}

static ssize_t show_algorithmdata(struct device *dev, char *buf)
{
	int ret = 0;
	void *data = NULL;

	data = kzalloc(sizeof(u16) * ((COL_SIZE) * (ROW_SIZE)), GFP_KERNEL);

	if ( NULL == data ) {
		TOUCH_E("mem_error\n");
		goto error;
	}

	TOUCH_I("======== algorithm data ========\n");

	ret = print_log(dev, buf, (int16_t*)data, get_data, CMD_ALGORITHM);
error:
	if ( NULL != data )
		kfree(data);

	return ret;
}
static ssize_t show_tcmdata(struct device *dev, char *buf)
{
	int ret = 0;
	void *data = NULL;

	data = kzalloc(sizeof(u32) * ((COL_SIZE/2)*ROW_SIZE), GFP_KERNEL);

	if ( NULL == data ) {
		TOUCH_E("mem_error\n");
		goto error;
	}


	TOUCH_I("======== tcm data ========\n");
	ret = print_log(dev, buf, (int16_t*)data, get_data, CMD_TCMDATA);

	if ( NULL != data )
		kfree(data);
error:
	return ret;
}
static ssize_t show_lpwg_sd(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct lg4895_data *d = to_lg4895_data(dev);
	int m1_rawdata_ret = 0;
	int m2_rawdata_ret = 0;
	int ret = 0;
	u32 test_mode_ctrl_cmd = 0;
	int try_cnt = 3;
	u32 dummy0 = 0;
	int do_retry = 0;

	/* file create , time log */
	write_file(dev, "\nShow_lpwg_sd Test Start", TIME_INFO_SKIP);
	write_file(dev, "\n", TIME_INFO_WRITE);
	TOUCH_I("Show_lpwg_sd Test Start\n");

	/* LCD mode check */
	if (d->lcd_mode != LCD_MODE_U0) {
		ret = snprintf(buf + ret, PAGE_SIZE - ret,
				"LCD mode is not U0. Test Result : Fail\n");
		write_file(dev, buf, TIME_INFO_SKIP);
		write_file(dev, "Show_sd Test End\n", TIME_INFO_WRITE);
		TOUCH_I("LCD mode is not U0. Test Result : Fail\n");
		return ret;
	}

	firmware_version_log(dev);
	ic_run_info_print(dev);

	mutex_lock(&ts->lock);
	do {
		do_retry = 0;
		test_mode_ctrl_cmd = cmd_test_enter;
		lg4895_reg_write(dev, tc_test_mode_ctl,
				(u32 *)&test_mode_ctrl_cmd,
				sizeof(u32));
		touch_msleep(30);
		TOUCH_I("write tc_test_mode_ctl= %x\n", cmd_test_enter);
		touch_interrupt_control(ts->dev, INTERRUPT_DISABLE);

		lg4895_tc_driving(dev, LCD_MODE_STOP);

		m2_rawdata_ret = prd_rawdata_test(dev, U0_M2_RAWDATA_TEST);
		m1_rawdata_ret = prd_rawdata_test(dev, U0_M1_RAWDATA_TEST);

		if (m2_rawdata_ret == 2 || m1_rawdata_ret == 2)
			do_retry = 1;

		lg4895_reg_read(dev, tc_status, (u32 *)&dummy0, sizeof(u32));
		TOUCH_I("tc_status= %x\n", dummy0);

		ret = snprintf(buf, PAGE_SIZE, "========RESULT=======\n");
		TOUCH_I("========RESULT=======\n");

		if (!m1_rawdata_ret && !m2_rawdata_ret) {
			ret += snprintf(buf + ret, PAGE_SIZE - ret,
					"LPWG RawData : %s\n", "Pass");
			TOUCH_I("LPWG RawData : %s\n", "Pass");
		} else {
			ret += snprintf(buf + ret, PAGE_SIZE - ret,
					"LPWG RawData : %s (%d/%d)\n", "Fail",
					m1_rawdata_ret ? 0 : 1, m2_rawdata_ret ? 0 : 1);
			TOUCH_I("LPWG RawData : %s (%d/%d)\n", "Fail",
					m1_rawdata_ret ? 0 : 1, m2_rawdata_ret ? 0 : 1);
		}
		TOUCH_I("=====================\n");

		write_file(dev, buf, TIME_INFO_SKIP);

		ts->driver->power(dev, POWER_OFF);
		ts->driver->power(dev, POWER_ON);
		touch_msleep(90);
		ts->driver->init(dev);
		touch_interrupt_control(ts->dev, INTERRUPT_ENABLE);
		test_mode_ctrl_cmd = cmd_test_exit;
		lg4895_reg_write(dev, tc_test_mode_ctl,
				(u32 *)&test_mode_ctrl_cmd,
				sizeof(u32));
		touch_msleep(30);
		TOUCH_I("write tc_test_mode_ctl= %x\n", cmd_test_exit);

		if (--try_cnt == 0) {
			TOUCH_E("lpwg_sd->try_cnt == 0\n");
			break;
		}
	} while(!(dummy0 & (1 << 29)) || (do_retry == 1));

	mutex_unlock(&ts->lock);

	write_file(dev, "Show_lpwg_sd Test End\n", TIME_INFO_WRITE);
	log_file_size_check(dev);
	TOUCH_I("Show_lpwg_sd Test End\n");

	return ret;
}

static TOUCH_ATTR(sd, show_sd, NULL);
static TOUCH_ATTR(delta, show_delta, NULL);
static TOUCH_ATTR(fdata, show_fdata, NULL);
static TOUCH_ATTR(rawdata, show_rawdata, NULL);
static TOUCH_ATTR(lpwg_sd, show_lpwg_sd, NULL);
static TOUCH_ATTR(label, show_labeldata, NULL);
static TOUCH_ATTR(baseline, show_baseline, NULL);
static TOUCH_ATTR(raw_siw, show_algorithmdata, NULL);
static TOUCH_ATTR(raw_tcm, show_tcmdata, NULL);
static TOUCH_ATTR(block_prd, NULL, store_block_prd);

static struct attribute *prd_attribute_list[] = {
	&touch_attr_sd.attr,
	&touch_attr_delta.attr,
	&touch_attr_fdata.attr,
	&touch_attr_rawdata.attr,
	&touch_attr_lpwg_sd.attr,
	&touch_attr_label.attr,
	&touch_attr_baseline.attr,
	&touch_attr_raw_siw.attr,
	&touch_attr_raw_tcm.attr,
	&touch_attr_block_prd.attr,
	NULL,
};

static const struct attribute_group prd_attribute_group = {
	.attrs = prd_attribute_list,
};

int lg4895_prd_register_sysfs(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;

	TOUCH_TRACE();

	ret = sysfs_create_group(&ts->kobj, &prd_attribute_group);

	if (ret < 0) {
		TOUCH_E("failed to create sysfs\n");
		return ret;
	}
	atomic_set(&block_prd, BLOCKED);

	return ret;
}
