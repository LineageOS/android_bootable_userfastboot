/*
 * Copyright (C) 2011 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <getopt.h>
#include <unistd.h>
#include "manage_device.h"

#define BACKUP_LOC 0xE0
#define OSIP_PREAMBLE 0x20
#define OSIP_SIG 0x24534f24	/* $OS$ */

#define FILE_EXT ".bin"
#define ANDROID_OS 0
#define POS 1
#define COS 3

#define OSII_TOTAL 7
#define DUMP_OSIP 1
#define NOT_DUMP 0
#define R_BCK 1
#define R_START 0

#ifdef __ANDROID__
#define MMC_DEV_POS "/dev/block/mmcblk0"
#else
#define MMC_DEV_POS "/dev/mmcblk0"
#endif

#define MMC_PAGES_PER_BLOCK 1
#define MMC_PAGE_SIZE "/sys/devices/pci0000:00/0000:00:01.0/mmc_host/mmc0/mmc0:0001/erase_size"
#define KBYTES 1024
#define STITCHED_IMAGE_PAGE_SIZE 512
#define STITCHED_IMAGE_BLOCK_SIZE 512

static int get_page_size(void)
{
	int mmc_page_size;
	char buf[16];
	int fd;

	memset((void *)buf, 0, sizeof(buf));
	fd = open(MMC_PAGE_SIZE, O_RDONLY);
	if (fd < 0) {
		printf("open mmc page size failed\n");
		return -1;
	}
	if (read(fd, buf, 16) < 0) {
		printf("read of mmc page size failed\n");
		close(fd);
		return -1;
	}
	printf("page size %s\n", buf);
	if (sscanf(buf, "%d", &mmc_page_size) != 1) {
		printf("sscanf of mmc page size failed\n");
		close(fd);
		return -1;
	}
	close(fd);

	return mmc_page_size / KBYTES;
}

static int get_block_size(void)
{
	int mmc_page_size;

	mmc_page_size = get_page_size();

	return mmc_page_size * MMC_PAGES_PER_BLOCK;
}

int write_stitch_image(void *data, size_t size, int update_number)
{
	struct OSIP_header osip;
	struct OSIP_header bck_osip;
	struct OSII *osii;
	void *blob;
	uint32 lba, temp_size_bytes;
	int block_size = get_block_size();
	int page_size = get_page_size();
	int carry, fd, num_pages, temp_offset;

	printf("now into write_stitch_image\n");
	if (block_size < 0) {
		printf("block size wrong\n");
		return -1;
	}
	if (crack_stitched_image(data, &osii, &blob) < 0) {
		printf("crack_stitched_image fails\n");
		return -1;
	}
	if ((osii->size_of_os_image * STITCHED_IMAGE_PAGE_SIZE) !=
	    size - STITCHED_IMAGE_BLOCK_SIZE) {
		printf("data format is not correct! \n");
		return -1;
	}
	if (read_OSIP_loc(&osip, R_START, NOT_DUMP) < 0) {
		printf("read_OSIP fails\n");
		return -1;
	}

	osip.num_images = 1;
	osii->logical_start_block =
	    osip.desc[update_number].logical_start_block;
/*	osii->logical_start_block =
	    MAX(osip.desc[PAYLOAD_OSII_REC].logical_start_block,
		osip.desc[POS_OSII_REC].logical_start_block);
	osip.desc[POS_OSII_REC].logical_start_block =
	    MIN(osip.desc[PAYLOAD_OSII_REC].logical_start_block,
		osip.desc[POS_OSII_REC].logical_start_block);*/

	osii->size_of_os_image =
	    (osii->size_of_os_image * STITCHED_IMAGE_PAGE_SIZE) / page_size + 1;

	memcpy(&(osip.desc[update_number]), osii, sizeof(struct OSII));
	printf("os_rev_major=0x%x,os_rev_minor=0x%x,ddr_load_address=0x%x\n",
	       osii->os_rev_major, osii->os_rev_minor, osii->ddr_load_address);
	printf("entry_point=0x%x,sizeof_osimage=0x%x,attribute=0x%x\n",
	       osii->entery_point, osii->size_of_os_image, osii->attribute);
	if (update_number == POS)
		write_OSIP(&osip);

	read_OSIP_loc(&bck_osip, R_BCK ,NOT_DUMP);
	if (bck_osip.sig != OSIP_SIG) {
		printf
		    ("There is no backup OSIP when flash image!\n");
		write_OSIP(&osip);
	}
	else
		write_OSII_entry(osii, update_number, R_BCK);

	fd = open(MMC_DEV_POS, O_RDWR);
	if (fd < 0) {
		printf("fail open %s\n", MMC_DEV_POS);
		return -1;
	}
	lseek(fd, osii->logical_start_block * block_size, SEEK_SET);
	if (write(fd, blob, size - STITCHED_IMAGE_BLOCK_SIZE) <
	    size - STITCHED_IMAGE_BLOCK_SIZE) {
		printf("fail write of blob\n");
		close(fd);
		return -1;
	}
	fsync(fd);
	close(fd);

	return 0;
}

