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
#include "sfs_api.h"

#define block_size 1024      // the size of each data block in bytes
#define num_blocks 1027      // the # of blocks
#define max_file_num 200     // the # of i-nodes
#define filename_length 10   // the filename has at most 10 characters
#define max_restore_time 4
#define unused '1'
#define used '0'
#define writeable '1'
#define readonly '0'

typedef struct i_node{
    int size;   // initial value is -1, indicating it's free; busy otherwise
    int pointer[15];
}i_node;

typedef struct superblock{
    int magic;      // specify the type of file system format for storing the data
    int b_size;     // 1024 bytes
    int f_size;     // # blocks
    int i_num;      // # i_nodes
    i_node root;    // the root is a j-node
    i_node shadow[max_restore_time];  
}superblock;

typedef struct disk{
    superblock s;
    char data[num_blocks-20][block_size];
    int fbm[num_blocks];   // each free bit is associated with one data block
    int wm[num_blocks];    // each write bit is associated with one data block
}disk;

typedef struct dir_entry{
    char filename[filename_length+2];
    int i_node_index;      // this is the index of its i-node in the i-node file (array)
}dir_entry;

typedef struct pointer{
    int block;
    int entry;
}ptr;

typedef struct fd_entry{
    int i_node_number;  // this is the one that corresponds to the file
    ptr read_ptr;
    ptr write_ptr;
}fd_entry;

int sp_start_block = 0,
    fbm_start_block = 1,
    wm_start_block = 2,
    file_start_block = 3,       // the i-node file starts from this block
    file_block_num = 13,        // # blocks that the i-node file takes up
    root_dir_start_block = 16,  // the root directory starts from this block
    root_dir_block_num= 4,      // # blocks that the root directory takes up
    data_start_block = 20,      // user data goes in blocks start from this one
    data_block_num = 1007,      // # data block for user
    commit_return_value = -1;
/* cache */
char       fbm[num_blocks], 
           wm[num_blocks];
superblock sp;
i_node     i_node_array[max_file_num];
dir_entry  root_dir[max_file_num];
fd_entry   fd_table[max_file_num];
char       a_block_buf[block_size];
// my helper functions
void load_sp(){
    superblock *buffer_sp = (superblock *)malloc(block_size); 
    if( read_blocks(sp_start_block, 1, buffer_sp) < 0) exit(EXIT_FAILURE);
    memcpy(&sp, buffer_sp, block_size);
    free(buffer_sp);
}

void load_fbm(){
    char *buffer_fbm = (char *)malloc(num_blocks);
    if( read_blocks(fbm_start_block, 1, buffer_fbm) < 0 ) exit(EXIT_FAILURE);
    memcpy(&fbm, buffer_fbm, num_blocks);
    free(buffer_fbm);
}

void load_wm(){
    int *buffer_wm = (int *)malloc(num_blocks);
    if( read_blocks(wm_start_block, 1, buffer_wm) < 0 ) exit(EXIT_FAILURE);
    memcpy(&wm, buffer_wm, num_blocks*sizeof(int));
    free(buffer_wm);
}

void commit_sp(){
    superblock *buffer_sp = (superblock *)malloc(block_size); 
    memcpy(buffer_sp, &sp, block_size);
    if( write_blocks(sp_start_block, 1, buffer_sp) < 0) exit(EXIT_FAILURE);    
    free(buffer_sp);
}

void commit_fbm(){
    char *buffer_fbm = (char *)malloc(num_blocks*sizeof(int));
    memcpy(buffer_fbm, &fbm, num_blocks);
    if( write_blocks(fbm_start_block, 1, buffer_fbm) < 0 ) exit(EXIT_FAILURE);
    free(buffer_fbm);
}

void commit_wm(){
    int *buffer_wm = (int *)malloc(num_blocks*sizeof(int));
    memcpy(buffer_wm, &wm, num_blocks*sizeof(int));
    if( write_blocks(wm_start_block, 1, buffer_wm) < 0 ) exit(EXIT_FAILURE);
    free(buffer_wm);
}

void load_i_node_file(){
    i_node *buffer = (i_node *)malloc(block_size), array[file_block_num][block_size/sizeof(i_node)];
    int i, j, k=0, m;
    //load_sp();
    for(i=0;i<file_block_num;i++)
    {
        if( read_blocks(sp.root.pointer[i], 1, buffer) < 0) exit(EXIT_FAILURE);
        memcpy(&array[i], buffer, block_size);
        for(j=0;j<block_size/sizeof(i_node);j++)
        {
            if(k>=max_file_num) break;
            i_node_array[k].size = array[i][j].size;
            for(m=0;m<15;m++)
            {
                i_node_array[k].pointer[m] = array[i][j].pointer[m];
                //printf("load i-node file: %d\n", i_node_array[k].pointer[m]);
            }
            k++;
        }
    }
    free(buffer);
}

