// mfs.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "mfs.h"
#include "ufs.h"
#include <assert.h>

static int fs_fd = -1;
static super_t superblock;
static char *fs_image_path;

int read_block(int block_num, void *buffer) {
    return pread(fs_fd, buffer, UFS_BLOCK_SIZE, block_num * UFS_BLOCK_SIZE);
}

int write_block(int block_num, void *buffer) {
    return pwrite(fs_fd, buffer, UFS_BLOCK_SIZE, block_num * UFS_BLOCK_SIZE);
}

int MFS_Init(char *filename, int port) {
    // For local filesystem, we ignore the port parameter
    fs_image_path = strdup(filename);
    fs_fd = open(fs_image_path, O_RDWR);
    if (fs_fd < 0) {
        perror("Unable to open filesystem image");
        return -1;
    }
}

int MFS_Lookup(int pinum, char *name) {

    // Read the parent inode
    inode_t parent_inode;
    int inode_block = superblock.inode_region_addr + (pinum / (UFS_BLOCK_SIZE / sizeof(inode_t)));
    int inode_offset = (pinum % (UFS_BLOCK_SIZE / sizeof(inode_t))) * sizeof(inode_t);
    
    if (pread(fs_fd, &parent_inode, sizeof(inode_t), inode_block * UFS_BLOCK_SIZE + inode_offset) != sizeof(inode_t)) {
        return -2;
    }

    if (parent_inode.type != UFS_DIRECTORY) {
        return -3;
    }

    // Search through the directory entries
    for (int i = 0; i < DIRECT_PTRS && parent_inode.direct[i] != -1; i++) {
        dir_ent_t dir_entries[UFS_BLOCK_SIZE / sizeof(dir_ent_t)];
        if (read_block(parent_inode.direct[i], dir_entries) != UFS_BLOCK_SIZE) {
            return -1;
        }

        for (int j = 0; j < UFS_BLOCK_SIZE / sizeof(dir_ent_t); j++) {
            if (dir_entries[j].inum != -1 && strcmp(dir_entries[j].name, name) == 0) {
                return dir_entries[j].inum;
            }
        }
    }

    return -1;  // File not found
}

int MFS_Stat(int inum, MFS_Stat_t *m) {
    if (inum < 0 || inum >= superblock.num_inodes || m == NULL) {
        return -1;
    }

    inode_t inode;
    int inode_block = superblock.inode_region_addr + (inum / (UFS_BLOCK_SIZE / sizeof(inode_t)));
    int inode_offset = (inum % (UFS_BLOCK_SIZE / sizeof(inode_t))) * sizeof(inode_t);
    
    if (pread(fs_fd, &inode, sizeof(inode_t), inode_block * UFS_BLOCK_SIZE + inode_offset) != sizeof(inode_t)) {
        return -1;
    }

    m->type = inode.type;
    m->size = inode.size;

    return 0;
}


int get_inode(int inum, inode_t *inode) {
    if (inum < 0 || inum >= superblock.num_inodes) {
        return -1;
    }

    int inode_block = superblock.inode_region_addr + (inum / (UFS_BLOCK_SIZE / sizeof(inode_t)));
    int inode_offset = (inum % (UFS_BLOCK_SIZE / sizeof(inode_t))) * sizeof(inode_t);
    
    if (pread(fs_fd, inode, sizeof(inode_t), inode_block * UFS_BLOCK_SIZE + inode_offset) != sizeof(inode_t)) {
        return -1;
    }

    return 0;
}

int put_inode(int inum, inode_t *inode) {
    if (inum < 0 || inum >= superblock.num_inodes) {
        return -1;
    }

    int inode_block = superblock.inode_region_addr + (inum / (UFS_BLOCK_SIZE / sizeof(inode_t)));
    int inode_offset = (inum % (UFS_BLOCK_SIZE / sizeof(inode_t))) * sizeof(inode_t);
    
    if (pwrite(fs_fd, inode, sizeof(inode_t), inode_block * UFS_BLOCK_SIZE + inode_offset) != sizeof(inode_t)) {
        return -1;
    }

    return 0;
}

