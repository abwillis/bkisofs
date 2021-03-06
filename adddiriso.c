/* From example.c */
#include <stdio.h>
#include <time.h>
#include <memory.h>

/* need to include bk.h for access to bkisofs functions and structures */
#include "bk.h"

void addProgressUpdaterCbk(VolInfo* volInfo);
void fatalError(const char* message);
void printNameAndContents(BkFileBase* item, int numSpaces);
void readProgressUpdaterCbk(VolInfo* volInfo);
void writeProgressUpdaterCbk(VolInfo* volInfo, double percentComplete);
char* forwardtoback(char* paths);

int main( int argv, char** argc)
{
    /* Check for proper number of arguments */
    if(argv <= 4)
        fatalError("Usage: adddiriso myfile.iso new-dir-name destination-path outfile.iso\nfull paths required");
        /* fatalError("Usage: addfileiso myfile.iso file-to-add destination-path outfile.iso\nfull paths required"); */ /* For add file */

    /* A variable of type VolInfo stores information about an image */
    VolInfo volInfo;
    /* bk functions return ints that need to be checked to see whether
    * the functions were successful or not */
    int rc;

    /* Variables for paths */
    /* char* fullpath = (char *) malloc(1 + strlen(argc[2]) + strlen(argc[3])); */ /* for add file */
    char* partpath;
    char* partpathws = (char *) malloc(256);
    /* char* spath = (char *) malloc(strlen(argc[3])); */
    /* strcpy(spath,argc[3]); */  /* This line and the previous is used for addfileiso */
    char* opath = (char *) malloc(strlen(argc[2]));

    printf("argc[2] %s\nargc[3] %s\n",argc[2], argc[3]);
    opath = forwardtoback(argc[3]);
    /* npath = forwardtoback(argc[2]); */

    /* initialise volInfo, set it up to scan for duplicate files */
    rc = bk_init_vol_info(&volInfo, true);
    if(rc <= 0)
        fatalError(bk_get_error_string(rc));
    
    /* open the iso file (supplied as argument 1) */
    rc = bk_open_image(&volInfo, argc[1]);
    if(rc <= 0)
        fatalError(bk_get_error_string(rc));
    
    /* read information about the volume (required before reading directory tree) */
    rc = bk_read_vol_info(&volInfo);
    if(rc <= 0)
        fatalError(bk_get_error_string(rc));
    
    /* read the directory tree */
    if(volInfo.filenameTypes & FNTYPE_ROCKRIDGE)
        rc = bk_read_dir_tree(&volInfo, FNTYPE_ROCKRIDGE, true, readProgressUpdaterCbk);
    else if(volInfo.filenameTypes & FNTYPE_JOLIET)
        rc = bk_read_dir_tree(&volInfo, FNTYPE_JOLIET, false, readProgressUpdaterCbk);
    else
        rc = bk_read_dir_tree(&volInfo, FNTYPE_9660, false, readProgressUpdaterCbk);
    if(rc <= 0)    
        fatalError(bk_get_error_string(rc));

    /* Add Directories */
;
        partpath = strtok(argc[2],"\\/");
        strcpy(partpathws, opath);
        while (partpath != NULL) {
            printf("partpathws %s\npartpath %s\n",partpathws, partpath);
            rc = bk_create_dir(&volInfo, partpathws, partpath);
            strcat(partpathws, partpath);
            strcat(partpathws, "/");
            partpath = strtok(NULL,"\\/");
        }

    if(rc <= 0) 
        fatalError(bk_get_error_string(rc));


#if 0  /* Below used for adding files */
    /* File to add to volume */
    rc = bk_add(&volInfo, argc[2], spath, addProgressUpdaterCbk);
    if(rc <= 0)   {
        if(rc == BKERROR_DUPLICATE_ADD) {
            strcpy(fullpath, spath);
            strcat(fullpath, "/");
            strcat(fullpath, argc[2]);
            rc = bk_delete(&volInfo, fullpath);
            if(rc <=0) {
                fatalError(bk_get_error_string(rc));
            }
            rc = bk_add(&volInfo, argc[2], spath, addProgressUpdaterCbk);
            if(rc <=0) {
                fatalError(bk_get_error_string(rc));
            }
        } else if (rc == BKERROR_DIR_NOT_FOUND_ON_IMAGE) {
            partpath = strtok(argc[3],"\\/");
            strcpy(partpathws, "");
            while (partpath != NULL) {
                strcat(partpathws, "/");
                rc = bk_create_dir(&volInfo, partpathws, partpath);
                strcat(partpathws, partpath);
                partpath = strtok(NULL,"\\/");
            }
           
            if(rc <= 0) 
                fatalError(bk_get_error_string(rc));
            rc = bk_add(&volInfo, argc[2], spath, addProgressUpdaterCbk);
            if(rc <= 0) 
                fatalError(bk_get_error_string(rc));
        }
    }
#endif
    
    /* print the entire directory tree */
    printNameAndContents(BK_BASE_PTR( &(volInfo.dirTree) ), 0);
    
    /* save the new ISO as the fourth argument given */
    /* note that bkisofs will print some stuff to stdout when writing an ISO */
    
    rc = bk_write_image(argc[4], &volInfo, time(NULL),
                        FNTYPE_9660 | FNTYPE_ROCKRIDGE | FNTYPE_JOLIET,
                        writeProgressUpdaterCbk);
    
    /* we're finished with this ISO, so clean up */
    bk_destroy_vol_info(&volInfo);
    
    return 0;
}