void commit_i_node_file(){
    i_node *buffer = (i_node *)malloc(block_size), array[file_block_num][block_size/sizeof(i_node)];
    int i, j, k=0, m;
    //load_sp();
    for(i=0;i<file_block_num;i++)
    {
        for(j=0;j<block_size/sizeof(i_node);j++)
        {
            if(k>=max_file_num) break;
            array[i][j].size = i_node_array[k].size;
            for(m=0;m<15;m++)
            { 
                array[i][j].pointer[m] = i_node_array[k].pointer[m];                
            }
            k++;
        }        
        memcpy(buffer, &array[i], block_size);
        if( write_blocks(sp.root.pointer[i], 1, buffer) < 0) exit(EXIT_FAILURE);        
    }
    free(buffer);
}

void load_root_dir(){
    dir_entry *buffer = (dir_entry *)malloc(block_size), array[root_dir_block_num][block_size/sizeof(dir_entry)];
    int i, j, k=0;
    //load_i_node_file();
    for(i=0;i<root_dir_block_num;i++)
    {        
        if( read_blocks(i_node_array[0].pointer[i], 1, buffer) < 0) exit(EXIT_FAILURE);
        memcpy(&array[i], buffer, block_size);
        for(j=0;j<block_size/sizeof(dir_entry);j++)
        {
            if(k>=max_file_num) break;
            root_dir[k].i_node_index = array[i][j].i_node_index;
            strcpy(root_dir[k].filename, array[i][j].filename);
            k++;
        }
    }
    free(buffer);
}

void commit_root_dir(){
    dir_entry *buffer = (dir_entry *)malloc(block_size), array[root_dir_block_num][block_size/sizeof(dir_entry)];
    int i, j, k=0;
    //load_i_node_file();
    for(i=0;i<root_dir_block_num;i++)
    {
        for(j=0;j<block_size/sizeof(dir_entry);j++)
        {
            if(k>=max_file_num) break;
            array[i][j].i_node_index = root_dir[k].i_node_index;
            strcpy(array[i][j].filename, root_dir[k].filename);
            k++;
        }
        memcpy(buffer, &array[i], block_size);
        if( write_blocks(i_node_array[0].pointer[i], 1, buffer) < 0) exit(EXIT_FAILURE);        
    }
    free(buffer);
}

/* returns the index of the first unused block
 * -1 if all the blocks are taken
 */
int unused_block(){
    int i;
    for(i=data_start_block;i<num_blocks-data_start_block;i++) { if(fbm[i]==unused) return i; }
    return -1;
}

int unused_i_node(){
    int i=0;
    for(i=0;i<max_file_num;i++) { 
        if(i_node_array[i].size == -1) return i; 
    }
    return -1;
}

int unused_fd_entry(){
    int i=0;
    for(i=0;i<max_file_num;i++) { if(fd_table[i].i_node_number==-1) return i; }
    return -1;
}

int unused_dir_entry(){
    load_root_dir();
    int i=0;
    for(i=0;i<max_file_num;i++) { if(root_dir[i].i_node_index==-1) return i; }
    return -1;
}

int write_file_to_blocks(int nblocks, void *buf, int *pointer){ 
    int i, block_index;
    for(i=0;i<nblocks;i++)
    {
        if((block_index = unused_block()) <0 ) {return -1;}
        //printf("block for file %d\n", block_index);
        if( write_blocks(block_index, 1, buf) < 0) return -1;
        fbm[block_index] = used;
        buf += block_size;
        pointer[i] = block_index;      
    }
    return 0;
}

/* return 0 if the block to write is newly allocated to this i-node
 * return 1 if the block to write is the last one belongs to the file associated with this i-node
 */
