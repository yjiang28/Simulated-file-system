#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/mman.h> 
#include <fcntl.h>
#include "disk_emu.h"

#define block_size 1024      // the size of each data block in bytes
#define num_blocks 1027      // the # of blocks
#define max_file_num 200     // the # of i-nodes
#define filename_length 10   // the filename has at most 10 characters
#define max_restore_time 8

void mkssfs(int fresh);                             // creates the file system
int ssfs_fopen(char *name);                         // opens the given file
int ssfs_fclose(int fileID);                        // closes the given file
int ssfs_frseek(int fileID, int loc);               // seek (Read) to the location from beginning
int ssfs_fwseek(int fileID, int loc);               // seek (Write) to the location from beginning
int ssfs_fwrite(int fileID, char *buf, int length); // write buf characters into disk
int ssfs_fread(int fileID, char *buf, int length);  // read characters from disk into buf
int ssfs_remove(char *file);                        // removes a file from the filesystem
int ssfs_commit();                                  // create a shadow of the file system
int ssfs_restore(int cnum);                         // restore the file system to a previous shadow

/* an i-node is associate with a file
 * each pointer stores the address of a data block contained in the corresponding file
 * all the i-nodes are stored in a file, which is pointed to by a j-node.
 */
typedef struct i_node{
    int size;   // initial value is -1, indicating it's free; busy otherwise
    int pointer[14];
    int ind_pointer;
}i_node;

/* a j-node is stored in the super block in the disk
 * it stores the pointers to the blocks contain the file that has all i-nodes
 */
typedef struct j_node{
    int size;
    int pointer[14];
    int ind_pointer;
}j_node;

typedef struct superblock{
    int magic;      // specify the type of file system format for storing the data
    int b_size;     // 1024 bytes
    int f_size;     // # blocks
    int i_num;      // # i_nodes
    j_node root;    // the root is a j-node
    j_node shadow[max_restore_time];  
}superblock;

typedef struct disk{
    superblock s;
    char data[num_blocks-20][block_size];
    int fbm[num_blocks];   // each free bit is associated with one data block
    int wm[num_blocks];    // each write bit is associated with one data block
}disk;

typedef struct dir_entry{
    char filename[filename_length+2];
    int i_node_index;   // this is the start index of its i-node
}dir_entry;

typedef struct fd_entry{
    int i_node_number;  // this is the one that corresponds to the file
    char *read_ptr;
    char *write_ptr;
}fd_entry;

i_node   *i_node_array;
disk     *my_disk_file;
fd_entry fd_table[max_file_num];

int sp_start_block = 0,
    fbm_start_block = 1,
    wm_start_block = 2,
    file_start_block = 3,       // the i-node file starts from this block
    file_block_num = 13,        // the number of blocks that the i-node file takes up
    root_dir_start_block = 16,  // the root directory starts from this block
    root_dir_block_num= 4,      // the number of blocks that the root directory takes up
    data_start_block = 20;      // user data goes in blocks start from this one
int i, j, k;
/* formats the virtual disk implemented by the disk emulator.
 * creates an instance of SSFS file system on top of it.
 * int fresh: a flag to signal that the file system should be created from scratch.
 * If flag is false, the file system is opened from the disk.
 */
