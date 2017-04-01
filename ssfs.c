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
    char data[num_blocks-3][block_size];
    int fbm[num_blocks];   // each free bit is associated with one data block
    int wm[num_blocks];    // each write bit is associated with one data block
}disk;

typedef struct dir_entry{
    char filename[filename_length+2];
    int i_node_index;   // this is the start index of its i-node
}dir_entry;

typedef struct fd_entry{
    int i_node_number;  // this is the one that corresponds to the file
    int read_ptr;
    int write_ptr;
}fd_entry;

i_node   *i_node_array;
disk     *my_disk_file;
fd_entry fd_table[max_file_num];
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
    disk my_disk; 
    int  i;

    if(fresh)
    {
        if(init_fresh_disk(filename, block_size, num_blocks) ==-1) exit(EXIT_FAILURE);

        /* setup the super block*/
        superblock sp;
            sp.magic = 0xACBD0005;
            sp.b_size = block_size;
            sp.f_size = num_blocks;
            sp.i_num = max_file_num;
            sp.root.size = 13;  // 13 pointers are used to point to the i-node file
            for(i=0;i<13;i++) { sp.root.pointer[i]=i+3; }

        /* setup the FBM & WM */
        disk my_disk; 
        my_disk.s = sp;

        for(i=0;i<num_blocks;i++) { my_disk.fbm[i] = 1; my_disk.wm[i] = 1; } // 1 indicates the data block is unused and writeable respectively
        my_disk.fbm[0] = 0; // the first block is used by the root  
        
        /* set up an array containing all the i-nodes */ 
        i_node i_node_array[max_file_num];
        for(i=0;i<4;i++){ i_node_array[0].pointer[i] = i+16; }  // the root directory takes up 4 blocks
        i_node_array[0].size = 4;
        for(i=1;i<max_file_num;i++) { i_node_array[i].size = -1; }
        
        /* store the i-node array into a file */
        i_node_file_fp = fopen(i_node_filename, "w+b");
        if (i_node_file_fp != NULL) 
        {
            fwrite(i_node_array, max_file_num*sizeof(i_node), 1, i_node_file_fp);
            fclose(i_node_file_fp);
        }

        /* set up the root directory */
        dir_entry root_dir[max_file_num];
        for(i=0;i<max_file_num;i++){ root_dir[i].i_node_index = -1; }

        /* map the superblock, fbm, wm and i-node file onto the disk */
        superblock *buffer_sp = (superblock *)malloc(block_size); 
        int        *buffer_fbm = (int *)malloc(block_size);
        int        *buffer_wm = (int *)malloc(block_size);
        i_node     *buffer_i_node_file = (i_node *)malloc(max_file_num*sizeof(i_node));
        dir_entry  *buffer_root_dir = (dir_entry *)malloc(max_file_num*sizeof(dir_entry));

        memcpy(buffer_sp, &(my_disk.s), block_size); 
        memcpy(buffer_fbm, &(my_disk.fbm), block_size); 
        memcpy(buffer_wm, &(my_disk.wm), block_size);
        memcpy(buffer_i_node_file, &i_node_array, max_file_num*sizeof(i_node));
        memcpy(buffer_root_dir, &root_dir, max_file_num*sizeof(dir_entry)); 

        if( write_blocks(0, 1, buffer_sp)<0 || write_blocks(1, 1, buffer_fbm)<0 || write_blocks(2, 1, buffer_wm)<0 
            || write_blocks(3, 13, buffer_i_node_file)<0 || write_blocks(16, 4, buffer_root_dir)<0 ) exit(EXIT_FAILURE);
        
        free(buffer_sp); free(buffer_wm); free(buffer_fbm); free(buffer_i_node_file); free(buffer_root_dir);
        
        /* Re-open it in read-only mode */
        i_node_file_fp = fopen(i_node_filename, "r");          
    }
    for(i=0;i<max_file_num;i++) { fd_table[i].i_node_number = -1; }