int set_bitmap(int bitmap_start, int bitmap_len, int index, int value) {
    int byte_index = index / 8;
    int bit_index = index % 8;
    unsigned char byte;

    if (byte_index >= bitmap_len * UFS_BLOCK_SIZE) {
        return -1;
    }

    int block = bitmap_start + (byte_index / UFS_BLOCK_SIZE);
    int offset = byte_index % UFS_BLOCK_SIZE;

    if (pread(fs_fd, &byte, 1, block * UFS_BLOCK_SIZE + offset) != 1) {
        return -1;
    }

    if (value)
        byte |= (1 << bit_index);
    else
        byte &= ~(1 << bit_index);

    if (pwrite(fs_fd, &byte, 1, block * UFS_BLOCK_SIZE + offset) != 1) {
        return -1;
    }

    return 0;
}

int find_free_bit(int bitmap_start, int bitmap_len, int num_bits) {
    unsigned char byte;
    for (int i = 0; i < num_bits; i++) {
        int byte_index = i / 8;
        int bit_index = i % 8;

        if (byte_index % UFS_BLOCK_SIZE == 0) {
            if (pread(fs_fd, &byte, 1, (bitmap_start + byte_index / UFS_BLOCK_SIZE) * UFS_BLOCK_SIZE) != 1) {
                return -1;
            }
        }

        if (!(byte & (1 << bit_index))) {
            return i;
        }
    }
    return -1;
}

int allocate_inode() {
    int inum = find_free_bit(superblock.inode_bitmap_addr, superblock.inode_bitmap_len, superblock.num_inodes);
    if (inum == -1) return -1;
    if (set_bitmap(superblock.inode_bitmap_addr, superblock.inode_bitmap_len, inum, 1) == -1) return -1;
    return inum;
}

int allocate_data_block() {
    int block_num = find_free_bit(superblock.data_bitmap_addr, superblock.data_bitmap_len, superblock.num_data);
    if (block_num == -1) return -1;
    if (set_bitmap(superblock.data_bitmap_addr, superblock.data_bitmap_len, block_num, 1) == -1) return -1;
    return superblock.data_region_addr + block_num;
}

int free_data_block(int block_num) {
    int rel_block_num = block_num - superblock.data_region_addr;
    return set_bitmap(superblock.data_bitmap_addr, superblock.data_bitmap_len, rel_block_num, 0);
}

int MFS_Read(int inum, char *buffer, int offset, int nbytes) {
    if (inum < 0 || buffer == NULL || offset < 0 || nbytes < 0) {
        return -1;
    }

    inode_t inode;
    if (get_inode(inum, &inode) != 0) {
        return -1;
    }

    if (offset >= inode.size) {
        return 0;  // Nothing to read
    }

    int bytes_to_read = (offset + nbytes > inode.size) ? (inode.size - offset) : nbytes;
    int bytes_read = 0;

    while (bytes_read < bytes_to_read) {
        int block_index = (offset + bytes_read) / UFS_BLOCK_SIZE;
        int block_offset = (offset + bytes_read) % UFS_BLOCK_SIZE;

        if (block_index >= DIRECT_PTRS) {
            break;  // We don't support indirect blocks in this implementation
        }

        int data_block = inode.direct[block_index];
        if (data_block == -1) {
            break;  // Reached the end of allocated blocks
        }

        char block_buffer[UFS_BLOCK_SIZE];
        if (read_block(data_block, block_buffer) != UFS_BLOCK_SIZE) {
            return -1;
        }

        int bytes_to_copy = UFS_BLOCK_SIZE - block_offset;
        if (bytes_to_copy > bytes_to_read - bytes_read) {
            bytes_to_copy = bytes_to_read - bytes_read;
        }

        memcpy(buffer + bytes_read, block_buffer + block_offset, bytes_to_copy);
        bytes_read += bytes_to_copy;
    }

    return bytes_read;
}

