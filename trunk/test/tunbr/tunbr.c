/* tunbr
 *
 * A small setuid wrapper to allocate and permit tap devices in bridges
 * for use by virtualization tools.
 *
 * 1) run tunctl as root with -u uid to allocate and change ownership
 *    of a device
 * 2) parse tunctl output to get device name
 * 3) set IFACE environment variable to device name
 * 4) add tap device to pre-configured bridge
 * 5) fork and run as original user the rest of the commandline.
 * 6) tunctl -d IFACE as root
 *
 * compiled program must be setuid root, and executable by the group
 * permitted to use tap devices.
 *
 * Copyright 2007 Google Inc.
 * Author: Drake Diedrich
 * License: GPLv2
 ***************************************************************************/  
 
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>

extern char **environ;

    
#ifndef BRIDGE
#define BRIDGE "br1"
#endif

#ifndef TUNCTL
#define TUNCTL "/usr/sbin/tunctl"
#endif

#ifndef BRCTL
#define BRCTL "/usr/sbin/brctl"
#endif

#ifndef IFCONFIG
#define IFCONFIG "/sbin/ifconfig"
#endif

#ifndef PXELINUXCFGDIR
#define PXELINUXCFGDIR "/tftpboot/pxelinux.cfg/"
#endif








int main(int argc,char **argv) {
  uid_t uid;
  pid_t child;
  char command[256], device[16];
  int n, tapn, nuid, rv, status;
  FILE *tunctlfp;
  int rfd, mfd;
  char macfile[256], macaddr[20];
  unsigned char mac[6];
  

  uid = getuid();

  
  rfd = open("/dev/urandom", O_RDONLY);
  if (rfd) {
    n = read(rfd,mac,sizeof(mac));
    if (n != sizeof(mac)) {
      perror("short read");
      return(-7);
    }
    close(rfd);
    mac[0] &= 0xfe;

    n = snprintf(macfile, sizeof(macfile),
		 PXELINUXCFGDIR "01-%02x-%02x-%02x-%02x-%02x-%02x",
		 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], mac[6]);
    mfd = creat(macfile,0644);
    if (mfd == -1) {
      perror("create macfile");
      return errno;
    }
    rv = close(mfd);
    if (rv == -1) {
      perror("close macfile fd");
      return errno;
    }
    rv = chown(macfile, uid, -1);
    if (rv == -1) {
      perror("chown");
      return errno;
    }
    rv = chmod(macfile, 0644);
    if (rv == -1) {
      perror("chmod");
      return errno;
    }
    rv = setenv("MACFILE", macfile);
    if (rv == -1) {
      fprintf(stderr, "insufficient environment space for MACFILE variable\n");
      return -12;
    }

    n = snprintf(macaddr, sizeof(macaddr),
		 "%02x:%02x:%02x:%02x:%02x:%02x",
		 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], mac[6]);
    rv = setenv("MACADDR", macaddr);
    if (rv == -1) {
      fprintf(stderr, "insufficient environment space for MACADDR variable\n");
      return -11;
    }
  }


  n = snprintf(command, sizeof(command), TUNCTL " -u %d", uid);


  tunctlfp = popen(command, "r");
  if (!tunctlfp) {
    perror(command);
    return errno;
  }

  n = fscanf(tunctlfp, "Set 'tap%d' persistent and owned by uid %d",
             &tapn, &nuid);
  if (n != 2) {
    fprintf(stderr, "'%s' output did not provide a device and uid\n",
	    command);
    return -1;
  }

  if (pclose(tunctlfp) == -1) {
    perror("tunctlfp close failed");
    return errno;
  }


  n = snprintf(command, sizeof(command), BRCTL " addif " BRIDGE " tap%d",
               tapn);
  rv = system(command);
  if (rv == -1) {
    perror(TUNCTL);
    return errno;
  }
  if (rv) {
    fprintf(stderr, "'%s' returned %d\n", command, rv);
    return -2;
  }


  n = snprintf(command, sizeof(command), IFCONFIG " tap%d up", tapn);
  rv = system(command);
  if (rv == -1) {
    perror(TUNCTL);
    return errno;
  }
  if (rv) {
    fprintf(stderr, "'%s' returned %d", command, rv);
    return -10;
  }


  n = snprintf(device, sizeof(device), "tap%d", tapn);
  rv = setenv("IFACE", device);
  if (rv == -1) {
    fprintf(stderr, "insufficient environment space for IFACE variable\n");
    return -5;
  }


  child = fork();
  if (child>0) {
    waitpid(child, &status, 0);
  } else if (child==0) {
    setuid(uid);
    execve(argv[1], argv+2, environ);
  } else {
    perror("fork");
    return errno;
  }


  n = snprintf(command, sizeof(command), BRCTL " delif " BRIDGE " tap%d",
               tapn);
  rv = system(command);
  if (rv == -1) {
    perror(TUNCTL);
    return errno;
  }
  if (rv) {
    fprintf(stderr, "'%s' returned %d\n", command, rv);
    return -3;
  }


  n = snprintf(command, sizeof(command), TUNCTL " -d tap%d", tapn);
  rv = system(command);
  if (rv == -1) {
    perror(TUNCTL);
    return errno;
  }
  if (rv) {
    fprintf(stderr, "'%s' returned %d\n", command, rv);
    return -4;
  }


  rv = unlink(macfile);
  if (rv == -1) {
    perror("unlink");
    return errno;
  }


  return 0;
}