/*
    else
    {
        if(init_disk(filename, block_size, num_blocks) ==-1){ exit(EXIT_FAILURE);}

        /* structure the file (disk) 
        int disk_fd = open(filename, O_RDWR);
        my_disk_file = (disk *)mmap(NULL, disk_size, PROT_READ|PROT_WRITE, MAP_SHARED, disk_fd, 0); if (my_disk_file == MAP_FAILED) exit(EXIT_FAILURE);
*/
         /* initialize the file containing all the i-nodes 
        i_node_file_fd = open(i_node_filename, O_RDONLY);
        i_node_array = (i_node *)malloc(max_file_num*sizeof(i_node));
        i_node_array = (i_node *)mmap(NULL, max_file_num*sizeof(i_node), PROT_READ|PROT_WRITE, MAP_SHARED, i_node_file_fd, 0); if (i_node_array == MAP_FAILED) exit(EXIT_FAILURE);
    }
   close(i_node_file_fd);
   */

}

/* opens the given file
 * returns an integer corresponds to the index of the entry of this file in the fd_table
 * if the file does not exist, it creates a new file and sets its size to 0. 
 * if the file exists, the file is opened in append mode (i.e., set the write file pointer to the end of the file and read at the beginning of the file).
 */
int ssfs_fopen(char *name)
{
    int file_start_block,       // the i-node file starts from this block
        root_dir_start_block,   // the root directory starts from this block
        root_dir_num_block;     // the number of blocks that the root directory takes up

    superblock *buffer_sp = (superblock *)malloc(block_size); 
    i_node     *i_node_buf = (i_node *)malloc(block_size);
    dir_entry  *root_dir_buf;
    superblock sp;
    dir_entry  root_dir[max_file_num];

    /* read the superblock into a buffer 
     * map the superblock buffer to a struct sp 
     */
    read_blocks(0, 1, buffer_sp);
    memcpy(&sp, buffer_sp, sizeof(superblock));
    /* read the first block that stores the i-node file into a buffer */
    file_start_block = sp.root.pointer[0]; printf("first_block_index = %d\n", file_start_block);
    if(read_blocks(file_start_block, 1, i_node_buf) < 0) exit(EXIT_FAILURE);
    /* read the root directory into a buffer
     * map the root directory buffer into a struct root_dir 
     */
    root_dir_start_block = i_node_buf[0].pointer[0]; 
    root_dir_num_block = i_node_buf[0].size;
    root_dir_buf = (dir_entry *)malloc(root_dir_num_block*block_size);
    printf("i-node file starts from the %d-th block\nthe first i-node has %d pointers\n", root_dir_start_block, i_node_buf[0].size);
    if(read_blocks(root_dir_start_block, i_node_buf[0].size, root_dir_buf) < 0) exit(EXIT_FAILURE);
    memcpy(&root_dir, root_dir_buf, max_file_num*sizeof(dir_entry));

    int i_node_number = 0;
    int i=0;
    for(i=0;i<max_file_num;i++)
    {
        if(root_dir[i].i_node_index!=-1 && strcmp(root_dir[i].filename, name)==0)
        {
            /* check if this file is already opened */
            int k;
            for(k=0;k<max_file_num;k++)
            {
                if(fd_table[k].i_node_number==i_node_number) 
                {
                    printf("The requested file is already opened\n");
                    return 0;
                }
            }
            /* if this file is not opened, open it */
            /* first, place its i_node number into the file descriptor table */
            for(k=0;k<max_file_num;k++)
            {
                if(fd_table[k].i_node_number==-1) 
                {
                    fd_table[k].i_node_number = i_node_number;
                    /* find where the file starts and ends */
                    return 0;
                }
            }
            /* then, place its read and write pointer to the proper position */

            i_node_number++;
        }
        //if(j<block_size) break;
    }
    
}


/* the newly created block copies remain writeable until we call this
 * by copy fbm into wm
 * returns index of the shadow root that holds previous commit
 */
int ssfs_commit()
{

}

/* restore the file system to a previous shadow
 * by copying the shadow root to root
 */
int ssfs_restore(int cnum)
{

}



int main()
{
    mkssfs(1);
    ssfs_fopen("abc");
}