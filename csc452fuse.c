/*
	FUSE: Filesystem in Userspace


	gcc -Wall `pkg-config fuse --cflags --libs` csc452fuse.c -o csc452


*/

#define	FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <ctype.h>

//size of a disk block
#define	BLOCK_SIZE 512

//we'll use 8.3 filenames
#define	MAX_FILENAME 8
#define	MAX_EXTENSION 3

//How many files can there be in one directory?
#define MAX_FILES_IN_DIR (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + (MAX_EXTENSION + 1) + sizeof(size_t) + sizeof(long))

//The attribute packed means to not align these things
struct csc452_directory_entry
{
	int nFiles;	//How many files are in this directory.
				//Needs to be less than MAX_FILES_IN_DIR

	struct csc452_file_directory
	{
		char fname[MAX_FILENAME + 1];	//filename (plus space for nul)
		char fext[MAX_EXTENSION + 1];	//extension (plus space for nul)
		size_t fsize;					//file size
		long nStartBlock;				//where the first block is on disk
	} __attribute__((packed)) files[MAX_FILES_IN_DIR];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.  
	char padding[BLOCK_SIZE - MAX_FILES_IN_DIR * sizeof(struct csc452_file_directory) - sizeof(int)];
} ;

typedef struct csc452_root_directory csc452_root_directory;

#define MAX_DIRS_IN_ROOT (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + sizeof(long))

struct csc452_root_directory
{
	int nDirectories;	//How many subdirectories are in the root
						//Needs to be less than MAX_DIRS_IN_ROOT
	struct csc452_directory
	{
		char dname[MAX_FILENAME + 1];	//directory name (plus space for nul)
		long nStartBlock;				//where the directory block is on disk
	} __attribute__((packed)) directories[MAX_DIRS_IN_ROOT];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.  
	char padding[BLOCK_SIZE - MAX_DIRS_IN_ROOT * sizeof(struct csc452_directory) - sizeof(int)];
} ;

typedef struct csc452_directory_entry csc452_directory_entry;

//How much data can one block hold?
#define	MAX_DATA_IN_BLOCK (BLOCK_SIZE)

struct csc452_disk_block
{
	//All of the space in the block can be used for actual data
	//storage.
	char data[MAX_DATA_IN_BLOCK];
};

typedef struct csc452_disk_block csc452_disk_block;

#define FAT_BLOCK_COUNT 40
#define FAT_BLOCK_SIZE (FAT_BLOCK_COUNT * BLOCK_SIZE)
#define FAT_ENTRIES ((FAT_BLOCK_SIZE / sizeof(short)) - FAT_BLOCK_COUNT)

//Prototypes
void open_root(csc452_root_directory *root);
void open_fat(short *fat_table);
int get_file(char *directory, char * file, char *extension);
char * read_file(char *directory, char * file, char *extension);
long get_directory(csc452_directory_entry *directory, char *directoryName);
int check_directory(char *directory);
int check_file(char *directory, char *file, char *extension);
int split_path(const char *path, char *directory, char *filename, char *extension);
void remove_directory(int pos, csc452_root_directory *root);
void set_fat_block(long blockAddr, short val);
long get_fat_block();
short get_fat_val(long blockAddr);
void update_file_size(size_t newSize, char * directory, char * file, char * extension);


/*
 * Called whenever the system wants to know the file attributes, including
 * simply whether the file exists or not.
 *
 * man -s 2 stat will show the fields of a stat structure
 */
static int csc452_getattr(const char *path, struct stat *stbuf)
{
	int res = 0;

	char directory[MAX_FILENAME + 1] = "";
	char file[MAX_FILENAME + 1] = "";
	char extension[MAX_EXTENSION + 1] = "";
    int fsize = -1;

	int file_type = split_path(path, directory, file, extension);
	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
	} 
	else if(file_type == 0 && check_directory(directory) == 1) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
	}
	else if(file_type >= 1 && (fsize = check_file(directory, file, extension)) != -1) {
		stbuf->st_mode = S_IFREG | 0666;
		stbuf->st_nlink = 2;
		stbuf->st_size = fsize;
	} 
	else {
		//Else return that path doesn't exist
		res = -ENOENT;
	}
	return res;
}