void mkssfs(int fresh)
{
    char *filename = "yjiang28_disk";
    char *i_node_filename = "yjiang28_i_node_file";
    FILE *i_node_file_fp;
    int  disk_size = block_size*num_blocks;
    int  available_block = num_blocks-file_block_num-root_dir_block_num-3;
    disk my_disk; 

    if(fresh)
    {
        if(init_fresh_disk(filename, block_size, num_blocks) ==-1) exit(EXIT_FAILURE);

        /* setup the super block*/
        superblock sp;
            sp.magic = 0xACBD0005;
            sp.b_size = block_size;
            sp.f_size = num_blocks;
            sp.i_num = max_file_num;
            sp.root.size = file_block_num;  // 13 pointers are used to point to the i-node file
            for(i=0;i<file_block_num;i++) { sp.root.pointer[i]=i+file_start_block; }

        /* setup the FBM & WM */
        disk my_disk; 
        my_disk.s = sp;
        for(i=0;i<data_start_block;i++){ my_disk.fbm[i] = 0; if(i<3) my_disk.wm[i] = 1; else my_disk.wm[i] = 0; }
        for(i=data_start_block;i<num_blocks;i++) { my_disk.fbm[i] = 1; my_disk.wm[i] = 0; } // 1 indicates the data block is unused and writeable respectively
        my_disk.fbm[0] = 0; // the first block is used by the root  
        
        /* set up the data blocks */
        for(i=0;i<available_block;i++) { for(j=0;j<block_size;j++) {my_disk.data[i][j] = '0'; } }

        /* set up an array containing all the i-nodes */ 
        i_node i_node_array[max_file_num];
        for(i=0;i<root_dir_block_num;i++){ i_node_array[0].pointer[i] = i+root_dir_start_block; }  // the root directory takes up 4 blocks
        i_node_array[0].size = root_dir_block_num;
        for(i=1;i<max_file_num;i++) { i_node_array[i].size = -1; }

        /* set up the root directory */
        dir_entry root_dir[max_file_num];
        for(i=0;i<max_file_num;i++){ root_dir[i].i_node_index = -1; }

        /* map the superblock, fbm, wm and i-node file onto the disk */
        superblock *buffer_sp = (superblock *)malloc(block_size); 
        int        *buffer_fbm = (int *)malloc(block_size);
        int        *buffer_wm = (int *)malloc(block_size);
        char       *buffer_db = (char *)malloc(available_block*block_size*sizeof(char));
        i_node     *buffer_i_node_file = (i_node *)malloc(max_file_num*sizeof(i_node));
        dir_entry  *buffer_root_dir = (dir_entry *)malloc(max_file_num*sizeof(dir_entry));

        memcpy(buffer_sp, &(my_disk.s), block_size); 
        memcpy(buffer_fbm, &(my_disk.fbm), block_size); 
        memcpy(buffer_wm, &(my_disk.wm), block_size);
        memcpy(buffer_db, &(my_disk.data), available_block*block_size*sizeof(char));
        memcpy(buffer_i_node_file, &i_node_array, max_file_num*sizeof(i_node));
        memcpy(buffer_root_dir, &root_dir, max_file_num*sizeof(dir_entry)); 

        if( write_blocks(sp_start_block, 1, buffer_sp)<0 || write_blocks(fbm_start_block, 1, buffer_fbm)<0 || write_blocks(wm_start_block, 1, buffer_wm)<0 
            || write_blocks(data_start_block, available_block, buffer_db) < 0 || write_blocks(file_start_block, file_block_num, buffer_i_node_file)<0 
            || write_blocks(root_dir_start_block, root_dir_block_num, buffer_root_dir)<0 
            ) exit(EXIT_FAILURE);
        
        free(buffer_sp); free(buffer_wm); free(buffer_fbm); free(buffer_i_node_file); free(buffer_root_dir);
        
        /* Re-open it in read-only mode */
        i_node_file_fp = fopen(i_node_filename, "r");          
    }
    else if(init_disk(filename, block_size, num_blocks) ==-1) exit(EXIT_FAILURE);
    for(i=0;i<max_file_num;i++) { fd_table[i].i_node_number = -1; }
}

/* opens the given file
 * returns an integer corresponds to the index of the entry of this file in the fd_table
 * if the file does not exist, it creates a new file and sets its size to 0. 
 * if the file exists, the file is opened in append mode (i.e., set the write file pointer to the end of the file and read at the beginning of the file).
 */
int ssfs_fopen(char *name)
{
    dir_entry *root_dir_buf = (dir_entry *)malloc(root_dir_block_num*block_size);
    i_node    *i_node_buf = (i_node *)malloc(file_block_num*block_size);
    dir_entry root_dir[max_file_num];
    i_node    i_node_array[max_file_num];
    char      *block_buf = (char *)malloc(block_size);

    /* map the root directory into a buffer *//* copy the blocks holding the root directory */
    if( read_blocks(root_dir_start_block, root_dir_block_num, root_dir_buf) < 0 ) exit(EXIT_FAILURE);
    memcpy(&root_dir, root_dir_buf, max_file_num*sizeof(dir_entry));
    /* map the i-node file (array) into a buffer *//* copy the block containing the i-node pointing to the root directory */
    if( read_blocks(file_start_block, file_block_num, i_node_buf) < 0) exit(EXIT_FAILURE);
    memcpy(&i_node_array, i_node_buf, max_file_num*sizeof(i_node));
    
    int i_node_number = 0;
    for(i=0;i<max_file_num;i++)
    {
        if(root_dir[i].i_node_index!=-1 && strcmp(root_dir[i].filename, name)==0)
        {
            /* check if this file is already opened */
            for(k=0;k<max_file_num;k++)
            {
                if(fd_table[k].i_node_number==i_node_number) 
                {
                    printf("The requested file is already opened\n");
                    return 0;
                }
            }
            /* if this file is not opened, open it */
            for(k=0;k<max_file_num;k++)
            {
                if(fd_table[k].i_node_number==-1) 
                {
                    /* place its i_node number into the file descriptor table */
                    fd_table[k].i_node_number = i_node_number;
                    /* place its read and write pointer to the proper position */
                    i_node file_i_node = i_node_buf[i_node_number];
                    int start_block = file_i_node.pointer[0], end_block = file_i_node.pointer[file_i_node.size-1];
                    /* map the start data block and the end data block into two buffers respectively */
                    if(read_blocks(start_block, 1, block_buf) <0 ) exit(EXIT_FAILURE);
                    *(fd_table[k].read_ptr) = *block_buf;
    
                    if(read_blocks(end_block, 1, block_buf) <0 ) exit(EXIT_FAILURE);
                    for(j=0;j<block_size;j++){ if(*block_buf+j == '0') break; }
                    *(fd_table[k].write_ptr) = *block_buf+j;
                    
                    return 0;
                }
            }
            i_node_number++;
        }
    }
    /* if the file doesn't exist, create a new one of size 0 */   
    /* copy the root to one of the available shadow roots */
    /* modify the root to point to the data block containing the modified i-node */

    free(root_dir_buf);
    free(i_node_buf);
    free(block_buf);
    return 0;
}

int main()
{
    mkssfs(1);
    //ssfs_fopen("abc");
}