/* you can use this to update a progress bar or something */
void addProgressUpdaterCbk(VolInfo* volInfo)
{
    printf("Add progress updater\n");
}

void fatalError(const char* message)
{
    printf("Fatal error: %s\n", message);
    exit(1);
}

void printNameAndContents(BkFileBase* base, int numSpaces)
{
    int count;
    
    /* print the spaces (indentation, for prettyness) */
    for(count = 0; count < numSpaces; count++)
        printf(" ");
    
    if(IS_DIR(base->posixFileMode))
    {
        /* print name of the directory */
        printf("%s (directory)\n", base->name);
        
        /* print all the directory's children */
        BkFileBase* child = BK_DIR_PTR(base)->children;
        while(child != NULL)
        {
            printNameAndContents(child, numSpaces + 2);
            child = child->next;
        }
    }
    else if(IS_REG_FILE(base->posixFileMode))
    {
        /* print name and size of the file */
        printf("%s (regular file), size %u\n", base->name, BK_FILE_PTR(base)->size);
    }
    else if(IS_SYMLINK(base->posixFileMode))
    {
        /* print name and target of the symbolic link */
        printf("%s -> %s (symbolic link)\n", base->name, BK_SYMLINK_PTR(base)->target);
    }
}

/* you can use this to update a progress bar or something */
void readProgressUpdaterCbk(VolInfo* volInfo)
{
    printf("Read progress updater\n");
}

/* you can use this to update a progress bar or something */
void writeProgressUpdaterCbk(VolInfo* volInfo, double percentComplete)
{
    printf("Write progress updater: ~%.2lf%% complete\n", percentComplete);
}

/* Need backward slashes but users used to forward slashes, change them */
char* forwardtoback(char* paths)
{
    char* partpath = (char *) malloc(256);
    char* partpathws = (char *) malloc(256);
    printf("paths %s\n", paths);
    partpath = strtok(paths,"\\/");
    printf("partpath %s\n", partpath);
    strcpy(partpathws, "/");
    while (partpath != NULL) {
        strcat(partpathws, partpath);
        partpath = strtok(NULL,"\\/");
        strcat(partpathws, "/");
    }
    printf("partpathws %s\n",partpathws);
    return partpathws;
}