int flash_stitch_image(char *argv, int update_number)
{
	char *fwBinFile = NULL;
	char *fwFileData = NULL;
	char *tempPtr;
	FILE *fwp = NULL;
	struct stat sb;

	printf("run into flash_stitch_image\n");
	/*Checks to see if the file is terminated by *.bin */
	if ((tempPtr = strrchr(argv, '.')) == NULL) {
		fprintf(stderr,
			"Invalid inputs, correct usage is --image FW.bin\n");
		exit(1);
	}

	if (strncmp(tempPtr, FILE_EXT, strlen(FILE_EXT))) {
		fprintf(stderr,
			"File doesnt have *.bin extn,correct usage is --image FW.bin\n");
		exit(1);
	}

	fwBinFile = argv;

	fprintf(stderr, "fw file is %s\n", fwBinFile);

	if (!(fwp = fopen(fwBinFile, "rb"))) {
		perror("fopen error:Unable to open file\n");
		exit(1);
	}

	if (fstat(fileno(fwp), &sb) == -1) {
		perror("fstat error\n");
		fclose(fwp);
		exit(1);
	}

	if ((fwFileData = calloc(sb.st_size, 1)) == NULL) {
		fclose(fwp);
		exit(1);
	}

	if (fread(fwFileData, 1, sb.st_size, fwp) < sb.st_size) {
		perror("unable to fread fw bin file into buffer\n");
		free(fwFileData);
		fclose(fwp);
		exit(1);
	}

	return write_stitch_image(fwFileData, sb.st_size, update_number);

}

void display_usage(void)
{
	printf("Update_osip Tool USAGE:\n");
	printf("--check     	| Print current OSIP header\n");
	printf("--backup    	| Backup all valid OSII in current OSIP\n");
	printf
	    ("--invalidate <attribute>   | Invalidate specified OSII with <attribute> ,used with --backup!\n");
	printf
	    ("--restore   	| Restore all valid OSII in backup region to current OSIP\n");
	printf
	    ("--update <OSII_Number> --image <xxx.bin>  | Update the specified OSII entry and flash xxx.bin\n");
	printf
	    ("--update <OSII_Number> -m xx -n xx -l xx -a xx -s xx -e xx | Update specified OSII with parameters following\n");
	exit(EXIT_FAILURE);
}

int read_OSIP_loc(struct OSIP_header *osip, int location, int dump)
{
	int fd;

	if (!location)
		printf("**************into read_OSIP*********************\n");
	else
		printf
		    ("==============into read_OSIP from backup location====\n");
	memset((void *)osip, 0, sizeof(*osip));
	fd = open(MMC_DEV_POS, O_RDONLY);
	if (fd < 0)
		return -1;

	if (location)
		lseek(fd, BACKUP_LOC, SEEK_SET);
	else
		lseek(fd, 0, SEEK_SET);

	if (read(fd, (void *)osip, sizeof(*osip)) < 0) {
		printf("read of osip failed\n");
		close(fd);
		return -1;
	}
	close(fd);

	if (osip->sig != OSIP_SIG) {
		printf
		    ("Invalid OSIP header detected!\n++++++++++++++++++!\n");
	}

	if ((dump) &&(osip->sig == OSIP_SIG)) {
		dump_osip_header(osip);
		if (location)
			printf("read of osip  from BACKUP_LOC works\n");
		else
			printf("read of osip works\n");
	}

	return 1;
}