int find_block_to_write(int i_node_number, int block){
    int k=0, block_to_write, new_i_node;
    for(k=0;k<14;k++) { if(i_node_array[i_node_number].pointer[k] == block) break; }
                
    /* if the write pointer is in the block pointed to by the last direct pointer  
     * and the indirect pointer is not used
     */
    if(k==13 && i_node_array[i_node_number].pointer[14]==-1)
    {
        //printf("1\n");
        /* create an i-node pointed to by the indirected pointer */
        new_i_node = unused_i_node(); block_to_write = unused_block();    
        if( block_to_write <0 || new_i_node <0 ) { printf("write overflow\n"); return -1;} 
       
        i_node_array[i_node_number].pointer[14] = new_i_node;
        i_node_array[new_i_node].size = 0;
        i_node_array[new_i_node].pointer[0] = block_to_write;
        int i; for(i=1;i<15;i++){ i_node_array[new_i_node].pointer[i] = -1; }
        //printf("writes to block %d\n", block_to_write);
    }
    /* if the write pointer is in the block pointed to by the last direct pointer
     * and the indirect pointer is used
     */
    else if(k==13 && i_node_array[i_node_number].pointer[14]!=-1)
    {
        //printf("2\n");
        /* go to the i-node pointed by the indirect pointer */
        new_i_node = i_node_array[i_node_number].pointer[14];
        block_to_write = i_node_array[new_i_node].pointer[0];
    }
    /* if the write pointer is not found in the blocks pointed to by the direct pointers
     * and the indirect pointer is used
     */
    else if(k==14 && i_node_array[i_node_number].pointer[14]!=-1)
    {
        //printf("3\n");
        /* go to the i-node pointed by the indirect pointer */
        new_i_node = i_node_array[i_node_number].pointer[14];
        block_to_write = find_block_to_write(new_i_node, block);
    }
    else if(k==14 && i_node_array[i_node_number].pointer[14]==-1) {printf("4\n");return -1;}
    else 
    {
        //printf("5\n");
        if(i_node_array[i_node_number].pointer[k+1]==-1)
        {//printf("5.1\n");
            block_to_write = unused_block();
            if(block_to_write <0 ) return -1;
            i_node_array[i_node_number].pointer[k+1] = block_to_write;          
        }
        else block_to_write = i_node_array[i_node_number].pointer[k+1];
    }
    fbm[block_to_write] = used;
    //printf("block_to_write &d\n", block_to_write);
    return block_to_write;
}

int find_block_to_read(int i_node_number, int block){
    int k=0, block_to_read, new_i_node;
    for(k=0;k<14;k++) { if(i_node_array[i_node_number].pointer[k] == block) break; }
              //printf("k=%d\ni_node_number%d\npointer[14]=%d\n", k, i_node_number, i_node_array[i_node_number].pointer[14]);  
    /* if the read pointer is in the block pointed to by the last direct pointer  
     * and the indirect pointer is not used
     */
    if(k==13 && i_node_array[i_node_number].pointer[14]==-1)
    {
        //printf("End of file reached\n");
        return -1;
    }
    /* if the read pointer is in the block pointed to by the last direct pointer
     * and the indirect pointer is used
     */
    else if(k==13 && i_node_array[i_node_number].pointer[14]!=-1)
    {
        /* go to the i-node pointed by the indirect pointer */
        new_i_node = i_node_array[i_node_number].pointer[14];
        block_to_read = i_node_array[new_i_node].pointer[0];
    }
    /* if the read pointer is not found in the blocks pointed to by the direct pointers
     * and the indirect pointer is used
     */
    else if(k==14 && i_node_array[i_node_number].pointer[14]!=-1)
    {
        /* go to the i-node pointed by the indirect pointer */
        new_i_node = i_node_array[i_node_number].pointer[14];
        block_to_read = find_block_to_read(new_i_node, block);
    }
    else if(k==14 && i_node_array[i_node_number].pointer[14]==-1) {printf("lll\n");return -1;}
    else
    {
        if(i_node_array[i_node_number].pointer[k+1] == -1) 
        {
            //printf("End of file reached\n");
            return -1; 
        }
        block_to_read = i_node_array[i_node_number].pointer[k+1];
    }
    //printf("block_to_read%d\n", block_to_read);
    //printf("reads from block %d\n", block_to_read);
    return block_to_read;
}

/* returns # chars written
 * -1 if the writing beyond boundary 
 */
int writes_block_by_char(int block_to_write, int offset, char *buf, int length){
    if(length<0 || offset+length > block_size) return -1;
    char *a_block = (char *)malloc(block_size);
    int j;
    if(read_blocks(block_to_write, 1, a_block) <0 ) exit(EXIT_FAILURE);
    for(j=0;j<length;j++){ a_block[offset+j] = buf[j]; }
        //printf("%d\n", offset+j-1);
    //a_block[j] = '\0'; printf("%s\n", a_block);
    write_blocks(block_to_write, 1, a_block);
    free(a_block);
    fbm[block_to_write] = used;
    return length;
}

