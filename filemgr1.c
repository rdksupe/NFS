#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include "ufs.h"

#define MAX_FILENAME_LENGTH 28

// Global variables
super_t sb;
typedef struct {
    int type;   // MFS_DIRECTORY or MFS_REGULAR
    int size;   // bytes
} MFS_Stat_t;
MFS_Stat_t mfs ; 
int disk_fd = -1;

int read_inode(int inode_num, inode_t *inode);
int write_inode(int inode_num, inode_t *inode);
int read_block(int block_num, void *buffer);
int write_block(int block_num, void *buffer);

int MFS_Init(char *disk_image_path) {
    disk_fd = open(disk_image_path, O_RDWR);
    if (disk_fd < 0) {
        perror("Failed to open disk image");
        return -1;
    }

    if (pread(disk_fd, &sb, sizeof(super_t), 0) != sizeof(super_t)) {
        perror("Failed to read superblock");
        close(disk_fd);
        return -1;
    }

    return 0;
}

int MFS_Lookup(int pinum, char *name) {
    inode_t parent_inode;
    if (read_inode(pinum, &parent_inode) < 0) {
        return -1;
    }

    if (parent_inode.type != UFS_DIRECTORY) {
        return -1;
    }

    for (int i = 0; i < DIRECT_PTRS; i++) {
        if (parent_inode.direct[i] == 0) continue;

        dir_ent_t entries[UFS_BLOCK_SIZE / sizeof(dir_ent_t)];
        if (read_block(parent_inode.direct[i], entries) < 0) {
            return -1;
        }

        for (int j = 0; j < UFS_BLOCK_SIZE / sizeof(dir_ent_t); j++) {
            if (entries[j].inum != -1 && strncmp(entries[j].name, name, MAX_FILENAME_LENGTH) == 0) {
                return entries[j].inum;
            }
        }
    }

    return -1;
}

int MFS_Stat(int inum, MFS_Stat_t *m) {
    inode_t inode;
    if (read_inode(inum, &inode) < 0) {
        return -1;
    }

    m->type = inode.type;
    m->size = inode.size;
    printf("%d\n",inode.size) ; 
    printf("%d\n",inode.type) ; 
    return 0;
}

int MFS_Write(int inum, char *buffer, int offset, int nbytes) {
    if (nbytes > UFS_BLOCK_SIZE) return -1;

    inode_t inode;
    if (read_inode(inum, &inode) < 0) {
        return -1;
    }

    if (inode.type != UFS_REGULAR_FILE) {
        return -1;
    }

    int block_num = offset / UFS_BLOCK_SIZE;
    if (block_num >= DIRECT_PTRS) {
        return -1;
    }

    if (inode.direct[block_num] == 0) {
        inode.direct[block_num] = sb.data_region_addr + block_num; // Example allocation
    }

    char block[UFS_BLOCK_SIZE];
    if (read_block(inode.direct[block_num], block) < 0) {
        return -1;
    }

    memcpy(block + (offset % UFS_BLOCK_SIZE), buffer, nbytes);
    if (write_block(inode.direct[block_num], block) < 0) {
        return -1;
    }

    if (offset + nbytes > inode.size) {
        inode.size = offset + nbytes;
    }

    if (write_inode(inum, &inode) < 0) {
        return -1;
    }

    return 0;
}

int MFS_Read(int inum, char *buffer, int offset, int nbytes) {
    if (nbytes > UFS_BLOCK_SIZE) return -1;

    inode_t inode;
    if (read_inode(inum, &inode) < 0) {
        return -1;
    }

    int block_num = offset / UFS_BLOCK_SIZE;
    if (block_num >= DIRECT_PTRS || inode.direct[block_num] == 0) {
        return -1;
    }

    char block[UFS_BLOCK_SIZE];
    if (read_block(inode.direct[block_num], block) < 0) {
        return -1;
    }

    memcpy(buffer, block + (offset % UFS_BLOCK_SIZE), nbytes);
    return 0;
}

