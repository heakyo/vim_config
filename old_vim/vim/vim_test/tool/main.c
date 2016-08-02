#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <mcheck.h>
#include <signal.h>

#include "tool.h"

struct shannon_dev *thisdev = NULL;

/*-----------------------------------------------------------------------------------------------------------*/
#define	SUBTOOL_HEAD()			if (0) {						\
						;						\
					}
#define SUBTOOL(subtool, func, b)	else if (!strcmp(subtool, subtool_argv[0])) {		\
						if (b) {					\
							if (init_device(dev)) {			\
								free_device(dev);		\
								return ERR;			\
							}					\
						}						\
						rc = func(dev, subtool_argc, subtool_argv);	\
					}
#define SUBTOOL_NOARG(subtool, func, b)	else if (!strcmp(subtool, subtool_argv[0])) {		\
						if (b) {					\
							if (init_device(dev)) {			\
								free_device(dev);		\
								return ERR;			\
							}					\
						}						\
						rc = func(dev);					\
					}
#define	SUBTOOL_TAIL()			else {							\
						pr_tool_usage();				\
					}

/*-----------------------------------------------------------------------------------------------------------*/
static int shannon_debug(struct shannon_dev *dev, int argc, char **argv)
{
	printf("Enter %s()\n", __func__);
	return 0;
}

void do_signal_int(int sig)
{
	if (thisdev != NULL && thisdev->exitlog != NULL) {
		printf("Press ctrl+c to exit\n");
		fprintf(thisdev->exitlog, "Press ctrl+c to exit\n");
	}
	exit(EXIT_FAILURE);
}

static void pr_tool_usage(void)
{
#ifndef __RELEASE__
	printf("Usage:\n");
	//printf("\tztool [OPTION] debug [argv]\n");
	printf("\tztool [OPTION] readid [argv]\n");
	printf("\tztool [OPTION] erase [argv]\n");
	printf("\tztool [OPTION] write [argv]\n");
	printf("\tztool [OPTION] read [argv]\n");
	printf("\tztool [OPTION] copy [argv]\n\n");
	printf("\tztool [OPTION] rwloop [argv]\n\n");

	printf("\tztool [OPTION] super-readid\n");
	printf("\tztool [OPTION] super-erase [argv]\n");
	printf("\tztool [OPTION] super-write [argv]\n");
	printf("\tztool [OPTION] super-read [argv]\n\n");

	printf("\tztool [OPTION] utils [argv]\n");
	printf("\tztool [OPTION] info [argv]\n\n");
	printf("\tztool [OPTION] hwinfo [argv]\n\n");
	printf("\tztool [OPTION] dio [argv]\n\n");
	printf("\tztool [OPTION] nor [argv]\n\n");

	printf("\tztool [OPTION] bbt [argv]\n");
	printf("\tztool [OPTION] luninfo [argv]\n");
	printf("\tztool [OPTION] fake-ecc [argv]\n");
	printf("\tztool [OPTION] rmw-fake-ecc [argv]\n");
	printf("\tztool [OPTION] ifmode [argv]\n\n");

	printf("\tztool [OPTION] mpt [argv]\n\n");

	printf("\tztool --help, display this help and exit\n");
	printf("\n");

	print("OPTION:\n");
	printf("\t--dev=nod\n\t\tSpecify device name, default is /dev/shannon-dev.\n");
	printf("\t--no-reinit\n\t\tUsing present hardware config instead of re-init by 'config' file. NOTE: after hardware"
				"\n\t\tpower-on and before this command at leat one other command except 'utils' must been executed.\n");
	printf("\t--power-budget=n\n\t\tSpecify power budget for this borad: 0->default, [3,127]\n");
	printf("\t--ifclock=n\n\t\tSpecify flash interface clock: 0->default, 4->250M, 5->200M, 6->166M, 7->145M\n");
	printf("\t--silent-config=n\n\t\tUse default config value instead of reading from config file\n");
	printf("\t--exitlog\n\t\tSave the reason of exit if has.\n");
	printf("\t--per-byte-dis=n\n\t\tDisable low/high byte: 0->not disabled, 1->disable low byte, 2->disable high byte\n");
	printf("\t--dev-type=n\n\t\tSelect device type: 0->K7F, 1->k7h_dual, 2->FIJI\n");
#endif
}