int backup_handle(struct OSIP_header *osip)
{
	int fd;
	struct OSIP_header bck_osip;

	fd = open(MMC_DEV_POS, O_RDWR);
	if (fd < 0) {
		printf("fail to open %s\n", MMC_DEV_POS);
		return -1;
	}
	lseek(fd, BACKUP_LOC, SEEK_SET);
	if (write(fd, (void *)osip, sizeof(*osip)) < 0) {
		close(fd);
		printf("fail writing osip\n");
		return -1;
	}
	fsync(fd);
	close(fd);

	read_OSIP_loc(&bck_osip, R_BCK, DUMP_OSIP);
	/*TODO: look for a way to flush nand */
	printf("write of osip to BACKUP_LOC addr worked\n");
	return 1;
}

int restore_handle(void)
{				/*don't restore all OSII,check entry if valid */
	struct OSIP_header bck_osip;
	int i, fd;

	printf("run into restore_handle\n");

	if (read_OSIP_loc(&bck_osip, R_BCK, DUMP_OSIP) < 0) {
		printf("read_backup_OSIP fails\n");
		return -1;	/*FAIL */
	}
	if (bck_osip.sig != OSIP_SIG)
		return -1;

/*	for (i = 0; i < osip.num_pointers; i++) {
		if ((bck_osip.desc[i].reserved[0] != 0xDD)
		    && (bck_osip.desc[i].ddr_load_address != 0xDD))
			osip.desc[i] = bck_osip.desc[i];
	}*/

	if (write_OSIP(&bck_osip) < 0) {
		printf("fail write OSIP when restore OSIP\n");
		return -1;
	}

	fd = open(MMC_DEV_POS, O_RDWR);
	if (fd < 0) {
		printf("fail to open %s\n", MMC_DEV_POS);
		return -1;
	}
	memset((void *)&bck_osip, 0, sizeof(bck_osip));	/*remove all backup entries */
	lseek(fd, BACKUP_LOC, SEEK_SET);
	if (write(fd, (void *)&bck_osip, sizeof(bck_osip)) < 0) {
		close(fd);
		printf("fail when deleting all backup entrys of OSII\n");
		return -1;
	}
	fsync(fd);
	close(fd);
	return 0;
}

int write_OSII_entry(struct OSII *upd_osii, int update_number, int location)
{
	int fd;

	fd = open(MMC_DEV_POS, O_RDWR);
	if (fd < 0) {
		printf("fail to open %s\n", MMC_DEV_POS);
		return -1;
	}
	if (location == R_START)
		lseek(fd, OSIP_PREAMBLE + sizeof(struct OSII) * update_number, SEEK_SET);
	else
		lseek(fd, BACKUP_LOC+OSIP_PREAMBLE + sizeof(struct OSII) * update_number, SEEK_SET);

	if (write(fd, (void *)upd_osii, sizeof(*upd_osii)) < 0) {
		close(fd);
		printf("fail when write OSII entry\n");
		return -1;
	}
	fsync(fd);
	close(fd);
	return 0;
}

int remove_backup_OSII(int update_number)
{
	int fd, ret;
	struct OSII osii;

	memset((void *)&osii, 0xDD, sizeof(struct OSII));	/*removed pattern 0xDD */
	fd = open(MMC_DEV_POS, O_RDWR);
	if (fd < 0) {
		printf("fail to open %s\n", MMC_DEV_POS);
		return -1;
	}
	lseek(fd,
	      BACKUP_LOC + OSIP_PREAMBLE + sizeof(struct OSII) * update_number,
	      SEEK_SET);
	if (write(fd, (void *)&osii, sizeof(osii)) < 0) {
		close(fd);
		printf("fail when write OSII entry\n");
		return -1;
	}
	fsync(fd);
	close(fd);
	printf("remove_OSII_entry worked!\n");
	return 0;
}