int reads_block_by_char(int block_to_read, int offset, char *buf, int length){  
    if(length<0 || offset+length > block_size) return -1;
    char *a_block = (char *)malloc(block_size);
    int j;
    if(read_blocks(block_to_read, 1, a_block) <0 ) exit(EXIT_FAILURE);
    for(j=0;j<length;j++){ buf[j] = a_block[offset+j]; }
        //printf("%d\n", offset+j-1);
    free(a_block);
    //buf[j] = '\0'; printf("%s\n", buf);
    return length;
}

void mkssfs(int fresh)
{
    int i, j;
    char *filename = "yjiang28_disk";

    /* set up the file descriptor table */
    for(i=0;i<max_file_num;i++) { fd_table[i].i_node_number = -1; }

    if(fresh)
    {
        if(init_fresh_disk(filename, block_size, num_blocks) ==-1) exit(EXIT_FAILURE);

        /* setup the super block*/
        sp.magic = 0xACBD0005;
        sp.b_size = block_size;
        sp.f_size = num_blocks;
        sp.i_num = max_file_num;
        sp.root.size = file_block_num*block_size;  
        // initialize shadow roots to be unused
        for(i=0;i<max_restore_time;i++) {sp.shadow[i].size = -1;}
        // set up the j-node associated with the i-node file
        for(i=0;i<file_block_num;i++) { sp.root.pointer[i]=i+file_start_block; }
        sp.root.size = max_file_num;    // =13

        /* setup the FBM & WM */
        for(i=0;i<data_start_block;i++){ 
            fbm[i] = used; 
            if(i<3) wm[i] = writeable; 
            else wm[i] = readonly; 
        }

        for(i=data_start_block;i<num_blocks;i++) { 
            fbm[i] = unused; 
            wm[i] = readonly; 
        } // 1 indicates the data block is unused and writeable respectively

        /* set up an array containing all the i-nodes */ 
        for(i=0;i<max_file_num;i++) 
        { 
            i_node_array[i].size = -1; 
            for(j=0;j<15;j++){ i_node_array[i].pointer[j] = -1;}
        }
        for(i=0;i<root_dir_block_num;i++){ i_node_array[0].pointer[i] = i+root_dir_start_block; }  // the root directory takes up 4 blocks
        i_node_array[0].size = root_dir_block_num*block_size;  // =4; the 1st i-node in the i-node file is associated with the root directory

        /* set up the root directory */
        root_dir[0].i_node_index = 0;   // the first i-node is associated with the root directory
        for(i=1;i<max_file_num;i++){ root_dir[i].i_node_index = -1; }
        
        /* map the superblock, fbm, wm and i-node file onto the disk */
        commit_sp(); commit_fbm(); commit_wm(); commit_i_node_file(); commit_root_dir(); 

        /* set up the data blocks and map it onto the disk */
        char data[num_blocks-20][block_size];
        char *buffer_db = (char *)malloc(data_block_num*block_size*sizeof(char));
        for(i=0;i<data_block_num;i++) { for(j=0;j<block_size;j++) {data[i][j] = '0'; } }
        memcpy(buffer_db, &data, data_block_num*block_size*sizeof(char));
        if( write_blocks(data_start_block, data_block_num, buffer_db) < 0) exit(EXIT_FAILURE);
        free(buffer_db);
    }
    else if(init_disk(filename, block_size, num_blocks) !=-1)
    {
        load_sp(); load_wm(); load_fbm(); load_i_node_file(); load_root_dir();
    } 
    else exit(EXIT_FAILURE);
}