/*
 * Called whenever the contents of a directory are desired. Could be from an 'ls'
 * or could even be when a user hits TAB to do autocompletion
 */
static int csc452_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{

	//Since we're building with -Wall (all warnings reported) we need
	//to "use" every parameter, so let's just cast them to void to
	//satisfy the compiler
	(void) offset;
	(void) fi;

    char directory[MAX_FILENAME + 1];
	char file[MAX_FILENAME + 1];
	char extension[MAX_EXTENSION + 1];
    int fileOrDir = split_path(path, directory, file, extension);

    if(strcmp(path, "/") == 0) {
		filler(buf, ".", NULL,0);
		filler(buf, "..", NULL, 0);
        
		csc452_root_directory root;
		open_root(&root);
        for(int i = 0; i < root.nDirectories; i++) {
            if(strcmp(root.directories[i].dname, "\0") != 0) {
                filler(buf, root.directories[i].dname, NULL, 0);
            }
        }
    }
    else if(fileOrDir == 0 && check_directory(directory) == 1) {
		filler(buf, ".", NULL,0);
		filler(buf, "..", NULL, 0);

        csc452_directory_entry entry;
        get_directory(&entry, directory);
        for(int i = 0; i < entry.nFiles; i++) {
            if(strcmp(entry.files[i].fname, "\0") != 0) {
                if(strcmp(entry.files[i].fext, "\0") == 0) { 
                    filler(buf, entry.files[i].fname, NULL, 0);
                }
                else {
                    char fullFileName[MAX_FILENAME + MAX_EXTENSION + 2];
                    strcpy(fullFileName, entry.files[i].fname);
                    strcat(fullFileName, ".");
                    strcat(fullFileName, entry.files[i].fext);
                    filler(buf, fullFileName, NULL, 0);
                }
             }
        }
    }
    else {
        return -ENOENT;
    }
	return 0;
}

/*
 * Creates a directory. We can ignore mode since we're not dealing with
 * permissions, as long as getattr returns appropriate ones for us.
 */
static int csc452_mkdir(const char *path, mode_t mode)
{
	(void) path;
	(void) mode;
	char directory[MAX_FILENAME + 1] = "";
	char file[MAX_FILENAME + 1] = "";
	char extension[MAX_EXTENSION + 1] = "";
	int res = 0;
	int type = split_path(path, directory, file, extension);

	if(strlen(directory) > MAX_FILENAME){
		return -ENAMETOOLONG;
	}
	else if(type != 0){
		return -EPERM;
	}
	else if(check_directory(directory) != 0){
		return -EEXIST;
	}

	csc452_root_directory root;
	open_root(&root);

	if(root.nDirectories >= MAX_DIRS_IN_ROOT){
		printf("The directory could not be created, you have reached the maximum directories allowed in the root.\n");
		return -1;
	}

	root.nDirectories += 1;
	long blockPos = get_fat_block();

	//Update FAT table to mark the directory
	set_fat_block(blockPos, -1);
	//Create directory entry
	csc452_directory_entry newDir;
	newDir.nFiles = 0;
	FILE *disk = fopen(".disk", "r+b");
	strcpy(root.directories[root.nDirectories-1].dname, directory);
	root.directories[root.nDirectories-1].nStartBlock = blockPos;

	//Update disk
	fseek(disk, 0, SEEK_SET);
	fwrite(&root, BLOCK_SIZE, 1, disk);
	fseek(disk, blockPos, SEEK_SET);
	fwrite(&newDir, BLOCK_SIZE, 1, disk);
	fclose(disk);
	
	return res;
}

/*
 * Does the actual creation of a file. Mode and dev can be ignored.
 *
 * Note that the mknod shell command is not the one to test this.
 * mknod at the shell is used to create "special" files and we are
 * only supporting regular files.
 *
 */
