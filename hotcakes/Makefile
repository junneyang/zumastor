kernel=/src/2.6.17-rc6-dd/drivers/md

#instdir=/usr/bin
instdir=/tmp/ddsnap

deps = Makefile trace.h diskio.h buffer.h list.h sock.h
ddsnap_deps = $(deps) ddsnap.h $(kernel)/dm-ddsnap.h
ddraid_deps = $(deps) ddraid.h $(kernel)/dm-ddraid.h

ddraid_binaries = mkddraid ddraidd ddraid-agent
ddsnap_binaries = mkddsnap ddsnapd ddsnap-agent ddsnap
binaries =  $(ddraid_binaries) $(ddsnap_binaries) devspam

includes = $(kernel)
flags = -g -Wall -D_FILE_OFFSET_BITS=64

all: $(binaries)

xdelta/xdelta3.o:
	cd xdelta && cc xdelta3.c -c $(flags)

delta.o: delta.c
	cc delta.c -c $(flags)

diskio.o: diskio.c $(deps)
	cc diskio.c -c $(flags)

buffer.o: buffer.c diskio.c $(deps)
	cc buffer.c -c $(flags)

daemonize.o: daemonize.c $(deps)
	cc daemonize.c -c $(flags)

mkddraid: ddraid.c diskio.o buffer.o $(ddraid_deps)
	cc -DCREATE ddraid.c $(flags) -I$(includes) diskio.o buffer.o -o mkddraid

ddraidd: ddraid.c diskio.o buffer.o $(ddraid_deps)
	cc -DSERVER ddraid.c $(flags) -I$(includes) diskio.o buffer.o -o ddraidd

ddraid-agent: ddraid.agent.c $(ddraid_deps)
	cc ddraid.agent.c $(flags) -I$(includes) -lpthread -o ddraid-agent -lpopt

mkddsnap: ddsnapd.c diskio.o buffer.o $(ddsnap_deps)
	cc -DCREATE ddsnapd.c $(flags) -I$(includes) diskio.o buffer.o -o mkddsnap -lpopt

ddsnapd: ddsnapd.c diskio.o buffer.o daemonize.o $(ddsnap_deps)
	cc -DSERVER ddsnapd.c $(flags) -I$(includes) diskio.o buffer.o daemonize.o -o ddsnapd -lpopt

ddsnap-agent: ddsnap.agent.c daemonize.o $(ddsnap_deps)
	cc ddsnap.agent.c $(flags) -I$(includes) daemonize.o -o ddsnap-agent -lpopt

ddsnap: ddsnap.c xdelta/xdelta3.o delta.o diskio.o daemonize.o $(ddsnap_deps)
	cc ddsnap.c $(flags) -I$(includes) xdelta/xdelta3.o delta.o diskio.o daemonize.o -o ddsnap -lpopt -lz

devspam: tests/devspam.c trace.h
	cd tests && cc devspam.c $(flags) -I$(includes) -o ../devspam