int MFS_Write(int inum, char *buffer, int offset, int nbytes) {
    if (inum < 0 || buffer == NULL || offset < 0 || nbytes < 0) {
        return -1;
    }

    inode_t inode;
    if (get_inode(inum, &inode) != 0) {
        return -1;
    }

    if (inode.type != UFS_REGULAR_FILE) {
        return -1;  // Can only write to regular files
    }

    int bytes_written = 0;

    while (bytes_written < nbytes) {
        int block_index = (offset + bytes_written) / UFS_BLOCK_SIZE;
        int block_offset = (offset + bytes_written) % UFS_BLOCK_SIZE;

        if (block_index >= DIRECT_PTRS) {
            break;  // We don't support indirect blocks in this implementation
        }

        if (inode.direct[block_index] == -1) {
            // Allocate a new block
            int new_block = allocate_data_block();
            if (new_block == -1) {
                return bytes_written;  // No more free blocks
            }
            inode.direct[block_index] = new_block;
        }

        char block_buffer[UFS_BLOCK_SIZE];
        if (read_block(inode.direct[block_index], block_buffer) != UFS_BLOCK_SIZE) {
            return -1;
        }

        int bytes_to_copy = UFS_BLOCK_SIZE - block_offset;
        if (bytes_to_copy > nbytes - bytes_written) {
            bytes_to_copy = nbytes - bytes_written;
        }

        memcpy(block_buffer + block_offset, buffer + bytes_written, bytes_to_copy);

        if (write_block(inode.direct[block_index], block_buffer) != UFS_BLOCK_SIZE) {
            return -1;
        }

        bytes_written += bytes_to_copy;
    }

    // Update inode size if necessary
    if (offset + bytes_written > inode.size) {
        inode.size = offset + bytes_written;
    }
    
    if (put_inode(inum, &inode) != 0) {
        return -1;
    }

    return bytes_written;
}

int MFS_Creat(int pinum, int type, char *name) {
    if (pinum < 0 || (type != UFS_REGULAR_FILE && type != UFS_DIRECTORY) || name == NULL || strlen(name) > 27) {
        return -1;
    }

    inode_t parent_inode;
    if (get_inode(pinum, &parent_inode) != 0 || parent_inode.type != UFS_DIRECTORY) {
        return -2;
    }

    // Check if the file/directory already exists
    int existing_inum = MFS_Lookup(pinum, name);
    if (existing_inum >= 0) {
        return 0;  // File/directory already exists, return success
    }

    // Allocate new inode
    int new_inum = allocate_inode();
    if (new_inum == -1) {
        return -3;  // No free inodes
    }

    // Initialize new inode
    inode_t new_inode;
    memset(&new_inode, 0, sizeof(inode_t));
    new_inode.type = type;
    new_inode.size = (type == UFS_DIRECTORY) ? 2 * sizeof(dir_ent_t) : 0;
    for (int i = 0; i < DIRECT_PTRS; i++) {
        new_inode.direct[i] = -1;
    }

    // For directories, allocate the first block and add . and .. entries
    if (type == UFS_DIRECTORY) {
        int new_block = allocate_data_block();
        if (new_block == -1) {
            set_bitmap(superblock.inode_bitmap_addr, superblock.inode_bitmap_len, new_inum, 0);
            return -5;
        }
        new_inode.direct[0] = new_block;

        dir_ent_t entries[2];
        strcpy(entries[0].name, ".");
        entries[0].inum = new_inum;
        strcpy(entries[1].name, "..");
        entries[1].inum = pinum;

        if (write_block(new_block, entries) != UFS_BLOCK_SIZE) {
            set_bitmap(superblock.inode_bitmap_addr, superblock.inode_bitmap_len, new_inum, 0);
            free_data_block(new_block);
            return -6;
        }
    }

    // Write new inode
    if (put_inode(new_inum, &new_inode) != 0) {
        set_bitmap(superblock.inode_bitmap_addr, superblock.inode_bitmap_len, new_inum, 0);
        if (type == UFS_DIRECTORY) {
            free_data_block(new_inode.direct[0]);
        }
        return -7;
    }

    // Add entry to parent directory
    for (int i = 0; i < DIRECT_PTRS; i++) {
        if (parent_inode.direct[i] == -1) {
            continue;
        }
        dir_ent_t entries[UFS_BLOCK_SIZE / sizeof(dir_ent_t)];
        if (read_block(parent_inode.direct[i], entries) != UFS_BLOCK_SIZE) {
            return -1;
        }
        for (int j = 0; j < UFS_BLOCK_SIZE / sizeof(dir_ent_t); j++) {
            if (entries[j].inum == -1) {
                strcpy(entries[j].name, name);
                entries[j].inum = new_inum;
                if (write_block(parent_inode.direct[i], entries) != UFS_BLOCK_SIZE) {
                    return -1;
                }
                parent_inode.size += sizeof(dir_ent_t);
                return put_inode(pinum, &parent_inode) == 0 ? 0 : -1;
            }
        }
    }

    // If we get here, the parent directory is full
    return -1;
}

