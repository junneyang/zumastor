kernel_tree=/src/2.6.17-rc6-dd/drivers/md

ddraid_binaries =  mkddraid ddraid-server ddraid-agent
ddsnap_binaries =  mkddsnap ddsnapd ddsnap-agent ddsnap
binaries =  $(ddraid_binaries) $(ddsnap_binaries) devspam
deps = Makefile trace.h buffer.h list.h sock.h diskio.h
ddsnap_deps = $(deps) ddsnapd.h $(kernel_tree)/dm-ddsnap.h 
ddraid_deps = $(deps) ddraid.h $(kernel_tree)/dm-ddraid.h

INC = $(kernel_tree)

all: $(binaries)

buffer.o: buffer.c $(deps)
	cc -g -Wall buffer.c -c

xdelta3.o: xdelta3.c
	cc -g -Wall xdelta3.c -c

delta.o: delta.c xdelta3.c
	cc -g -Wall delta.c -c

mkddraid: ddraid.c buffer.o $(ddraid_deps)
	cc -g -Wall -I$(INC) ddraid.c buffer.o -DCREATE -o mkddraid

ddraid-server: ddraid.c buffer.o $(ddraid_deps)
	cc -g -Wall -I$(INC) ddraid.c buffer.o -DSERVER -o ddraid-server

ddraid-agent: ddraid.agent.c $(ddraid_deps)
	cc -Wall -I$(INC) ddraid.agent.c -lpthread -o ddraid-agent

mkddsnap: ddsnapd.c buffer.o $(ddsnap_deps)
	cc -g -Wall -I$(INC) ddsnapd.c buffer.o -DCREATE -o mkddsnap -lpopt

ddsnapd: ddsnapd.c buffer.o $(ddsnap_deps)
	cc -g -Wall -D_FILE_OFFSET_BITS=64 -I$(INC) ddsnapd.c buffer.o -DSERVER -o ddsnapd -lpopt

ddsnap-agent: ddsnap.agent.c $(ddsnap_deps)
	cc -g -Wall -I$(INC) ddsnap.agent.c -o ddsnap-agent

ddsnap: ddsnap.c xdelta3.o delta.o $(ddsnap_deps)
	cc -g -Wall -I$(INC) -D_FILE_OFFSET_BITS=64 xdelta3.o delta.o ddsnap.c -o ddsnap -lpopt

devspam: devspam.c trace.h
	cc -g -Wall -I$(INC) devspam.c -o devspam

clean:
	rm -f $(binaries) *.o a.out

install:
	cp $(binaries) /usr/bin

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

ddraid-test: ddraid-test1 ddraid-test2

ddraid-test1:
	killall ddraid-server || true
	sudo killall ddraid-agent || true
	./mkddraid /dev/$(meta)
	sudo /sbin/dmsetup remove $(device) || true
	sudo ./ddraid-agent @test
	./ddraid-server /dev/$(meta) $(data) $(sock) $(port)

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
	killall ddraid-server

ddraid-test9:
	sudo /sbin/dmsetup remove $(device)

#ddsnap-test: ddsnap-test1 ddsnap-test2 ddsnap-test3

dd-init: 
	ln -s /dev/hdb2 /dev/test-snapstore || true
	ln -s /dev/hdb1 /dev/test-origin || true
#	insmod /src/2.6.17-rc6-dd/drivers/md/dm-ddsnap.ko || true
	killall ddsnapd || true
	killall ddsnap-agent || true
	./mkddsnap /dev/test-snapstore /dev/test-origin 
	/sbin/dmsetup remove_all || true

dd-agent:
	./ddsnap-agent /tmp/control

dd-server:
	./ddsnapd /dev/test-snapstore /dev/test-origin /tmp/control /tmp/server

dd-create:
	./ddsnap create-snapshot /tmp/server 0
 
dd-delete:
	./ddsnap delete-snapshot /tmp/server 0
 
origin: 
	echo 0 16064936 ddsnap /dev/test-snapstore /dev/test-origin /tmp/control -1 | sudo /sbin/dmsetup create vol

snap:
	echo 0 16064936 ddsnap /dev/test-snapstore /dev/test-origin /tmp/control 0 | sudo /sbin/dmsetup create snap0

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