int ssfs_fopen(char *name)
{
    int i=0, j=0, k=0, new_fd_entry=-1;
    for(i=0;i<max_file_num;i++)
    {
        /* if the file exists */
        if(root_dir[i].i_node_index!=-1 && strcmp(root_dir[i].filename, name)==0)
        {
            int i_node_number, new_fd_entry;

            i_node_number = root_dir[i].i_node_index;
            /* check if this file is already opened */
            for(k=0;k<max_file_num;k++){ 
                if(fd_table[k].i_node_number==i_node_number) { 
                    printf("The requested file is already opened\n"); 
                    return -1;
                }
            }

            /* if this file is not opened, open it */
            if((new_fd_entry = unused_fd_entry()) <0 ) return -1;

            fd_table[new_fd_entry].i_node_number = i_node_number;          
            fd_table[new_fd_entry].read_ptr.block = i_node_array[i_node_number].pointer[0]; 
            fd_table[new_fd_entry].read_ptr.entry = -1;
            int size = i_node_array[i_node_number].size;
            for(j=0;j<size/(block_size*14);j++)
            {
                i_node_number = i_node_array[i_node_number].pointer[14];
            }
            fd_table[new_fd_entry].write_ptr.block = i_node_array[i_node_number].pointer[(size/block_size)%14];
            fd_table[new_fd_entry].write_ptr.entry = i_node_array[i_node_number].size/block_size-1;
            printf("filename %s\nfd entry %d\nfilesize %d\nwrite_pt %d:%d\nread_pt %d:%d\n", name, new_fd_entry, size, 
                fd_table[new_fd_entry].write_ptr.block, fd_table[new_fd_entry].write_ptr.entry, fd_table[new_fd_entry].read_ptr.block, fd_table[new_fd_entry].read_ptr.entry);
            /* map the end data block into a buffer */      
            return new_fd_entry;         
        }
    }
    /* if the file doesn't exist, create a new one of size 0 */
    if(i==max_file_num)
    {        
        int new_dir_entry=0, new_file_block=0, new_i_node=0, new_fd_entry=0;

        /* copy the root to one of the available shadow roots */
        for(i=0;i<max_restore_time;i++){ if(sp.shadow[i].size==-1) break;}
        /* if the shadow list is full */
        if(i==max_restore_time)
        {
            /* remove the i-node file and root directory associated with the evicted shadow root */
            /* 1. free all the blocks taken by the root directory */        
            i_node *buf = (i_node *)malloc(block_size);
            if( read_blocks(sp.shadow[0].pointer[0], 1, buf) <0 ) return -1;
            for(k=0;k<root_dir_block_num;k++)
            {
                fbm[buf[0].pointer[k]] = unused;
            }
            /* 2. free all the blocks taken by the i-node file */
            for(k=0;k<file_block_num;k++)
            {
                fbm[sp.shadow[0].pointer[k]] = unused;
            }

            /* evict the first one and shift the rest one spot above */
            for(k=0;k<max_restore_time-1;k++)
            {
                sp.shadow[k].size = sp.shadow[k+1].size;
                for(j=0;j<15;j++){ sp.shadow[k].pointer[j] = sp.shadow[k+1].pointer[j]; }
            }
            i = max_restore_time-1;
        }
        // let a j-node to store the current root 
        sp.shadow[i].size = sp.root.size;
        for(j=0;j<file_block_num;j++){ sp.shadow[i].pointer[j] = sp.root.pointer[j]; }
        commit_return_value = i;
        // 1. find an empty block in the data block to place the file
        if((new_file_block = unused_block()) <0) {printf("1\n");return -1;}
        fbm[new_file_block] = used;

        // 2.1 create an i-node in the copy of the i-node file
        if((new_i_node = unused_i_node()) <0 ) {printf("2\n");return -1;}
        i_node_array[new_i_node].size = 0;
        i_node_array[new_i_node].pointer[0] = new_file_block;
        for(k=1;k<15;k++){ i_node_array[new_i_node].pointer[k] = -1; }
        /////////////// 2.2 write the copy of the i-node file onto the disk  ///////////////
        if( write_file_to_blocks(file_block_num, &i_node_array, &sp.root.pointer) <0 ) {printf("3\n");return -1;}
        /////////////// 2.3 write the sp back to the disk  ///////////////
               
        // 3.1 create a new entry in the copy of the root directory
        if((new_dir_entry = unused_dir_entry()) <0 ) {printf("4\n");return -1;}
        root_dir[new_dir_entry].i_node_index = new_i_node;
        strcpy(root_dir[new_dir_entry].filename, name);
        /////////////// 3.2 write the copy of the root directory onto the disk ///////////////
        if( write_file_to_blocks(root_dir_block_num, &root_dir, &i_node_array[0].pointer) <0 ) {printf("5\n");return -1;}
        commit_sp();commit_i_node_file();commit_root_dir();
        // 4. create a new entry in the file descriptor table
        if((new_fd_entry = unused_fd_entry()) <0 ) {printf("6\n");return -1;}
        fd_table[new_fd_entry].i_node_number   = new_i_node;
        fd_table[new_fd_entry].read_ptr.block  = new_file_block; 
        fd_table[new_fd_entry].read_ptr.entry  = -1;
        fd_table[new_fd_entry].write_ptr.block = new_file_block; 
        fd_table[new_fd_entry].write_ptr.entry = i_node_array[new_i_node].size/block_size-1;

        //printf("use block %d\n", new_file_block);
        return new_fd_entry;
    }
    printf("fopen cannot reach here!\n");
    return -1;  
}

int inc_size(int fileID, int inc)
{
    if(fileID<0 || fileID>=max_file_num) return -1;
    int k=0, i_node_number=fd_table[fileID].i_node_number;
    while(i_node_number!=-1)
    {       
        i_node_array[i_node_number].size += inc;
        i_node_number = i_node_array[i_node_number].pointer[14];        
    }
    return inc;
}

