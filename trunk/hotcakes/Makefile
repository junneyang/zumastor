kernel=./kernel

instdir=/usr/bin
#instdir=/tmp/ddsnap

deps = Makefile trace.h diskio.h buffer.h list.h sock.h build.h
ddsnap_agent_deps = $(deps) ddsnap.agent.h $(kernel)/dm-ddsnap.h
ddsnapd_deps = $(deps) ddsnapd.h $(kernel)/dm-ddsnap.h
ddsnap_deps = $(deps) ddsnap.h ddsnapd.h ddsnap.agent.h $(kernel)/dm-ddsnap.h
ddraid_deps = $(deps) ddraid.h $(kernel)/dm-ddraid.h

ddraid_binaries = mkddraid ddraidd ddraid-agent
ddsnap_binaries = ddsnap
binaries =  $(ddraid_binaries) $(ddsnap_binaries) devspam

includes = $(kernel)
flags = -g -Wall -D_FILE_OFFSET_BITS=64
#flags = -g -Wall -Wno-parentheses -Wbad-function-cast -Wcast-align -Wwrite-strings -Wstrict-prototypes -Wnested-externs -Wredundant-decls -Wendif-labels -Wcast-qual -D_FILE_OFFSET_BITS=64

all: $(binaries)
.PHONY: all

xdelta/xdelta3.o:
	cd xdelta && cc xdelta3.c -c $(flags)

delta.o: delta.c
	cc delta.c -c $(flags)

ddsnap.common.o: ddsnap.common.c $(deps)
	cc ddsnap.common.c -c $(flags)

ddsnap.agent.o: ddsnap.agent.c $(ddsnap_agent_deps)
	cc ddsnap.agent.c -I$(includes) -c $(flags)

ddsnapd.o: ddsnapd.c $(ddsnapd_deps)
	cc ddsnapd.c -I$(includes) -c $(flags)

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

#mkddsnap: ddsnapd.c ddsnap.common.o diskio.o buffer.o $(ddsnap_deps)
#	cc -DCREATE ddsnapd.c $(flags) -I$(includes) ddsnap.common.o diskio.o buffer.o -o mkddsnap -lpopt

#ddsnapd: ddsnapd.c ddsnap.common.o diskio.o buffer.o daemonize.o $(ddsnap_deps)
#	cc -DSERVER ddsnapd.c $(flags) -I$(includes) ddsnap.common.o diskio.o buffer.o daemonize.o -o ddsnapd -lpopt

#ddsnap-agent: ddsnap.agent.c daemonize.o $(ddsnap_deps)
#	cc ddsnap.agent.c $(flags) -I$(includes) daemonize.o -o ddsnap-agent -lpopt

ddsnap: ddsnap.c buffer.o ddsnapd.o ddsnap.agent.o ddsnap.common.o xdelta/xdelta3.o delta.o diskio.o daemonize.o $(ddsnap_deps)
	cc ddsnap.c $(flags) -I$(includes) buffer.o ddsnapd.o ddsnap.agent.o ddsnap.common.o xdelta/xdelta3.o delta.o diskio.o daemonize.o -o ddsnap -lpopt -lz

devspam: tests/devspam.c trace.h
	cd tests && cc devspam.c $(flags) -I$(includes) -o ../devspam

clean:
	rm -f $(binaries) *.o xdelta/*.o a.out
.PHONY: clean

install: $(binaries)
	mkdir -p $(instdir) || true
	cp $(binaries) $(instdir)
.PHONY: install

kernel:
	cd ../../.. && make bzImage

kern:
	cd ../../.. && make bzImage SUBDIRS=drivers/md

build.h:
	rm -f $@
	@echo '/*' > $@
	@echo ' * This file is generated automatically by the Makefile.' >> $@
	@echo ' */' >> $@
	@echo '' >> $@
	@echo '#define SVN_VERSION "'`svnversion .`'"' >> $@
	@echo '#define BUILD_DATE "'`date`'"' >> $@
	@echo '#define BUILD_USER "'`whoami`'"' >> $@
	@echo '#define BUILD_HOST "'`hostname`'"' >> $@


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
origindev=/dev/hdb1
snapstoredev=/dev/hdb2

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
	./ddsnap initialize /dev/test-snapstore /dev/test-origin 

dd-agent:
	./ddsnap agent /tmp/control

dd-server:
	./ddsnap server /dev/test-snapstore /dev/test-origin /tmp/control /tmp/server

dd-create:
	./ddsnap create /tmp/server 0
 
dd-delete:
	./ddsnap delete /tmp/server 0

 
origin: 
	echo 0 $$((2 * $$(egrep $$(echo $(origindev) | sed -e 's@^.*/@@')'$$' /proc/partitions | awk '{print $$3}'))) ddsnap /dev/test-snapstore /dev/test-origin /tmp/control -1 | sudo /sbin/dmsetup create vol
.PHONY: origin

snap0:
	sudo ./ddsnap create /tmp/server 0
	echo 0 $$((2 * $$(egrep $$(echo $(origindev) | sed -e 's@^.*/@@')'$$' /proc/partitions | awk '{print $$3}'))) ddsnap /dev/test-snapstore /dev/test-origin /tmp/control 0 | sudo /sbin/dmsetup create snapshot0
.PHONY: snap0

snap1:
	sudo ./ddsnap create /tmp/server 1
	echo 0 $$((2 * $$(egrep $$(echo $(origindev) | sed -e 's@^.*/@@')'$$' /proc/partitions | awk '{print $$3}'))) ddsnap /dev/test-snapstore /dev/test-origin /tmp/control 1 | sudo /sbin/dmsetup create snapshot1
.PHONY: snap1

snap2:
	sudo ./ddsnap create /tmp/server 2
	echo 0 $$((2 * $$(egrep $$(echo $(origindev) | sed -e 's@^.*/@@')'$$' /proc/partitions | awk '{print $$3}'))) ddsnap /dev/test-snapstore /dev/test-origin /tmp/control 2 | sudo /sbin/dmsetup create snapshot2
.PHONY: snap2


list:
	sudo ./ddsnap list /tmp/server
.PHONY: list


changelist:	changelist0-1
.PHONY: changelist

changelist0-1:
	sudo ./ddsnap delta changelist /tmp/server $@ 0 1

changelist0-2:
	sudo ./ddsnap delta changelist /tmp/server $@ 0 2

changelist1-2:
	sudo ./ddsnap delta changelist /tmp/server $@ 1 2


delta:	deltafile0-1
.PHONY: delta

deltafile0-1: changelist0-1
	sudo ./ddsnap delta create -r changelist0-1 $@ /dev/mapper/snapshot0 /dev/mapper/snapshot1

deltafile0-2: changelist0-2
	sudo ./ddsnap delta create -r changelist0-2 $@ /dev/mapper/snapshot0 /dev/mapper/snapshot2

deltafile1-2: changelist1-2
	sudo ./ddsnap delta create -r changelist1-2 $@ /dev/mapper/snapshot1 /dev/mapper/snapshot2


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