static int csc452_mknod(const char *path, mode_t mode, dev_t dev)
{
	(void) path;
	(void) mode;
    (void) dev;
	
	char directory[MAX_FILENAME + 1] = "";
	char file[MAX_FILENAME + 1] = "";
	char extension[MAX_EXTENSION + 1] = "";
	int res = 0;

	split_path(path, directory, file, extension);

	if(strcmp(path, "/") == 0){
		res = -EPERM;
	}
	else if(strlen(file) > MAX_FILENAME){
		res = -ENAMETOOLONG;
	}
	else if(check_file(directory, file, extension) >= 0){
		res = -EEXIST;
	}
	else{
		csc452_directory_entry entry;
		long directoryStart = get_directory(&entry, directory);
		long blockPos = get_fat_block();
		entry.nFiles += 1;

		//Update FAT table to mark the file location
		set_fat_block(blockPos, -1);
		//Create directory entry
		entry.files[entry.nFiles-1].nStartBlock = blockPos;
		strcpy(entry.files[entry.nFiles-1].fname, file);
		if(strcmp(extension, "\0") == 0){
			strcpy(entry.files[entry.nFiles-1].fext, "\0");
		}
		else{
			strcpy(entry.files[entry.nFiles-1].fext, extension);
		}		
		entry.files[entry.nFiles-1].fsize = 0;

		//Update disk
		FILE *file = fopen(".disk", "r+b");
		fseek(file, directoryStart, SEEK_SET);
		fwrite(&entry, BLOCK_SIZE, 1, file);
		fclose(file);
	}

	return res;
}

/*
 * Read size bytes from file into buf starting from offset
 *
 */
static int csc452_read(const char *path, char *buf, size_t size, off_t offset,
			  struct fuse_file_info *fi)
{
	(void) buf;
	(void) offset;
	(void) fi;
	(void) path;

	char directory[MAX_FILENAME + 1] = "";
	char file[MAX_FILENAME + 1] = "";
	char extension[MAX_EXTENSION + 1] = "";
	int res = 0;

	split_path(path, directory, file, extension);

	if(check_file(directory, file, extension) <= 0){
		return -ENOENT;
	}



	//check to make sure path exists
	//check that size is > 0
	//check that offset is <= to the file size
	//read in data
	//return success, or error

	return size;
}

/*
 * Write size bytes from buf into file starting from offset
 *
 */