clean:
	rm -f $(binaries) *.o xdelta/*.o a.out

install: $(binaries)
	mkdir $(instdir) || true
	cp $(binaries) $(instdir)

kernel:
	cd ../../.. && make bzImage

kern:
	cd ../../.. && make bzImage SUBDIRS=drivers/md

# Just tests from here on.
device=test
port=8080

# Use an anonymous socket (@ is ddraid's anon socket name extension)
sock=@test

# Intended to run as non-root user (recommended)
# These devices need a+rw permission:
meta=hda5

data0=hda6
#data0=nbd0

data1=hda7
#data1=mapper/loop0

data0=hda6
data1=hda7
data2=hda8
data3=hda9
data4=hda10

data=/dev/$(data0) /dev/$(data1) /dev/$(data2) /dev/$(data3) /dev/$(data4)
#data=/dev/$(data0) /dev/$(data1)

# README test values
origindev=/dev/sda3
snapstoredev=/dev/sda4

ddraid-test: ddraid-test1 ddraid-test2

ddraid-test1:
	killall ddraidd || true
	sudo killall ddraid-agent || true
	./mkddraid /dev/$(meta)
	sudo /sbin/dmsetup remove $(device) || true
	sudo ./ddraid-agent @test
	./ddraidd /dev/$(meta) $(data) $(sock) $(port)

ddraid-test2: ddraid-order2

ddraid-order0:
	echo 0 204800 ddraid 2 $(data) $(sock) | sudo /sbin/dmsetup create $(device)
	sudo chmod a+rw /dev/mapper/$(device)

ddraid-order1:
	echo 0 204800 ddraid 3 $(data) $(sock) | sudo /sbin/dmsetup create $(device)
	sudo chmod a+rw /dev/mapper/$(device)

ddraid-order2:
	echo 0 204800 ddraid 5 $(data) $(sock) | sudo /sbin/dmsetup create $(device)
	sudo chmod a+rw /dev/mapper/$(device)

ddraid-test3:
	./devspam /dev/mapper/$(device) write 100 20 77

ddraid-test4:
	./devspam /dev/mapper/$(device) read 100 20 77

ddraid-test8:
	killall ddraidd

ddraid-test9:
	sudo /sbin/dmsetup remove $(device)

#ddsnap-test: ddsnap-test1 ddsnap-test2 ddsnap-test3

dd-init: 
	ln -s $(snapstoredev) /dev/test-snapstore || true
	ln -s $(origindev) /dev/test-origin || true
#	insmod /src/2.6.17-rc6-dd/drivers/md/dm-ddsnap.ko || true
	killall ddsnapd || true
	killall ddsnap-agent || true
	/sbin/dmsetup remove_all || true
	./mkddsnap /dev/test-snapstore /dev/test-origin 

dd-agent:
	./ddsnap-agent /tmp/control

dd-server:
	./ddsnapd /dev/test-snapstore /dev/test-origin /tmp/control /tmp/server
	#./ddsnapd -f /dev/test-snapstore /dev/test-origin /tmp/control /tmp/server

dd-create:
	./ddsnap create-snapshot /tmp/server 0
 
dd-delete:
	./ddsnap delete-snapshot /tmp/server 0
 
origin: 
	echo 0 $$((2 * $$(egrep $$(echo $(origindev) | sed -e 's@^.*/@@')'$$' /proc/partitions | awk '{print $$3}'))) ddsnap /dev/test-snapstore /dev/test-origin /tmp/control -1 | sudo /sbin/dmsetup create vol

snap0:
	sudo ./ddsnap create-snapshot /tmp/server 0
	echo 0 $$((2 * $$(egrep $$(echo $(origindev) | sed -e 's@^.*/@@')'$$' /proc/partitions | awk '{print $$3}'))) ddsnap /dev/test-snapstore /dev/test-origin /tmp/control 0 | sudo /sbin/dmsetup create snapshot0

snap1:
	sudo ./ddsnap create-snapshot /tmp/server 1
	echo 0 $$((2 * $$(egrep $$(echo $(origindev) | sed -e 's@^.*/@@')'$$' /proc/partitions | awk '{print $$3}'))) ddsnap /dev/test-snapstore /dev/test-origin /tmp/control 1 | sudo /sbin/dmsetup create snapshot1

snap2:
	sudo ./ddsnap create-snapshot /tmp/server 2
	echo 0 $$((2 * $$(egrep $$(echo $(origindev) | sed -e 's@^.*/@@')'$$' /proc/partitions | awk '{print $$3}'))) ddsnap /dev/test-snapstore /dev/test-origin /tmp/control 2 | sudo /sbin/dmsetup create snapshot2

changelist:
	sudo ./ddsnap list
	sudo ./ddsnap generate-changelist /tmp/server changelist0-1 0 1

delta:
	sudo ./ddsnap create-delta -r changelist0-1 deltafile0-1 /dev/mapper/snapshot0 /dev/mapper/snapshot1

ddsnap-test3: 
#	sudo ./devpoke /dev/mapper/torigin write 2
#	sudo ./devpoke /dev/mapper/torigin read 2
#	sleep 1
#	killall ddsnap-server
	sudo ./devspam /dev/mapper/torigin write 19 77

ddsnap-test4:
#	sudo ./devspam /dev/mapper/torigin write 1 77

ddsnap-test9:
	sudo /sbin/dmsetup remove torigin

