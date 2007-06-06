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

int main(int argc,char **argv) {
  uid_t uid;
  pid_t child;
  char command[256], device[16];
  int n, tapn, nuid, rv, status;
  FILE *tunctlfp;
  
  uid = getuid();
  

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

  return 0;
}