static int csc452_write(const char *path, const char *buf, size_t size,
			  off_t offset, struct fuse_file_info *fi) {
	(void) buf;
	(void) offset;
	(void) fi;
	(void) path;
    char directory[MAX_FILENAME + 1];
	char file[MAX_FILENAME + 1];
	char extension[MAX_EXTENSION + 1];
    int fileOrDir = split_path(path, directory, file, extension);
	size_t res = size;
	size_t fileSize = 0;

    if(offset > (fileSize = check_file(directory, file, extension))) {
        return -EFBIG;    
    }
    else if(check_directory(directory) == 1) {
        long fileStart = get_file(directory, file, extension);
        long fileStartIndex = fileStart / BLOCK_SIZE;
        int offsetIndex = offset / BLOCK_SIZE;
        int beginWriting = offset % BLOCK_SIZE;

		update_file_size((fileSize + res), directory, file, extension);
 
	    FILE *disk = fopen(".disk", "r+b");
		csc452_disk_block block;
        // Walk to the block where we want to modify
        for(int i = 0; i < offsetIndex; i++) {
            if(get_fat_val(fileStartIndex * BLOCK_SIZE) != -1) {
                fileStartIndex = get_fat_val(fileStartIndex * BLOCK_SIZE);
            }
        }
        
        // Grab that block
        fseek(disk, (fileStartIndex * BLOCK_SIZE), SEEK_SET); 
        fread(&block, sizeof(csc452_disk_block), 1, disk);
        
        // When the size is smaller than the block        
        if((strlen(block.data) + size) <= BLOCK_SIZE) {
            // Need to add beginWriting to handle adding offset
            strncpy(block.data + beginWriting, buf, size);
            fseek(disk, fileStartIndex * BLOCK_SIZE, SEEK_SET);
            fwrite(&block, sizeof(csc452_disk_block), 1, disk);
        }
        else {
            // Need to add beginWriting to handle adding offset
            strncpy(block.data + beginWriting, buf, (BLOCK_SIZE - beginWriting));
            fseek(disk, fileStartIndex * BLOCK_SIZE, SEEK_SET);
            fwrite(&block, sizeof(csc452_disk_block), 1, disk);
			printf("block written...\n");
			fflush(0);
            // Increment the buffer
            buf += (BLOCK_SIZE) - beginWriting;
            size = size - (BLOCK_SIZE - beginWriting);
            
            // As long as there are available blocks to use

            while(get_fat_val(fileStartIndex * BLOCK_SIZE) != -1) {
                fileStartIndex = get_fat_val(fileStartIndex * BLOCK_SIZE);
                fseek(disk, fileStartIndex * BLOCK_SIZE, SEEK_SET);
                if(size > BLOCK_SIZE) {
                    strncpy(block.data, buf, BLOCK_SIZE);
                    fwrite(&block, sizeof(csc452_disk_block), 1, disk);
                    buf += BLOCK_SIZE;
                    size = size - BLOCK_SIZE;
                }
                else {
                    strncpy(block.data, buf, size);
                    fwrite(&block, sizeof(csc452_disk_block), 1, disk);
                    size = 0; 
                }
            }
            // More to write and need more blocks
			long prevBlock = 0;
            while(size > 0) {
                if(size > BLOCK_SIZE) {
                    strncpy(block.data, buf, BLOCK_SIZE);
                    long nextBlock = get_fat_block();
					if(prevBlock != 0){
						set_fat_block(prevBlock, (nextBlock/BLOCK_SIZE));
					}
					prevBlock = nextBlock;
                    fseek(disk, nextBlock, SEEK_SET);
                    fwrite(&block, sizeof(csc452_disk_block), 1, disk);
                    set_fat_block(nextBlock, -1);
                    buf += BLOCK_SIZE;
                    size = size - BLOCK_SIZE;
                }
                else {
                    strncpy(block.data, buf, size);
                    long nextBlock = get_fat_block();
					if(prevBlock != 0){
						set_fat_block(prevBlock, (nextBlock/BLOCK_SIZE));
					}
					prevBlock = nextBlock;
                    fseek(disk, nextBlock, SEEK_SET);
                    fwrite(&block, sizeof(csc452_disk_block), 1, disk);
                    set_fat_block(nextBlock, -1);
                    buf += BLOCK_SIZE;
                    break;
                }
            } 
        }
    }

	return res;
}

/*
 * Removes a directory (must be empty)
 *
 */
static int csc452_rmdir(const char *path)
{
	int res = 0;
	char directory[MAX_FILENAME + 1] = "";
	char file[MAX_FILENAME + 1] = "";
	char extension[MAX_EXTENSION + 1] = "";

	int file_type = split_path(path, directory, file, extension);

	if(strcmp(file, "\0") != 0){
		res = -ENOTDIR;
		return res;
	}

	if(check_directory(directory) != 1){
		res = -ENOENT;
		return res;
	}

	csc452_directory_entry entry;
	get_directory(&entry, directory);

	if(entry.nFiles > 0){
		res = -ENOTEMPTY;
	} else{
		csc452_root_directory root;
		open_root(&root);
		for(int i = 0; i < root.nDirectories; i++){
			if(strcmp(directory, root.directories[i].dname) == 0){
				remove_directory(i, &root);
				set_fat_block(root.directories[i].nStartBlock, 0);
				//Update disk
				FILE *file = fopen(".disk", "r+b");
				fseek(file, 0, SEEK_SET);
				fwrite(&root, BLOCK_SIZE, 1, file);
				fclose(file);
				break;
			}
		}
	}

	return res;
}

/*
 * Removes a file.
 *
 */
static int csc452_unlink(const char *path)
{
        (void) path;
        return 0;
}


