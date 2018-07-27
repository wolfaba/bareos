#!/bin/bash
/usr/sbin/bareos stop

rm /var/run/bareos/working/*

/etc/init.d/mhvtl stop

/root/git/bareos/src/cats/drop_bareos_database
/root/git/bareos/src/cats/create_bareos_database
/root/git/bareos/src/cats/make_bareos_tables
/root/git/bareos/src/cats/grant_bareos_privileges

cp /etc/bareos/mtx-changer.conf.nowait /etc/bareos/mtx-changer.conf

rm -Rf /opt/mhvtl/*
/etc/init.d/mhvtl start
/usr/sbin/bareos start

echo "label barcodes drive=3 pool=Full  slots=1-100 yes"  | bconsole


# cleanup done

echo "--- starting bareos daemons ---"

/usr/sbin/bareos stop
cp /usr/local/bin/mtx-changer /etc/bareos
cp /etc/bareos/mtx-changer.conf.wait /etc/bareos/mtx-changer.conf
/usr/sbin/bareos start

mtx -f /dev/sg13 transfer 11 1

echo "!!!!! ***** please attach gdb if wanted ***** !!!!!"
#sleep 15
read -p "Please press Enter to continue"

for i in `seq 1 1`; do
	echo "run job=backup-bareos-fd level=full yes" | bconsole
done