int ssfs_fwrite(int fileID, char *buf, int length)
{
	if(length == 0) return 0;
	//printf("write %d\n", length);
    if(fileID<0 || fileID>=max_file_num) return -1;

    int k=0, block_to_write,
        i_node_index[max_file_num]={-1},
        i_node_number=fd_table[fileID].i_node_number;
    if(length == 0) return 0;
   //printf("first block %d\n", i_node_array[i_node_number].pointer[0]);
    /* if the file is opened */
    if(i_node_number != -1)
    {
        int block = fd_table[fileID].write_ptr.block;
        int entry = fd_table[fileID].write_ptr.entry;    // writing starts from the (entry+1)-th entry in this block   
        int offset = entry+1;    // # filled entries in the block containing the write pointer
        
        /* if the available entry in this block is more than enough */
        if(offset+length <= block_size) 
        {            //printf("case1\n");
            int acc = writes_block_by_char(block, offset, buf, length);
            if(acc != -1)
            {
                //printf("write ptr was%d:%d\n", fd_table[fileID].write_ptr.block, fd_table[fileID].write_ptr.entry);
                fd_table[fileID].write_ptr.entry += acc; 
                int inc = fd_table[fileID].write_ptr.entry - i_node_array[i_node_number].size%block_size+1;           
                /* if this block is the last one belongs to this file, then the file size may be incremented */
                if( find_block_to_read(i_node_number, block) == -1 && inc>0 ) inc_size(fileID, inc);
                //printf("write ptr %d:%d\n", fd_table[fileID].write_ptr.block, fd_table[fileID].write_ptr.entry);               
                commit_i_node_file(); load_i_node_file();
                //printf("write pointer at %d %d\n", fd_table[fileID].write_ptr.block, fd_table[fileID].write_ptr.entry);
                //printf("fwrite:\nfd-entry %d\nfilesize %d\nwrite ptr %d:%d\n", fileID, i_node_array[i_node_number].size, fd_table[fileID].write_ptr.block, fd_table[fileID].write_ptr.entry);
                return length;
            } 
            else return -1;
        }

        /* if the write pointer is at the last entry in a block and the length is smaller than block size */
        else if(entry==block_size-1 && length<=block_size)
        {   //printf("case2\n");
    		int temp = find_block_to_write(i_node_number, block);
            if(temp <0 ) {return -1;}
            fd_table[fileID].write_ptr.block = temp;
            fd_table[fileID].write_ptr.entry = -1;
            //printf("write ptr %d:%d\n", fd_table[fileID].write_ptr.block, fd_table[fileID].write_ptr.entry);
            return ssfs_fwrite(fileID, buf, length);                                            
        }

        /* if the available entry in this block is not enough, divide the buf into pieces for recursing */
        else
        {            //printf("case3\n"); 
            int avail = block_size-offset;  // # available entries in this block 
            int rest  = length-avail;       // # chars to be written in other blocks
            int piece = rest/block_size;    // # blocks to write to
            int last  = rest%block_size;    // # chars to be written in the last block
            int acc = 0;
            int temp;
            acc += ssfs_fwrite(fileID, buf, avail);
            //printf("acc1: %d\n", acc);
            for(k=0;k<piece;k++)
            { 
                if((temp = ssfs_fwrite(fileID, buf+avail+block_size*k, block_size))<0) { return -1;}
                else acc += temp; 
            }
            if((temp = ssfs_fwrite(fileID, buf+avail+ block_size*k, last))<0) { return -1;}
            else acc += temp;
            //printf("acc5: %d\n", acc);
            //buf[length] = '\0';
            //printf("write:\n%s\n", buf);
            //printf("write ptr %d:%d\n", fd_table[fileID].write_ptr.block, fd_table[fileID].write_ptr.entry);
            return acc;
        }
    }
    printf("fwrite: requested file is not opened\n");
    return -1;
}