/******************************************************************************
 *
 *  DO NOT MODIFY ANYTHING BELOW THIS LINE
 *
 *****************************************************************************/

/*
 * truncate is called when a new file is created (with a 0 size) or when an
 * existing file is made shorter. We're not handling deleting files or
 * truncating existing ones, so all we need to do here is to initialize
 * the appropriate directory entry.
 *
 */
static int csc452_truncate(const char *path, off_t size)
{
	(void) path;
	(void) size;

    return 0;
}

/*
 * Called when we open a file
 *
 */
static int csc452_open(const char *path, struct fuse_file_info *fi)
{
	(void) path;
	(void) fi;
    /*
        //if we can't find the desired file, return an error
        return -ENOENT;
    */

    //It's not really necessary for this project to anything in open

    /* We're not going to worry about permissions for this project, but
	   if we were and we don't have them to the file we should return an error

        return -EACCES;
    */

    return 0; //success!
}

/*
 * Called when close is called on a file descriptor, but because it might
 * have been dup'ed, this isn't a guarantee we won't ever need the file
 * again. For us, return success simply to avoid the unimplemented error
 * in the debug log.
 */
static int csc452_flush (const char *path , struct fuse_file_info *fi)
{
	(void) path;
	(void) fi;

	return 0; //success!
}


//register our new functions as the implementations of the syscalls
static struct fuse_operations csc452_oper = {
    .getattr	= csc452_getattr,
    .readdir	= csc452_readdir,
    .mkdir		= csc452_mkdir,
    .read		= csc452_read,
    .write		= csc452_write,
    .mknod		= csc452_mknod,
    .truncate	= csc452_truncate,
    .flush		= csc452_flush,
    .open		= csc452_open,
    .unlink		= csc452_unlink,
    .rmdir		= csc452_rmdir
};

void open_root(csc452_root_directory *root){
	FILE *file = fopen(".disk", "r+b");
	if(file != NULL){
		if(fread(root, sizeof(csc452_root_directory), 1, file) == (size_t)0){
			printf("File could not be read\n");
			return;
		}
		fclose(file);
	}
}

void open_fat(short *fat_table){
	FILE *file = fopen(".disk", "r+b");

	if(file != NULL){
		fseek(file, -sizeof(fat_table), SEEK_END);
		if(fread(fat_table, sizeof(fat_table), 1, file) == (size_t)0){
			printf("File could not be read\n");
			return;
		}
		fclose(file);
	}
}

long get_directory(csc452_directory_entry *directory, char *directoryName){
	long startBlock = 0;
	csc452_root_directory root;
	open_root(&root);
	for(int i = 0; i < root.nDirectories; i++){
		if(strcmp(directoryName, root.directories[i].dname) == 0){
			startBlock = root.directories[i].nStartBlock;
			break;
		}
	}

	FILE *file = fopen(".disk", "r");
	if(file != NULL){
		fseek(file, startBlock, SEEK_SET);
		fread(directory, sizeof(csc452_directory_entry), 1, file);
		fclose(file);
	}
	return startBlock;
}

// Will always check_file before calling get_file
// Returns the Start Block of that file
int get_file(char *directory, char * file, char *extension) {
    csc452_directory_entry entry;
    get_directory(&entry, directory);

    for(int i = 0; i < entry.nFiles; i++) {
        if(strcmp(entry.files[i].fname, file) == 0 && 
            strcmp(entry.files[i].fext, extension) == 0) {
            return entry.files[i].nStartBlock;
        }
    }
    return -1;
}

// Will always check_file before calling get_file to call read_file
// Returns a string of the entire file 
char * read_file(char *directory, char * file, char *extension) {
    int startBlock = 0; 
    if((startBlock = get_file(directory, file, extension)) == -1) {
        return NULL;    
    }
    
    int fsize = check_file(directory, file, extension);
    short fatIndex = get_file(directory, file, extension) / BLOCK_SIZE; 
   
    FILE * disk = fopen(".disk", "r");
    char *buf = calloc(1, fsize); 
    
    for(int i = 0; i < ((fsize / BLOCK_SIZE)) + 1; i++) {
        fseek(disk, BLOCK_SIZE * fatIndex, SEEK_SET);
        fread(buf + (i * BLOCK_SIZE), BLOCK_SIZE, 1, disk);
        fatIndex = get_fat_val(fatIndex); 
    }
   
    fclose(disk);  
    return buf;
}