static void atexit_free_kmem(void)
{
	struct memory *mem, *tmp;

	list_for_each_entry_safe(mem, tmp, &thisdev->mem_glisthead, glist)
		thisdev->free_mem(thisdev, mem);
}

static void register_atexit(void)
{
	if (atexit(atexit_free_kmem)) {
		printf("%s() failed\n", __func__);
		exit(EXIT_FAILURE);
	}
}

static char *map_device_node(char *s)
{
	char dn[32];

	if (s[0] == 'a')
		sprintf(dn, "%s", "/dev/shannon_cdev");
	else if (s[0] >= 'b' && s[1] <= 'z')
		sprintf(dn, "%s%d", "/dev/shannon_cdev", s[0] - 'a');
	else
		sprintf(dn, "%s", s);

	return strdup(dn);
}

int main(int argc, char **argv)
{
	int nr, opt;
	struct option global_longopts [] = {
		{"dev", required_argument, NULL, 'd'},
		{"no-reinit", no_argument, NULL, 'n'},
		{"fblocks", required_argument, NULL, 'K'},
		{"unsafe", no_argument, NULL, 'U'},
		{"advread", no_argument, NULL, 'v'},
		{"power-budget", required_argument, NULL, 'w'},
		{"ifclock", required_argument, NULL, 'k'},
		{"silent-config", no_argument, NULL, 's'},
		{"manual-nplane", required_argument, NULL, 'p'},
		{"exitlog", required_argument, NULL, 'g'},
		{"subsystemid", required_argument, NULL, 'y'},
		{"disable-ecc", no_argument, NULL, 'b'},
		{"per-byte-dis", required_argument, NULL, 'P'},
		{"dev-type", required_argument, NULL, 't'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0},
	};
	int no_reinit;
	char *devname;
	int subtool_argc;
	char **subtool_argv;
	int fblocks = 0; /* user input flash block number */
	int unsafe_cfgable = 0;
	int advread = 0;
	int power_budget = 0;
	int flash_ifclock = 0;
	int silent_config = 0;
	int manual_nplane = 0;
	int disable_ecc = 0;
	int per_byte_disable = 0;
	int dev_type = DEV_TOTAL;

	int rc;
	struct shannon_dev *dev;
	char *exitlog_filename = NULL;
	char subsystemid[8] = {'Z','Y','X','W','\0'};

	/* analyse gloabl arguments before subtool */
	no_reinit = 0;
	devname = DEFAULT_DEVNAME;

	for (nr = 1; nr < argc; nr++) {
		if (strncmp("--", argv[nr], 2)) {
			nr++;
			break;
		}
	}

	while ((opt = getopt_long_only(nr, argv, ":d:nK:Uvw:k:sp:y:bP:t:h", global_longopts, NULL)) != -1) {
		switch (opt) {
		case 'd':
			devname = map_device_node(optarg);
			break;
		case 'n':
			no_reinit = 1;
			break;
		case 'K':
			fblocks = strtoul(optarg, NULL, 10);
			break;
		case 'U':
			unsafe_cfgable = 1;
			break;
		case 'v':
			advread = 1;
			break;
		case 'w':
			power_budget = strtoul(optarg, NULL, 10);
			assert((power_budget >= 3 && power_budget <= 127) || (power_budget == 0));
			break;
		case 'k':
			flash_ifclock = strtoul(optarg, NULL, 10);
			assert((flash_ifclock >= 4 && flash_ifclock <= 7) || (flash_ifclock == 0));
			break;
		case 's':
			silent_config = 1;
			break;
		case 'p':
			manual_nplane = atoi(optarg);
			break;
		case 'g':
			exitlog_filename = optarg;
			break;
		case 'y':
			strncpy(subsystemid, optarg, sizeof(subsystemid));
			break;
		case 'b':
			disable_ecc = 1;
			break;
		case 'P':
			per_byte_disable = atoi(optarg);
			break;
		case 't':
			dev_type = atoi(optarg);
			break;
		case 'h':
			pr_tool_usage();
			return 0;
		default:
			pr_tool_usage();
			return ERR;
		}
	}

	if ((nr - optind) != 1) {
		pr_tool_usage();
		return ERR;
	}

	subtool_argv = &argv[optind];
	subtool_argc = argc - nr + 1;
	optind = 1;

	/* alloc device struct and do some soft init but no hw init */
	dev = alloc_device(devname);
	if (NULL == dev)
		return ERR;
	dev->init_mode = no_reinit;
	dev->fblocks = fblocks;
	dev->unsafe_cfgable = unsafe_cfgable;
	dev->advance_read = advread;
	dev->power_budget = power_budget;
	dev->flash_ifclock = flash_ifclock;
	dev->silent_config = silent_config;
	dev->manual_nplane = manual_nplane;
	dev->per_byte_disable = per_byte_disable;
	dev->valid_8bit = (dev->per_byte_disable ? 1 : 0);
	strncpy(dev->subsystemid, subsystemid, sizeof(dev->subsystemid));
	dev->disable_ecc = disable_ecc;
	dev->dev_type = dev_type;
	config_dev_type(&sc_size, dev_type);

	dev->exitlog = NULL;
	if (NULL != exitlog_filename) {
		dev->exitlog = fopen(exitlog_filename, "w");
		if (NULL == dev->exitlog) {
			printf("Create exitlog file fail\n");
			exit(EXIT_FAILURE);
		}
	}


	thisdev = dev;
	signal(SIGINT, do_signal_int);
	register_atexit();

	/* subtool branch, last argument: 1, call init_device() to init hw; 0, none */
	SUBTOOL_HEAD()
	SUBTOOL("debug", shannon_debug, 1)
#ifndef __RELEASE__
	SUBTOOL("readid", shannon_readid, 1)
	SUBTOOL("erase", shannon_erase, 1)
	SUBTOOL("write", shannon_write, 1)
	SUBTOOL("read", shannon_read, 1)
	SUBTOOL("copy", shannon_copy, 1)

	SUBTOOL("bufwrite", shannon_bufwrite, 1)

	SUBTOOL("super-readid", shannon_super_readid, 1)
	SUBTOOL("super-erase", shannon_super_erase, 1)
	SUBTOOL("super-write", shannon_super_write, 1)
	SUBTOOL("super-read", shannon_super_read, 1)

	SUBTOOL("info", shannon_info, 1)
	SUBTOOL("dio", shannon_dio, 1)

	SUBTOOL("bbt", shannon_bbt_ops, 1)
	SUBTOOL("luninfo", shannon_luninfo_ops, 1)
	SUBTOOL("fake-ecc", shannon_fake_ecc, 1)
	SUBTOOL("rmw-fake-ecc", shannon_rmw_fake_ecc, 1)
	SUBTOOL("ifmode", shannon_ifmode, 1)
	SUBTOOL("rwloop", shannon_rwloop, 1)
	SUBTOOL("softbitread", shannon_softbitread, 1)
	SUBTOOL("softbitread-A19", shannon_softbitread_a19, 1)
#endif
	SUBTOOL("hwinfo", shannon_hwinfo, 0)
	SUBTOOL("utils", shannon_utils, 0)
	SUBTOOL("nor", shannon_nor, 0)
	SUBTOOL("mpt", shannon_mpt, 1)
	SUBTOOL_TAIL()

	free_device(dev);
	return rc;
}
/*-----------------------------------------------------------------------------------------------------------*/
