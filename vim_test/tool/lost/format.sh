#!/bin/bash
# script of format ssd

apptool=./ztool
module_dir=../cdev

check_permission()
{
	if [ $EUID != 0 ]; then
		echo "Operation not permitted! Prefix \"sudo\" then try again"
		exit 1
	fi
}

check_apptool()
{
	if [ ! -e $apptool ]; then
		echo "$apptool no exist"
		echo 1
	fi
}

check_module()
{
	current_dir=`pwd`
	cd $module_dir
	if ! sudo ./install; then
		exit 1
	fi
	cd $current_dir
}

usage()
{
	printf "Usage:\n"
	printf "\t`basename $0` [--fblocks=num] [--unsafe] [--dev=nod] [options], format device\n"
	printf "\t`basename $0` --lsdev, list shannon devices\n\n"

	# below shoud keep consistent with 'stool mpt'
	printf "Options:\n"
	printf "\t--dev=nod\n\t\tSelect device, format all shannon devices if no this option\n"
	printf "\t-n, --new\n\t\tFlash is unused\n"
	printf "\t-u, --used\n\t\tFlash is used, bbt info in MBR block will be reuse\n"
	printf "\t-t, --absent-luns=loglun|phylun:x1-x2,x3,x4-x5...\n\t\tSpecify absent luns\n"
	printf "\t-T, --present-luns=loglun|phylun:x1-x2,x3,x4-x5...\n\t\tSpecify present luns\n"
	printf "\t-f, --force\n\t\tFlash is used, ignore bbt info in MBR block\n"
	printf "\t-M, --mbr\n\t\tDisplay MBR info\n"
	printf "\t-B, --bbt\n\t\tDisplay MBR and BBT info\n"
	printf "\t-V, --mbr-version=MBR_VERSION(hex)\n\t\tSet MBR version\n"
	printf "\t-h, --help\n\t\tDisplay this help and exit\n"
}

# main
if [ $# -lt 1 ]; then
	usage
	exit 1
elif [ "-h" = $1 -o "--help" = $1 ]; then
	usage
	exit 0
fi

check_permission
check_apptool
check_module

rc_status=0

if echo $1 | grep "^--fblocks=" > /dev/null; then
	fblocks=$1
	shift
fi

if echo $1 | grep "^--unsafe" > /dev/null; then
	unsafe=$1
	shift
fi

if [ "--lsdev" = $1 ]; then
	for devnod in `ls /dev/shannon_cdev*`; do
		printf "=== $devnod ===\n"
		$apptool --dev=$devnod hwinfo | head -n4
		printf "\n"
	done
	exit 0
elif echo $1 | grep "^--dev=" > /dev/null; then
	devnod=$1
	shift
	$apptool $devnod $fblocks $unsafe mpt $@
	rc_status=$?
else
	for devnod in `ls /dev/shannon_cdev*`; do
		printf "Format devname=$devnod\n"

		$apptool --dev=$devnod $fblocks $unsafe mpt $@
		rc_status=$?
		[ "$rc_status" -ne 0 ] && break;

		printf "\n"
	done
fi

sudo /sbin/rmmod shannon_cdev
exit $rc_status