int MFS_Creat(int pinum, int type, char *name) {
    inode_t parent_inode;
    if (read_inode(pinum, &parent_inode) < 0) {
        return -1;
    }

    if (parent_inode.type != UFS_DIRECTORY) {
        return -1;
    }

    for (int i = 0; i < DIRECT_PTRS; i++) {
        if (parent_inode.direct[i] == 0) {
            parent_inode.direct[i] = sb.data_region_addr + i; // Example allocation
            char block[UFS_BLOCK_SIZE] = {0};
            if (write_block(parent_inode.direct[i], block) < 0) {
                return -1;
            }
        }

        dir_ent_t entries[UFS_BLOCK_SIZE / sizeof(dir_ent_t)];
        if (read_block(parent_inode.direct[i], entries) < 0) {
            return -1;
        }

        for (int j = 0; j < UFS_BLOCK_SIZE / sizeof(dir_ent_t); j++) {
            if (entries[j].inum == -1) {
                entries[j].inum = sb.num_inodes++;
                strncpy(entries[j].name, name, MAX_FILENAME_LENGTH);

                if (write_block(parent_inode.direct[i], entries) < 0) {
                    return -1;
                }

                inode_t new_inode = {.type = type, .size = 0};
                if (write_inode(entries[j].inum, &new_inode) < 0) {
                    return -1;
                }

                if (write_inode(pinum, &parent_inode) < 0) {
                    return -1;
                }

                return 0;
            }
        }
    }

    return -1;
}

int MFS_Unlink(int pinum, char *name) {
    inode_t parent_inode;
    if (read_inode(pinum, &parent_inode) < 0) {
        return -1;
    }

    if (parent_inode.type != UFS_DIRECTORY) {
        return -1;
    }

    for (int i = 0; i < DIRECT_PTRS; i++) {
        if (parent_inode.direct[i] == 0) continue;

        dir_ent_t entries[UFS_BLOCK_SIZE / sizeof(dir_ent_t)];
        if (read_block(parent_inode.direct[i], entries) < 0) {
            return -1;
        }

        for (int j = 0; j < UFS_BLOCK_SIZE / sizeof(dir_ent_t); j++) {
            if (entries[j].inum != -1 && strncmp(entries[j].name, name, MAX_FILENAME_LENGTH) == 0) {
                inode_t inode;
                if (read_inode(entries[j].inum, &inode) < 0) {
                    return -1;
                }

                if (inode.type == UFS_DIRECTORY) {
                    for (int k = 0; k < DIRECT_PTRS; k++) {
                        if (inode.direct[k] != 0) {
                            return -1; // Directory is not empty
                        }
                    }
                }

                entries[j].inum = -1;
                if (write_block(parent_inode.direct[i], entries) < 0) {
                    return -1;
                }

                return 0;
            }
        }
    }

    return -1;
}

int MFS_Shutdown() {
    if (disk_fd >= 0) {
        fsync(disk_fd);
        close(disk_fd);
    }
    exit(0);
    return 0; // This will never be reached
}

int read_inode(int inode_num, inode_t *inode) {
    int inode_offset = sb.inode_region_addr * UFS_BLOCK_SIZE + sizeof(inode_t) * inode_num;
    if (pread(disk_fd, inode, sizeof(inode_t), inode_offset) != sizeof(inode_t)) {
        return -1;
    }
    return 0;
}

int write_inode(int inode_num, inode_t *inode) {
    int inode_offset = sb.inode_region_addr * UFS_BLOCK_SIZE + sizeof(inode_t) * inode_num;
    if (pwrite(disk_fd, inode, sizeof(inode_t), inode_offset) != sizeof(inode_t)) {
        return -1;
    }
    return 0;
}

int read_block(int block_num, void *buffer) {
    int block_offset = block_num * UFS_BLOCK_SIZE;
    if (pread(disk_fd, buffer, UFS_BLOCK_SIZE, block_offset) != UFS_BLOCK_SIZE) {
        return -1;
    }
    return 0;
}

int write_block(int block_num, void *buffer) {
    int block_offset = block_num * UFS_BLOCK_SIZE;
    if (pwrite(disk_fd, buffer, UFS_BLOCK_SIZE, block_offset) != UFS_BLOCK_SIZE) {
        return -1;
    }
    return 0;
}

