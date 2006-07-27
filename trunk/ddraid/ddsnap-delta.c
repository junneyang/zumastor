#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "ddsnap.h"
#include <string.h>
#include <stdlib.h>
#include <libgen.h>

int ddsnap_create_delta(char* filename, char* changelistname, char* dev1name, char* dev2name) {
	int deltafile = creat(filename, S_IRWXU);
    	int changelist, snapdev2;
    	char header[4];
    	u64 chunkadd;
    	char* chunkdata;
    	int check_chunk_num=0;
    	int chunk_num=0;
    	u32 chunk_size;
    	u32 chunk_size_bit;
	
	if (deltafile<0) {
      		printf("delta file was not created properly.\n");
      		return 1;
    	} else {
      		changelist = open(changelistname, O_RDONLY);
      		snapdev2 = open(dev2name, O_RDONLY);

		if (changelist<0 || snapdev2<0) {
        		printf("Either changelist or snapdev2 was not opened properly.\n");
        		return 1;
      		} else {

        		/* Make sure it's a proper changelist */
        		read(changelist,header,3);
			header[3] = 0;
			printf("header is %s\n", header);	
        		if(strcmp(header,"rln")!= 0) {
          			printf("Not a proper changelist file.\n");
          			return 1;
        		} else {

          			/* Variable set up */
          			read(changelist,&check_chunk_num,sizeof(int));
				printf("chunk_num in changelist: %d\t", check_chunk_num);
          			read(changelist,&chunk_size_bit,sizeof(u32));
				printf("chunksize bit: %u\t", chunk_size_bit);
				chunk_size = 1<<chunk_size_bit;
				printf("chunksize: %u\n", chunk_size);
          			chunkdata = (char*) malloc (chunk_size);

          			/* Header set-up */
          			write(deltafile,&check_chunk_num,sizeof(int));
				write(deltafile,&chunk_size,sizeof(u32));
				write(deltafile,"jc",2);

          			/* Chunk address followed by CHUNK_SIZE bytes of chunk data */
          			while(read(changelist,&chunkadd,sizeof(u64))>0) {
            				chunk_num = chunk_num+1;
	            			printf("current chunkadd: %Lu\n", chunkadd);
					chunkadd = chunkadd<<chunk_size_bit;
        	    			write(deltafile,&chunkadd,sizeof(u64));
            				pread(snapdev2,chunkdata,chunk_size,chunkadd);
            				write(deltafile,chunkdata,chunk_size);
          			}

          			/* Make sure everything in changelist was properly transmitted */
          			if (chunk_num != check_chunk_num) {
            				printf("Number of chunks don't match up.\n");
            				return 1;
          			}
        		}
      		}
    	}
	return 0;
}

int ddsnap_apply_delta (char* filename, char* devname) {
    int deltafile = open(filename, O_RDONLY);
    int snapdev = open(devname, O_WRONLY);
    char header[3];
    u64 chunkadd;
    char* chunkdata;
    int check_chunk_num=0;
    int chunk_num=0;
    int chunk_size=0;

    if (deltafile<0 || snapdev<0) {
      printf("delta file or snapdev was not opened properly.\n");
      return 1;
    } else {

      read(deltafile,&check_chunk_num,sizeof(int));
      read(deltafile,&chunk_size,sizeof(int));
      read(deltafile,header,2);

      /* Make sure it's a proper delta file */
	header[2] = 0;
      if(strcmp(header,"jc")!= 0) {
        printf("Not a proper delta file.\n");
        return 1;
      } else {
        chunkdata = (char *) malloc (chunk_size);
        while(read(deltafile,&chunkadd,sizeof(u64))>0) {
          printf("Updating chunkadd: %Lu", chunkadd);
          read(deltafile,chunkdata,chunk_size);
          pwrite(snapdev,chunkdata,chunk_size,chunkadd);
	  chunk_num = chunk_num+1;		
	}

	if (chunk_num != check_chunk_num) {
          printf("Number of chunks don't match up.\n");
          return 1;
        }
      }
    }
    return 0;
}

int main(int argc, char *argv[]) {
	char* command = basename(argv[0]);
	
  if (strcmp(command,"ddsnap-create-delta")==0) {
    if (argc!=5) {
      printf("Incorrect Format: ddsnap-create-delta <deltafile> <changelist> <snapdev1> <snapdev2> \n");
      return 1;
    } else {
      return ddsnap_create_delta(argv[1],argv[2],argv[3],argv[4]);
    }
  } else if (strcmp(command, "ddsnap-apply-delta")==0) {
    if (argc!=3) {
      printf("Incorrect Format: ddsnap-apply-delta <deltafile> <dev> \n");
      return 1;
    } else {
      return ddsnap_apply_delta(argv[1],argv[2]);
    }
  }
  printf("Unrecognized command.\n");
  return 1;
}