int update_handle(struct OSII *osii, int update_number)
{
	struct OSII upd_osii;
	struct OSIP_header *osip;

	osip = (struct OSIP_header *)malloc(sizeof(struct OSIP_header));

	read_OSIP_loc(osip, R_START, NOT_DUMP);
	printf("run into update handle\n");

	upd_osii = osip->desc[update_number];
	if (osii->os_rev_major != 0xffff)
		upd_osii.os_rev_major = osii->os_rev_major;
	if (osii->os_rev_minor != 0xffff)
		upd_osii.os_rev_minor = osii->os_rev_minor;
	if (osii->logical_start_block != 0xffffffff)
		upd_osii.logical_start_block = osii->logical_start_block;
	if (osii->ddr_load_address != 0xffffffff)
		upd_osii.ddr_load_address = osii->ddr_load_address;
	if (osii->entery_point != 0xffffffff)
		upd_osii.entery_point = osii->entery_point;
	if (osii->size_of_os_image != 0xffffffff)
		upd_osii.size_of_os_image = osii->size_of_os_image;
	if (osii->attribute != 0xff)
		upd_osii.attribute = osii->attribute;

	osip->desc[update_number] = upd_osii;

/*	printf
	    (" os_min_rev = 0x%0hx, os__maj_rev = 0x%hx, logcial_start_block = 0x%x\n",
	     upd_osii.os_rev_minor, upd_osii.os_rev_major,
	     upd_osii.logical_start_block);
	printf
	    (" ddr_load_address = 0x%0x, entry_point = 0x%0x, size_of_os_image= 0x%x, attribute = 0x%x\n",
	     upd_osii.ddr_load_address, upd_osii.entery_point,
	     upd_osii.size_of_os_image, upd_osii.attribute);*/

	printf("into write_OSII_entry!\n");


	if (write_OSIP(osip) < 0) {
		printf("fail write OSIP\n");
		return -1;
	}
/*	if (write_OSII_entry(&upd_osii, update_number, R_START) < 0) {
		printf("fail write OSIP when update OSII\n");
		return -1;
	}*/

/*	if (restore_flag != 1) {
		if (remove_backup_OSII(update_number) < 0) {
			printf("fail remove backup entry\n");
			return -1;
		}
	}*/

	read_OSIP_loc(osip, R_START, DUMP_OSIP);
	return 0;
}