int ssfs_fread(int fileID, char *buf, int length)
{
	//printf("read: %d\n", length);
    int k, block_to_read, i_node_number = fd_table[fileID].i_node_number;
    if(length == 0) return 0;
    if(i_node_array[i_node_number].size == 0) return 0;
    //printf("filesize:%d\n", i_node_array[i_node_number].size);
    //printf("filesize %d\n", i_node_array[i_node_number].size);
    //printf("fread:\nfd-entry %d\nfilesize %d\nread ptr %d:%d\n", fileID, i_node_array[i_node_number].size, fd_table[fileID].read_ptr.block, fd_table[fileID].read_ptr.entry);
    /* if the file is opened */
    if(fd_table[fileID].i_node_number != -1)
    {
        //printf("fread: fd-entry %d using i-node %d\n", fileID, fd_table[fileID].i_node_number);
        int block = fd_table[fileID].read_ptr.block;
        int entry = fd_table[fileID].read_ptr.entry;    // writing starts from the (entry+1)-th entry in this block   
        int offset = entry+1;    // # filled entries in the block containing the read pointer
        //printf("read pointer at %d\n", entry);
        /* if the read pointer will not go outside of this block */
        if(offset+length <= block_size) 
        {
            //printf("case1\n");
            int acc = reads_block_by_char(block, offset, buf, length); 
            if(acc != -1)
            {   //printf("case1.1\n");
                //printf("read ptr was%d:%d\n", fd_table[fileID].read_ptr.block, fd_table[fileID].read_ptr.entry);
                fd_table[fileID].read_ptr.entry += acc;
                //printf("read ptr %d:%d\n", fd_table[fileID].read_ptr.block, fd_table[fileID].read_ptr.entry);
                
                return acc;
            }
            else {printf("000\n"); return -1;}
        } 

        /* if the read pointer is at the last entry in a block and the length is smaller than block size */
        else if(entry==block_size-1 && length<=block_size)
        {//printf("case2\n");
    		int temp = find_block_to_read(i_node_number, block);
            if(temp <0 ) return -1;
            fd_table[fileID].read_ptr.block = temp;
            fd_table[fileID].read_ptr.entry = -1;
            //printf("read ptr %d:%d\n", fd_table[fileID].read_ptr.block, fd_table[fileID].read_ptr.entry);
            return ssfs_fread(fileID, buf, length);
        }

        /* if the available entry in this block is not enough, divide the buf into pieces for recursing */
        else
        {             //printf("case3\n");
            int avail = block_size-offset;   // # available entries in this block 
            int rest  = length-avail;       // # chars to be written in other blocks
            int piece = rest/block_size;    // # blocks to write to
            int last  = rest%block_size;    // # chars to be written in the last block
            int acc = 0;
            int temp;
            acc += ssfs_fread(fileID, buf, avail);
            //printf("avail=%d\nrest=%d\npiece=%d\nlast=%d\n", avail, rest, piece, last);
            //printf("acc1=%d\n", acc);
            for(k=0;k<piece;k++)
            { 
            	if((temp = ssfs_fread(fileID, buf+avail+block_size*k, block_size)) <0 ) { /*printf("acc2=%d\n", acc);*/return acc-(block_size-i_node_array[i_node_number].size%block_size);}
            	else acc+=temp; //printf("acc3=%d\n", acc);
            }
            if((temp = ssfs_fread(fileID, buf+avail+block_size*k, last)) <0 ) { /*printf("acc4=%d\n", acc);*/return acc;}
            else acc+=temp; //printf("acc5=%d\n", acc);
        	//buf[length] = '\0';
            //printf("read:\n%s\n", buf); 
            //printf("read ptr %d:%d\n", fd_table[fileID].read_ptr.block, fd_table[fileID].read_ptr.entry);
            return acc;
        }
    }
    else
    {
        printf("fread: requested file is not opened\n");
        return -1;
    }
    
}

int ssfs_fclose(int fileID)
{
    //printf("fileID: %d\n", fileID);
    if(fileID >= 0 && fileID < max_file_num)
    {
        if(fd_table[fileID].i_node_number == -1) return -1;
        else 
        {
            fd_table[fileID].i_node_number = -1;
        }
        return 0; 
    }
    return -1;
}