// Add this function to free an inode
int free_inode(int inum) {
    return set_bitmap(superblock.inode_bitmap_addr, superblock.inode_bitmap_len, inum, 0);
}

// Recursive function to remove all contents of a directory
int remove_directory_contents(int inum) {
    inode_t inode;
    if (get_inode(inum, &inode) != 0) {
        return -9;
    }

    for (int i = 0; i < DIRECT_PTRS && inode.direct[i] != -1; i++) {
        dir_ent_t entries[UFS_BLOCK_SIZE / sizeof(dir_ent_t)];
        if (read_block(inode.direct[i], entries) != UFS_BLOCK_SIZE) {
            return -8;
        }

        for (int j = 0; j < UFS_BLOCK_SIZE / sizeof(dir_ent_t); j++) {
            if (entries[j].inum != -1 && 
                strcmp(entries[j].name, ".") != 0 && 
                strcmp(entries[j].name, "..") != 0) {
                
                inode_t child_inode;
                if (get_inode(entries[j].inum, &child_inode) != 0) {
                    return -7;
                }

                if (child_inode.type == UFS_DIRECTORY) {
                    if (remove_directory_contents(entries[j].inum) != 0) {
                        return -1;
                    }
                }

                // Free all data blocks
                for (int k = 0; k < DIRECT_PTRS && child_inode.direct[k] != -1; k++) {
                    if (free_data_block(child_inode.direct[k]) != 0) {
                        return -1;
                    }
                }

                // Free the inode
                if (free_inode(entries[j].inum) != 0) {
                    return -1;
                }
            }
        }
    }

    return 0;
}

int MFS_Unlink(int pinum, char *name) {
    if (pinum < 0 || name == NULL || strlen(name) > 27) {
        return -2;
    }

    inode_t parent_inode;
    if (get_inode(pinum, &parent_inode) != 0 || parent_inode.type != UFS_DIRECTORY) {
        return -3;
    }

    int target_inum = -1;
    int target_block = -1;
    int target_entry = -1;

    // Find the entry to be removed
    for (int i = 0; i < DIRECT_PTRS && parent_inode.direct[i] != -1; i++) {
        dir_ent_t entries[UFS_BLOCK_SIZE / sizeof(dir_ent_t)];
        if (read_block(parent_inode.direct[i], entries) != UFS_BLOCK_SIZE) {
            return -4;
        }

        for (int j = 0; j < UFS_BLOCK_SIZE / sizeof(dir_ent_t); j++) {
            if (entries[j].inum != -1 && strcmp(entries[j].name, name) == 0) {
                target_inum = entries[j].inum;
                target_block = i;
                target_entry = j;
                break;
            }
        }

        if (target_inum != -1) break;
    }

    if (target_inum == -1) {
        return 0;  // File or directory not found, which is considered success
    }

    inode_t target_inode;
    if (get_inode(target_inum, &target_inode) != 0) {
        return -5;
    }

    // If it's a directory, make sure it's empty (except for . and ..)
    if (target_inode.type == UFS_DIRECTORY) {
        if (target_inode.size > 2 * sizeof(dir_ent_t)) {
            return -1;  // Directory not empty
        }
        
        // Remove contents (this is just a safety measure, as the directory should be empty)
        if (remove_directory_contents(target_inum) != 0) {
            return -1;
        }
    }

    // Free all data blocks
    for (int i = 0; i < DIRECT_PTRS && target_inode.direct[i] != -1; i++) {
        if (free_data_block(target_inode.direct[i]) != 0) {
            return -1;
        }
    }

    // Free the inode
    if (free_inode(target_inum) != 0) {
        return -1;
    }

    // Remove the entry from the parent directory
    dir_ent_t entries[UFS_BLOCK_SIZE / sizeof(dir_ent_t)];
    if (read_block(parent_inode.direct[target_block], entries) != UFS_BLOCK_SIZE) {
        return -1;
    }

    entries[target_entry].inum = -1;  // Mark the entry as unused
    memset(entries[target_entry].name, 0, 28);  // Clear the name

    if (write_block(parent_inode.direct[target_block], entries) != UFS_BLOCK_SIZE) {
        return -1;
    }

    // Update parent directory size
    parent_inode.size -= sizeof(dir_ent_t);
    if (put_inode(pinum, &parent_inode) != 0) {
        return -1;
    }

    return 0;
}