// New function to list all files in a directory
int MFS_ListFiles(int pinum) {
    inode_t parent_inode;
    if (read_inode(pinum, &parent_inode) < 0) {
        return -1;
    }

    if (parent_inode.type != UFS_DIRECTORY) {
        return -1;
    }

    printf("Listing files in directory (inode %d):\n", pinum);

    for (int i = 0; i < DIRECT_PTRS; i++) {
        if (parent_inode.direct[i] == 0) continue;

        dir_ent_t entries[UFS_BLOCK_SIZE / sizeof(dir_ent_t)];
        if (read_block(parent_inode.direct[i], entries) < 0) {
            return -1;
        }

        for (int j = 0; j < UFS_BLOCK_SIZE / sizeof(dir_ent_t); j++) {
            if (entries[j].inum != -1) {
                printf("Name: %s, Inode: %d\n", entries[j].name, entries[j].inum);
            }
        }
    }

    return 0;
}


char *get_rand_str(int len) {
	char *ret = malloc(sizeof(char) * (len + 1));
	ret[len] = '\0';
	for (int i = 0; i < len; ++i) {
		ret[i] = 'a' + (rand() % 26);
	}
	return ret;
}

int test(void) {
	puts("----------->WARNING: RUN ON EMPTY DISK<------------");
	srand(time(NULL));

	char *hostname = "localhost"; int portnum = 6969;
	MFS_Init("fs1");

	//Run tests on empty disk image
	assert(MFS_Lookup(0, "..") == 0);
	assert(MFS_Lookup(0, ".") == 0);

	/*
	 * Make file /dir/dir2/file
	 * Write a 10,000 byte random string to this file, by writing 5 times (2000 bytes each).
	 * Then do a few random reads of 4000 bytes and compare.
	 */
	//assert(MFS_Creat(0, UFS_DIRECTORY, "dir") == 0);
	//assert(MFS_Creat(1, UFS_DIRECTORY, "dir2") == 0);
	assert(MFS_Creat(2, UFS_REGULAR_FILE, "file") == 0);

	assert(MFS_Lookup(0, "dir") == 1);
	//assert(MFS_Lookup(1, "dir2") == 2);
	assert(MFS_Lookup(2, "file") == 3);

	char *str = get_rand_str(10000); 
	for (int i = 0; i < 5; ++i) {
		assert(MFS_Write(3, str + (2000 * i), 2000 * i, 2000) == 0); 
	}

	for (int i = 0; i <= 5; ++i) {
		char *buf = malloc(4010);
		assert(MFS_Read(3, buf, 1000 * i, 4000) == 0);  
		assert(!memcmp(buf, str + 1000 * i, 4000));
	}

	/*
	 * Change some bytes from 3000-6000.
	 */

	char *str2 = get_rand_str(3000);
	memcpy(str + 3000, str2, 3000);
	assert(MFS_Write(3, str2, 3000, 3000) == 0);

	for (int i = 0; i <= 5; ++i) {
		char *buf = malloc(4010);
		assert(MFS_Read(3, buf, 1000 * i, 4000) == 0);  
		assert(!memcmp(buf, str + 1000 * i, 4000));
	}
	free(str);
	free(str2);

	//assert(MFS_Unlink(1, "dir2") == -1);
	//assert(MFS_Unlink(2, "file") == 0);
	assert(MFS_Lookup(2, "file") == -1);
	//assert(MFS_Unlink(1, "dir2") == 0);
	assert(MFS_Lookup(1, "dir2") == -1);

	return 0;
}


// Example usage of the new function
int main() {
    if (MFS_Init("fs1") != 0) {
        fprintf(stderr, "Failed to initialize filesystem\n");
        return EXIT_FAILURE;
    }

    // List files in the root directory (inode 0)
    MFS_ListFiles(0) ; 
    printf("%d\n",MFS_Lookup(0,".")) ; 
    MFS_Stat(0,&mfs) ; 
    test() ; 

    MFS_Shutdown();
    return EXIT_SUCCESS;
 
}