int check_directory(char *directory){
	int flag = 0;
	csc452_root_directory root;
	open_root(&root);
	for(int i = 0; i < root.nDirectories; i++){
		if(strcmp(directory, root.directories[i].dname) == 0){
			flag = 1;
			break; 
		}
	}

	return flag;
}

int check_file(char *directory, char * file, char *extension){
	int flag = -1;

	if(check_directory(directory) == 0){
		return flag;
	} 
	else {
		csc452_directory_entry entry;
		get_directory(&entry, directory);
		for(int i = 0; i < entry.nFiles; i++){
			if(strcmp(entry.files[i].fname, file) == 0 && strcmp(extension, entry.files[i].fext) == 0){
				flag = entry.files[i].fsize;
				break;
			}
		}
	}
	return flag;
}

void remove_directory(int pos, csc452_root_directory *root){
	if(pos == (root->nDirectories - 1)){
		strcpy(root->directories[pos].dname, "\0");
		root->nDirectories -= 1;
	}
	else{
		root->directories[pos] = root->directories[root->nDirectories-1];
		strcpy(root->directories[root->nDirectories-1].dname, "\0");
		root->nDirectories -=1;
	}
}

void update_file_size(size_t newSize, char * directory, char * file, char * extension){
	csc452_directory_entry entry;
	long startBlock = get_directory(&entry, directory);

	for(int i = 0; i < entry.nFiles; i++){
		if(strcmp(entry.files[i].fname, file) == 0 && strcmp(extension, entry.files[i].fext) == 0){
			entry.files[i].fsize = newSize;
			FILE *file = fopen(".disk", "r+b");
			fseek(file, startBlock, SEEK_SET);
			fwrite(&entry, BLOCK_SIZE, 1, file);
			fclose(file);
			break;
		}
	}
}

int split_path(const char *path, char *directory, char *file, char *extension){
	int readIn = sscanf(path, "/%[^/]/%[^.].%s", directory, file, extension);
	directory[MAX_FILENAME] = '\0';
	file[MAX_FILENAME] = '\0';
	extension[MAX_EXTENSION] = '\0';

	int file_type = -1;
	file_type += readIn;

	if(file_type == 1){
		extension = "\0";
	}

	return file_type;
}

long get_fat_block(){
	FILE *disk = fopen(".disk", "r+b");
	short fat_val;
	fseek(disk, ((-FAT_BLOCK_SIZE) + sizeof(short)), SEEK_END);
	fread(&fat_val, sizeof(short), 1, disk);
	for(int i = 1; i < FAT_ENTRIES; i++){
		if(fat_val == 0){
			return i * 512;
		}
		fread(&fat_val, sizeof(short), 1, disk);
	}
	fclose(disk);
	return -1;
}

void set_fat_block(long blockAddr, short val){
	FILE *disk = fopen(".disk", "r+b");
	short fat_entry = blockAddr / BLOCK_SIZE;
	fseek(disk, (-FAT_BLOCK_SIZE + ((sizeof(short) * fat_entry))), SEEK_END);
	fwrite(&val, sizeof(short), 1, disk);
	fclose(disk);
}

short get_fat_val(long blockAddr){
	FILE *disk = fopen(".disk", "r+b");
	short fat_entry = blockAddr / BLOCK_SIZE;
	short res = 0;
	fseek(disk, (-FAT_BLOCK_SIZE + ((sizeof(short) * fat_entry))), SEEK_END);
	fread(&res, sizeof(short), 1, disk);
	fclose(disk);
	return res;
}

//Don't change this.
int main(int argc, char *argv[])
{
	return fuse_main(argc, argv, &csc452_oper, NULL);
}