// ... [MFS_Unlink and MFS_Shutdown implementations] ...

int MFS_Shutdown() {
    if (fs_fd != -1) {
        close(fs_fd);
        fs_fd = -1;
    }
    free(fs_image_path);
    return 0;
}



void print_superblock(super_t *s) {
    printf("Superblock Contents:\n");
    printf("-------------------\n");
    printf("Inode Bitmap Address: %d\n", s->inode_bitmap_addr);
    printf("Inode Bitmap Length: %d blocks\n", s->inode_bitmap_len);
    printf("Data Bitmap Address: %d\n", s->data_bitmap_addr);
    printf("Data Bitmap Length: %d blocks\n", s->data_bitmap_len);
    printf("Inode Region Address: %d\n", s->inode_region_addr);
    printf("Inode Region Length: %d blocks\n", s->inode_region_len);
    printf("Data Region Address: %d\n", s->data_region_addr);
    printf("Data Region Length: %d blocks\n", s->data_region_len);
    printf("Number of Inodes: %d\n", s->num_inodes);
    printf("Number of Data Blocks: %d\n", s->num_data);
}


int test(void) {
    puts("----------->WARNING: RUN ON EMPTY DISK<------------");
    srand(time(NULL));

    MFS_Init("fs4",0);

    // Run tests on empty disk image
    assert(MFS_Lookup(0, "..") == 0);
    assert(MFS_Lookup(0, ".") == 0);

    printf("Lookup passed") ; 

    // Make directory /dir
    assert(MFS_Creat(0, UFS_DIRECTORY, "dir") == 0);
    assert(MFS_Lookup(0, "dir") == 1);

     printf("Create passed") ; 

    // Make directory /dir/dir2
    assert(MFS_Creat(1, UFS_DIRECTORY, "dir2") == 0);
    assert(MFS_Lookup(1, "dir2") == 2);

    printf("Create dir passed") ;

    // Create file /file1
    assert(MFS_Creat(0, UFS_REGULAR_FILE, "file1") == 0);
    assert(MFS_Lookup(0, "file1") == 3);

    printf("Create regular passed") ;

    // Create file /dir/file2
    assert(MFS_Creat(1, UFS_REGULAR_FILE, "file2") == 0);
    assert(MFS_Lookup(1, "file2") == 4);

    // Unlink file /file1
    assert(MFS_Unlink(0, "file1") == 0);
    assert(MFS_Lookup(0, "file1") == -1);
    printf("Unlink passed") ;
    // Unlink directory /dir/dir2
    assert(MFS_Unlink(1, "dir2") == 0);
    assert(MFS_Lookup(1, "dir2") == -1);
    printf("Unlink 2  passed") ;

    // Cleanup
    MFS_Shutdown();

    return 0;
}

int main() {
    test() ; 
    
    
}