int invalidate_handle(int inval_cnt, int *inval_values)
{
	int i, j;
	struct OSIP_header osip;
	int lsb, size_of_os_image;

	if (read_OSIP_loc(&osip, R_START, NOT_DUMP) < 0) {
		printf("fail reading OSIP\n");
		return -1;
	}

	for (i = 0; i < inval_cnt; i++) {
		for (j = 0; j < OSII_TOTAL; j++) {
			if (inval_values[i] == osip.desc[j].attribute) {
				printf("into invalidate entry\n");
				printf("invalidate attribute = %d\n",
				       osip.desc[j].attribute);
				lsb = osip.desc[j].logical_start_block;
				size_of_os_image =
				    osip.desc[j].size_of_os_image;
				memset((void *)&osip.desc[j], 0,
				       sizeof(struct OSII));
				osip.desc[j].logical_start_block = lsb;
				osip.desc[j].size_of_os_image =
				    size_of_os_image;
				osip.desc[j].attribute = inval_values[i];
				break;
			}
		}

		if (j >= OSII_TOTAL)
			printf("Can't find attribute %d\n", inval_values[i]);
	}

	if (write_OSIP(&osip) < 0) {
		printf("fail write OSIP\n");
		return -1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	int c, ret, update_num = 0;
	int get_flag = 0;
	int inval_cnt = 0;
	int inval_values[10];
	int backup_flag = 0, restore_flag = 0, update_flag = 0, inval_flag =
	    0, check_flag = 0;
	struct OSII osii;
	struct OSIP_header ori_osip;
	struct OSIP_header dis_osip;
	char *fwBinFile = NULL;

	memset((void *)&osii, 0xFF, sizeof(osii));	/*set osii to all 1 by default */
	memset((void *)inval_values, 0, sizeof(inval_values));

	while (1) {
		static struct option osip_options[] = {
			{"backup", no_argument, NULL, 'b'},
			{"check", no_argument, NULL, 'c'},
			{"invalidate", required_argument, NULL, 'i'},
			{"image", required_argument, NULL, 'g'},
			{"restore", no_argument, NULL, 'r'},
			{"update", required_argument, NULL, 'u'},
			/*below options are parameters of OSII
			   TODO:lba should not be changed           */
			{"revmaj", required_argument, NULL, 'm'},
			{"revmin", required_argument, NULL, 'n'},
			{"addr", required_argument, NULL, 'a'},
			{"entry", required_argument, NULL, 'e'},
			{"lba", required_argument, NULL, 'l'},
			{"size", required_argument, NULL, 's'},
			{"attrib", required_argument, NULL, 't'},
			{0, 0, 0, 0}	/*end of long_options */
		};

		/* getopt_long stores the option index here. */
		int option_index = 0;

		c = getopt_long(argc, argv, "hbcrg:i:u:m:n:a:e:l:s:t:",
				osip_options, &option_index);

		/* Detect the end of the options. */
		if (c == -1) {
			if (!get_flag)
				display_usage();
			break;
		}
		get_flag = 1;

		switch (c) {
		case 0:
			/* If this option set a flag, do nothing else now. */
			if (osip_options[option_index].flag != 0)
				break;
			printf("option %s", osip_options[option_index].name);
			if (optarg)
				printf(" with arg %s", optarg);
			printf("\n");
			break;

		case 'b':
			printf("option -back up\n");
			backup_flag = 1;
			break;

		case 'c':
			printf("option -check\n");
			check_flag = 1;
			break;

		case 'r':
			printf("option restore osip!\n");
			restore_flag = 1;
			break;

		case 'i':
			printf("option -invalidate with value `%s'\n", optarg);
			inval_values[inval_cnt++] = atoi(optarg);
			inval_flag = 1;
			break;

		case 'g':
			printf("option --image with value `%s'\n", optarg);
			fwBinFile = optarg;
			break;

		case 'u':
			printf("option -update with value `%s'\n", optarg);
			update_num = atoi(optarg);
			update_flag = 1;
			break;

		case 'm':
			printf("option -m with value `%s'\n", optarg);
			osii.os_rev_major = atoi(optarg);
			break;

		case 'n':
			printf("option -n with value `%s'\n", optarg);
			osii.os_rev_minor = atoi(optarg);
			break;

		case 'a':
			printf("option -a with value `%s'\n", optarg);
			osii.ddr_load_address = atoi(optarg);
			break;

		case 'e':
			printf("option -e with value `%s'\n", optarg);
			osii.entery_point = atoi(optarg);
			break;

		case 'l':
			printf("option -l with value `%s'\n", optarg);
			osii.logical_start_block = atoi(optarg);
			break;

		case 's':
			printf("option -s with value `%s'\n", optarg);
			osii.size_of_os_image = atoi(optarg);
			break;

		case 't':
			printf("option -t with value `%s'\n", optarg);
			osii.attribute = atoi(optarg);
			break;

		case 'h':
		case '?':
			display_usage();
			/* getopt_long already printed an error message. */
			break;

		default:
			abort();
		}
	}

	/* Print any remaining command line arguments (not options). */
	if (optind < argc) {
		printf("non-option ARGV-elements: ");
		while (optind < argc)
			printf("%s ", argv[optind++]);
		putchar('\n');
	}

/*	printf("update_flag = %d update_number=%d\ninval_flag=%d\n",
	       update_flag, update_num, inval_flag);

	printf("inval_cnt=%d, inval_values = %d ||| %d ||| %d\n", inval_cnt,
	       inval_values[0], inval_values[1], inval_values[2]);  */
	/*Handle all possible situations. */
	if (backup_flag == 1) {
		read_OSIP_loc(&ori_osip, R_START, DUMP_OSIP);
		backup_handle(&ori_osip);
	}

	if (restore_flag == 1)
		restore_handle();

	if (update_flag == 1) {
		if (fwBinFile)
			flash_stitch_image(fwBinFile, update_num);
		update_handle(&osii, update_num);
	}

	if (inval_flag == 1) {
		if (backup_flag != 1) {
			printf
			    ("You have to backup valid OSIP before invalidate!\n");
			exit(EXIT_FAILURE);
		}
		invalidate_handle(inval_cnt, inval_values);
	}

	if (check_flag == 1) {
		read_OSIP_loc(&ori_osip, R_START, DUMP_OSIP);
		read_OSIP_loc(&ori_osip, R_BCK, DUMP_OSIP);
	}
	exit(0);
}