/* ptr is either 'r' or 'w' */
int fseek_helper(int fileID, int loc, char ptr)
{
    int block_index,
        i_node_number = fd_table[fileID].i_node_number;
    if(ptr == 'r')
    {
        /* check if this entry goes beyond this file */
        if(i_node_array[i_node_number].size < loc) {printf("filesize %d\n",i_node_array[i_node_number].size); printf("loc>size\n");return -1;}
        /* find the number of blocks the read pointer has to walk through */
        else if(loc/block_size == 0) 
        { 
            fd_table[fileID].read_ptr.entry = loc%block_size-1; 
            return 0; 
        }
        else
        {
            /* find the next block the read pointer should move to */
            block_index = find_block_to_read(i_node_number, fd_table[fileID].read_ptr.block);
            if(block_index == -1) { printf("read pointer out of bound\n"); return -1; }
            fd_table[fileID].read_ptr.block = block_index;
            fd_table[fileID].read_ptr.entry = 0;
            loc -= block_size;
            return fseek_helper(fileID, loc, 'r');
        }
    }
    else if(ptr == 'w')
    {
        /* check if this entry goes beyond this file */
        if(i_node_array[i_node_number].size < loc) return -1;
        /* find the number of blocks the read pointer has to walk through */
        else if(loc/block_size == 0) 
        { 
            fd_table[fileID].write_ptr.entry = loc%block_size-1; 
            return 0; 
        }
        else
        {
            /* find the next block the read pointer should move to */
            block_index = find_block_to_read(i_node_number, fd_table[fileID].write_ptr.block);
            if(block_index == -1) { printf("read pointer out of bound\n"); return -1; }
            fd_table[fileID].write_ptr.block = block_index;
            fd_table[fileID].write_ptr.entry = 0;
            loc -= block_size;
            return fseek_helper(fileID, loc, 'w');
        }
    }    
    else{ printf("Invalid paramenter\n"); return -1;}
}

int ssfs_frseek(int fileID, int loc)
{
    //printf("seek loc %d\n", loc);
    if(fileID >= 0 && fileID < max_file_num && loc>=0)
    {
        int i_node_number = fd_table[fileID].i_node_number;
        if( i_node_number == -1) { printf("case1\n"); return -1;}

        /* move the read pointer to the beginning of this file */
        fd_table[fileID].read_ptr.block = i_node_array[i_node_number].pointer[0];
        fd_table[fileID].read_ptr.entry = -1;
        //printf("read pointer at block %d\n", fd_table[fileID].read_ptr.block);
        return fseek_helper(fileID, loc, 'r');
    }
    else
    {
        printf("case3\n");
        return -1;
    }  
}

int ssfs_fwseek(int fileID, int loc)
{
    if(fileID >= 0 && fileID < max_file_num && loc>=0)
    {
        int i_node_number = fd_table[fileID].i_node_number;
        if( i_node_number == -1) return -1;

        /* move the read pointer to the beginning of this file */
        fd_table[fileID].write_ptr.block = i_node_array[i_node_number].pointer[0];
        fd_table[fileID].write_ptr.entry = -1;     

        return fseek_helper(fileID, loc, 'w');        
    }
    else return -1;
}

int ssfs_remove(char *file)
{
    int i, i_node_number, block;
    char empty[block_size] = {'0'};
    
    /* find the index of the i-node associated with this file in root directory */
    for(i=0;i<max_file_num;i++)
    {
        if(strcmp(root_dir[i].filename, file)==0) 
        { 
            i_node_number = root_dir[i].i_node_index; 
            root_dir[i].i_node_index = -1;
            //root_dir[i].filename = {'0'};
            break;
        }
    }
    if(i==max_file_num) return -1;
    /* if this file is opened, close it first */
    for(i=0;i<max_file_num;i++)
    { 
        if(fd_table[i].i_node_number == i_node_number) ssfs_fclose(i); 
    }
    /* find the i-node associated with this file in the i-node file (array)
     * write an empty block into every block this file takes up 
     * and set the fbm entry to be 1
     */
    block = i_node_array[i_node_number].pointer[0]; 
    while(block != -1)
    {
        if( write_blocks(block, 1, &empty) < 0) exit(EXIT_FAILURE);
        fbm[block] = unused; printf("free block: %d\n", block);
        block = find_block_to_read(i_node_number, block);    
    }

    /* clear all the i-nodes associated with this file in the i-node file (array) */
    int temp;
    do
    {
        i_node_array[i_node_number].size = -1;
        for(i=0;i<14;i++)
        {
            i_node_array[i_node_number].pointer[i] = -1;
        }
        temp = i_node_array[i_node_number].pointer[14];
        i_node_array[i_node_number].pointer[14] = -1;
        i_node_number = temp;
    }while(i_node_number != -1);

    /* remove the i-node file and root-dir file created when this file was opened */


    commit_fbm();
    commit_i_node_file();
    commit_root_dir();
    load_root_dir();
    return 0;
}
/*
int ssfs_commit()
{
    if(commit_return_value == -1) { printf("nothing to commit"); return -1;}
    int i;
    for(i=0;i<file_block_num;i++)
    {
        wm[sp.root.pointer[i]] = readonly;
    }
    for(i=0;i<root_dir_block_num;i++)
    {
        wm[i_node_array[0].pointer[i]] = readonly;
    }
    return commit_return_value;
}
int ssfs_restore(int cnum)
{
    if(cnum<0 or cnum>=max_restore_time){ printf("Invalid input\n"); return -1;}
}*/